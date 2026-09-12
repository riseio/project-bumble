#include "native_modern_sky.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "librecomp/addresses.hpp"
#include "native_campaign_levels.hpp"
#include "native_widescreen.hpp"

#if defined(BUMBLE_MODERN_SKY_RT64) && BUMBLE_MODERN_SKY_RT64
#   include "render/rt64_bumble_sky.h"
#endif

namespace {

constexpr uint32_t kDisplayListCursor = 0x80035F30u;
constexpr uint32_t kFrameMatrixBase = 0x80105224u;
constexpr uint32_t kFrameBaseFromMatrixBase = 0x68u;
constexpr uint32_t kDisplayListArenaOffset = 0xB068u;
constexpr uint32_t kDisplayListArenaEndOffset = 0x14CA8u;
constexpr uint32_t kCommandBytes = 8u;
constexpr uint32_t kExtendedOpcode = 0x64u;
constexpr uint32_t kBumbleSkyMarker = 0x35u;

constexpr uint32_t kFrontendState = 0x800FFF80u;
constexpr uint32_t kFrontendWorldActiveOffset = 0x24u;
constexpr uint32_t kFrontendPlayerLayoutOffset = 0x0Cu;
constexpr uint32_t kSinglePlayerLayout = 1u;
constexpr uint32_t kCurrentLevelIndexAddress = 0x800E9640u;
constexpr uint32_t kLevelBackgroundRgb = 0x800E952Cu;
constexpr uint32_t kSinglePlayerCamera = 0x800E9254u;
constexpr uint32_t kPlayerOneOwnerSlot = 0x800E91D4u;
constexpr uint32_t kPlayerVtable = 0x800445E8u;
constexpr float kPlayerCeilingY = 800.0f;

enum class ReviewedSkyPreset : uint32_t {
    None = 0u,
    GardenCloudyDay = 1u,
    TrenchIndustrialDarkOvercast = 2u,
};

constexpr std::array<uint32_t, 6> kGardenCloudyDayIndices{
    1u, 2u, 3u, 4u, 5u, 6u,
};
constexpr std::array<uint32_t, 9> kTrenchIndustrialOvercastIndices{
    7u, 9u, 10u, 11u, 12u, 13u, 15u, 16u, 18u,
};
constexpr std::array<uint32_t, 9> kFailClosedCampaignIndices{
    8u, 14u, 17u, 19u, 20u, 21u, 22u, 23u, 25u,
};

template <size_t Size>
constexpr bool contains_index(
    const std::array<uint32_t, Size>& indices,
    uint32_t level_index
) {
    for (const uint32_t index : indices) {
        if (index == level_index) {
            return true;
        }
    }
    return false;
}

constexpr ReviewedSkyPreset reviewed_sky_preset(uint32_t level_index) {
    if (contains_index(kGardenCloudyDayIndices, level_index)) {
        return ReviewedSkyPreset::GardenCloudyDay;
    }
    if (contains_index(kTrenchIndustrialOvercastIndices, level_index)) {
        return ReviewedSkyPreset::TrenchIndustrialDarkOvercast;
    }
    return ReviewedSkyPreset::None;
}

constexpr bool reviewed_campaign_partition_is_exact() {
    if (kGardenCloudyDayIndices.size() +
            kTrenchIndustrialOvercastIndices.size() +
            kFailClosedCampaignIndices.size() !=
        bumble::campaign_levels::kRecords.size()) {
        return false;
    }

    for (const bumble::campaign_levels::Record& record :
         bumble::campaign_levels::kRecords) {
        const uint32_t index = record.level_index;
        const uint32_t memberships =
            (contains_index(kGardenCloudyDayIndices, index) ? 1u : 0u) +
            (contains_index(kTrenchIndustrialOvercastIndices, index)
                ? 1u : 0u) +
            (contains_index(kFailClosedCampaignIndices, index) ? 1u : 0u);
        if (memberships != 1u) {
            return false;
        }
    }
    return true;
}

static_assert(reviewed_campaign_partition_is_exact());

std::atomic_bool g_enabled{false};
std::atomic_bool g_extended_gbi_required{false};
std::atomic_uint64_t g_markers{0};
std::atomic_uint64_t g_skips{0};

gpr guest_address(uint32_t address) {
    return static_cast<gpr>(static_cast<int32_t>(address));
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
    (void)rdram;
    return static_cast<uint32_t>(MEM_W(0, guest_address(address)));
}

float read_f32(uint8_t* rdram, uint32_t address) {
    return std::bit_cast<float>(read_u32(rdram, address));
}

void write_u32(uint8_t* rdram, uint32_t address, uint32_t value) {
    (void)rdram;
    MEM_W(0, guest_address(address)) = static_cast<int32_t>(value);
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

uint32_t extended_command(uint32_t operation) {
    return (kExtendedOpcode << 24) | operation;
}

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vec3 subtract(const Vec3& left, const Vec3& right) {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

Vec3 cross(const Vec3& left, const Vec3& right) {
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

bool normalize(Vec3& value) {
    const float length_squared =
        value.x * value.x + value.y * value.y + value.z * value.z;
    if (!std::isfinite(length_squared) || length_squared < 1.0e-8f) {
        return false;
    }
    const float reciprocal = 1.0f / std::sqrt(length_squared);
    value.x *= reciprocal;
    value.y *= reciprocal;
    value.z *= reciprocal;
    return std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.z);
}

bool full_campaign_record_matches(
    uint8_t* rdram,
    const bumble::campaign_levels::Record& expected
) {
    (void)rdram;
    const uint32_t record_address =
        bumble::campaign_levels::record_address(expected.level_index);
    if (!rdram_address(
            record_address,
            bumble::campaign_levels::kRecordSize
        ) ||
        read_u32(rdram, record_address + 0x14u) != expected.load_offset ||
        read_u32(rdram, record_address + 0x18u) !=
            expected.record_word_18 ||
        read_u32(rdram, record_address + 0x1Cu) !=
            expected.record_word_1c ||
        read_u32(rdram, record_address + 0x20u) !=
            expected.record_word_20) {
        return false;
    }

    for (uint32_t index = 0u;
         index < bumble::campaign_levels::kIdentifierSize;
         ++index) {
        const uint8_t expected_byte = index < expected.identifier.size()
            ? static_cast<uint8_t>(expected.identifier[index])
            : 0u;
        if (MEM_BU(index, guest_address(record_address)) != expected_byte) {
            return false;
        }
    }
    return true;
}

bool reviewed_outdoor_world(
    uint8_t* rdram,
    const bumble::campaign_levels::Record*& record,
    ReviewedSkyPreset& preset
) {
    if (!rdram_address(kFrontendState, 0x28u) ||
        read_u32(rdram, kFrontendState + kFrontendWorldActiveOffset) == 0u ||
        read_u32(rdram, kFrontendState + kFrontendPlayerLayoutOffset) !=
            kSinglePlayerLayout ||
        !rdram_address(kCurrentLevelIndexAddress, 4u)) {
        return false;
    }

    const uint32_t level_index =
        read_u32(rdram, kCurrentLevelIndexAddress);
    const bumble::campaign_levels::Record* expected =
        bumble::campaign_levels::find(level_index);
    const ReviewedSkyPreset expected_preset =
        reviewed_sky_preset(level_index);
    if (expected == nullptr ||
        !full_campaign_record_matches(rdram, *expected)) {
        return false;
    }

    record = expected;
    preset = expected_preset;
    return true;
}

const char* preset_name(ReviewedSkyPreset preset) {
    switch (preset) {
    case ReviewedSkyPreset::GardenCloudyDay:
        return "cloudy_day";
    case ReviewedSkyPreset::TrenchIndustrialDarkOvercast:
        return "dark_overcast";
    case ReviewedSkyPreset::None:
    default:
        return "none";
    }
}

bool read_camera_basis(
    uint8_t* rdram,
    Vec3& eye,
    Vec3& forward,
    Vec3& right,
    Vec3& up
) {
    if (!rdram_address(kSinglePlayerCamera, 0x24u)) {
        return false;
    }

    eye = {
        read_f32(rdram, kSinglePlayerCamera + 0x00u),
        read_f32(rdram, kSinglePlayerCamera + 0x04u),
        read_f32(rdram, kSinglePlayerCamera + 0x08u),
    };
    const Vec3 target{
        read_f32(rdram, kSinglePlayerCamera + 0x0Cu),
        read_f32(rdram, kSinglePlayerCamera + 0x10u),
        read_f32(rdram, kSinglePlayerCamera + 0x14u),
    };
    const Vec3 authored_up{
        read_f32(rdram, kSinglePlayerCamera + 0x18u),
        read_f32(rdram, kSinglePlayerCamera + 0x1Cu),
        read_f32(rdram, kSinglePlayerCamera + 0x20u),
    };
    const bool finite =
        std::isfinite(eye.x) && std::isfinite(eye.y) &&
        std::isfinite(eye.z) && std::isfinite(target.x) &&
        std::isfinite(target.y) && std::isfinite(target.z) &&
        std::isfinite(authored_up.x) && std::isfinite(authored_up.y) &&
        std::isfinite(authored_up.z);
    if (!finite) {
        return false;
    }

    forward = subtract(target, eye);
    if (!normalize(forward)) {
        return false;
    }
    right = cross(forward, authored_up);
    if (!normalize(right)) {
        return false;
    }
    up = cross(right, forward);
    return normalize(up);
}

bool read_player_one_y(uint8_t* rdram, float& player_y) {
    if (!rdram_address(kPlayerOneOwnerSlot, 4u)) {
        return false;
    }
    const uint32_t actor = read_u32(rdram, kPlayerOneOwnerSlot);
    if (actor == 0u || !rdram_address(actor, 0x8Cu) ||
        read_u32(rdram, actor + 0x88u) != kPlayerVtable) {
        return false;
    }
    player_y = read_f32(rdram, actor + 0x44u);
    return std::isfinite(player_y);
}

bool append_marker(uint8_t* rdram, uint32_t ticket, uint32_t& new_cursor) {
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
    const uint32_t cursor = read_u32(rdram, kDisplayListCursor);
    if (!rdram_address(arena_start, frame_end - arena_start) ||
        cursor < arena_start || cursor > frame_end ||
        frame_end - cursor < kCommandBytes) {
        return false;
    }

    write_command(
        rdram,
        cursor,
        extended_command(kBumbleSkyMarker),
        ticket
    );
    new_cursor = cursor + kCommandBytes;
    write_u32(rdram, kDisplayListCursor, new_cursor);
    return true;
}

void log_skip(const char* reason) {
    const uint64_t skip = g_skips.fetch_add(1, std::memory_order_relaxed) + 1u;
    if (skip <= 4u || skip == 60u || (skip % 600u) == 0u) {
        std::fprintf(
            stderr,
            "BUMBLE_SKY stage=emit_skipped skip=%" PRIu64
            " reason=%s policy=reviewed_outdoor_presets_only\n",
            skip,
            reason
        );
        std::fflush(stderr);
    }
}

} // namespace

void bumble::modern_sky::set_enabled(bool enabled_value) {
    const bool previous = g_enabled.exchange(
        enabled_value,
        std::memory_order_acq_rel
    );
    if (previous != enabled_value) {
        std::fprintf(
            stderr,
            "BUMBLE_SKY stage=mode_configured enabled=%d"
            " asset=none model=direction_space_procedural"
            " reviewed_presets=garden_cloudy_day,trench_industrial_dark_overcast"
            " other_playable_worlds=boundary_only denied=cinematics"
            " identity_contract=full_0x24\n",
            enabled_value ? 1 : 0
        );
        std::fflush(stderr);
    }
}

bool bumble::modern_sky::enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

bool bumble::modern_sky::extended_gbi_required() {
    return g_extended_gbi_required.load(std::memory_order_acquire);
}

extern "C" void bumble_emit_modern_sky_pass(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!bumble::modern_sky::enabled()) {
        return;
    }
    if (rdram == nullptr || context == nullptr) {
        log_skip("invalid_context");
        return;
    }
    const bumble::campaign_levels::Record* campaign_record = nullptr;
    ReviewedSkyPreset reviewed_preset = ReviewedSkyPreset::None;
    if (!reviewed_outdoor_world(
            rdram,
            campaign_record,
            reviewed_preset
        )) {
        log_skip("unreviewed_or_inactive_level");
        return;
    }

    Vec3 eye{};
    Vec3 forward{};
    Vec3 right{};
    Vec3 up{};
    if (!read_camera_basis(rdram, eye, forward, right, up) ||
        !rdram_address(kLevelBackgroundRgb, 4u)) {
        log_skip("invalid_camera_or_level_color");
        return;
    }
    float player_y = eye.y;
    const bool player_y_owned = read_player_one_y(rdram, player_y);
    const bool boundary_only = reviewed_preset == ReviewedSkyPreset::None;
    if (boundary_only && (!player_y_owned ||
            campaign_record->level_index == 23u ||
            campaign_record->level_index == 25u)) {
        return;
    }

#if defined(BUMBLE_MODERN_SKY_RT64) && BUMBLE_MODERN_SKY_RT64
    static_assert(
        static_cast<uint32_t>(ReviewedSkyPreset::GardenCloudyDay) ==
        RT64::BumbleSkyPresetCloudyDay
    );
    static_assert(
        static_cast<uint32_t>(
            ReviewedSkyPreset::TrenchIndustrialDarkOvercast
        ) == RT64::BumbleSkyPresetDarkOvercast
    );
    RT64::BumbleSkySnapshot snapshot{};
    snapshot.mode = boundary_only ? RT64::BumbleSkyModeBoundaryOnly : 1u;
    snapshot.preset = static_cast<uint32_t>(reviewed_preset);
    snapshot.levelIndex = campaign_record->level_index;
    snapshot.levelIdWord = read_u32(
        rdram,
        bumble::campaign_levels::record_address(snapshot.levelIndex)
    );
    snapshot.horizonRgb = read_u32(rdram, kLevelBackgroundRgb);
    snapshot.fogScale = std::clamp(
        bumble::widescreen::fog_scale(),
        0.0f,
        1.0f
    );
    snapshot.playerY = player_y;
    snapshot.eye = {eye.x, eye.y, eye.z};
    snapshot.forward = {forward.x, forward.y, forward.z};
    snapshot.right = {right.x, right.y, right.z};
    snapshot.up = {up.x, up.y, up.z};

    const uint32_t ticket = RT64::publishBumbleSkySnapshot(snapshot);
    uint32_t cursor = 0u;
    if (!append_marker(rdram, ticket, cursor)) {
        log_skip("display_list_capacity");
        return;
    }
    g_extended_gbi_required.store(true, std::memory_order_release);
    const uint64_t marker =
        g_markers.fetch_add(1, std::memory_order_relaxed) + 1u;
    if (marker <= 4u || marker == 60u || (marker % 600u) == 0u) {
        std::fprintf(
            stderr,
            "BUMBLE_SKY stage=gpu_marker_published marker=%" PRIu64
            " ticket=%" PRIu32 " opcode=0x64 operation=0x35"
            " hook=0x8005508C level=%" PRIu32
            " preset=%s level_identifier=\"%.*s\""
            " level_rgb=0x%08" PRIX32
            " load_offset=0x%08" PRIX32
            " record_word_18=0x%08" PRIX32
            " record_word_1c=0x%08" PRIX32
            " record_word_20=0x%08" PRIX32
            " fog_scale=%.3f eye=%.3f,%.3f,%.3f player_y=%.3f"
            " proximity_owner=%s ceiling_y=%.3f cursor=0x%08" PRIX32
            " ordering=after_view_before_terrain ownership=value_copy"
            " identity_contract=full_0x24\n",
            marker,
            ticket,
            snapshot.levelIndex,
            preset_name(reviewed_preset),
            static_cast<int>(campaign_record->identifier.size()),
            campaign_record->identifier.data(),
            snapshot.horizonRgb,
            campaign_record->load_offset,
            campaign_record->record_word_18,
            campaign_record->record_word_1c,
            campaign_record->record_word_20,
            static_cast<double>(snapshot.fogScale),
            static_cast<double>(snapshot.eye.x),
            static_cast<double>(snapshot.eye.y),
            static_cast<double>(snapshot.eye.z),
            static_cast<double>(snapshot.playerY),
            player_y_owned ? "player_one_actor" : "camera_eye_fallback",
            static_cast<double>(kPlayerCeilingY),
            cursor
        );
        std::fflush(stderr);
    }
#else
    (void)eye;
    (void)forward;
    (void)right;
    (void)up;
    (void)player_y;
    (void)player_y_owned;
    log_skip("rt64_surface_not_compiled");
#endif
}
