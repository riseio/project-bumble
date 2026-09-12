#include "native_level_editor.hpp"

#if defined(_WIN32)
#include <Windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <json/json.hpp>

#include "funcs.h"
#include "librecomp/addresses.hpp"
#include "native_collision_debug.hpp"
#include "native_graphics_options.hpp"
#include "native_key_codes.hpp"
#include "native_modern_controls.hpp"
#include "native_player_collision.hpp"
#include "native_rt64_renderer.hpp"
#include "native_text_overlay_state.hpp"
#include "native_weapon_system.hpp"

namespace {

using Words = std::array<uint32_t, 13>;

constexpr uint32_t kCurrentLevel = 0x800E9640u;
constexpr uint32_t kPlayerOneOwner = 0x800E91D4u;
constexpr uint32_t kPlayerOneCamera = 0x800E9254u;
constexpr uint32_t kCurrentPlacementInterpreter = 0x800D735Cu;
constexpr uint32_t kObjectListBase = 0x800D7460u;
constexpr uint32_t kObjectListStride = 0xC8Cu;
constexpr uint32_t kObjectListCount = 14u;
constexpr uint32_t kMaximumActorsPerList = 8192u;
constexpr uint32_t kPlayerVtable = 0x800445E8u;
constexpr uint32_t kTerrainCellBase = 0x803CE000u;
constexpr uint32_t kTerrainCellStride = 8u;
constexpr uint32_t kTerrainCellDescriptorOffset = 2u;
constexpr size_t kTerrainCellCount = 64u * 64u;
constexpr uint32_t kScratchBytes = 0x120u;
constexpr uint32_t kScratchRecordOffset = 0xC0u;
constexpr uint32_t kRecordBytes = 0x34u;
constexpr uint32_t kRomSpawnCommand = 0x80092144u;
constexpr uint32_t kF3 = 0x72u;
constexpr size_t kPageSize = 10u;
constexpr size_t kMaximumEdits = 4096u;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kMaximumWorldCoordinate = 200000.0f;

enum class Mode : uint32_t {
    Inactive,
    Menu,
    Place,
    Delete,
};

const char* mode_name(Mode mode) {
    switch (mode) {
    case Mode::Inactive:
        return "inactive";
    case Mode::Menu:
        return "menu";
    case Mode::Place:
        return "place";
    case Mode::Delete:
        return "delete";
    }
    return "unknown";
}

struct CatalogEntry {
    Words words{};
    uint32_t vtable = 0u;
    uint32_t source_count = 0u;
    uint32_t first_rom_offset = 0u;
    std::string label;
    uint32_t level_resource = 0u;
    uint32_t observed_level = UINT32_MAX;
};

enum class CatalogFilter : uint8_t {
    All,
    Enemies,
    Pickups,
    World,
};

struct SavedEntry {
    uint64_t id = 0u;
    uint32_t level = 0u;
    uint32_t vtable = 0u;
    uint32_t actor = 0u;
    Words words{};
    std::vector<uint32_t> actors;
    bool active = false;
};

struct Scratch {
    uint8_t* allocation = nullptr;
    uint8_t* rdram = nullptr;
    uint32_t guest = 0u;
};

std::atomic_bool g_initialized{false};
std::atomic<Mode> g_mode{Mode::Inactive};
std::atomic_bool g_f3_down{false};
std::atomic_bool g_q_down{false};
std::atomic_bool g_e_down{false};
std::atomic_bool g_mouse_down{false};
std::atomic_uint64_t g_click_revision{0u};
std::atomic_int32_t g_yaw_degrees{0};

std::mutex g_mutex;
std::filesystem::path g_edits_path;
std::vector<CatalogEntry> g_catalog;
uint64_t g_authored_record_count = 0u;
size_t g_authored_signature_count = 0u;
size_t g_non_placeable_control_signature_count = 0u;
std::vector<SavedEntry> g_saved;
uint64_t g_next_id = 1u;
uint32_t g_level = UINT32_MAX;
size_t g_page = 0u;
std::optional<size_t> g_selected;
CatalogFilter g_filter = CatalogFilter::All;
bool g_saved_spawned = false;
Scratch g_scratch{};
bumble::level_editor::Preview g_preview{};
uint32_t g_last_screen = UINT32_MAX;
uint64_t g_last_click = 0u;
uint64_t g_last_pointer_motion = 0u;
Mode g_last_mode = Mode::Inactive;
thread_local bool g_editor_spawn = false;
thread_local bool g_preview_update_tracking = false;
thread_local std::unordered_set<uint32_t> g_preview_update_actors_before;
uint32_t g_live_preview_actor = 0u;
std::vector<uint32_t> g_live_preview_actors;
std::unordered_set<uint32_t> g_preview_updates_pending;
std::unordered_set<uint32_t> g_destruction_requested;
std::unordered_set<uint32_t> g_deferred_actor_updates;
uint32_t g_live_preview_vtable = 0u;
size_t g_live_preview_catalog = SIZE_MAX;
size_t g_preview_failed_catalog = SIZE_MAX;
bool g_menu_publish_logged = false;
bool g_clear_level_armed = false;

gpr guest_address(uint32_t address) {
    return static_cast<gpr>(static_cast<int32_t>(address));
}

bool valid_guest(uint32_t address, size_t bytes) {
    if (address < 0x80000000u) {
        return false;
    }
    const uint64_t offset = static_cast<uint64_t>(address - 0x80000000u);
    return offset <= recomp::mem_size &&
        static_cast<uint64_t>(bytes) <= recomp::mem_size - offset;
}

uint32_t read_u32(uint8_t* rdram, uint32_t address) {
    (void)rdram;
    return static_cast<uint32_t>(MEM_W(0, guest_address(address)));
}

void write_u32(uint8_t* rdram, uint32_t address, uint32_t value) {
    (void)rdram;
    MEM_W(0, guest_address(address)) = static_cast<int32_t>(value);
}

void write_f32(uint8_t* rdram, uint32_t address, float value) {
    write_u32(rdram, address, std::bit_cast<uint32_t>(value));
}

float read_f32(uint8_t* rdram, uint32_t address) {
    return std::bit_cast<float>(read_u32(rdram, address));
}

bool finite_world(float value) {
    return std::isfinite(value) &&
        std::abs(value) <= kMaximumWorldCoordinate;
}

int32_t authored_coordinate(float world, bool horizontal) {
    const double adjusted = horizontal
        ? (static_cast<double>(world) + 5120.0) / 1.6
        : static_cast<double>(world) / 1.6;
    return static_cast<int32_t>(std::llround(adjusted));
}

float world_coordinate(int32_t authored, bool horizontal) {
    const float value = static_cast<float>(authored) * 1.6f;
    return horizontal ? value - 5120.0f : value;
}

bool coordinate_self_check() {
    constexpr std::array<float, 5> values{
        -5100.8f, -512.0f, 0.0f, 123.2f, 5118.4f
    };
    for (float value : values) {
        const int32_t encoded = authored_coordinate(value, true);
        if (std::abs(world_coordinate(encoded, true) - value) > 0.81f) {
            return false;
        }
    }
    return authored_coordinate(160.0f, false) == 100 &&
        std::abs(world_coordinate(100, false) - 160.0f) < 0.01f;
}

bool valid_words(const Words& words) {
    return (words[8] & 0x2u) != 0u &&
        words[10] != 1u &&
        words[7] < kObjectListCount &&
        static_cast<int32_t>(words[0]) >= -200000 &&
        static_cast<int32_t>(words[0]) <= 200000 &&
        static_cast<int32_t>(words[1]) >= -200000 &&
        static_cast<int32_t>(words[1]) <= 200000 &&
        static_cast<int32_t>(words[2]) >= -200000 &&
        static_cast<int32_t>(words[2]) <= 200000;
}

bool non_placeable_control(const Words& words) {
    return words[10] == 0x40000000u ||
        (words[10] == 0x00020000u && (words[9] & 0x1Fu) == 21u);
}

bool same_factory_signature(const Words& left, const Words& right) {
    return left[10] == right[10] &&
        left[9] == right[9] &&
        left[8] == right[8] &&
        (left[10] != 0x40000000u ||
         (left[12] & 0x7u) == (right[12] & 0x7u));
}

uint32_t effective_commander_model(const Words& words) {
    const uint32_t model = words[9] >> 5u;
    return model == 0u ? 1u : model;
}

bool same_catalog_key(const Words& left, const Words& right) {
    if (left[10] == 0x00000004u && right[10] == 0x00000004u) {
        return effective_commander_model(left) ==
            effective_commander_model(right);
    }
    return same_factory_signature(left, right);
}

const char* pickup_name(uint32_t subtype) {
    if (subtype < bumble::weapon_system::weapon_count()) {
        return bumble::weapon_system::editor_label(subtype);
    }
    if (subtype == 12u) {
        return "Small health";
    }
    if (subtype == 13u) {
        return "Full health";
    }
    return nullptr;
}

const char* actor_class_name(uint32_t vtable) {
    switch (vtable) {
    case 0x80043F08u: return "Ant";
    case 0x800466D8u: return "Web box trap";
    case 0x80043F40u: return "Ant nest";
    case 0x80045260u: return "Acid drop";
    case 0x800452D0u: return "Bug Tank nest";
    case 0x800470B0u: return "Mini Bumble";
    case 0x80047258u: return "Cranefly";
    case 0x80047428u: return "Dragonfly";
    case 0x80047818u: return "Wasp wave";
    case 0x80047858u: return "Wasp portal";
    case 0x800478C8u: return "Wasp";
    case 0x80047900u: return "Wasp nest";
    case 0x80047BD8u: return "Web Spider";
    case 0x80047DA8u: return "Hoverfly";
    case 0x80047DE0u: return "Hoverfly wave";
    case 0x80047E50u: return "Hoverfly nest";
    case 0x80047E88u: return "Block Bug";
    case 0x800480A8u: return "Killerpiller";
    case 0x80049608u: return "Golden Flea";
    case 0x80049640u: return "Grab Bug";
    case 0x80049678u: return "Grab Bug nest";
    case 0x80049908u: return "Transporter";
    case 0x80049B18u: return "Weevil";
    case 0x8004A080u: return "Spotter";
    case 0x8004A1F0u: return "Water Beetle";
    case 0x8004A308u: return "Louse gun";
    case 0x8004B170u: return "Commander";
    case 0x8004B390u: return "Scorpion";
    case 0x8004B678u: return "Chain Moth";
    case 0x8004B9F0u: return "Zeppelin";
    case 0x8004C360u: return "Mantis";
    case 0x8004C550u: return "Wood Wasp";
    default: return nullptr;
    }
}

bool enemy_selector(uint32_t selector) {
    switch (selector) {
    case 0x00000002u:
    case 0x00000004u:
    case 0x00000008u:
    case 0x00000010u:
    case 0x00000020u:
    case 0x00000040u:
    case 0x00000080u:
    case 0x00000100u:
    case 0x00000200u:
    case 0x00000400u:
    case 0x00000800u:
    case 0x00001000u:
    case 0x00002000u:
    case 0x00004000u:
    case 0x00008008u:
    case 0x00008040u:
    case 0x00008200u:
    case 0x00008800u:
    case 0x00040000u:
    case 0x00080000u:
    case 0x00100000u:
    case 0x00800000u:
    case 0x80000000u:
    case 0x80008000u:
        return true;
    default:
        return false;
    }
}

CatalogFilter catalog_filter(const Words& words) {
    if (words[10] == 0x00020000u) {
        return CatalogFilter::Pickups;
    }
    if (enemy_selector(words[10])) {
        return CatalogFilter::Enemies;
    }
    return CatalogFilter::World;
}

std::string object_label(const Words& words, uint32_t vtable = 0u) {
    const uint32_t selector = words[10];
    const uint32_t group = words[9] & 0x1Fu;
    const uint32_t model = words[9] >> 5u;
    if (const char* name = actor_class_name(vtable); name != nullptr) {
        return name;
    }
    if (selector == 0u) {
        return "Static level object";
    }
    if (selector == 0x00000002u) {
        constexpr std::array<const char*, 5> bosses{{
            "Scorpion", "Zeppelin", "Mantis", "Wood Wasp", "Scorpion"
        }};
        return model >= 1u && model <= bosses.size()
            ? bosses[model - 1u]
            : "Boss";
    }
    if (selector == 0x00000004u) {
        return "Commander";
    }
    if (selector == 0x00000008u) {
        return words[8] & 0x80u ? "Wasp nest" :
            words[8] & 0x20u ? "Wasp wave" : "Wasp";
    }
    if (selector == 0x00000010u) {
        return "Spotter";
    }
    if (selector == 0x00000020u) {
        return words[8] & 0x20u ? "Dragonfly wave" : "Dragonfly";
    }
    if (selector == 0x00000040u) {
        return words[8] & 0x80u ? "Block Bug nest" :
            words[8] & 0x20u ? "Block Bug wave" : "Block Bug";
    }
    if (selector == 0x00000080u) {
        return "Weevil";
    }
    if (selector == 0x00000100u) {
        return "Killerpiller";
    }
    if (selector == 0x00000200u) {
        return "Bug Tank nest";
    }
    if (selector == 0x00000400u) {
        return words[8] & 0x20u ? "Cranefly wave" : "Cranefly";
    }
    if (selector == 0x00000800u) {
        return "Ant nest";
    }
    if (selector == 0x00001000u) {
        return "Hoverfly";
    }
    if (selector == 0x00002000u) {
        return "Transporter";
    }
    if (selector == 0x00004000u) {
        return "Mini Bumble";
    }
    if (selector == 0x00008002u) {
        return "Web box trap";
    }
    if (selector == 0x00008008u) {
        return "Wasp portal";
    }
    if (selector == 0x00008040u) {
        return "Hoverfly nest";
    }
    if (selector == 0x00008200u) {
        return "Acid drop";
    }
    if (selector == 0x00008800u) {
        return "Ant";
    }
    if (selector == 0x00020000u) {
        if (const char* name = pickup_name(group); name != nullptr) {
            return std::string(name) + " pickup";
        }
        return "Pickup " + std::to_string(group);
    }
    if (selector == 0x00040000u) {
        return "Water Beetle";
    }
    if (selector == 0x00080000u) {
        return "Chain Moth";
    }
    if (selector == 0x00100000u) {
        return "Louse gun";
    }
    if (selector == 0x00200000u) {
        return "Water effect";
    }
    if (selector == 0x00400000u) {
        return "Enemy wave / objective trigger";
    }
    if (selector == 0x00800000u) {
        return "Grab Bug";
    }
    if (selector == 0x04000000u) {
        return "Water / sewer effect";
    }
    if (selector == 0x08000000u) {
        return "Triggered explosion";
    }
    if (selector == 0x40000000u) {
        return "Animated level object";
    }
    if (selector == 0x80000000u) {
        return "Web Spider";
    }
    if (selector == 0x80008000u) {
        return "Golden Flea";
    }

    char text[72]{};
    std::snprintf(
        text,
        sizeof(text),
        "World actor %08X / model %u",
        selector,
        model
    );
    return text;
}

std::string catalog_label(const Words& words, uint32_t vtable = 0u) {
    if (words[10] == 0x00000004u) {
        return effective_commander_model(words) == 2u
            ? "Commander (objective)"
            : "Commander";
    }
    char variant[64]{};
    if (words[10] == 0x40000000u) {
        std::snprintf(
            variant,
            sizeof(variant),
            " [M%u V%u F%X T%u]",
            words[9] >> 5u,
            words[9] & 0x1Fu,
            words[8],
            words[12] & 0x7u
        );
    }
    else {
        std::snprintf(
            variant,
            sizeof(variant),
            " [M%u V%u F%X]",
            words[9] >> 5u,
            words[9] & 0x1Fu,
            words[8]
        );
    }
    return object_label(words, vtable) + variant;
}

bool catalog_label_self_check() {
    Words words{};
    words[8] = 0x2u;
    words[7] = 0u;
    words[10] = 0x8u;
    Words commander = words;
    commander[10] = 0x4u;
    Words commander_alias = commander;
    commander_alias[9] = 0x21u;
    Words objective_commander = commander;
    objective_commander[9] = 0x40u;
    Words web_box = words;
    web_box[10] = 0x8002u;
    return object_label(words) == "Wasp" &&
        object_label(web_box) == "Web box trap" &&
        object_label(web_box, 0x800466D8u) == "Web box trap" &&
        object_label(words, 0x800478C8u) == "Wasp" &&
        catalog_label(words).rfind("Wasp ", 0u) == 0u &&
        catalog_label(commander) == "Commander" &&
        catalog_label(objective_commander) == "Commander (objective)" &&
        same_catalog_key(commander, commander_alias) &&
        !same_catalog_key(commander, objective_commander);
}

uint32_t read_be32(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) << 24u |
        static_cast<uint32_t>(bytes[offset + 1u]) << 16u |
        static_cast<uint32_t>(bytes[offset + 2u]) << 8u |
        static_cast<uint32_t>(bytes[offset + 3u]);
}

