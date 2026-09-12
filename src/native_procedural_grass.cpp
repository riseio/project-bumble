#include "native_procedural_grass.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cinttypes>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

#include "funcs.h"
#include "librecomp/addresses.hpp"
#include "native_collision_debug.hpp"
#include "native_widescreen.hpp"

#if defined(BUMBLE_GPU_GRASS_RT64) && BUMBLE_GPU_GRASS_RT64
#   include "render/rt64_bumble_grass.h"
#   include "render/rt64_bumble_collision_debug.h"
#endif

namespace {

using bumble::procedural_grass::Mode;

constexpr uint32_t kTerrainDescriptorBase = 0x800EA4D0u;
constexpr uint32_t kTerrainDescriptorEnd = 0x80100000u;
constexpr uint32_t kTerrainDescriptorStride = 0x1Cu;
constexpr uint32_t kDisplayListCursor = 0x80035F30u;
constexpr uint32_t kFrameMatrixBase = 0x80105224u;
constexpr uint32_t kFrontendState = 0x800FFF80u;
constexpr uint32_t kFrontendWorldActiveOffset = 0x24u;
constexpr uint32_t kFrontendPlayerLayoutOffset = 0x0Cu;
constexpr uint32_t kSinglePlayerLayout = 1u;
constexpr uint32_t kCurrentLevelIndexAddress = 0x800E9640u;
constexpr uint32_t kMissionOneLevelIndex = 1u;
constexpr uint32_t kPlayerOneOwnerSlot = 0x800E91D4u;
constexpr uint32_t kPlayerTwoOwnerSlot = 0x800E91D8u;
constexpr uint32_t kPlayerVtable = 0x800445E8u;
constexpr float kPlayerFootProbeOffsetY = -36.0f;
constexpr uint32_t kFrameBaseFromMatrixBase = 0x68u;
constexpr uint32_t kDisplayListArenaOffset = 0xB068u;
constexpr uint32_t kDisplayListArenaEndOffset = 0x14CA8u;
constexpr uint32_t kDownstreamDisplayListReserve = 0x2800u;

constexpr size_t kCandidateCapacity = 512u;
constexpr size_t kMaterialObservationCapacity = 32u;
constexpr size_t kLowCellBudget = 48u;
constexpr size_t kHighCellBudget = 96u;
constexpr float kHighDetailDistance = 650.0f;
constexpr float kLowGpuCollectDistance = 1164.0f;
constexpr float kHighGpuCollectDistance = 1614.0f;

constexpr size_t kTextureWidth = 16u;
constexpr size_t kTextureHeight = 16u;
constexpr size_t kTextureBytes = kTextureWidth * kTextureHeight;
constexpr size_t kVariantCount = 8u;
constexpr size_t kBladesPerLowCluster = 16u;
constexpr size_t kBladesPerHighCluster = 32u;
constexpr int32_t kBladeBaseHeight = 1;
constexpr size_t kVerticesPerBlade = 8u;
constexpr size_t kVertexBytes = 16u;
constexpr size_t kVerticesPerVariant =
    kBladesPerHighCluster * kVerticesPerBlade;
constexpr size_t kVariantVertexBytes = kVerticesPerVariant * kVertexBytes;
constexpr size_t kCommandsPerFourBlades = 9u;
constexpr size_t kLowDisplayListCommands =
    (kBladesPerLowCluster / 4u) * kCommandsPerFourBlades + 1u;
constexpr size_t kHighDisplayListCommands =
    (kBladesPerHighCluster / 4u) * kCommandsPerFourBlades + 1u;
constexpr size_t kCommandBytes = 8u;
constexpr size_t kLowDisplayListBytes =
    kLowDisplayListCommands * kCommandBytes;
constexpr size_t kHighDisplayListBytes =
    kHighDisplayListCommands * kCommandBytes;
constexpr size_t kVariantDisplayListStride = 896u;
static_assert(
    kLowDisplayListBytes + kHighDisplayListBytes <=
    kVariantDisplayListStride
);

constexpr uint32_t kExtendedOpcode = 0x64u;
constexpr uint32_t kPushOtherMode = 0x19u;
constexpr uint32_t kPopOtherMode = 0x1Au;
constexpr uint32_t kPushCombine = 0x1Bu;
constexpr uint32_t kPopCombine = 0x1Cu;
constexpr uint32_t kPushPrimColor = 0x27u;
constexpr uint32_t kPopPrimColor = 0x28u;
constexpr uint32_t kPushGeometryMode = 0x29u;
constexpr uint32_t kPopGeometryMode = 0x2Au;
constexpr uint32_t kSetRdramExtended = 0x2Cu;
constexpr uint32_t kBumbleGrassMarker = 0x34u;

constexpr uint32_t kGeometryClear =
    0x00020000u | // G_LIGHTING
    0x00000600u | // G_CULL_BOTH (F3DEX2)
    0x000C0000u;  // G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR
constexpr uint32_t kGeometrySet =
    0x00000001u | // G_ZBUFFER
    0x00000004u | // G_SHADE
    0x00200000u;  // G_SHADING_SMOOTH (F3DEX2)

constexpr uint32_t kGrassCombineWord0 = 0xFC119623u; // MODULATEIA_PRIM
constexpr uint32_t kGrassCombineWord1 = 0xFF2FFFFFu;
constexpr uint32_t kGrassRenderMode = 0x00553078u; // AA_ZB_TEX_EDGE, both cycles
constexpr uint32_t kGrassPrimitiveColor = 0x629B3CFFu;

std::atomic<uint32_t> g_mode{static_cast<uint32_t>(Mode::Off)};
std::atomic<uint32_t> g_last_logged_mode{UINT32_MAX};
std::atomic_bool g_extended_gbi_required{false};
std::atomic_uint64_t g_emitted_passes{0};
std::atomic_bool g_allocation_failure_logged{false};
std::atomic_bool g_ground_contract_logged{false};
std::atomic_uint64_t g_emit_skips{0};
#if defined(BUMBLE_GPU_GRASS_RT64) && BUMBLE_GPU_GRASS_RT64
std::atomic_uint64_t g_gpu_markers{0};
std::atomic_uint64_t g_gpu_marker_failures{0};
#endif

constexpr std::array<uint16_t, 1u> kMissionOneFlatGroundBaseIds{0x0070u};

struct Candidate {
    uint32_t matrix = 0;
    uint32_t material_key = 0;
    uint32_t placement_hash = 0;
    float distance_squared = 0.0f;
    float world_x = 0.0f;
    float world_y = 0.0f;
    float world_z = 0.0f;
};

struct MaterialObservation {
    uint32_t first_descriptor = 0;
    uint32_t second_descriptor = 0;
    uint32_t material_key = 0;
    uint32_t count = 0;
    int32_t sample_world_x = 0;
    int32_t sample_world_z = 0;
    float nearest_distance_squared = 0.0f;
};

struct TerrainPass {
    std::array<Candidate, kCandidateCapacity> candidates{};
    size_t count = 0;
    uint32_t original_last_matrix = 0;
    uint32_t translucent_rejections = 0;
    uint32_t ground_whitelist_rejections = 0;
    uint32_t distance_rejections = 0;
    std::array<MaterialObservation, kMaterialObservationCapacity>
        material_observations{};
    size_t material_observation_count = 0;
    float camera_x = 0.0f;
    float camera_y = 0.0f;
    float camera_z = 0.0f;
    uint32_t camera_source = 0;
};

struct MaterialCacheEntry {
    uint32_t descriptor = 0;
    uint32_t signature = 0;
    bool translucent = false;
};

struct GrassAssets {
    uint8_t* rdram = nullptr;
    uint32_t texture = 0;
    std::array<uint32_t, kVariantCount> low_display_lists{};
    std::array<uint32_t, kVariantCount> high_display_lists{};
};

thread_local TerrainPass g_terrain_pass{};
thread_local std::array<MaterialCacheEntry, 256u> g_material_cache{};
GrassAssets g_assets{};

gpr guest_address(uint32_t address) {
    return static_cast<gpr>(static_cast<int32_t>(address));
}

uint32_t low_guest_address(gpr address) {
    return static_cast<uint32_t>(address);
}

bool rdram_address(uint32_t address, size_t bytes) {
    if (address < 0x80000000u) {
        return false;
    }
    const uint64_t offset = static_cast<uint64_t>(address - 0x80000000u);
    const uint64_t limit = static_cast<uint64_t>(recomp::mem_size);
    return offset <= limit && static_cast<uint64_t>(bytes) <= limit - offset;
}

uint32_t read_u32(uint8_t* rdram, uint32_t address) {
    return static_cast<uint32_t>(MEM_W(0, guest_address(address)));
}

uint16_t read_u16(uint8_t* rdram, uint32_t address) {
    return static_cast<uint16_t>(MEM_HU(0, guest_address(address)));
}

int16_t read_s16(uint8_t* rdram, uint32_t address) {
    return static_cast<int16_t>(MEM_H(0, guest_address(address)));
}

float read_f32(uint8_t* rdram, uint32_t address) {
    return std::bit_cast<float>(read_u32(rdram, address));
}

void write_u32(uint8_t* rdram, uint32_t address, uint32_t value) {
    MEM_W(0, guest_address(address)) = static_cast<int32_t>(value);
}

void write_f32(uint8_t* rdram, uint32_t address, float value) {
    write_u32(rdram, address, std::bit_cast<uint32_t>(value));
}

void write_s16(uint8_t* rdram, uint32_t address, int16_t value) {
    MEM_H(0, guest_address(address)) = value;
}

void write_u8(uint8_t* rdram, uint32_t address, uint8_t value) {
    MEM_B(0, guest_address(address)) = static_cast<int8_t>(value);
}

uint32_t mix32(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    value ^= value >> 16;
    return value;
}

uint32_t next_random(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

bool unsafe_material_discovery_enabled() {
    static const bool enabled = [] {
        std::string value;
#if defined(_WIN32)
        char* raw_value = nullptr;
        size_t value_size = 0;
        if (_dupenv_s(
                &raw_value,
                &value_size,
                "BUMBLE_GRASS_UNSAFE_MATERIAL_DISCOVERY") == 0 &&
            raw_value != nullptr) {
            value.assign(raw_value);
        }
        std::free(raw_value);
#else
        const char* raw_value = std::getenv(
            "BUMBLE_GRASS_UNSAFE_MATERIAL_DISCOVERY"
        );
        if (raw_value != nullptr) {
            value.assign(raw_value);
        }
#endif
        std::transform(
            value.begin(),
            value.end(),
            value.begin(),
            [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            }
        );
        return value == "1" || value == "on" || value == "true" ||
            value == "yes";
    }();
    return enabled;
}

bool debug_visualization_enabled() {
    static const bool enabled = [] {
        std::string value;
#if defined(_WIN32)
        char* raw_value = nullptr;
        size_t value_size = 0;
        if (_dupenv_s(
                &raw_value,
                &value_size,
                "BUMBLE_GRASS_DEBUG_VISUAL") == 0 &&
            raw_value != nullptr) {
            value.assign(raw_value);
        }
        std::free(raw_value);
#else
        const char* raw_value = std::getenv("BUMBLE_GRASS_DEBUG_VISUAL");
        if (raw_value != nullptr) {
            value.assign(raw_value);
        }
#endif
        std::transform(
            value.begin(),
            value.end(),
            value.begin(),
            [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            }
        );
        return value == "1" || value == "on" || value == "true" ||
            value == "yes";
    }();
    return enabled;
}

bool mission_one_flat_ground(uint16_t base_id) {
    return std::find(
        kMissionOneFlatGroundBaseIds.begin(),
        kMissionOneFlatGroundBaseIds.end(),
        base_id
    ) != kMissionOneFlatGroundBaseIds.end();
}

void observe_material(
    uint32_t first_descriptor,
    uint32_t second_descriptor,
    uint32_t material_key,
    int32_t world_x,
    int32_t world_z,
    float distance_squared
) {
    TerrainPass& pass = g_terrain_pass;
    for (size_t index = 0; index < pass.material_observation_count; ++index) {
        MaterialObservation& observation = pass.material_observations[index];
        if (observation.first_descriptor == first_descriptor &&
            observation.second_descriptor == second_descriptor &&
            observation.material_key == material_key) {
            ++observation.count;
            if (distance_squared < observation.nearest_distance_squared) {
                observation.sample_world_x = world_x;
                observation.sample_world_z = world_z;
                observation.nearest_distance_squared = distance_squared;
            }
            return;
        }
    }
    if (pass.material_observation_count >=
        pass.material_observations.size()) {
        return;
    }
    pass.material_observations[pass.material_observation_count++] = {
        first_descriptor,
        second_descriptor,
        material_key,
        1u,
        world_x,
        world_z,
        distance_squared,
    };
}

uint32_t descriptor_index(uint32_t descriptor) {
    if (descriptor <= kTerrainDescriptorBase ||
        descriptor >= kTerrainDescriptorEnd) {
        return 0;
    }
    const uint32_t delta = descriptor - kTerrainDescriptorBase;
    if ((delta % kTerrainDescriptorStride) != 0u) {
        return 0;
    }
    return delta / kTerrainDescriptorStride;
}

bool direct_display_list_address(uint32_t address) {
    return address >= 0x80000000u && address < 0x80800000u &&
        rdram_address(address, kCommandBytes);
}

struct DescriptorGeometryScan {
    int32_t minimum_y = std::numeric_limits<int32_t>::max();
    int32_t maximum_y = std::numeric_limits<int32_t>::min();
    int64_t sum_y = 0;
    uint32_t vertex_count = 0;
    uint32_t vertex_loads = 0;
    uint32_t unresolved_addresses = 0;
};

uint32_t direct_vertex_address(uint32_t address, size_t bytes) {
    uint32_t guest = 0u;
    if ((address & 0x80000000u) != 0u) {
        guest = address;
    }
    else if ((address & 0xFF000000u) == 0u) {
        guest = 0x80000000u | address;
    }
    return guest != 0u && rdram_address(guest, bytes) ? guest : 0u;
}

void scan_geometry_display_list(
    uint8_t* rdram,
    uint32_t display_list,
    DescriptorGeometryScan& scan,
    uint32_t depth
) {
    if (depth > 2u || !direct_display_list_address(display_list)) {
        ++scan.unresolved_addresses;
        return;
    }
    constexpr size_t kMaximumGeometryCommands = 256u;
    for (size_t index = 0; index < kMaximumGeometryCommands; ++index) {
        const uint32_t address = display_list +
            static_cast<uint32_t>(index * kCommandBytes);
        if (!rdram_address(address, kCommandBytes)) {
            ++scan.unresolved_addresses;
            return;
        }
        const uint32_t word0 = read_u32(rdram, address);
        const uint32_t word1 = read_u32(rdram, address + 4u);
        const uint8_t opcode = static_cast<uint8_t>(word0 >> 24);
        if (opcode == 0x01u) {
            const uint32_t count = (word0 >> 12) & 0xFFu;
            const size_t bytes = static_cast<size_t>(count) * kVertexBytes;
            const uint32_t vertices = direct_vertex_address(word1, bytes);
            if (count == 0u || vertices == 0u) {
                ++scan.unresolved_addresses;
                continue;
            }
            ++scan.vertex_loads;
            for (uint32_t vertex = 0; vertex < count; ++vertex) {
                const int32_t y = read_s16(
                    rdram,
                    vertices + vertex * static_cast<uint32_t>(kVertexBytes) +
                        2u
                );
                scan.minimum_y = std::min(scan.minimum_y, y);
                scan.maximum_y = std::max(scan.maximum_y, y);
                scan.sum_y += y;
                ++scan.vertex_count;
            }
        }
        else if (opcode == 0xDEu) {
            const uint32_t nested = direct_vertex_address(word1, kCommandBytes);
            if (nested != 0u) {
                scan_geometry_display_list(rdram, nested, scan, depth + 1u);
            }
            else {
                ++scan.unresolved_addresses;
            }
        }
        else if (opcode == 0xDFu) {
            return;
        }
    }
}

DescriptorGeometryScan scan_descriptor_geometry(
    uint8_t* rdram,
    uint32_t descriptor
) {
    DescriptorGeometryScan scan{};
    if (descriptor == 0u ||
        !rdram_address(descriptor, kTerrainDescriptorStride)) {
        return scan;
    }
    const uint32_t full = read_u32(rdram, descriptor);
    const uint32_t continuation = read_u32(rdram, descriptor + 0x10u);
    if (full != 0u) {
        scan_geometry_display_list(rdram, full, scan, 0u);
    }
    if (continuation != 0u && continuation != full) {
        scan_geometry_display_list(rdram, continuation, scan, 0u);
    }
    return scan;
}

bool scan_translucent_display_list(uint8_t* rdram, uint32_t display_list) {
    if (!direct_display_list_address(display_list)) {
        return false;
    }

    constexpr size_t kMaximumCommands = 96u;
    for (size_t index = 0; index < kMaximumCommands; ++index) {
        const uint32_t address = display_list +
            static_cast<uint32_t>(index * kCommandBytes);
        if (!rdram_address(address, kCommandBytes)) {
            break;
        }
        const uint32_t word0 = read_u32(rdram, address);
        const uint32_t word1 = read_u32(rdram, address + 4u);
        const uint8_t opcode = static_cast<uint8_t>(word0 >> 24);
        if (opcode == 0xE2u || opcode == 0xEFu) {
            constexpr uint32_t kZModeMask = 0x00000C00u;
            constexpr uint32_t kForceBlend = 0x00004000u;
            if ((word1 & (kZModeMask | kForceBlend)) != 0u) {
                return true;
            }
        }
        else if (opcode == 0xFAu && (word1 & 0xFFu) < 0xC0u) {
            return true;
        }
        else if (opcode == 0xDFu) {
            break;
        }
    }
    return false;
}

bool descriptor_is_translucent(uint8_t* rdram, uint32_t descriptor) {
    const uint32_t index = descriptor_index(descriptor);
    if (index == 0u || !rdram_address(descriptor, kTerrainDescriptorStride)) {
        return true;
    }

    const uint32_t full_display_list = read_u32(rdram, descriptor);
    const uint32_t continuation_display_list = read_u32(rdram, descriptor + 0x10u);
    const uint32_t state_before = read_u32(rdram, descriptor + 0x14u);
    const uint32_t state_after = read_u32(rdram, descriptor + 0x18u);
    const uint32_t signature = mix32(
        full_display_list ^ std::rotl(continuation_display_list, 7) ^
        std::rotl(state_before, 13) ^ std::rotl(state_after, 19)
    );
    MaterialCacheEntry& cached =
        g_material_cache[static_cast<size_t>(index) % g_material_cache.size()];
    if (cached.descriptor == descriptor && cached.signature == signature) {
        return cached.translucent;
    }

    const bool translucent =
        scan_translucent_display_list(rdram, full_display_list) ||
        scan_translucent_display_list(rdram, continuation_display_list);
    cached = {descriptor, signature, translucent};
    return translucent;
}

bool terrain_material(
    uint8_t* rdram,
    uint32_t first,
    uint32_t second,
    uint32_t& material_key
) {
    if (first == 0u && second == 0u) {
        return false;
    }
    if ((first != 0u && descriptor_is_translucent(rdram, first)) ||
        (second != 0u && descriptor_is_translucent(rdram, second))) {
        return false;
    }

    const uint32_t first_index = descriptor_index(first);
    const uint32_t second_index = descriptor_index(second);
    if ((first != 0u && first_index == 0u) ||
        (second != 0u && second_index == 0u)) {
        return false;
    }
    const uint32_t owner = first != 0u ? first : second;
    const uint32_t state_before = read_u32(rdram, owner + 0x14u);
    const uint32_t state_after = read_u32(rdram, owner + 0x18u);
    material_key = mix32(
        first_index ^ std::rotl(second_index, 11) ^
        std::rotl(state_before, 17) ^ state_after
    );
    return true;
}

void retain_nearest_candidate(const Candidate& candidate) {
    TerrainPass& pass = g_terrain_pass;
    if (pass.count < pass.candidates.size()) {
        pass.candidates[pass.count++] = candidate;
        return;
    }

    size_t farthest = 0;
    for (size_t index = 1; index < pass.candidates.size(); ++index) {
        if (pass.candidates[index].distance_squared >
            pass.candidates[farthest].distance_squared) {
            farthest = index;
        }
    }
    if (candidate.distance_squared <
        pass.candidates[farthest].distance_squared) {
        pass.candidates[farthest] = candidate;
    }
}

constexpr size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

void write_command(
    uint8_t* rdram,
    uint32_t address,
    uint32_t word0,
    uint32_t word1
) {
    write_u32(rdram, address, word0);
    write_u32(rdram, address + 4u, word1);
}

uint32_t triangle_word(uint32_t a, uint32_t b, uint32_t c) {
    // F3DEX2 uses raw 7-bit indices at bits 17/9/1; do not double them.
    return (a << 17) | (b << 9) | (c << 1);
}

void write_vertex(
    uint8_t* rdram,
    uint32_t address,
    int16_t x,
    int16_t y,
    int16_t z,
    int16_t s,
    int16_t t
) {
    write_s16(rdram, address + 0u, x);
    write_s16(rdram, address + 2u, y);
    write_s16(rdram, address + 4u, z);
    write_s16(rdram, address + 6u, 0);
    write_s16(rdram, address + 8u, s);
    write_s16(rdram, address + 10u, t);
    write_u8(rdram, address + 12u, 0xFFu);
    write_u8(rdram, address + 13u, 0xFFu);
    write_u8(rdram, address + 14u, 0xFFu);
    write_u8(rdram, address + 15u, 0xFFu);
}

void write_quad(
    uint8_t* rdram,
    uint32_t address,
    int32_t center_x,
    int32_t center_z,
    int32_t lateral_x,
    int32_t lateral_z,
    int32_t lean_x,
    int32_t lean_z,
    int32_t height
) {
    constexpr int16_t kTextureMaximum = 15 * 32;
    write_vertex(
        rdram, address + 0u * kVertexBytes,
        static_cast<int16_t>(center_x - lateral_x),
        static_cast<int16_t>(kBladeBaseHeight),
        static_cast<int16_t>(center_z - lateral_z), 0, kTextureMaximum
    );
    write_vertex(
        rdram, address + 1u * kVertexBytes,
        static_cast<int16_t>(center_x + lateral_x),
        static_cast<int16_t>(kBladeBaseHeight),
        static_cast<int16_t>(center_z + lateral_z), kTextureMaximum,
        kTextureMaximum
    );
    write_vertex(
        rdram, address + 2u * kVertexBytes,
        static_cast<int16_t>(center_x + lateral_x / 4 + lean_x),
        static_cast<int16_t>(kBladeBaseHeight + height),
        static_cast<int16_t>(center_z + lateral_z / 4 + lean_z),
        kTextureMaximum, 0
    );
    write_vertex(
        rdram, address + 3u * kVertexBytes,
        static_cast<int16_t>(center_x - lateral_x / 4 + lean_x),
        static_cast<int16_t>(kBladeBaseHeight + height),
        static_cast<int16_t>(center_z - lateral_z / 4 + lean_z), 0, 0
    );
}

void build_variant_vertices(uint8_t* rdram, uint32_t base, size_t variant) {
    const bool debug_visual = debug_visualization_enabled();
    uint32_t random = mix32(
        0xB04B1E5u ^ static_cast<uint32_t>(variant * 0x9E3779B9u)
    );
    for (size_t blade = 0; blade < kBladesPerHighCluster; ++blade) {
        const uint32_t sample0 = next_random(random);
        const uint32_t sample1 = next_random(random);
        const int32_t center_x = static_cast<int32_t>(sample0 % 113u) - 56;
        const int32_t center_z = static_cast<int32_t>(sample1 % 113u) - 56;
        const int32_t half_width = debug_visual
            ? 16
            : 3 + static_cast<int32_t>(next_random(random) % 3u);
        const int32_t height = debug_visual
            ? 80
            : 12 + static_cast<int32_t>(next_random(random) % 9u);
        const int32_t lean_x =
            static_cast<int32_t>(next_random(random) % 5u) - 2;
        const int32_t lean_z =
            static_cast<int32_t>(next_random(random) % 5u) - 2;
        const bool diagonal = (next_random(random) & 1u) != 0u;
        const int32_t axis = diagonal ? (half_width * 3) / 4 : half_width;
        const int32_t first_x = axis;
        const int32_t first_z = diagonal ? axis : 0;
        const int32_t second_x = diagonal ? -axis : 0;
        const int32_t second_z = axis;
        const uint32_t blade_address = base + static_cast<uint32_t>(
            blade * kVerticesPerBlade * kVertexBytes
        );
        write_quad(
            rdram, blade_address,
            center_x, center_z, first_x, first_z, lean_x, lean_z, height
        );
        write_quad(
            rdram, blade_address + 4u * kVertexBytes,
            center_x, center_z, second_x, second_z, lean_x, lean_z, height
        );
    }
}

uint32_t write_triangle_batch(uint8_t* rdram, uint32_t cursor) {
    for (uint32_t blade = 0; blade < 4u; ++blade) {
        const uint32_t vertex = blade * static_cast<uint32_t>(kVerticesPerBlade);
        write_command(
            rdram, cursor,
            0x06000000u | triangle_word(vertex, vertex + 1u, vertex + 2u),
            triangle_word(vertex, vertex + 2u, vertex + 3u)
        );
        cursor += static_cast<uint32_t>(kCommandBytes);
        write_command(
            rdram, cursor,
            0x06000000u |
                triangle_word(vertex + 4u, vertex + 5u, vertex + 6u),
            triangle_word(vertex + 4u, vertex + 6u, vertex + 7u)
        );
        cursor += static_cast<uint32_t>(kCommandBytes);
    }
    return cursor;
}

void build_variant_display_lists(
    uint8_t* rdram,
    uint32_t vertex_base,
    uint32_t low_display_list,
    uint32_t high_display_list
) {
    constexpr uint32_t kVertexCommand32 = 0x01020040u;
    auto write_batches = [&](uint32_t cursor, size_t blade_count) {
        for (size_t first_blade = 0u;
             first_blade < blade_count;
             first_blade += 4u) {
            write_command(
                rdram,
                cursor,
                kVertexCommand32,
                vertex_base + static_cast<uint32_t>(
                    first_blade * kVerticesPerBlade * kVertexBytes
                )
            );
            cursor += static_cast<uint32_t>(kCommandBytes);
            cursor = write_triangle_batch(rdram, cursor);
        }
        write_command(rdram, cursor, 0xDF000000u, 0u);
    };
    write_batches(low_display_list, kBladesPerLowCluster);
    write_batches(high_display_list, kBladesPerHighCluster);
}

bool ensure_assets(uint8_t* rdram) {
    if (g_assets.rdram == rdram && g_assets.texture != 0u) {
        return true;
    }
    g_assets = {};

    constexpr size_t kVertexOffset = align_up(kTextureBytes, 16u);
    constexpr size_t kDisplayListOffset = align_up(
        kVertexOffset + kVariantCount * kVariantVertexBytes,
        16u
    );
    constexpr size_t kAllocationBytes = align_up(
        kDisplayListOffset +
            kVariantCount * kVariantDisplayListStride,
        16u
    );
    auto* allocation = static_cast<uint8_t*>(
        recomp::alloc(rdram, kAllocationBytes)
    );
    if (allocation == nullptr) {
        bool expected = false;
        if (g_allocation_failure_logged.compare_exchange_strong(expected, true)) {
            std::fprintf(
                stderr,
                "BUMBLE_GRASS stage=asset_allocation_failed bytes=%zu\n",
                kAllocationBytes
            );
            std::fflush(stderr);
        }
        return false;
    }

    const uint64_t host_offset = static_cast<uint64_t>(allocation - rdram);
    if (host_offset + kAllocationBytes > recomp::mem_size ||
        host_offset > UINT32_MAX) {
        return false;
    }
    const uint32_t allocation_guest =
        0x80000000u + static_cast<uint32_t>(host_offset);
    g_assets.rdram = rdram;
    g_assets.texture = allocation_guest;

    for (size_t y = 0; y < kTextureHeight; ++y) {
        const int32_t center_twice = 15;
        const int32_t width_twice = 2 + static_cast<int32_t>(y * 8u / 15u);
        for (size_t x = 0; x < kTextureWidth; ++x) {
            const int32_t distance_twice = std::abs(
                static_cast<int32_t>(x * 2u) - center_twice
            );
            uint8_t alpha = debug_visualization_enabled() ? 0x0Fu : 0u;
            if (!debug_visualization_enabled() &&
                distance_twice <= width_twice) {
                alpha = 0x0Fu;
            }
            else if (!debug_visualization_enabled() &&
                distance_twice == width_twice + 1) {
                alpha = 0x07u;
            }
            const uint8_t intensity = static_cast<uint8_t>(
                0x0Bu + ((x + y) & 0x03u)
            );
            write_u8(
                rdram,
                g_assets.texture + static_cast<uint32_t>(y * kTextureWidth + x),
                static_cast<uint8_t>((intensity << 4) | alpha)
            );
        }
    }

    for (size_t variant = 0; variant < kVariantCount; ++variant) {
        const uint32_t vertex_base = allocation_guest +
            static_cast<uint32_t>(
                kVertexOffset + variant * kVariantVertexBytes
            );
        const uint32_t display_list_base = allocation_guest +
            static_cast<uint32_t>(
                kDisplayListOffset + variant * kVariantDisplayListStride
            );
        g_assets.low_display_lists[variant] = display_list_base;
        g_assets.high_display_lists[variant] =
            display_list_base + static_cast<uint32_t>(kLowDisplayListBytes);
        build_variant_vertices(rdram, vertex_base, variant);
        build_variant_display_lists(
            rdram,
            vertex_base,
            g_assets.low_display_lists[variant],
            g_assets.high_display_lists[variant]
        );
    }

    std::fprintf(
        stderr,
        "BUMBLE_GRASS stage=assets_ready bytes=%zu texture=0x%08" PRIX32
        " variants=%zu low_blades=%zu high_blades=%zu local_base_y=%d"
        " source=procedural_ia8 debug_visual=%d\n",
        kAllocationBytes,
        g_assets.texture,
        kVariantCount,
        kBladesPerLowCluster,
        kBladesPerHighCluster,
        kBladeBaseHeight,
        debug_visualization_enabled() ? 1 : 0
    );
    std::fflush(stderr);
    return true;
}

bool single_player_world(uint8_t* rdram) {
    return rdram_address(kFrontendState, 0x28u) &&
        read_u32(rdram, kFrontendState + kFrontendWorldActiveOffset) != 0u &&
        read_u32(rdram, kFrontendState + kFrontendPlayerLayoutOffset) ==
            kSinglePlayerLayout &&
        rdram_address(kCurrentLevelIndexAddress, 4u) &&
        read_u32(rdram, kCurrentLevelIndexAddress) == kMissionOneLevelIndex;
}

uint32_t extended_command(uint32_t operation);


#if defined(BUMBLE_GPU_GRASS_RT64) && BUMBLE_GPU_GRASS_RT64
thread_local std::array<RT64::BumbleGrassPosition, 2> g_previous_players{};
thread_local uint32_t g_previous_player_mask = 0;
thread_local uint32_t g_previous_grounded_player_mask = 0;

RT64::BumbleGrassPosition player_foot_position(
    uint8_t* rdram,
    uint32_t actor
) {
    return {
        read_f32(rdram, actor + 0x40u) +
            kPlayerFootProbeOffsetY * read_f32(rdram, actor + 0x20u),
        read_f32(rdram, actor + 0x44u) +
            kPlayerFootProbeOffsetY * read_f32(rdram, actor + 0x24u),
        read_f32(rdram, actor + 0x48u) +
            kPlayerFootProbeOffsetY * read_f32(rdram, actor + 0x28u),
    };
}

uint32_t capture_players(
    uint8_t* rdram,
    RT64::BumbleGrassSnapshot& snapshot
) {
    constexpr std::array<uint32_t, 2> kOwnerSlots{
        kPlayerOneOwnerSlot,
        kPlayerTwoOwnerSlot,
    };
    uint32_t mask = 0;
    uint32_t grounded_mask = 0;
    for (uint32_t player = 0; player < kOwnerSlots.size(); ++player) {
        if (!rdram_address(kOwnerSlots[player], 4u)) {
            continue;
        }
        const uint32_t actor = read_u32(rdram, kOwnerSlots[player]);
        if (!rdram_address(actor, 0x90u) ||
            read_u32(rdram, actor + 0x88u) != kPlayerVtable) {
            continue;
        }

        const uint32_t state = read_u32(rdram, actor + 0x8Cu);
        const bool grounded = state >= 6u && state <= 8u;

        const RT64::BumbleGrassPosition current =
            player_foot_position(rdram, actor);
        if (!std::isfinite(current.x) || !std::isfinite(current.y) ||
            !std::isfinite(current.z)) {
            continue;
        }

        RT64::BumbleGrassPosition previous = current;
        if ((g_previous_player_mask & (1u << player)) != 0) {
            previous = g_previous_players[player];
            const float dx = current.x - previous.x;
            const float dy = current.y - previous.y;
            const float dz = current.z - previous.z;
            if (dx * dx + dy * dy + dz * dz > 400.0f * 400.0f) {
                previous = current;
            }
        }
        if (grounded &&
            (g_previous_grounded_player_mask & (1u << player)) == 0) {
            previous = current;
        }

        snapshot.playerCurrent[player] = current;
        snapshot.playerPrevious[player] = previous;
        g_previous_players[player] = current;
        mask |= 1u << player;
        if (grounded) {
            grounded_mask |= 1u << player;
        }
    }
    g_previous_player_mask = mask;
    g_previous_grounded_player_mask = grounded_mask;
    snapshot.playerMask = mask;
    snapshot.playerGroundedMask = grounded_mask;
    return mask;
}

bool publish_gpu_grass_marker(
    uint8_t* rdram,
    const TerrainPass& pass,
    Mode active_mode
) {
    if (!rdram_address(kDisplayListCursor, 4u) ||
        !rdram_address(kFrameMatrixBase, 4u)) {
        return false;
    }
    uint32_t arena_start = bumble::widescreen::display_list_arena_base();
    uint32_t frame_end = bumble::widescreen::display_list_arena_end();
    if (arena_start == 0u || frame_end <= arena_start) {
        uint32_t frame_base = bumble::widescreen::display_list_frame_base();
        if (frame_base == 0u) {
        const uint32_t matrix_base = read_u32(rdram, kFrameMatrixBase);
        if (matrix_base < 0x80000000u + kFrameBaseFromMatrixBase) {
            return false;
        }
        frame_base = matrix_base - kFrameBaseFromMatrixBase;
        }
        arena_start = frame_base + kDisplayListArenaOffset;
        frame_end = frame_base + kDisplayListArenaEndOffset;
    }
    uint32_t cursor = read_u32(rdram, kDisplayListCursor);
    if (!rdram_address(arena_start, frame_end - arena_start) ||
        cursor < arena_start ||
        cursor > frame_end ||
        frame_end - cursor <= kDownstreamDisplayListReserve + kCommandBytes) {
        return false;
    }

    RT64::BumbleGrassSnapshot snapshot{};
    snapshot.mode = static_cast<uint32_t>(active_mode);
    snapshot.camera = {pass.camera_x, pass.camera_y, pass.camera_z};
    snapshot.patchCount = static_cast<uint32_t>(std::min(
        pass.count,
        static_cast<size_t>(RT64::BumbleGrassMaxPatches)
    ));
    for (uint32_t index = 0; index < snapshot.patchCount; ++index) {
        const Candidate& candidate = pass.candidates[index];
        snapshot.patches[index] = {
            {candidate.world_x, candidate.world_y, candidate.world_z},
            candidate.placement_hash,
        };
    }
    capture_players(rdram, snapshot);

    const uint32_t ticket = RT64::publishBumbleGrassSnapshot(snapshot);
    write_command(
        rdram,
        cursor,
        extended_command(kBumbleGrassMarker),
        ticket
    );
    cursor += static_cast<uint32_t>(kCommandBytes);
    write_u32(rdram, kDisplayListCursor, cursor);
    g_extended_gbi_required.store(true, std::memory_order_release);

    const uint64_t marker =
        g_gpu_markers.fetch_add(1, std::memory_order_relaxed) + 1;
    if (marker == 1 || marker == 60) {
        std::fprintf(
            stderr,
            "BUMBLE_GRASS stage=gpu_marker_published marker=%" PRIu64
            " ticket=%" PRIu32 " mode=%s patches=%" PRIu32
            " players=0x%X cursor=0x%08" PRIX32
            " camera_source=%s camera=%.3f,%.3f,%.3f legacy_cards=%d\n",
            marker,
            ticket,
            active_mode == Mode::High ? "high" : "low",
            snapshot.patchCount,
            snapshot.playerMask,
            cursor,
            pass.camera_source == 1u
                ? "visibility_packet"
                : pass.camera_source == 2u ? "player" : "stack_fallback",
            pass.camera_x,
            pass.camera_y,
            pass.camera_z,
            debug_visualization_enabled() ? 1 : 0
        );
        std::fflush(stderr);
    }
    return true;
}
#endif

uint32_t extended_command(uint32_t operation) {
    return (kExtendedOpcode << 24) | operation;
}


} // namespace

void bumble::procedural_grass::set_mode(Mode mode_value) {
    const uint32_t raw = static_cast<uint32_t>(mode_value);
    const uint32_t accepted = raw <= static_cast<uint32_t>(Mode::High)
        ? raw
        : static_cast<uint32_t>(Mode::Off);
    g_mode.store(accepted, std::memory_order_release);
    const uint32_t previous =
        g_last_logged_mode.exchange(accepted, std::memory_order_acq_rel);
    if (previous != accepted) {
        const char* name = accepted == static_cast<uint32_t>(Mode::High)
            ? "high"
            : accepted == static_cast<uint32_t>(Mode::Low) ? "low" : "off";
        std::fprintf(
            stderr,
            "BUMBLE_GRASS stage=mode_configured mode=%s gpu_enabled=%d"
            " legacy_cards=%d off_instances=%d off_draws=%d\n",
            name,
#if defined(BUMBLE_GPU_GRASS_RT64) && BUMBLE_GPU_GRASS_RT64
            accepted != static_cast<uint32_t>(Mode::Off) ? 1 : 0,
#else
            0,
#endif
            debug_visualization_enabled() ? 1 : 0,
            accepted == static_cast<uint32_t>(Mode::Off) ? 0 : -1,
            accepted == static_cast<uint32_t>(Mode::Off) ? 0 : -1
        );
        std::fflush(stderr);
    }
}

Mode bumble::procedural_grass::mode() {
    return static_cast<Mode>(g_mode.load(std::memory_order_acquire));
}

bool bumble::procedural_grass::enabled() {
    return mode() != Mode::Off;
}

bool bumble::procedural_grass::extended_gbi_required() {
    return g_extended_gbi_required.load(std::memory_order_acquire);
}


extern "C" void bumble_collect_procedural_grass_cell(
    uint8_t* rdram,
    recomp_context* context
) {
    const Mode active_mode = bumble::procedural_grass::mode();
    if (active_mode == Mode::Off || rdram == nullptr || context == nullptr) {
        g_terrain_pass = {};
        return;
    }

    const uint32_t grid_record = low_guest_address(context->r8);
    const uint32_t matrix = low_guest_address(context->r6);
    const uint32_t stack = low_guest_address(context->r29);
    if (!rdram_address(grid_record, 8u) ||
        !rdram_address(matrix, 0x40u) ||
        !rdram_address(stack + 0xD8u, 4u)) {
        return;
    }
    const uint16_t base_id = read_u16(rdram, grid_record);
    if (base_id != static_cast<uint16_t>(context->r2)) {
        return;
    }
    g_terrain_pass.original_last_matrix = matrix;

    const bool unsafe_discovery = unsafe_material_discovery_enabled();
    bool expected_ground_log = false;
    if (g_ground_contract_logged.compare_exchange_strong(
            expected_ground_log,
            true,
            std::memory_order_acq_rel)) {
        std::fprintf(
            stderr,
            "BUMBLE_GRASS stage=ground_base_contract mission=1"
            " hook=0x80085B84 approved_base_ids=%zu flat_id=0x%04X"
            " unsafe_discovery=%d local_surface_y=%d\n",
            kMissionOneFlatGroundBaseIds.size(),
            kMissionOneFlatGroundBaseIds.front(),
            unsafe_discovery ? 1 : 0,
            kBladeBaseHeight
        );
        std::fflush(stderr);
    }
    if (!unsafe_discovery && !mission_one_flat_ground(base_id)) {
        ++g_terrain_pass.ground_whitelist_rejections;
        return;
    }

    const int32_t world_x = read_s16(rdram, matrix + 0x18u);
    const int32_t world_y = read_s16(rdram, matrix + 0x1Au);
    const int32_t world_z = read_s16(rdram, matrix + 0x1Cu);
    float eye_x = read_f32(rdram, stack + 0xD0u);
    float eye_y = read_f32(rdram, stack + 0xD4u);
    float eye_z = read_f32(rdram, stack + 0xD8u);
    if (!std::isfinite(eye_x) || !std::isfinite(eye_y) ||
        !std::isfinite(eye_z)) {
        return;
    }
    uint32_t camera_source = 0;
    const auto visibility_camera =
        bumble::widescreen::visibility_camera_xz();
    if (visibility_camera.valid &&
        std::isfinite(visibility_camera.eye_x) &&
        std::isfinite(visibility_camera.eye_z)) {
        eye_x = visibility_camera.eye_x;
        eye_z = visibility_camera.eye_z;
        camera_source = 1u;
    }

    if (rdram_address(kPlayerOneOwnerSlot, 4u)) {
        const uint32_t actor = read_u32(rdram, kPlayerOneOwnerSlot);
        if (rdram_address(actor, 0x8Cu) &&
            read_u32(rdram, actor + 0x88u) == kPlayerVtable) {
            const float player_x = read_f32(rdram, actor + 0x40u);
            const float player_y = read_f32(rdram, actor + 0x44u);
            const float player_z = read_f32(rdram, actor + 0x48u);
            if (std::isfinite(player_x) && std::isfinite(player_y) &&
                std::isfinite(player_z)) {
                eye_y = player_y;
                if (camera_source == 0u) {
                    eye_x = player_x;
                    eye_z = player_z;
                    camera_source = 2u;
                }
            }
        }
    }

    g_terrain_pass.camera_x = eye_x;
    g_terrain_pass.camera_y = eye_y;
    g_terrain_pass.camera_z = eye_z;
    g_terrain_pass.camera_source = camera_source;
    const float delta_x = static_cast<float>(world_x) - eye_x;
    const float delta_z = static_cast<float>(world_z) - eye_z;
    const float distance_squared = delta_x * delta_x + delta_z * delta_z;
    const float distance_limit = active_mode == Mode::High
        ? kHighGpuCollectDistance
        : kLowGpuCollectDistance;
    if (distance_squared > distance_limit * distance_limit) {
        ++g_terrain_pass.distance_rejections;
        return;
    }

    const uint32_t placement_hash = mix32(
        static_cast<uint32_t>(base_id) ^
        static_cast<uint32_t>(world_x) ^
        std::rotl(static_cast<uint32_t>(world_z), 16)
    );
    retain_nearest_candidate({
        matrix,
        static_cast<uint32_t>(base_id),
        placement_hash,
        distance_squared,
        static_cast<float>(world_x),
        static_cast<float>(world_y + kBladeBaseHeight),
        static_cast<float>(world_z),
    });
}

extern "C" void bumble_emit_procedural_grass_pass(
    uint8_t* rdram,
    recomp_context* context
) {
    bumble::collision_debug::emit_frame(rdram, context);
    (void)context;
    TerrainPass pass = g_terrain_pass;
    g_terrain_pass = {};

    const Mode active_mode = bumble::procedural_grass::mode();
    const bool world_active =
        rdram != nullptr && single_player_world(rdram);
    if (active_mode == Mode::Off || pass.count == 0u || rdram == nullptr ||
        !world_active) {
#if defined(BUMBLE_GPU_GRASS_RT64) && BUMBLE_GPU_GRASS_RT64
        g_previous_player_mask = 0;
        g_previous_grounded_player_mask = 0;
#endif
        if (active_mode != Mode::Off) {
            const uint64_t skip =
                g_emit_skips.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (skip <= 8u || skip == 60u || (skip % 600u) == 0u) {
                const char* reason = rdram == nullptr
                    ? "null_rdram"
                    : pass.count == 0u
                        ? "collector_empty"
                        : "world_inactive";
                std::fprintf(
                    stderr,
                    "BUMBLE_GRASS stage=emit_skipped skip=%" PRIu64
                    " reason=%s mode=%s pass_count=%zu"
                    " world_active=%d translucent_reject=%" PRIu32
                    " ground_reject=%" PRIu32
                    " distance_reject=%" PRIu32 "\n",
                    skip,
                    reason,
                    active_mode == Mode::High ? "high" : "low",
                    pass.count,
                    world_active ? 1 : 0,
                    pass.translucent_rejections,
                    pass.ground_whitelist_rejections,
                    pass.distance_rejections
                );
                std::fflush(stderr);
            }
        }
        return;
    }

    std::sort(
        pass.candidates.begin(),
        pass.candidates.begin() + static_cast<std::ptrdiff_t>(pass.count),
        [](const Candidate& left, const Candidate& right) {
            if (left.distance_squared != right.distance_squared) {
                return left.distance_squared < right.distance_squared;
            }
            return left.placement_hash < right.placement_hash;
        }
    );

#if defined(BUMBLE_GPU_GRASS_RT64) && BUMBLE_GPU_GRASS_RT64
    if (!publish_gpu_grass_marker(rdram, pass, active_mode)) {
        const uint64_t failure = g_gpu_marker_failures.fetch_add(
            1,
            std::memory_order_relaxed
        ) + 1u;
        if (failure <= 8u || failure == 60u || (failure % 600u) == 0u) {
            const uint32_t cursor = rdram_address(kDisplayListCursor, 4u)
                ? read_u32(rdram, kDisplayListCursor)
                : 0u;
            const uint32_t matrix_base = rdram_address(kFrameMatrixBase, 4u)
                ? read_u32(rdram, kFrameMatrixBase)
                : 0u;
            std::fprintf(
                stderr,
                "BUMBLE_GRASS stage=gpu_marker_publish_failed"
                " failure=%" PRIu64 " mode=%s pass_count=%zu"
                " cursor=0x%08" PRIX32
                " matrix_base=0x%08" PRIX32
                " reserve=0x%" PRIX32 "\n",
                failure,
                active_mode == Mode::High ? "high" : "low",
                pass.count,
                cursor,
                matrix_base,
                kDownstreamDisplayListReserve
            );
            std::fflush(stderr);
        }
    }
#endif

    if (!debug_visualization_enabled() || !ensure_assets(rdram)) {
        return;
    }

    if (!rdram_address(kDisplayListCursor, 4u) ||
        !rdram_address(kFrameMatrixBase, 4u)) {
        return;
    }
    const uint32_t matrix_base = read_u32(rdram, kFrameMatrixBase);
    uint32_t arena_start = bumble::widescreen::display_list_arena_base();
    uint32_t frame_end = bumble::widescreen::display_list_arena_end();
    if (arena_start == 0u || frame_end <= arena_start) {
        if (matrix_base < 0x80000000u + kFrameBaseFromMatrixBase) {
            return;
        }
        const uint32_t frame_base = matrix_base - kFrameBaseFromMatrixBase;
        arena_start = frame_base + kDisplayListArenaOffset;
        frame_end = frame_base + kDisplayListArenaEndOffset;
    }
    uint32_t cursor = read_u32(rdram, kDisplayListCursor);
    if (!rdram_address(arena_start, frame_end - arena_start) ||
        cursor < arena_start || cursor > frame_end ||
        frame_end - cursor <= kDownstreamDisplayListReserve) {
        return;
    }

    constexpr size_t kFixedCommands = 26u;
    const size_t available_commands = static_cast<size_t>(
        (frame_end - cursor - kDownstreamDisplayListReserve) /
        static_cast<uint32_t>(kCommandBytes)
    );
    if (available_commands <= kFixedCommands) {
        return;
    }
    const size_t mode_budget = active_mode == Mode::High
        ? kHighCellBudget
        : kLowCellBudget;
    const size_t cell_budget = std::min({
        pass.count,
        mode_budget,
        (available_commands - kFixedCommands) / 2u,
    });
    if (cell_budget == 0u) {
        return;
    }

    auto append = [&](uint32_t word0, uint32_t word1) {
        write_command(rdram, cursor, word0, word1);
        cursor += static_cast<uint32_t>(kCommandBytes);
    };

    append(extended_command(kSetRdramExtended), 1u);
    append(extended_command(kPushOtherMode), 0u);
    append(extended_command(kPushCombine), 0u);
    append(extended_command(kPushPrimColor), 0u);
    append(extended_command(kPushGeometryMode), 0u);
    append(0xE7000000u, 0u); // gDPPipeSync
    append(
        0xD9000000u | ((~kGeometryClear) & 0x00FFFFFFu),
        kGeometrySet
    );
    append(0xFD68000Fu, g_assets.texture); // IA8 16x16 source
    append(0xF5680000u, 0x07000000u); // load tile
    append(0xE6000000u, 0u); // gDPLoadSync
    append(0xF3000000u, 0x070FF400u); // 256 IA8 texels
    append(0xE7000000u, 0u);
    append(0xF5680400u, 0x00090240u); // render tile, clamp 16x16
    append(0xF2000000u, 0x0003C03Cu);
    append(0xD7000002u, 0xFFFFFFFFu); // texture on, full scale
    append(kGrassCombineWord0, kGrassCombineWord1);
    append(0xE200001Cu, kGrassRenderMode);
    append(
        0xFA000000u,
        debug_visualization_enabled() ? 0xFF00FFFFu : kGrassPrimitiveColor
    );

    const float high_detail_squared =
        kHighDetailDistance * kHighDetailDistance;
    size_t high_detail_cells = 0;
    for (size_t index = 0; index < cell_budget; ++index) {
        const Candidate& candidate = pass.candidates[index];
        const size_t variant = static_cast<size_t>(
            candidate.placement_hash % static_cast<uint32_t>(kVariantCount)
        );
        const bool high_detail = active_mode == Mode::High &&
            candidate.distance_squared <= high_detail_squared;
        if (high_detail) {
            ++high_detail_cells;
        }
        append(0xDA380003u, candidate.matrix);
        append(
            0xDE000000u,
            high_detail
                ? g_assets.high_display_lists[variant]
                : g_assets.low_display_lists[variant]
        );
    }

    append(0xDA380003u, pass.original_last_matrix);
    append(0xD7000000u, 0u);
    append(0xE7000000u, 0u);
    append(extended_command(kPopGeometryMode), 0u);
    append(extended_command(kPopPrimColor), 0u);
    append(extended_command(kPopCombine), 0u);
    append(extended_command(kPopOtherMode), 0u);
    append(extended_command(kSetRdramExtended), 0u);

    write_u32(rdram, kDisplayListCursor, cursor);
    g_extended_gbi_required.store(true, std::memory_order_release);
    const uint64_t emitted =
        g_emitted_passes.fetch_add(1, std::memory_order_acq_rel) + 1u;
    if (emitted == 1u || emitted == 60u) {
        std::fprintf(
            stderr,
            "BUMBLE_GRASS stage=pass_emitted pass=%" PRIu64
            " mode=%s cells=%zu high_detail=%zu material_reject=%" PRIu32
            " ground_whitelist_reject=%" PRIu32
            " distance_reject=%" PRIu32 " cursor=0x%08" PRIX32
            " unsafe_discovery=%d\n",
            emitted,
            active_mode == Mode::High ? "high" : "low",
            cell_budget,
            high_detail_cells,
            pass.translucent_rejections,
            pass.ground_whitelist_rejections,
            pass.distance_rejections,
            cursor,
            unsafe_material_discovery_enabled() ? 1 : 0
        );
        for (size_t index = 0;
             index < pass.material_observation_count;
             ++index) {
            const MaterialObservation& observation =
                pass.material_observations[index];
            std::fprintf(
                stderr,
                "BUMBLE_GRASS stage=material_observed pass=%" PRIu64
                " first=0x%08" PRIX32 " second=0x%08" PRIX32
                " key=0x%08" PRIX32 " cells=%" PRIu32
                " sample_world_x=%" PRId32 " sample_world_z=%" PRId32
                " nearest_distance=%.3f\n",
                emitted,
                observation.first_descriptor,
                observation.second_descriptor,
                observation.material_key,
                observation.count,
                observation.sample_world_x,
                observation.sample_world_z,
                std::sqrt(observation.nearest_distance_squared)
            );
            DescriptorGeometryScan geometry = scan_descriptor_geometry(
                rdram,
                observation.first_descriptor != 0u
                    ? observation.first_descriptor
                    : observation.second_descriptor
            );
            std::fprintf(
                stderr,
                "BUMBLE_GRASS stage=descriptor_geometry pass=%" PRIu64
                " descriptor=0x%08" PRIX32
                " vertex_loads=%" PRIu32 " vertices=%" PRIu32
                " min_y=%" PRId32 " max_y=%" PRId32
                " mean_y=%.3f unresolved=%" PRIu32 "\n",
                emitted,
                observation.first_descriptor != 0u
                    ? observation.first_descriptor
                    : observation.second_descriptor,
                geometry.vertex_loads,
                geometry.vertex_count,
                geometry.vertex_count != 0u ? geometry.minimum_y : 0,
                geometry.vertex_count != 0u ? geometry.maximum_y : 0,
                geometry.vertex_count != 0u
                    ? static_cast<double>(geometry.sum_y) /
                        static_cast<double>(geometry.vertex_count)
                    : 0.0,
                geometry.unresolved_addresses
            );
        }
        std::fflush(stderr);
    }
}