bool load_catalog_from_rom_locked(const std::filesystem::path& rom_path) {
    std::ifstream stream(rom_path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return false;
    }
    const std::streamoff size = stream.tellg();
    if (size < static_cast<std::streamoff>(kRecordBytes + 4u) ||
        size > static_cast<std::streamoff>(32u * 1024u * 1024u)) {
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    stream.seekg(0, std::ios::beg);
    stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    if (!stream) {
        return false;
    }

    g_catalog.clear();
    g_authored_record_count = 0u;
    for (size_t offset = 0u;
         offset + 4u + kRecordBytes <= bytes.size();
         offset += 4u) {
        if (read_be32(bytes, offset) != kRomSpawnCommand) {
            continue;
        }
        Words words{};
        for (size_t index = 0u; index < words.size(); ++index) {
            words[index] = read_be32(
                bytes,
                offset + 4u + index * sizeof(uint32_t)
            );
        }
        if (!valid_words(words)) {
            continue;
        }
        ++g_authored_record_count;
        const auto existing = std::find_if(
            g_catalog.begin(),
            g_catalog.end(),
            [&](const CatalogEntry& entry) {
                return same_factory_signature(words, entry.words);
            }
        );
        if (existing != g_catalog.end()) {
            ++existing->source_count;
            continue;
        }
        g_catalog.push_back({
            words,
            0u,
            1u,
            static_cast<uint32_t>(offset),
            catalog_label(words),
        });
    }
    g_authored_signature_count = g_catalog.size();
    std::vector<CatalogEntry> consolidated;
    consolidated.reserve(g_catalog.size());
    for (CatalogEntry& entry : g_catalog) {
        const auto existing = std::find_if(
            consolidated.begin(),
            consolidated.end(),
            [&](const CatalogEntry& candidate) {
                return same_catalog_key(entry.words, candidate.words);
            }
        );
        if (existing == consolidated.end()) {
            consolidated.push_back(std::move(entry));
        }
        else {
            existing->source_count += entry.source_count;
        }
    }
    g_catalog = std::move(consolidated);
    g_non_placeable_control_signature_count = std::erase_if(
        g_catalog,
        [](const CatalogEntry& entry) {
            return non_placeable_control(entry.words);
        }
    );
    std::sort(
        g_catalog.begin(),
        g_catalog.end(),
        [](const CatalogEntry& left, const CatalogEntry& right) {
            const CatalogFilter left_filter = catalog_filter(left.words);
            const CatalogFilter right_filter = catalog_filter(right.words);
            if (left_filter != right_filter) {
                return left_filter < right_filter;
            }
            if (left.words[10] != right.words[10]) {
                return left.words[10] < right.words[10];
            }
            if (left.words[9] != right.words[9]) {
                return left.words[9] < right.words[9];
            }
            if (left.words[8] != right.words[8]) {
                return left.words[8] < right.words[8];
            }
            return (left.words[12] & 7u) < (right.words[12] & 7u);
        }
    );
    return !g_catalog.empty();
}

bool write_document_locked() {
    nlohmann::json document{
        {"schema_version", 1},
        {"rom_xxh3", "06BDD92F3F4AE7BD"},
        {"next_id", g_next_id},
        {"levels", nlohmann::json::object()},
    };
    for (const SavedEntry& entry : g_saved) {
        document["levels"][std::to_string(entry.level)].push_back({
            {"id", entry.id},
            {"vtable", entry.vtable},
            {"words", entry.words},
        });
    }

    std::error_code error;
    std::filesystem::create_directories(g_edits_path.parent_path(), error);
    if (error) {
        return false;
    }
    const std::filesystem::path temporary =
        g_edits_path.string() + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return false;
        }
        stream << document.dump(2) << '\n';
        stream.flush();
        if (!stream) {
            return false;
        }
    }
#if defined(_WIN32)
    if (MoveFileExW(
            temporary.c_str(),
            g_edits_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        std::filesystem::remove(temporary, error);
        return false;
    }
#else
    std::filesystem::rename(temporary, g_edits_path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
#endif
    return true;
}

bool load_document_locked() {
    if (!std::filesystem::exists(g_edits_path)) {
        return true;
    }
    try {
        std::ifstream stream(g_edits_path, std::ios::binary);
        const nlohmann::json document = nlohmann::json::parse(stream);
        if (document.at("schema_version").get<uint32_t>() != 1u ||
            document.at("rom_xxh3").get<std::string>() !=
                "06BDD92F3F4AE7BD") {
            return false;
        }

        const uint64_t next_id = document.at("next_id").get<uint64_t>();
        const nlohmann::json& levels = document.at("levels");
        if (!levels.is_object() || next_id == 0u) {
            return false;
        }

        std::vector<SavedEntry> loaded;
        uint64_t highest_id = 0u;
        for (auto iterator = levels.begin(); iterator != levels.end();
             ++iterator) {
            size_t consumed = 0u;
            const unsigned long level_value =
                std::stoul(iterator.key(), &consumed, 10);
            if (consumed != iterator.key().size() ||
                level_value > 31u || !iterator.value().is_array()) {
                return false;
            }
            for (const nlohmann::json& item : iterator.value()) {
                if (loaded.size() >= kMaximumEdits) {
                    return false;
                }
                SavedEntry entry{};
                entry.id = item.at("id").get<uint64_t>();
                entry.level = static_cast<uint32_t>(level_value);
                entry.vtable = item.at("vtable").get<uint32_t>();
                const nlohmann::json& words = item.at("words");
                if (entry.id == 0u || entry.vtable == 0u ||
                    !words.is_array() || words.size() != entry.words.size()) {
                    return false;
                }
                for (size_t index = 0u; index < entry.words.size(); ++index) {
                    entry.words[index] = words.at(index).get<uint32_t>();
                }
                if (!valid_words(entry.words) ||
                    std::any_of(
                        loaded.begin(),
                        loaded.end(),
                        [&](const SavedEntry& existing) {
                            return existing.id == entry.id;
                        })) {
                    return false;
                }
                highest_id = std::max(highest_id, entry.id);
                loaded.push_back(entry);
            }
        }
        if (next_id <= highest_id) {
            return false;
        }
        g_saved = std::move(loaded);
        g_next_id = next_id;
        return true;
    }
    catch (...) {
        return false;
    }
}

void reset_level_locked(uint32_t level) {
    g_level = level;
    g_page = 0u;
    g_saved_spawned = false;
    g_preview = {};
    g_live_preview_actor = 0u;
    g_live_preview_actors.clear();
    g_preview_updates_pending.clear();
    g_preview_update_tracking = false;
    g_preview_update_actors_before.clear();
    g_destruction_requested.clear();
    g_deferred_actor_updates.clear();
    g_live_preview_vtable = 0u;
    g_live_preview_catalog = SIZE_MAX;
    g_preview_failed_catalog = SIZE_MAX;
    g_clear_level_armed = false;
    g_selected.reset();
    for (CatalogEntry& entry : g_catalog) {
        entry.level_resource = 0u;
        entry.observed_level = UINT32_MAX;
    }
    for (SavedEntry& entry : g_saved) {
        entry.actor = 0u;
        entry.actors.clear();
        entry.active = false;
    }
}

bool allocate_scratch_locked(uint8_t* rdram) {
    if (g_scratch.rdram == rdram && g_scratch.allocation != nullptr) {
        return true;
    }
    g_scratch = {};
    auto* allocation = static_cast<uint8_t*>(
        recomp::alloc(rdram, kScratchBytes)
    );
    if (allocation == nullptr) {
        return false;
    }
    const ptrdiff_t offset = allocation - rdram;
    if (offset < 0 ||
        static_cast<size_t>(offset) + kScratchBytes > recomp::mem_size ||
        static_cast<uint64_t>(offset) > UINT32_C(0x7FFFFFFF)) {
        recomp::free(rdram, allocation);
        return false;
    }
    g_scratch = {
        allocation,
        rdram,
        UINT32_C(0x80000000) + static_cast<uint32_t>(offset),
    };
    return true;
}

bool valid_list_owner(uint32_t owner) {
    if (owner < kObjectListBase) {
        return false;
    }
    const uint32_t offset = owner - kObjectListBase;
    return offset % kObjectListStride == 0u &&
        offset / kObjectListStride < kObjectListCount;
}

bool live_actor(uint8_t* rdram, uint32_t actor) {
    if (!valid_guest(actor, 0x90u)) {
        return false;
    }
    const uint32_t owner = read_u32(rdram, actor + 0x0Cu);
    const uint32_t previous = read_u32(rdram, actor + 0x08u);
    return valid_list_owner(owner) &&
        (previous != 0u
            ? valid_guest(previous, 0x0Cu) &&
                read_u32(rdram, previous + 0x04u) == actor
            : read_u32(rdram, owner) == actor);
}

bool collect_live_actors(
    uint8_t* rdram,
    std::vector<uint32_t>& actors
) {
    actors.clear();
    std::unordered_set<uint32_t> seen;
    seen.reserve(1024u);
    for (uint32_t list = 0u; list < kObjectListCount; ++list) {
        const uint32_t owner =
            kObjectListBase + list * kObjectListStride;
        if (!valid_guest(owner, sizeof(uint32_t))) {
            return false;
        }
        uint32_t actor = read_u32(rdram, owner);
        uint32_t traversed = 0u;
        while (actor != 0u) {
            if (++traversed > kMaximumActorsPerList ||
                !valid_guest(actor, 0x90u) ||
                !seen.insert(actor).second) {
                return false;
            }
            actors.push_back(actor);
            actor = read_u32(rdram, actor + 0x04u);
        }
    }
    return true;
}

uint32_t spawn_locked(
    uint8_t* rdram,
    recomp_context* context,
    const Words& source,
    std::vector<uint32_t>* created = nullptr
) {
    if (context == nullptr || !valid_words(source) ||
        !allocate_scratch_locked(rdram)) {
        return 0u;
    }

    const bool commander = source[10] == 0x00000004u;
    const uint32_t terrain_bytes =
        static_cast<uint32_t>(kTerrainCellCount) * kTerrainCellStride;
    if (commander && !valid_guest(kTerrainCellBase, terrain_bytes)) {
        return 0u;
    }
    std::optional<std::array<uint16_t, kTerrainCellCount>>
        commander_terrain_before;
    if (commander) {
        commander_terrain_before.emplace();
        for (size_t index = 0u; index < kTerrainCellCount; ++index) {
            (*commander_terrain_before)[index] = static_cast<uint16_t>(
                MEM_H(
                    0,
                    guest_address(
                        kTerrainCellBase +
                        static_cast<uint32_t>(index) * kTerrainCellStride +
                        kTerrainCellDescriptorOffset
                    )
                )
            );
        }
    }

    std::unordered_set<uint32_t> actors_before;
    if (created != nullptr) {
        std::vector<uint32_t> actors;
        if (!collect_live_actors(rdram, actors)) {
            return 0u;
        }
        actors_before.insert(actors.begin(), actors.end());
        created->clear();
    }

    std::memset(g_scratch.allocation, 0, kScratchBytes);
    const uint32_t interpreter = g_scratch.guest;
    const uint32_t record = interpreter + kScratchRecordOffset;
    for (size_t index = 0u; index < source.size(); ++index) {
        write_u32(
            rdram,
            record + static_cast<uint32_t>(index * sizeof(uint32_t)),
            source[index]
        );
    }
    write_u32(rdram, interpreter, record);

    const uint32_t previous_interpreter =
        read_u32(rdram, kCurrentPlacementInterpreter);
    write_u32(rdram, kCurrentPlacementInterpreter, interpreter);
    recomp_context spawn_context = *context;
    g_editor_spawn = true;
    func_80092144(rdram, &spawn_context);
    g_editor_spawn = false;
    size_t restored_terrain_cells = 0u;
    if (commander_terrain_before.has_value()) {
        for (size_t index = 0u; index < kTerrainCellCount; ++index) {
            const uint32_t address =
                kTerrainCellBase +
                static_cast<uint32_t>(index) * kTerrainCellStride +
                kTerrainCellDescriptorOffset;
            const uint16_t before = (*commander_terrain_before)[index];
            if (static_cast<uint16_t>(
                    MEM_H(0, guest_address(address))) == before) {
                continue;
            }
            MEM_H(0, guest_address(address)) = before;
            ++restored_terrain_cells;
        }
        if (restored_terrain_cells != 0u) {
            std::fprintf(
                stderr,
                "BUMBLE_EDITOR stage=commander_terrain_restored cells=%zu\n",
                restored_terrain_cells
            );
            std::fflush(stderr);
        }
    }
    write_u32(
        rdram,
        kCurrentPlacementInterpreter,
        previous_interpreter
    );

    uint32_t actor = read_u32(rdram, interpreter + 0xA0u);
    if (created != nullptr) {
        std::vector<uint32_t> actors_after;
        if (collect_live_actors(rdram, actors_after)) {
            for (uint32_t candidate : actors_after) {
                if (!actors_before.contains(candidate)) {
                    created->push_back(candidate);
                }
            }
        }
        if (std::find(created->begin(), created->end(), actor) ==
                created->end() &&
            live_actor(rdram, actor)) {
            created->push_back(actor);
        }
        if (!live_actor(rdram, actor)) {
            const float target_x =
                world_coordinate(static_cast<int32_t>(source[0]), true);
            const float target_y =
                world_coordinate(static_cast<int32_t>(source[1]), false);
            const float target_z =
                world_coordinate(static_cast<int32_t>(source[2]), true);
            float best_distance = std::numeric_limits<float>::max();
            for (uint32_t candidate : *created) {
                const float x = read_f32(rdram, candidate + 0x40u);
                const float y = read_f32(rdram, candidate + 0x44u);
                const float z = read_f32(rdram, candidate + 0x48u);
                if (!live_actor(rdram, candidate) ||
                    !finite_world(x) || !finite_world(y) ||
                    !finite_world(z)) {
                    continue;
                }
                const float dx = x - target_x;
                const float dy = y - target_y;
                const float dz = z - target_z;
                const float distance = dx * dx + dy * dy + dz * dz;
                if (distance < best_distance) {
                    best_distance = distance;
                    actor = candidate;
                }
            }
            if (live_actor(rdram, actor)) {
                std::fprintf(
                    stderr,
                    "BUMBLE_EDITOR stage=factory_root_recovered"
                    " actor=0x%08X parts=%zu selector=0x%08X\n",
                    actor,
                    created->size(),
                    source[10]
                );
                std::fflush(stderr);
            }
        }
    }
    return live_actor(rdram, actor) ? actor : 0u;
}

bool destroy_actor(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t actor
) {
    if (context == nullptr) {
        return false;
    }
    if (!live_actor(rdram, actor)) {
        g_destruction_requested.erase(actor);
        return true;
    }
    if (g_destruction_requested.contains(actor)) {
        return false;
    }

    const uint32_t vtable = read_u32(rdram, actor + 0x88u);
    if (!valid_guest(vtable + 0x24u, sizeof(uint32_t))) {
        return false;
    }
    const int16_t subobject_offset = static_cast<int16_t>(
        MEM_H(0, guest_address(vtable + 0x20u))
    );
    const uint32_t callback = read_u32(rdram, vtable + 0x24u);
    recomp_func_t* destructor = LOOKUP_FUNC(callback);
    if (destructor == nullptr) {
        return false;
    }

    write_f32(rdram, actor + 0x40u, 0.0f);
    write_f32(rdram, actor + 0x44u, -100000.0f);
    write_f32(rdram, actor + 0x48u, 0.0f);
    g_destruction_requested.insert(actor);
    recomp_context remove_context = *context;
    remove_context.r4 = guest_address(
        actor + static_cast<uint32_t>(subobject_offset)
    );
    remove_context.r5 = 3;
    destructor(rdram, &remove_context);
    if (!live_actor(rdram, actor)) {
        g_destruction_requested.erase(actor);
        return true;
    }
    return false;
}

bool destroy_actor_group_locked(
    uint8_t* rdram,
    recomp_context* context,
    std::vector<uint32_t>& actors,
    uint32_t root = 0u
) {
    for (uint32_t actor : actors) {
        g_preview_updates_pending.erase(actor);
        g_deferred_actor_updates.erase(actor);
    }
    g_preview_updates_pending.erase(root);
    g_deferred_actor_updates.erase(root);
    const auto discard_destroyed = [&]() {
        std::erase_if(
            actors,
            [&](uint32_t actor) {
                if (live_actor(rdram, actor)) {
                    return false;
                }
                g_destruction_requested.erase(actor);
                return true;
            }
        );
    };

    discard_destroyed();
    if (root != 0u && live_actor(rdram, root)) {
        (void)destroy_actor(rdram, context, root);
    }
    for (uint32_t actor : actors) {
        if (actor != root && live_actor(rdram, actor)) {
            (void)destroy_actor(rdram, context, actor);
        }
    }
    discard_destroyed();
    return actors.empty();
}

bool clear_current_level_locked(
    uint8_t* rdram,
    recomp_context* context
) {
    const size_t before = g_saved.size();
    if (std::none_of(
            g_saved.begin(),
            g_saved.end(),
            [](const SavedEntry& entry) { return entry.level == g_level; })) {
        return true;
    }

    std::vector<SavedEntry> previous = g_saved;
    std::erase_if(
        g_saved,
        [](const SavedEntry& entry) { return entry.level == g_level; }
    );
    if (!write_document_locked()) {
        g_saved = std::move(previous);
        std::fprintf(
            stderr,
            "BUMBLE_EDITOR stage=clear_level_rolled_back"
            " level=%u reason=persistence_failed\n",
            g_level
        );
        std::fflush(stderr);
        return false;
    }

    size_t live_groups = 0u;
    for (SavedEntry& entry : previous) {
        if (entry.level != g_level) {
            continue;
        }
        if (entry.actor != 0u || !entry.actors.empty()) {
            ++live_groups;
        }
        (void)destroy_actor_group_locked(
            rdram,
            context,
            entry.actors,
            entry.actor
        );
    }
    std::fprintf(
        stderr,
        "BUMBLE_EDITOR stage=level_cleared"
        " level=%u removed=%zu live_groups=%zu remaining=%zu\n",
        g_level,
        before - g_saved.size(),
        live_groups,
        g_saved.size()
    );
    std::fflush(stderr);
    return true;
}

CatalogEntry* matching_catalog_locked(const Words& words) {
    const auto entry = std::find_if(
        g_catalog.begin(),
        g_catalog.end(),
        [&](const CatalogEntry& candidate) {
            return same_catalog_key(words, candidate.words);
        }
    );
    return entry != g_catalog.end() ? &*entry : nullptr;
}

Words runtime_words_locked(const CatalogEntry& catalog, Words words) {
    words[6] = catalog.observed_level == g_level
        ? catalog.level_resource
        : 0u;
    return words;
}

bool spawn_saved_entry_locked(
    uint8_t* rdram,
    recomp_context* context,
    SavedEntry& entry,
    CatalogEntry& catalog
) {
    entry.active = true;
    entry.actor = spawn_locked(
        rdram,
        context,
        runtime_words_locked(catalog, entry.words),
        &entry.actors
    );
    if (entry.actor == 0u) {
        return false;
    }
    entry.vtable = read_u32(rdram, entry.actor + 0x88u);
    g_deferred_actor_updates.insert(
        entry.actors.begin(),
        entry.actors.end()
    );
    return true;
}

void spawn_saved_locked(uint8_t* rdram, recomp_context* context) {
    if (g_saved_spawned || g_catalog.empty() ||
        !bumble::modern_controls::gameplay_input_active()) {
        return;
    }
    uint32_t spawned = 0u;
    uint32_t rejected = 0u;
    for (SavedEntry& entry : g_saved) {
        if (entry.level != g_level || entry.actor != 0u) {
            continue;
        }
        CatalogEntry* catalog = matching_catalog_locked(entry.words);
        if (catalog == nullptr) {
            ++rejected;
            continue;
        }
        if (spawn_saved_entry_locked(rdram, context, entry, *catalog)) {
            ++spawned;
        }
        else {
            (void)destroy_actor_group_locked(
                rdram,
                context,
                entry.actors
            );
            ++rejected;
        }
    }
    g_saved_spawned = true;
    if (spawned == 0u && rejected == 0u) {
        return;
    }
    std::fprintf(
        stderr,
        "BUMBLE_EDITOR stage=level_edits_loaded level=%u"
        " spawned=%u rejected_factory=%u\n",
        g_level,
        spawned,
        rejected
    );
    std::fflush(stderr);
}

struct Ray {
    float origin_x = 0.0f;
    float origin_y = 0.0f;
    float origin_z = 0.0f;
    float direction_x = 0.0f;
    float direction_y = 0.0f;
    float direction_z = 1.0f;
    float focus_distance = 0.0f;
};

bool placement_target(
    uint8_t* rdram,
    recomp_context* context,
    bool snap_to_ground,
    Ray& ray,
    bumble::level_editor::Preview& preview
) {
    if (!valid_guest(kPlayerOneOwner, 4u) ||
        !valid_guest(kPlayerOneCamera, 0x24u)) {
        return false;
    }
    const uint32_t actor = read_u32(rdram, kPlayerOneOwner);
    if (!valid_guest(actor, 0x90u) ||
        read_u32(rdram, actor + 0x88u) != kPlayerVtable) {
        return false;
    }
    const float eye_x = read_f32(rdram, kPlayerOneCamera + 0x00u);
    const float eye_y = read_f32(rdram, kPlayerOneCamera + 0x04u);
    const float eye_z = read_f32(rdram, kPlayerOneCamera + 0x08u);
    const float direction_x =
        read_f32(rdram, kPlayerOneCamera + 0x0Cu) - eye_x;
    const float direction_y =
        read_f32(rdram, kPlayerOneCamera + 0x10u) - eye_y;
    const float direction_z =
        read_f32(rdram, kPlayerOneCamera + 0x14u) - eye_z;
    const float focus_distance = std::sqrt(
        direction_x * direction_x +
        direction_y * direction_y +
        direction_z * direction_z
    );
    if (!finite_world(eye_x) || !finite_world(eye_y) ||
        !finite_world(eye_z) || !std::isfinite(focus_distance) ||
        focus_distance < 1.0f) {
        return false;
    }

    ray = {
        eye_x,
        eye_y,
        eye_z,
        direction_x / focus_distance,
        direction_y / focus_distance,
        direction_z / focus_distance,
        focus_distance,
    };

    const float ray_end_x = ray.origin_x + ray.direction_x * 5000.0f;
    const float ray_end_y = ray.origin_y + ray.direction_y * 5000.0f;
    const float ray_end_z = ray.origin_z + ray.direction_z * 5000.0f;
    BumbleStaticCollisionHit sight_hit{};
    const bool sight_blocked = bumble_sweep_modern_static_sphere(
        rdram,
        context,
        ray.origin_x,
        ray.origin_y,
        ray.origin_z,
        ray_end_x,
        ray_end_y,
        ray_end_z,
        1.0f,
        &sight_hit
    ) != 0u;
    float target_distance = snap_to_ground ? 1200.0f : 600.0f;
    if (sight_blocked) {
        const float hit_distance =
            (sight_hit.x - ray.origin_x) * ray.direction_x +
            (sight_hit.y - ray.origin_y) * ray.direction_y +
            (sight_hit.z - ray.origin_z) * ray.direction_z;
        if (std::isfinite(hit_distance)) {
            target_distance = std::clamp(
                hit_distance - 8.0f,
                24.0f,
                target_distance
            );
        }
    }
    const float target_x =
        ray.origin_x + ray.direction_x * target_distance;
    const float target_y =
        ray.origin_y + ray.direction_y * target_distance;
    const float target_z =
        ray.origin_z + ray.direction_z * target_distance;

    BumbleStaticCollisionHit ground_hit{};
    bool grounded = false;
    if (snap_to_ground) {
        grounded = bumble_sweep_modern_static_sphere(
            rdram,
            context,
            target_x,
            target_y + 2048.0f,
            target_z,
            target_x,
            target_y - 4096.0f,
            target_z,
            1.0f,
            &ground_hit
        ) != 0u;
        if (!grounded) {
            return false;
        }
    }

    const int32_t placement_yaw = g_yaw_degrees.load(
        std::memory_order_acquire
    );
    const float placement_radians =
        static_cast<float>(placement_yaw) * kPi / 180.0f;
    preview = {
        grounded ? ground_hit.x : target_x,
        grounded ? ground_hit.y : target_y,
        grounded ? ground_hit.z : target_z,
        -std::sin(placement_radians),
        std::cos(placement_radians),
        18.0f,
        0u,
        false,
        true,
    };
    return finite_world(preview.x) &&
        finite_world(preview.y) &&
        finite_world(preview.z);
}

Words placed_words(
    const CatalogEntry& entry,
    const bumble::level_editor::Preview& preview
) {
    Words words = entry.words;
    words[0] = static_cast<uint32_t>(
        authored_coordinate(preview.x, true)
    );
    words[1] = static_cast<uint32_t>(
        authored_coordinate(preview.y, false)
    );
    words[2] = static_cast<uint32_t>(
        authored_coordinate(preview.z, true)
    );
    words[3] = 0u;
    words[4] = static_cast<uint32_t>(
        g_yaw_degrees.load(std::memory_order_acquire)
    );
    words[5] = 0u;
    words[6] = 0u;
    return words;
}

bool destroy_live_preview_locked(
    uint8_t* rdram,
    recomp_context* context
) {
    if (g_live_preview_actor != 0u &&
        std::find(
            g_live_preview_actors.begin(),
            g_live_preview_actors.end(),
            g_live_preview_actor
        ) == g_live_preview_actors.end()) {
        g_live_preview_actors.push_back(g_live_preview_actor);
    }
    if (!destroy_actor_group_locked(
            rdram,
            context,
            g_live_preview_actors,
            g_live_preview_actor)) {
        return false;
    }
    g_live_preview_actor = 0u;
    g_live_preview_vtable = 0u;
    g_live_preview_catalog = SIZE_MAX;
    return true;
}

bool update_live_preview_locked(
    uint8_t* rdram,
    recomp_context* context,
    size_t catalog_index,
    const bumble::level_editor::Preview& target
) {
    if (catalog_index >= g_catalog.size() || !target.valid) {
        return false;
    }
    if (g_live_preview_actor != 0u &&
        (!live_actor(rdram, g_live_preview_actor) ||
         g_live_preview_catalog != catalog_index)) {
        if (g_live_preview_catalog == catalog_index) {
            g_preview_failed_catalog = catalog_index;
        }
        if (!destroy_live_preview_locked(rdram, context)) {
            return false;
        }
    }
    if (g_live_preview_actor == 0u) {
        if (g_preview_failed_catalog == catalog_index) {
            return false;
        }
        CatalogEntry& catalog = g_catalog[catalog_index];
        const uint32_t actor = spawn_locked(
            rdram,
            context,
            runtime_words_locked(catalog, placed_words(catalog, target)),
            &g_live_preview_actors
        );
        if (actor == 0u) {
            (void)destroy_actor_group_locked(
                rdram,
                context,
                g_live_preview_actors
            );
            g_preview_failed_catalog = catalog_index;
            std::fprintf(
                stderr,
                "BUMBLE_EDITOR stage=preview_failed catalog=%zu"
                " selector=0x%08X\n",
                catalog_index,
                catalog.words[10]
            );
            std::fflush(stderr);
            return false;
        }
        g_live_preview_actor = actor;
        g_live_preview_vtable = read_u32(rdram, actor + 0x88u);
        g_live_preview_catalog = catalog_index;
        g_preview_updates_pending.insert(
            g_live_preview_actors.begin(),
            g_live_preview_actors.end()
        );
        g_catalog[catalog_index].vtable = g_live_preview_vtable;
        g_catalog[catalog_index].label = catalog_label(
            g_catalog[catalog_index].words,
            g_live_preview_vtable
        );
        std::fprintf(
            stderr,
            "BUMBLE_EDITOR stage=preview_created catalog=%zu"
            " actor=0x%08X vtable=0x%08X parts=%zu"
            " selector=0x%08X\n",
            catalog_index,
            actor,
            g_live_preview_vtable,
            g_live_preview_actors.size(),
            catalog.words[10]
        );
        std::fflush(stderr);
    }

    const float previous_x =
        read_f32(rdram, g_live_preview_actor + 0x40u);
    const float previous_y =
        read_f32(rdram, g_live_preview_actor + 0x44u);
    const float previous_z =
        read_f32(rdram, g_live_preview_actor + 0x48u);
    const float delta_x = finite_world(previous_x)
        ? target.x - previous_x
        : 0.0f;
    const float delta_y = finite_world(previous_y)
        ? target.y - previous_y
        : 0.0f;
    const float delta_z = finite_world(previous_z)
        ? target.z - previous_z
        : 0.0f;
    for (uint32_t actor : g_live_preview_actors) {
        if (!live_actor(rdram, actor)) {
            continue;
        }
        const float x = read_f32(rdram, actor + 0x40u);
        const float y = read_f32(rdram, actor + 0x44u);
        const float z = read_f32(rdram, actor + 0x48u);
        if (finite_world(x) && finite_world(y) && finite_world(z)) {
            write_f32(rdram, actor + 0x40u, x + delta_x);
            write_f32(rdram, actor + 0x44u, y + delta_y);
            write_f32(rdram, actor + 0x48u, z + delta_z);
        }
    }
    write_f32(rdram, g_live_preview_actor + 0x40u, target.x);
    write_f32(rdram, g_live_preview_actor + 0x44u, target.y);
    write_f32(rdram, g_live_preview_actor + 0x48u, target.z);
    write_f32(
        rdram,
        g_live_preview_actor + 0x54u,
        static_cast<float>(
            g_yaw_degrees.load(std::memory_order_acquire)
        )
    );
    g_preview = target;
    g_preview.actor = g_live_preview_actor;
    const float radius = read_f32(rdram, g_live_preview_actor + 0x6Cu);
    g_preview.radius = std::isfinite(radius) &&
            radius >= 12.0f && radius <= 4096.0f
        ? radius
        : 36.0f;
    return true;
}

std::optional<size_t> delete_target_locked(
    uint8_t* rdram,
    const Ray& ray,
    bumble::level_editor::Preview& preview
) {
    std::optional<size_t> best;
    float best_score = std::numeric_limits<float>::max();
    for (size_t index = 0u; index < g_saved.size(); ++index) {
        SavedEntry& entry = g_saved[index];
        if (entry.level != g_level || !live_actor(rdram, entry.actor)) {
            entry.actor = 0u;
            continue;
        }
        const float x = read_f32(rdram, entry.actor + 0x40u);
        const float y = read_f32(rdram, entry.actor + 0x44u);
        const float z = read_f32(rdram, entry.actor + 0x48u);
        if (!finite_world(x) || !finite_world(y) || !finite_world(z)) {
            continue;
        }
        const float dx = x - ray.origin_x;
        const float dy = y - ray.origin_y;
        const float dz = z - ray.origin_z;
        const float along = dx * ray.direction_x +
            dy * ray.direction_y + dz * ray.direction_z;
        if (along < 0.0f || along > 5000.0f) {
            continue;
        }
        const float perpendicular_squared =
            dx * dx + dy * dy + dz * dz - along * along;
        float radius = read_f32(rdram, entry.actor + 0x6Cu);
        if (!std::isfinite(radius) || radius < 12.0f ||
            radius > 4096.0f) {
            radius = 36.0f;
        }
        const float limit = radius + 32.0f;
        if (perpendicular_squared > limit * limit) {
            continue;
        }
        const float score = perpendicular_squared + along * 0.001f;
        if (score >= best_score) {
            continue;
        }
        best_score = score;
        best = index;
        const float placement_radians =
            static_cast<float>(static_cast<int32_t>(entry.words[4])) *
            kPi / 180.0f;
        preview = {
            x,
            y,
            z,
            -std::sin(placement_radians),
            std::cos(placement_radians),
            radius,
            entry.actor,
            true,
            true,
        };
    }
    return best;
}

constexpr float kPanelLeftFromRight = 158.0f;
constexpr float kPanelRightMargin = 4.0f;
constexpr uint32_t kPanelAuthoredX = 162u;
constexpr uint32_t kPanelWidth = 154u;
constexpr uint32_t kPanelTextInset = 152u;
constexpr uint32_t kFirstItemY = 49u;
constexpr uint32_t kItemSpacing = 11u;

struct GuestPointer {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    bool valid = false;
};

GuestPointer guest_pointer(
    const bumble::graphics_options::PointerSnapshot& pointer
) {
    if (!pointer.inside_client || pointer.client_width == 0u ||
        pointer.client_height == 0u) {
        return {};
    }
    const float scale =
        static_cast<float>(pointer.client_height) / 240.0f;
    if (!(scale > 0.0f)) {
        return {};
    }
    return {
        static_cast<float>(pointer.client_x) / scale,
        static_cast<float>(pointer.client_y) / scale,
        static_cast<float>(pointer.client_width) / scale,
        true,
    };
}

std::vector<size_t> filtered_catalog_locked() {
    std::vector<size_t> result;
    result.reserve(g_catalog.size());
    for (size_t index = 0u; index < g_catalog.size(); ++index) {
        if (g_filter == CatalogFilter::All ||
            catalog_filter(g_catalog[index].words) == g_filter) {
            result.push_back(index);
        }
    }
    return result;
}

const char* filter_name(CatalogFilter filter) {
    switch (filter) {
    case CatalogFilter::All: return "ALL";
    case CatalogFilter::Enemies: return "ENEMIES";
    case CatalogFilter::Pickups: return "PICKUPS";
    case CatalogFilter::World: return "WORLD / BUILDINGS";
    }
    return "ALL";
}

enum class MenuAction : uint8_t {
    None,
    Item,
    Filter,
    Previous,
    Next,
    Clear,
    Delete,
    Place,
    Stop,
};

MenuAction page_action(float panel_x) {
    if (panel_x <= 13.0f) {
        return MenuAction::Previous;
    }
    if (panel_x >= 28.0f && panel_x <= 48.0f) {
        return MenuAction::Next;
    }
    return MenuAction::None;
}

bool page_action_self_check() {
    return page_action(7.0f) == MenuAction::Previous &&
        page_action(32.5f) == MenuAction::Next &&
        page_action(20.0f) == MenuAction::None &&
        page_action(60.0f) == MenuAction::None;
}

struct MenuHit {
    MenuAction action = MenuAction::None;
    size_t row = 0u;
};

MenuHit menu_hit(
    const bumble::graphics_options::PointerSnapshot& pointer,
    size_t item_count
) {
    const GuestPointer guest = guest_pointer(pointer);
    if (!guest.valid ||
        guest.x < guest.width - kPanelLeftFromRight ||
        guest.x > guest.width - kPanelRightMargin) {
        return {};
    }
    if (std::abs(guest.y - 34.0f) <= 5.0f) {
        return {MenuAction::Filter, 0u};
    }
    for (size_t row = 0u; row < item_count; ++row) {
        const float row_y = static_cast<float>(
            kFirstItemY + static_cast<uint32_t>(row) * kItemSpacing
        );
        if (std::abs(guest.y - row_y) <= 5.0f) {
            return {MenuAction::Item, row};
        }
    }
    if (guest.y >= 156.0f && guest.y <= 168.0f) {
        return {
            page_action(
                guest.x - (guest.width - kPanelLeftFromRight)
            ),
            0u,
        };
    }
    if (std::abs(guest.y - 204.0f) <= 4.0f) {
        return {MenuAction::Clear, 0u};
    }
    if (std::abs(guest.y - 214.0f) <= 4.0f) {
        return {MenuAction::Delete, 0u};
    }
    if (std::abs(guest.y - 224.0f) <= 4.0f) {
        return {MenuAction::Place, 0u};
    }
    if (std::abs(guest.y - 234.0f) <= 4.0f) {
        return {MenuAction::Stop, 0u};
    }
    return {};
}

void draw_text(
    uint32_t y,
    const std::string& text,
    uint32_t colour,
    uint64_t slot,
    float scale = 0.72f
) {
    (void)bumble::text_overlay::observe(
        bumble::text_overlay::TextKind::Editor,
        kPanelTextInset,
        y,
        text.c_str(),
        bumble::text_overlay::HorizontalAnchor::RightInset,
        bumble::text_overlay::VerticalAnchor::Authored,
        scale,
        colour,
        0x000000FFu,
        slot
    );
}

void draw_center_text(
    uint32_t y,
    const std::string& text,
    uint32_t colour,
    uint64_t slot,
    float scale
) {
    (void)bumble::text_overlay::observe(
        bumble::text_overlay::TextKind::Editor,
        160u,
        y,
        text.c_str(),
        bumble::text_overlay::HorizontalAnchor::Center,
        bumble::text_overlay::VerticalAnchor::Center,
        scale,
        colour,
        0x000000FFu,
        slot
    );
}

void draw_menu_locked(
    uint8_t* rdram,
    const bumble::graphics_options::PointerSnapshot& pointer
) {
    constexpr uint32_t white = 0xFFF4D8FFu;
    constexpr uint32_t grey = 0x8A8A8AFFu;
    constexpr uint32_t yellow = 0xFFD64AFFu;
    constexpr uint32_t green = 0x72F18BFFu;
    constexpr uint32_t red = 0xFF7676FFu;
    bumble::text_overlay::clear_kind(
        bumble::text_overlay::TextKind::Editor
    );
    (void)bumble::text_overlay::observe_panel(
        bumble::text_overlay::TextKind::Editor,
        kPanelAuthoredX - 2u,
        2u,
        kPanelWidth + 2u,
        236u,
        0x284966F4u,
        bumble::text_overlay::HorizontalAnchor::Right,
        bumble::text_overlay::VerticalAnchor::Top,
        0xED000001u
    );
    (void)bumble::text_overlay::observe_panel(
        bumble::text_overlay::TextKind::Editor,
        kPanelAuthoredX,
        4u,
        kPanelWidth,
        232u,
        0x09111BE8u,
        bumble::text_overlay::HorizontalAnchor::Right,
        bumble::text_overlay::VerticalAnchor::Top,
        0xED000002u
    );

    draw_text(
        10u,
        "F3 / LEVEL EDITOR",
        yellow,
        0xED100001u,
        0.68f
    );
    const size_t saved_count = static_cast<size_t>(std::count_if(
        g_saved.begin(),
        g_saved.end(),
        [](const SavedEntry& entry) { return entry.level == g_level; }
    ));
    draw_text(
        22u,
        std::to_string(g_catalog.size()) + " TYPES / " +
            "ALL UNLOCKED / " +
            std::to_string(saved_count) + " SAVED HERE",
        white,
        0xED100002u,
        0.43f
    );

    const std::vector<size_t> visible = filtered_catalog_locked();
    const size_t first = g_page * kPageSize;
    const size_t item_count = first < visible.size()
        ? std::min(kPageSize, visible.size() - first)
        : 0u;
    const MenuHit hover = menu_hit(pointer, item_count);
    draw_text(
        34u,
        std::string("FILTER: ") + filter_name(g_filter) + "  [CLICK]",
        hover.action == MenuAction::Filter ? green : white,
        0xED100003u,
        0.48f
    );
    if (item_count == 0u) {
        draw_text(
            49u,
            "NO OBJECTS IN THIS FILTER",
            grey,
            0xED100010u,
            0.48f
        );
    }
    for (size_t row = 0u; row < item_count; ++row) {
        const size_t index = visible[first + row];
        uint32_t colour = g_selected.has_value() && *g_selected == index
            ? yellow
            : white;
        if (hover.action == MenuAction::Item && hover.row == row) {
            colour = green;
        }
        draw_text(
            kFirstItemY + static_cast<uint32_t>(row) * kItemSpacing,
            (g_selected == index ? "> " : "  ") +
                g_catalog[index].label,
            colour,
            0xED100100u + row,
            0.43f
        );
    }

    const size_t page_count = std::max<size_t>(
        1u,
        (visible.size() + kPageSize - 1u) / kPageSize
    );
    const bool previous = g_page > 0u;
    const bool next = g_page + 1u < page_count;
    draw_text(
        162u,
        std::string(previous ? "< PREV" : "  ----") +
            "     PAGE " + std::to_string(g_page + 1u) + "/" +
            std::to_string(page_count) +
            "     " + (next ? "NEXT >" : "----  "),
        hover.action == MenuAction::Previous && previous
            ? green
            : hover.action == MenuAction::Next && next
                ? green
                : white,
        0xED100200u,
        0.43f
    );

    if (g_selected.has_value() && *g_selected < g_catalog.size()) {
        const CatalogEntry& selected = g_catalog[*g_selected];
        char details[128]{};
        if (selected.words[10] == 0x00000004u) {
            std::snprintf(
                details,
                sizeof(details),
                "TYPE %s / ROLE %s",
                filter_name(catalog_filter(selected.words)),
                effective_commander_model(selected.words) == 2u
                    ? "OBJECTIVE"
                    : "STANDARD"
            );
        }
        else {
            std::snprintf(
                details,
                sizeof(details),
                "TYPE %s / MODEL %u / VARIANT %u",
                filter_name(catalog_filter(selected.words)),
                selected.words[9] >> 5u,
                selected.words[9] & 0x1Fu
            );
        }
        draw_text(
            174u,
            details,
            white,
            0xED100210u,
            0.38f
        );
        std::snprintf(
            details,
            sizeof(details),
            "FLAGS %08X / CLASS %08X",
            selected.words[8],
            selected.vtable
        );
        draw_text(
            184u,
            details,
            white,
            0xED100211u,
            0.38f
        );
        const bool live_preview =
            g_live_preview_actor != 0u &&
            g_live_preview_catalog == *g_selected &&
            g_preview_updates_pending.empty() &&
            live_actor(rdram, g_live_preview_actor);
        const bool active_here = selected.observed_level == g_level;
        const bool health_relevant =
            live_preview &&
            catalog_filter(selected.words) == CatalogFilter::Enemies &&
            actor_class_name(g_live_preview_vtable) != nullptr &&
            valid_guest(g_live_preview_actor + 0x7Cu, sizeof(int16_t));
        if (health_relevant) {
            std::snprintf(
                details,
                sizeof(details),
                "USES %u / HP %d / R %.1f / %zu PARTS / %s",
                selected.source_count,
                static_cast<int>(
                    static_cast<int16_t>(
                        MEM_H(0, guest_address(
                            g_live_preview_actor + 0x7Cu
                        ))
                    )
                ),
                static_cast<double>(g_preview.radius),
                g_live_preview_actors.size(),
                active_here ? "LEVEL RESOURCE" : "CROSS-LEVEL DEFAULT"
            );
        }
        else {
            std::snprintf(
                details,
                sizeof(details),
                "USES %u / R %.1f / %zu PARTS / %s",
                selected.source_count,
                static_cast<double>(
                    live_preview ? g_preview.radius : 0.0f
                ),
                live_preview ? g_live_preview_actors.size() : 0u,
                live_preview
                    ? active_here ? "LEVEL RESOURCE" : "CROSS-LEVEL DEFAULT"
                    : g_preview_failed_catalog == *g_selected
                        ? "FACTORY FAILED"
                        : "SELECTED"
            );
        }
        draw_text(
            194u,
            details,
            g_live_preview_actor != 0u ? green : white,
            0xED100212u,
            0.38f
        );
    }

    draw_text(
        204u,
        saved_count == 0u
            ? "CURRENT LEVEL IS CLEAN"
            : g_clear_level_armed
                ? "CONFIRM CLEAR CURRENT LEVEL"
                : "CLEAR CURRENT LEVEL ADDITIONS (" +
                    std::to_string(saved_count) + ")",
        saved_count == 0u
            ? grey
            : hover.action == MenuAction::Clear
                ? green
                : g_clear_level_armed ? red : white,
        0xED100220u,
        0.39f
    );
    draw_text(
        214u,
        "DELETE EDITOR-PLACED OBJECT",
        hover.action == MenuAction::Delete ? green : white,
        0xED100221u,
        0.39f
    );
    const bool can_place = g_selected.has_value() &&
        g_live_preview_actor != 0u &&
        g_live_preview_catalog == *g_selected &&
        g_preview_updates_pending.empty();
    draw_text(
        224u,
        can_place ? "PLACE SELECTED OBJECT" :
            g_selected.has_value()
                ? "WAITING FOR LIVE PREVIEW"
                : "SELECT AN OBJECT",
        hover.action == MenuAction::Place && can_place
            ? green
            : can_place ? white : grey,
        0xED100222u,
        0.39f
    );
    draw_text(
        234u,
        "CLOSE EDITOR",
        hover.action == MenuAction::Stop ? green : white,
        0xED100223u,
        0.39f
    );
    if (!g_menu_publish_logged) {
        const std::vector<bumble::text_overlay::Observation> active =
            bumble::text_overlay::active_observations();
        const size_t observations = static_cast<size_t>(std::count_if(
            active.begin(),
            active.end(),
            [](const bumble::text_overlay::Observation& observation) {
                return observation.kind ==
                    bumble::text_overlay::TextKind::Editor;
            }
        ));
        std::fprintf(
            stderr,
            "BUMBLE_EDITOR stage=overlay_published observations=%zu\n",
            observations
        );
        std::fflush(stderr);
        g_menu_publish_logged = true;
    }
}

void set_mode_locked(Mode mode) {
    const Mode previous = g_mode.exchange(mode, std::memory_order_acq_rel);
    if (previous == Mode::Menu && mode == Mode::Place) {
        g_live_preview_catalog = SIZE_MAX;
    }
    if (previous != mode) {
        std::fprintf(
            stderr,
            "BUMBLE_EDITOR stage=mode_changed mode=%s\n",
            mode_name(mode)
        );
        std::fflush(stderr);
    }
    g_preview = {};
    bumble::collision_debug::set_editor_preview_active(false);
    if (mode == Mode::Inactive) {
        g_mouse_down.store(false, std::memory_order_release);
        g_q_down.store(false, std::memory_order_release);
        g_e_down.store(false, std::memory_order_release);
        g_clear_level_armed = false;
    }
    if (mode != Mode::Menu) {
        g_menu_publish_logged = false;
        bumble::text_overlay::clear_kind(
            bumble::text_overlay::TextKind::Editor
        );
    }
    bumble::modern_controls::set_primary_fire(false);
}

void close_from_input() {
    const Mode previous = g_mode.exchange(
        Mode::Inactive,
        std::memory_order_acq_rel
    );
    if (previous == Mode::Inactive) {
        return;
    }
    std::fprintf(
        stderr,
        "BUMBLE_EDITOR stage=mode_changed mode=inactive\n"
    );
    std::fflush(stderr);
    g_mouse_down.store(false, std::memory_order_release);
    g_q_down.store(false, std::memory_order_release);
    g_e_down.store(false, std::memory_order_release);
    g_clear_level_armed = false;
    bumble::collision_debug::set_editor_preview_active(false);
    bumble::text_overlay::clear_kind(
        bumble::text_overlay::TextKind::Editor
    );
    bumble::modern_controls::set_primary_fire(false);
}

void handle_menu_click_locked(
    uint8_t* rdram,
    recomp_context* context,
    const bumble::graphics_options::PointerSnapshot& pointer
) {
    const std::vector<size_t> visible = filtered_catalog_locked();
    const size_t first = g_page * kPageSize;
    const size_t item_count = first < visible.size()
        ? std::min(kPageSize, visible.size() - first)
        : 0u;
    const MenuHit hit = menu_hit(pointer, item_count);
    const GuestPointer guest = guest_pointer(pointer);
    std::fprintf(
        stderr,
        "BUMBLE_EDITOR stage=menu_click action=%u"
        " guest=%.1f,%.1f valid=%d\n",
        static_cast<unsigned>(hit.action),
        static_cast<double>(guest.x),
        static_cast<double>(guest.y),
        guest.valid ? 1 : 0
    );
    std::fflush(stderr);
    if (hit.action != MenuAction::Clear) {
        g_clear_level_armed = false;
    }
    if (hit.action == MenuAction::Item && hit.row < item_count) {
        g_selected = visible[first + hit.row];
        g_preview_failed_catalog = SIZE_MAX;
        return;
    }

    const size_t page_count = std::max<size_t>(
        1u,
        (visible.size() + kPageSize - 1u) / kPageSize
    );
    if (hit.action == MenuAction::Clear) {
        const bool has_additions = std::any_of(
            g_saved.begin(),
            g_saved.end(),
            [](const SavedEntry& entry) { return entry.level == g_level; }
        );
        if (!has_additions) {
            g_clear_level_armed = false;
        }
        else if (!g_clear_level_armed) {
            g_clear_level_armed = true;
            std::fprintf(
                stderr,
                "BUMBLE_EDITOR stage=clear_level_armed level=%u\n",
                g_level
            );
            std::fflush(stderr);
        }
        else {
            (void)clear_current_level_locked(rdram, context);
            g_clear_level_armed = false;
        }
    }
    else if (hit.action == MenuAction::Filter) {
        g_filter = static_cast<CatalogFilter>(
            (static_cast<uint8_t>(g_filter) + 1u) % 4u
        );
        g_page = 0u;
        g_selected.reset();
    }
    else if (hit.action == MenuAction::Previous && g_page > 0u) {
        --g_page;
        g_selected.reset();
    }
    else if (hit.action == MenuAction::Next &&
             g_page + 1u < page_count) {
        ++g_page;
        g_selected.reset();
    }
    else if (hit.action == MenuAction::Delete) {
        set_mode_locked(Mode::Delete);
    }
    else if (hit.action == MenuAction::Place &&
             g_selected.has_value() &&
             g_live_preview_actor != 0u &&
             g_live_preview_catalog == *g_selected) {
        set_mode_locked(Mode::Place);
    }
    else if (hit.action == MenuAction::Stop) {
        set_mode_locked(Mode::Inactive);
    }
}

void draw_placement_locked(const char* mode_name) {
    constexpr uint32_t white = 0xFFF4D8FFu;
    constexpr uint32_t yellow = 0xFFD64AFFu;
    constexpr uint32_t green = 0x72F18BFFu;
    constexpr uint32_t red = 0xFF7676FFu;
    const bool deleting =
        g_mode.load(std::memory_order_acquire) == Mode::Delete;
    bumble::text_overlay::clear_kind(
        bumble::text_overlay::TextKind::Editor
    );
    draw_center_text(
        14u,
        mode_name,
        yellow,
        0xED200001u,
        0.76f
    );
    if (g_preview.valid) {
        char position[160]{};
        std::snprintf(
            position,
            sizeof(position),
            "TARGET %.1f  %.1f  %.1f / YAW %d",
            static_cast<double>(g_preview.x),
            static_cast<double>(g_preview.y),
            static_cast<double>(g_preview.z),
            g_yaw_degrees.load(std::memory_order_acquire)
        );
        draw_center_text(
            29u,
            position,
            white,
            0xED200002u,
            0.58f
        );
        draw_center_text(
            120u,
            "+",
            green,
            0xED200003u,
            1.25f
        );
        draw_center_text(
            43u,
            deleting
                ? g_preview.actor != 0u
                    ? "CLICK TO DELETE TARGETED OBJECT"
                    : "AIM AT AN EDITOR-PLACED OBJECT"
                : g_live_preview_actor != 0u
                    ? "LIVE 3D PREVIEW + COLLISION SILHOUETTE"
                    : "PREVIEW FAILED - CLICK WILL NOT SAVE",
            deleting
                ? g_preview.actor != 0u ? green : red
                : g_live_preview_actor != 0u ? green : red,
            0xED200005u,
            0.52f
        );
    }
    else {
        draw_center_text(
            29u,
            deleting
                ? "AIM AT AN EDITOR-PLACED OBJECT"
                : "NO VALID TERRAIN TARGET",
            white,
            0xED200002u,
            0.58f
        );
    }
    draw_center_text(
        226u,
        deleting
            ? "LEFT CLICK: DELETE    F3/ESC: CLOSE"
            : "LEFT CLICK: SAVE    Q/E: ROTATE    F3/ESC: CLOSE",
        white,
        0xED200004u,
        0.58f
    );
}

void process_tick_locked(
    uint8_t* rdram,
    recomp_context* context,
    const bumble::graphics_options::PointerSnapshot& pointer,
    uint64_t click_revision,
    Mode mode
) {
    const uint32_t level = valid_guest(kCurrentLevel, 4u)
        ? read_u32(rdram, kCurrentLevel)
        : UINT32_MAX;
    if (level != g_level) {
        reset_level_locked(level);
    }
    spawn_saved_locked(rdram, context);
    if (mode != Mode::Inactive &&
        !bumble::modern_controls::gameplay_input_active()) {
        set_mode_locked(Mode::Inactive);
        mode = Mode::Inactive;
    }

    if (mode == Mode::Menu) {
        if (click_revision != g_last_click) {
            handle_menu_click_locked(rdram, context, pointer);
            mode = g_mode.load(std::memory_order_acquire);
        }
        if (mode == Mode::Menu) {
            if (!g_selected.has_value()) {
                const std::vector<size_t> visible =
                    filtered_catalog_locked();
                const size_t first = g_page * kPageSize;
                if (first < visible.size()) {
                    const auto page_begin =
                        visible.begin() + static_cast<ptrdiff_t>(first);
                    const auto page_end =
                        visible.begin() + static_cast<ptrdiff_t>(
                            std::min(visible.size(), first + kPageSize)
                        );
                    auto ready = std::find_if(
                        page_begin,
                        page_end,
                        [](size_t index) {
                            return g_catalog[index].observed_level == g_level;
                        }
                    );
                    g_selected = ready != page_end
                        ? *ready
                        : visible[first];
                    g_preview_failed_catalog = SIZE_MAX;
                }
            }
            g_preview = {};
            if (g_selected.has_value() &&
                *g_selected < g_catalog.size()) {
                Ray ray{};
                bumble::level_editor::Preview target{};
                if (placement_target(
                        rdram,
                        context,
                        false,
                        ray,
                        target)) {
                    const float actor_radius = g_live_preview_actor != 0u
                        ? read_f32(rdram, g_live_preview_actor + 0x6Cu)
                        : 36.0f;
                    const float radius =
                        std::isfinite(actor_radius) &&
                            actor_radius >= 12.0f &&
                            actor_radius <= 4096.0f
                        ? actor_radius
                        : 36.0f;
                    const CatalogEntry& selected = g_catalog[*g_selected];
                    const CatalogFilter filter =
                        catalog_filter(selected.words);
                    const bool large_enemy =
                        filter == CatalogFilter::Enemies &&
                        (selected.words[10] == 0x00000002u ||
                         radius >= 60.0f);
                    const float distance =
                        large_enemy
                            ? std::clamp(
                                ray.focus_distance + radius * 2.5f,
                                600.0f,
                                1800.0f
                            )
                            : filter == CatalogFilter::World
                                ? std::clamp(
                                    ray.focus_distance + radius * 2.0f,
                                    240.0f,
                                    2000.0f
                                )
                                : std::clamp(
                                    ray.focus_distance + radius,
                                    180.0f,
                                    600.0f
                                );
                    target.x =
                        ray.origin_x + ray.direction_x * distance;
                    target.y =
                        ray.origin_y + ray.direction_y * distance;
                    target.z =
                        ray.origin_z + ray.direction_z * distance;
                    const float right_x = -ray.direction_z;
                    const float right_z = ray.direction_x;
                    const float right_length = std::sqrt(
                        right_x * right_x + right_z * right_z
                    );
                    if (std::isfinite(right_length) &&
                        right_length > 0.01f) {
                        const float side_offset = distance * 0.28f;
                        target.x -=
                            right_x / right_length * side_offset;
                        target.z -=
                            right_z / right_length * side_offset;
                        target.y += distance * 0.08f;
                    }
                    target.radius = radius;
                    const bool preview_active = update_live_preview_locked(
                        rdram,
                        context,
                        *g_selected,
                        target
                    );
                    bumble::collision_debug::set_editor_preview_active(
                        preview_active
                    );
                    if (!preview_active) {
                        g_preview = {};
                    }
                }
            }
            draw_menu_locked(rdram, pointer);
        }
    }
    else if (mode == Mode::Place && g_selected.has_value() &&
             *g_selected < g_catalog.size()) {
        Ray ray{};
        g_preview = {};
        (void)placement_target(
            rdram,
            context,
            catalog_filter(g_catalog[*g_selected].words) !=
                CatalogFilter::Enemies,
            ray,
            g_preview
        );
        bool preview_active = false;
        if (g_preview.valid) {
            preview_active = update_live_preview_locked(
                rdram,
                context,
                *g_selected,
                g_preview
            );
        }
        if (!preview_active) {
            g_preview = {};
        }
        bumble::collision_debug::set_editor_preview_active(preview_active);
        if (click_revision != g_last_click && g_preview.valid &&
            g_live_preview_actor != 0u &&
            g_preview_updates_pending.empty() &&
            g_saved.size() < kMaximumEdits) {
            CatalogEntry& selected = g_catalog[*g_selected];
            SavedEntry addition{};
            addition.id = g_next_id;
            addition.level = g_level;
            addition.vtable = g_live_preview_vtable;
            addition.words = placed_words(selected, g_preview);
            addition.actor = g_live_preview_actor;
            addition.actors = std::move(g_live_preview_actors);
            g_live_preview_actors.clear();
            addition.active = true;
            g_live_preview_actor = 0u;
            g_live_preview_vtable = 0u;
            g_live_preview_catalog = SIZE_MAX;
            g_preview_updates_pending.clear();
            g_deferred_actor_updates.insert(
                addition.actors.begin(),
                addition.actors.end()
            );
            selected.vtable = addition.vtable;
            ++g_next_id;
            g_saved.push_back(std::move(addition));
            SavedEntry& saved = g_saved.back();
            if (!write_document_locked()) {
                (void)destroy_actor_group_locked(
                    rdram,
                    context,
                    saved.actors,
                    saved.actor
                );
                g_saved.pop_back();
                --g_next_id;
                std::fprintf(
                    stderr,
                    "BUMBLE_EDITOR stage=place_rolled_back"
                    " reason=persistence_failed\n"
                );
            }
            else {
                std::fprintf(
                    stderr,
                    "BUMBLE_EDITOR stage=placed level=%u id=%llu"
                    " actor=0x%08X selector=0x%08X"
                    " xyz=%.2f,%.2f,%.2f yaw=%d active=%d\n",
                    saved.level,
                    static_cast<unsigned long long>(saved.id),
                    saved.actor,
                    saved.words[10],
                    static_cast<double>(g_preview.x),
                    static_cast<double>(g_preview.y),
                    static_cast<double>(g_preview.z),
                    static_cast<int32_t>(saved.words[4]),
                    saved.active ? 1 : 0
                );
                std::fflush(stderr);
                set_mode_locked(Mode::Inactive);
                return;
            }
        }
        draw_placement_locked(
            ("PLACE / " + g_catalog[*g_selected].label).c_str()
        );
    }
    else if (mode == Mode::Delete) {
        (void)destroy_live_preview_locked(rdram, context);
        Ray ray{};
        bumble::level_editor::Preview ground{};
        g_preview = {};
        std::optional<size_t> target;
        if (placement_target(rdram, context, false, ray, ground)) {
            target = delete_target_locked(rdram, ray, g_preview);
        }
        bumble::collision_debug::set_editor_preview_active(
            g_preview.valid
        );
        if (click_revision != g_last_click && target.has_value()) {
            SavedEntry removed = std::move(g_saved[*target]);
            g_saved.erase(g_saved.begin() +
                static_cast<ptrdiff_t>(*target));
            if (!write_document_locked()) {
                g_saved.insert(
                    g_saved.begin() + static_cast<ptrdiff_t>(*target),
                    std::move(removed)
                );
                std::fprintf(
                    stderr,
                    "BUMBLE_EDITOR stage=delete_rolled_back"
                    " id=%llu reason=persistence_failed\n",
                    static_cast<unsigned long long>(
                        g_saved[*target].id
                    )
                );
            }
            else {
                (void)destroy_actor_group_locked(
                    rdram,
                    context,
                    removed.actors,
                    removed.actor
                );
                std::fprintf(
                    stderr,
                    "BUMBLE_EDITOR stage=deleted level=%u id=%llu"
                    " actor=0x%08X\n",
                    removed.level,
                    static_cast<unsigned long long>(removed.id),
                    removed.actor
                );
                set_mode_locked(Mode::Inactive);
            }
            std::fflush(stderr);
            return;
        }
        draw_placement_locked(
            g_preview.valid
                ? "DELETE / EDITOR-PLACED OBJECT TARGETED"
                : "DELETE / AIM AT AN EDITOR-PLACED OBJECT"
        );
    }
    else {
        (void)destroy_live_preview_locked(rdram, context);
        g_preview = {};
        bumble::collision_debug::set_editor_preview_active(false);
    }
}

} // namespace

bool bumble::level_editor::initialize(
    const std::filesystem::path& data_root,
    const std::filesystem::path& rom_path,
    bool enabled
) {
    std::lock_guard lock(g_mutex);
    g_edits_path = data_root / "level-edits.json";
    g_catalog.clear();
    g_authored_record_count = 0u;
    g_authored_signature_count = 0u;
    g_non_placeable_control_signature_count = 0u;
    g_saved.clear();
    g_next_id = 1u;
    g_level = UINT32_MAX;
    g_selected.reset();
    g_page = 0u;
    g_filter = CatalogFilter::All;
    g_preview = {};
    g_live_preview_actor = 0u;
    g_live_preview_actors.clear();
    g_preview_updates_pending.clear();
    g_preview_update_tracking = false;
    g_preview_update_actors_before.clear();
    g_destruction_requested.clear();
    g_deferred_actor_updates.clear();
    g_live_preview_vtable = 0u;
    g_live_preview_catalog = SIZE_MAX;
    g_preview_failed_catalog = SIZE_MAX;
    g_clear_level_armed = false;
    g_menu_publish_logged = false;
    g_last_screen = UINT32_MAX;
    g_last_click = 0u;
    g_last_pointer_motion = 0u;
    g_last_mode = Mode::Inactive;
    g_mode.store(Mode::Inactive, std::memory_order_release);
    if (!enabled) {
        g_initialized.store(false, std::memory_order_release);
        return true;
    }
    if (!coordinate_self_check() || !catalog_label_self_check() ||
        !page_action_self_check() ||
        !load_catalog_from_rom_locked(rom_path) ||
        !load_document_locked()) {
        std::fprintf(
            stderr,
            "BUMBLE_EDITOR stage=initialize_failed"
            " path=%s reason=invalid_rom_catalog_or_edits_file\n",
            g_edits_path.string().c_str()
        );
        std::fflush(stderr);
        return false;
    }
    g_initialized.store(true, std::memory_order_release);
    std::fprintf(
        stderr,
        "BUMBLE_EDITOR stage=initialized path=%s saved=%zu"
        " catalog=%zu authored_signatures=%zu authored_records=%llu"
        " excluded_non_placeable_controls=%zu\n",
        g_edits_path.string().c_str(),
        g_saved.size(),
        g_catalog.size(),
        g_authored_signature_count,
        static_cast<unsigned long long>(g_authored_record_count),
        g_non_placeable_control_signature_count
    );
    std::fflush(stderr);
    return true;
}

void bumble::level_editor::shutdown() {
    std::lock_guard lock(g_mutex);
    g_initialized.store(false, std::memory_order_release);
    g_mode.store(Mode::Inactive, std::memory_order_release);
    g_preview = {};
    g_preview_update_tracking = false;
    g_preview_update_actors_before.clear();
    bumble::collision_debug::set_editor_preview_active(false);
    bumble::text_overlay::clear_kind(
        bumble::text_overlay::TextKind::Editor
    );
    // recomp::start has already retired its guest allocator at shutdown.
    g_scratch = {};
}

bool bumble::level_editor::menu_open() {
    return g_initialized.load(std::memory_order_acquire) &&
        g_mode.load(std::memory_order_acquire) == Mode::Menu;
}

bool bumble::level_editor::active() {
    return g_initialized.load(std::memory_order_acquire) &&
        g_mode.load(std::memory_order_acquire) != Mode::Inactive;
}

bool bumble::level_editor::handle_key(
    uint32_t virtual_key,
    bool pressed
) {
    if (!g_initialized.load(std::memory_order_acquire)) {
        return false;
    }
    if (virtual_key == kF3) {
        const bool was_down = g_f3_down.exchange(
            pressed,
            std::memory_order_acq_rel
        );
        if (pressed && !was_down) {
            const Mode current = g_mode.load(std::memory_order_acquire);
            if (current != Mode::Inactive) {
                close_from_input();
            }
            else if (bumble::modern_controls::gameplay_input_active()) {
                g_mode.store(Mode::Menu, std::memory_order_release);
                std::fprintf(
                    stderr,
                    "BUMBLE_EDITOR stage=mode_changed mode=menu\n"
                );
                std::fflush(stderr);
                g_mouse_down.store(false, std::memory_order_release);
                bumble::modern_controls::set_primary_fire(false);
            }
        }
        return true;
    }

    const Mode mode = g_mode.load(std::memory_order_acquire);
    if ((virtual_key == bumble::key::Escape ||
         virtual_key == bumble::key::Enter) &&
        mode != Mode::Inactive) {
        if (pressed) {
            close_from_input();
        }
        return false;
    }
    if (virtual_key != 'Q' && virtual_key != 'E') {
        return false;
    }
    if (mode == Mode::Inactive) {
        return false;
    }
    std::atomic_bool& down = virtual_key == 'Q' ? g_q_down : g_e_down;
    const bool was_down = down.exchange(pressed, std::memory_order_acq_rel);
    if (pressed && !was_down) {
        int32_t yaw = g_yaw_degrees.load(std::memory_order_acquire);
        yaw += virtual_key == 'Q' ? -15 : 15;
        yaw %= 360;
        if (yaw < 0) {
            yaw += 360;
        }
        g_yaw_degrees.store(yaw, std::memory_order_release);
    }
    return true;
}

bool bumble::level_editor::handle_primary_mouse(bool pressed) {
    if (!g_initialized.load(std::memory_order_acquire)) {
        return false;
    }
    if (g_mode.load(std::memory_order_acquire) == Mode::Inactive) {
        return false;
    }
    const bool was_down = g_mouse_down.exchange(
        pressed,
        std::memory_order_acq_rel
    );
    if (pressed && !was_down) {
        g_click_revision.fetch_add(1u, std::memory_order_acq_rel);
    }
    bumble::modern_controls::set_primary_fire(false);
    return true;
}

void bumble::level_editor::release_input_state() {
    g_f3_down.store(false, std::memory_order_release);
    g_q_down.store(false, std::memory_order_release);
    g_e_down.store(false, std::memory_order_release);
    g_mouse_down.store(false, std::memory_order_release);
    bumble::modern_controls::set_primary_fire(false);
}

bool bumble::level_editor::preview(Preview& value) {
    std::unique_lock lock(g_mutex, std::try_to_lock);
    if (!lock) {
        return false;
    }
    value = g_preview;
    return value.valid;
}

extern "C" void bumble_level_editor_observe_authored_record(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!g_initialized.load(std::memory_order_acquire) ||
        g_editor_spawn || rdram == nullptr || context == nullptr) {
        return;
    }
    const uint32_t interpreter = static_cast<uint32_t>(context->r2);
    const uint32_t record = static_cast<uint32_t>(context->r5);
    if (!valid_guest(interpreter, 0xACu) ||
        !valid_guest(record, kRecordBytes)) {
        return;
    }
    const uint32_t actor = read_u32(rdram, interpreter + 0xA0u);
    if (!valid_guest(actor, 0x90u)) {
        return;
    }
    const uint32_t vtable = read_u32(rdram, actor + 0x88u);
    if (vtable == 0u || vtable == kPlayerVtable) {
        return;
    }

    Words words{};
    for (size_t index = 0u; index < words.size(); ++index) {
        words[index] = read_u32(
            rdram,
            record + static_cast<uint32_t>(index * sizeof(uint32_t))
        );
    }
    const uint32_t level_resource = words[6];
    words[0] = 0u;
    words[1] = 0u;
    words[2] = 0u;
    words[3] = 0u;
    words[4] = 0u;
    words[5] = 0u;
    words[6] = 0u;
    if (!valid_words(words) || non_placeable_control(words)) {
        return;
    }

    const uint32_t level = read_u32(rdram, kCurrentLevel);
    std::lock_guard lock(g_mutex);
    if (level != g_level) {
        reset_level_locked(level);
    }
    const auto existing = std::find_if(
        g_catalog.begin(),
        g_catalog.end(),
        [&](const CatalogEntry& entry) {
            return same_catalog_key(words, entry.words);
        }
    );
    if (existing != g_catalog.end()) {
        existing->vtable = vtable;
        existing->label = catalog_label(words, vtable);
        existing->level_resource = level_resource;
        existing->observed_level = level;
        if (g_selected.has_value() &&
            &*existing == &g_catalog[*g_selected]) {
            g_preview_failed_catalog = SIZE_MAX;
        }
    }
    else {
        g_catalog.push_back({
            words,
            vtable,
            1u,
            0u,
            catalog_label(words, vtable),
            level_resource,
            level,
        });
    }
}

extern "C" uint32_t bumble_level_editor_preserve_louse_actor() {
    return g_editor_spawn ? 1u : 0u;
}

extern "C" void bumble_level_editor_repair_missing_target(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!g_initialized.load(std::memory_order_acquire) ||
        g_editor_spawn || rdram == nullptr || context == nullptr ||
        context->r19 != 0) {
        return;
    }

    std::lock_guard lock(g_mutex);
    const uint32_t actor = static_cast<uint32_t>(context->r20);
    const auto contains_actor = [actor](const std::vector<uint32_t>& actors) {
        return std::find(actors.begin(), actors.end(), actor) != actors.end();
    };
    const bool managed = contains_actor(g_live_preview_actors) ||
        std::any_of(
            g_saved.begin(),
            g_saved.end(),
            [&](const SavedEntry& entry) {
                return entry.actor == actor || contains_actor(entry.actors);
            }
        );
    if (!managed || !live_actor(rdram, actor)) {
        return;
    }

    const uint32_t owner = read_u32(rdram, actor + 0x60u);
    const uint32_t mask = read_u32(rdram, actor + 0x64u);
    const uint32_t previous = static_cast<uint32_t>(context->r21);
    const uint32_t player_kind = read_u32(rdram, kPlayerVtable + 0x20u);
    uint32_t compatible = 0u;
    uint32_t player = 0u;
    uint32_t first = 0u;
    for (uint32_t list = 0u; list < kObjectListCount; ++list) {
        const uint32_t list_owner =
            kObjectListBase + list * kObjectListStride;
        if (!valid_guest(list_owner, 0xC88u)) {
            continue;
        }
        const uint32_t count = read_u32(rdram, list_owner + 0xC84u);
        if (count > 100u) {
            continue;
        }
        for (uint32_t index = 0u; index < count; ++index) {
            const uint32_t candidate =
                list_owner + 4u + index * 0x20u;
            if (first == 0u) {
                first = candidate;
            }
            if (read_u32(rdram, candidate) == player_kind &&
                (player == 0u || player == previous)) {
                player = candidate;
            }
            if (list_owner == owner &&
                (read_u32(rdram, candidate + 0x08u) & mask) != 0u &&
                (compatible == 0u || compatible == previous)) {
                compatible = candidate;
            }
        }
    }
    const uint32_t fallback = compatible != 0u
        ? compatible
        : player != 0u ? player : first;
    if (fallback == 0u) {
        return;
    }

    context->r19 = guest_address(fallback);
    if (previous == 0u) {
        std::fprintf(
            stderr,
            "BUMBLE_EDITOR stage=target_fallback actor=0x%08X"
            " target=0x%08X\n",
            actor,
            fallback
        );
        std::fflush(stderr);
    }
}

extern "C" uint32_t bumble_level_editor_tick(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!g_initialized.load(std::memory_order_acquire) ||
        rdram == nullptr || context == nullptr) {
        return 0u;
    }
    const Mode mode = g_mode.load(std::memory_order_acquire);
    const uint64_t click = g_click_revision.load(std::memory_order_acquire);
    const bumble::graphics_options::PointerSnapshot pointer =
        bumble::graphics_options::pointer_snapshot();
    const uint32_t screen = bumble::rt64_renderer::screen_update_count();
    const bool interaction_changed =
        click != g_last_click ||
        pointer.motion_revision != g_last_pointer_motion ||
        mode != g_last_mode;
    const bool process = screen != g_last_screen || interaction_changed;
    std::lock_guard lock(g_mutex);
    if (process) {
        process_tick_locked(
            rdram,
            context,
            pointer,
            click,
            mode
        );
        g_last_screen = screen;
        g_last_click = click;
        g_last_pointer_motion = pointer.motion_revision;
        g_last_mode = g_mode.load(std::memory_order_acquire);
    }
    return 0u;
}

extern "C" uint32_t bumble_level_editor_skip_actor_update(
    uint8_t* rdram,
    uint32_t update_subobject
) {
    const Mode mode = g_mode.load(std::memory_order_acquire);
    if (!g_initialized.load(std::memory_order_acquire) ||
        rdram == nullptr) {
        return 0u;
    }

    std::unique_lock lock(g_mutex, std::try_to_lock);
    if (!lock) {
        return 0u;
    }
    const auto owns_update = [&](uint32_t actor) {
        if (!live_actor(rdram, actor)) {
            return false;
        }
        const uint32_t vtable = read_u32(rdram, actor + 0x88u);
        if (!valid_guest(vtable + 0x10u, sizeof(uint16_t))) {
            return false;
        }
        const int16_t offset = static_cast<int16_t>(
            MEM_H(0, guest_address(vtable + 0x10u))
        );
        return actor + static_cast<uint32_t>(offset) == update_subobject;
    };
    if (mode == Mode::Menu || mode == Mode::Place) {
        for (uint32_t actor : g_live_preview_actors) {
            if (owns_update(actor)) {
                if (g_destruction_requested.contains(actor)) {
                    return 0u;
                }
                if (g_preview_updates_pending.erase(actor) != 0u) {
                    std::vector<uint32_t> before;
                    g_preview_update_tracking =
                        collect_live_actors(rdram, before);
                    g_preview_update_actors_before.clear();
                    if (g_preview_update_tracking) {
                        g_preview_update_actors_before.insert(
                            before.begin(),
                            before.end()
                        );
                    }
                    return 0u;
                }
                return 1u;
            }
        }
    }
    for (auto actor = g_deferred_actor_updates.begin();
         actor != g_deferred_actor_updates.end();) {
        if (!live_actor(rdram, *actor)) {
            actor = g_deferred_actor_updates.erase(actor);
            continue;
        }
        if (owns_update(*actor)) {
            g_deferred_actor_updates.erase(actor);
            return 1u;
        }
        ++actor;
    }
    return 0u;
}

extern "C" void bumble_level_editor_finish_actor_update(uint8_t* rdram) {
    if (!g_preview_update_tracking) {
        return;
    }
    g_preview_update_tracking = false;

    std::vector<uint32_t> after;
    if (!g_initialized.load(std::memory_order_acquire) ||
        rdram == nullptr || !collect_live_actors(rdram, after)) {
        g_preview_update_actors_before.clear();
        return;
    }

    std::lock_guard lock(g_mutex);
    size_t added = 0u;
    for (uint32_t actor : after) {
        if (g_preview_update_actors_before.contains(actor) ||
            std::find(
                g_live_preview_actors.begin(),
                g_live_preview_actors.end(),
                actor
            ) != g_live_preview_actors.end()) {
            continue;
        }
        g_live_preview_actors.push_back(actor);
        g_preview_updates_pending.insert(actor);
        ++added;
    }
    g_preview_update_actors_before.clear();
    if (added != 0u) {
        std::fprintf(
            stderr,
            "BUMBLE_EDITOR stage=preview_children_tracked"
            " added=%zu total=%zu\n",
            added,
            g_live_preview_actors.size()
        );
        std::fflush(stderr);
    }
}
