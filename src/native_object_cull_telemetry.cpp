#include "native_object_cull_telemetry.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "native_widescreen.hpp"

namespace {

constexpr uint32_t kRdramSize = 0x00800000u;
constexpr uint32_t kSharedMatrixCount = 0x80105228u;
constexpr uint32_t kAuthoredSharedMatrixCapacity = 700u;
constexpr double kOriginalAspect = 4.0 / 3.0;
constexpr double kAuthoredDistanceExpansionScale = 1.0;
constexpr double kNoFogGenericDistanceExpansionScale = 8.0;
constexpr double kPreviousNoFogStructuralDistanceExpansionScale = 8.0;
constexpr double kNoFogStructuralDistanceExpansionScale = 16.0;
constexpr uint32_t kStructuralObjectCategory = 13u;
constexpr uint8_t kStructuralObjectMode = 1u;
constexpr float kStructuralMinimumBound = 1600.0f;
constexpr float kExactNoFogScale = 0.0f;
constexpr std::array<uint32_t, 11> kWeaponProjectileDescriptors{{
    0x80044FE8u, // lock-on plasma source (80044D10 is the minigame ball)
    0x80044D80u, // multi bomb
    0x80044DB8u, // homing / fly-by-wire missile
    0x80044DF0u, // lightning child arc
    0x80044E28u, // lightning source
    0x80044E60u, // auto-crossbow
    0x80044E98u, // stinger
    0x80044ED0u, // plasma blaster
    0x80044F08u, // blaster
    0x80044F40u, // laser
    0x80044F78u, // grenade
}};
std::atomic_uint64_t g_pvs_reject_count{0};
std::atomic_uint64_t g_projectile_pvs_visibility_promotion_count{0};
std::atomic_uint64_t g_structural_shell_pvs_visibility_promotion_count{0};
std::atomic_uint64_t g_stable_in_range_pvs_visibility_promotion_count{0};
std::atomic_uint64_t g_distance_reject_count{0};
std::atomic_uint64_t g_projectile_distance_visibility_promotion_count{0};
std::atomic_uint64_t g_distance_visibility_promotion_count{0};
std::atomic_bool g_far_structural_visibility_promotion_logged{false};
std::atomic_uint64_t g_object_capacity_suppression_count{0};
std::atomic_uint64_t g_post_object_capacity_suppression_count{0};
std::atomic_uint64_t g_matrix_world_pass_count{0};
std::atomic_uint64_t g_matrix_world_pass_overflow_count{0};
std::atomic<int32_t> g_max_world_pass_matrix_count{0};
std::atomic_bool g_diagnostics_enabled{true};
std::atomic_uint64_t g_performance_sample_count{0};
std::atomic_uint64_t g_performance_sample_nanoseconds{0};

bool performance_reporting_enabled() {
    static const bool enabled = [] {
        const char* detail =
            std::getenv("BUMBLE_RT64_FRAME_PACING_DETAIL");
        return detail != nullptr && detail[0] != '\0' && detail[0] != '0';
    }();
    return enabled;
}

bool cull_sample_cache_enabled() {
    static const bool enabled =
        std::getenv("BUMBLE_DISABLE_CULL_SAMPLE_CACHE") == nullptr;
    return enabled;
}

struct PerformanceSampleTimer {
    bool enabled = false;
    std::chrono::steady_clock::time_point start{};

    PerformanceSampleTimer()
        : enabled(performance_reporting_enabled()) {
        if (enabled) {
            start = std::chrono::steady_clock::now();
        }
    }

    ~PerformanceSampleTimer() {
        if (!enabled) {
            return;
        }
        const uint64_t nanoseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start
            ).count()
        );
        g_performance_sample_count.fetch_add(1u, std::memory_order_relaxed);
        g_performance_sample_nanoseconds.fetch_add(
            nanoseconds,
            std::memory_order_relaxed
        );
    }
};

bool diagnostics_enabled() {
    return g_diagnostics_enabled.load(std::memory_order_relaxed);
}

gpr guest_address(uint32_t address) {
    return static_cast<gpr>(static_cast<int32_t>(address));
}

uint32_t low_guest_address(gpr address) {
    return static_cast<uint32_t>(address);
}

bool guest_rdram_range(
    uint32_t address,
    uint32_t size,
    uint32_t alignment
) {
    if (size == 0u || alignment == 0u ||
        (address & (alignment - 1u)) != 0u ||
        (address & 0xE0000000u) != 0x80000000u) {
        return false;
    }
    const uint32_t offset = address & 0x1FFFFFFFu;
    return size <= kRdramSize && offset <= kRdramSize - size;
}

uint8_t read_u8(uint8_t* rdram, uint32_t address) {
    return static_cast<uint8_t>(MEM_BU(0, guest_address(address)));
}

int32_t read_s32(uint8_t* rdram, uint32_t address) {
    return static_cast<int32_t>(MEM_W(0, guest_address(address)));
}

float read_f32(uint8_t* rdram, uint32_t address) {
    const uint32_t word = static_cast<uint32_t>(
        MEM_W(0, guest_address(address))
    );
    float value = 0.0f;
    static_assert(sizeof(value) == sizeof(word));
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

bool should_emit_sample(uint64_t reason_count) {
    return reason_count <= 4u ||
        (reason_count & (reason_count - 1u)) == 0u;
}

bool is_weapon_projectile_descriptor(uint32_t descriptor) {
    return std::find(
        kWeaponProjectileDescriptors.begin(),
        kWeaponProjectileDescriptors.end(),
        descriptor
    ) != kWeaponProjectileDescriptors.end();
}

bool visibility_capacity_guard_enabled() {
    return bumble::widescreen::visibility_expansion_enabled();
}

uint32_t active_matrix_arena_capacity() {
    return std::max(
        bumble::widescreen::matrix_arena_capacity(),
        kAuthoredSharedMatrixCapacity
    );
}

bool matrix_arena_has_capacity(int32_t matrix_count) {
    const uint32_t capacity = active_matrix_arena_capacity();
    return matrix_count >= 0 &&
        static_cast<uint32_t>(matrix_count) < capacity;
}

void emit_capacity_suppression(
    const char* owner,
    uint64_t owner_count,
    int32_t matrix_count,
    uint32_t subject,
    uint32_t index,
    uint32_t mode_or_type,
    uint32_t original_branch_value,
    uint32_t forced_branch_value
) {
    std::fprintf(
        stderr,
        "BUMBLE_MATRIX_ARENA stage=allocation_suppressed owner=%s"
        " owner_count=%" PRIu64
        " sample_policy=first_four_then_powers_of_two"
        " matrix_count=%" PRId32 " matrix_capacity=%" PRIu32
        " subject=0x%08" PRIX32 " index=%" PRIu32
        " mode_or_type=%" PRIu32
        " original_branch_value=0x%08" PRIX32
        " forced_branch_value=0x%08" PRIX32
        " reason=invalid_or_full_matrix_arena"
        " guest_registers_mutated=1 guest_memory_mutated=0\n",
        owner,
        owner_count,
        matrix_count,
        active_matrix_arena_capacity(),
        subject,
        index,
        mode_or_type,
        original_branch_value,
        forced_branch_value
    );
    std::fflush(stderr);
}

struct CullSample {
    uint32_t object = 0;
    uint32_t camera = 0;
    uint32_t descriptor = 0;
    uint32_t category = 0;
    uint8_t mode = 0;
    uint8_t explicit_object_mask = 0;
    uint8_t resolved_object_mask = 0;
    uint8_t camera_mask = 0;
    float camera_midpoint_x = 0.0f;
    float camera_midpoint_z = 0.0f;
    float object_x = 0.0f;
    float object_z = 0.0f;
    float bound = 0.0f;
    float base_threshold_squared = 0.0f;
    float distance_squared = 0.0f;
    float threshold_squared = 0.0f;
    float aspect_threshold_squared = 0.0f;
    float expanded_threshold_squared = 0.0f;
    bool original_distance_pass = false;
    bool aspect_distance_pass = false;
    bool expanded_distance_pass = false;
    int32_t matrix_count = 0;
    uint32_t matrix_capacity = kAuthoredSharedMatrixCapacity;
    uint32_t matrix_slots_remaining = 0;
    float fog_scale = 1.0f;
    double horizontal_expansion_scale = 1.0;
    double fog_distance_expansion_scale = 1.0;
    double distance_expansion_scale = 1.0;
    double target_aspect = kOriginalAspect;
    bool structural_distance_policy = false;
    bool weapon_projectile = false;
};

bool capture_common_sample(
    uint8_t* rdram,
    recomp_context* context,
    CullSample& sample
) {
    PerformanceSampleTimer performance_timer;
    if (rdram == nullptr || context == nullptr) {
        return false;
    }

    sample.object = low_guest_address(context->r17);
    sample.camera = low_guest_address(context->r19);
    if (!guest_rdram_range(sample.object, 0x8Cu, 4u) ||
        !guest_rdram_range(sample.camera, 0x31u, 4u)) {
        return false;
    }

    sample.descriptor = static_cast<uint32_t>(
        read_s32(rdram, sample.object + 0x88u)
    );
    sample.weapon_projectile =
        is_weapon_projectile_descriptor(sample.descriptor);
    const bool visibility_expansion_enabled =
        bumble::widescreen::visibility_expansion_enabled();
    if (!visibility_expansion_enabled && !sample.weapon_projectile) {
        return false;
    }

    sample.horizontal_expansion_scale = visibility_expansion_enabled
        ? bumble::widescreen::horizontal_expansion_scale()
        : 1.0;
    sample.fog_scale = visibility_expansion_enabled
        ? std::clamp(bumble::widescreen::fog_scale(), 0.0f, 1.0f)
        : 1.0f;
    sample.category = low_guest_address(context->r21);
    sample.mode = read_u8(rdram, sample.object + 0x85u);
    sample.explicit_object_mask = read_u8(rdram, sample.object + 0x86u);
    sample.resolved_object_mask = static_cast<uint8_t>(context->r18);
    sample.camera_mask = read_u8(rdram, sample.camera + 0x30u);
    sample.camera_midpoint_x = read_f32(rdram, sample.camera + 0x24u);
    sample.camera_midpoint_z = read_f32(rdram, sample.camera + 0x2Cu);
    sample.object_x = read_f32(rdram, sample.object + 0x40u);
    sample.object_z = read_f32(rdram, sample.object + 0x48u);
    sample.bound = read_f32(rdram, sample.object + 0x6Cu);
    sample.base_threshold_squared = context->f28.fl;

    sample.structural_distance_policy =
        sample.category == kStructuralObjectCategory &&
        sample.mode == kStructuralObjectMode &&
        std::isfinite(sample.bound) &&
        sample.bound >= kStructuralMinimumBound;
    const double no_fog_distance_expansion_scale =
        sample.structural_distance_policy
            ? kNoFogStructuralDistanceExpansionScale
            : kNoFogGenericDistanceExpansionScale;
    sample.fog_distance_expansion_scale =
        sample.fog_scale == kExactNoFogScale
            ? no_fog_distance_expansion_scale
            : kAuthoredDistanceExpansionScale;
    sample.distance_expansion_scale = std::max(
        sample.horizontal_expansion_scale,
        sample.fog_distance_expansion_scale
    );
    if (!(sample.distance_expansion_scale > 1.000001) &&
        !sample.weapon_projectile &&
        active_matrix_arena_capacity() <= kAuthoredSharedMatrixCapacity) {
        return false;
    }

    const float delta_x = sample.camera_midpoint_x - sample.object_x;
    const float delta_z = sample.camera_midpoint_z - sample.object_z;
    const float bound_squared = sample.bound * sample.bound;
    sample.distance_squared = delta_x * delta_x + delta_z * delta_z;
    sample.threshold_squared =
        sample.base_threshold_squared + bound_squared;
    const float aspect_squared = static_cast<float>(
        sample.horizontal_expansion_scale *
            sample.horizontal_expansion_scale
    );
    sample.aspect_threshold_squared =
        sample.base_threshold_squared * aspect_squared + bound_squared;
    const float distance_scale_squared = static_cast<float>(
        sample.distance_expansion_scale * sample.distance_expansion_scale
    );
    sample.expanded_threshold_squared =
        sample.base_threshold_squared * distance_scale_squared + bound_squared;
    sample.original_distance_pass =
        sample.distance_squared < sample.threshold_squared;
    sample.aspect_distance_pass =
        sample.distance_squared < sample.aspect_threshold_squared;
    sample.expanded_distance_pass =
        sample.distance_squared < sample.expanded_threshold_squared;

    sample.matrix_count = read_s32(rdram, kSharedMatrixCount);
    sample.matrix_capacity = active_matrix_arena_capacity();
    sample.matrix_slots_remaining =
        sample.matrix_count >= 0 &&
        static_cast<uint32_t>(sample.matrix_count) < sample.matrix_capacity
            ? sample.matrix_capacity -
                static_cast<uint32_t>(sample.matrix_count)
            : 0u;
    sample.target_aspect =
        kOriginalAspect * sample.horizontal_expansion_scale;
    return true;
}

struct CullSampleCache {
    bool ready = false;
    bool valid = false;
    uint32_t object = 0;
    uint32_t camera = 0;
    uint32_t category = 0;
    uint32_t resolved_mask = 0;
    CullSample sample{};
};

thread_local CullSampleCache g_cull_sample_cache{};

bool cached_sample_matches(recomp_context* context) {
    return context != nullptr && g_cull_sample_cache.ready &&
        g_cull_sample_cache.object == low_guest_address(context->r17) &&
        g_cull_sample_cache.camera == low_guest_address(context->r19) &&
        g_cull_sample_cache.category == low_guest_address(context->r21) &&
        g_cull_sample_cache.resolved_mask == low_guest_address(context->r18);
}

struct StructuralPvsPromotion {
    uint32_t intersection = 0u;
    bool all_sectors = false;
};

struct StableInRangePvsPromotion {
    uint32_t intersection = 0u;
    bool all_sectors = false;
};

struct WeaponProjectilePvsPromotion {
    uint32_t intersection = 0u;
    bool collision_lifetime_owned = false;
};

struct NativePvsPromotion {
    uint32_t intersection = 0u;
    WeaponProjectilePvsPromotion projectile{};
    StructuralPvsPromotion structural{};
    StableInRangePvsPromotion stable_in_range{};
};

WeaponProjectilePvsPromotion native_weapon_projectile_pvs_promotion(
    const CullSample& sample
) {
    WeaponProjectilePvsPromotion result{};
    const uint32_t authored_intersection = static_cast<uint32_t>(
        sample.resolved_object_mask & sample.camera_mask
    );
    if (!sample.weapon_projectile || authored_intersection != 0u ||
        !matrix_arena_has_capacity(sample.matrix_count)) {
        return result;
    }

    result.intersection = sample.resolved_object_mask != 0u
        ? sample.resolved_object_mask
        : 1u;
    result.collision_lifetime_owned = true;
    return result;
}

StructuralPvsPromotion native_structural_pvs_promotion(
    const CullSample& sample
) {
    StructuralPvsPromotion result{};
    const uint32_t authored_intersection = static_cast<uint32_t>(
        sample.resolved_object_mask & sample.camera_mask
    );
    if (sample.category != kStructuralObjectCategory ||
        sample.mode != kStructuralObjectMode ||
        !std::isfinite(sample.bound) ||
        sample.bound < kStructuralMinimumBound ||
        !matrix_arena_has_capacity(sample.matrix_count) ||
        authored_intersection != 0u ||
        !std::isfinite(sample.distance_squared) ||
        !std::isfinite(sample.expanded_threshold_squared) ||
        !sample.expanded_distance_pass) {
        return result;
    }

    result.intersection = sample.resolved_object_mask;
    result.all_sectors = result.intersection != 0u;
    return result;
}

StableInRangePvsPromotion native_stable_in_range_pvs_promotion(
    const CullSample& sample
) {
    StableInRangePvsPromotion result{};
    const uint32_t authored_intersection = static_cast<uint32_t>(
        sample.resolved_object_mask & sample.camera_mask
    );
    if (authored_intersection != 0u || sample.resolved_object_mask == 0u ||
        sample.matrix_capacity <= kAuthoredSharedMatrixCapacity ||
        !matrix_arena_has_capacity(sample.matrix_count) ||
        !std::isfinite(sample.bound) || sample.bound < 0.0f ||
        !std::isfinite(sample.distance_squared) ||
        !std::isfinite(sample.expanded_threshold_squared) ||
        !sample.expanded_distance_pass) {
        return result;
    }

    result.intersection = sample.resolved_object_mask;
    result.all_sectors = result.intersection != 0u;
    return result;
}

NativePvsPromotion native_pvs_promotion(const CullSample& sample) {
    NativePvsPromotion result{};
    result.projectile = native_weapon_projectile_pvs_promotion(sample);
    if (result.projectile.intersection != 0u) {
        result.intersection = result.projectile.intersection;
        return result;
    }
    result.structural = native_structural_pvs_promotion(sample);
    if (result.structural.intersection != 0u) {
        result.intersection = result.structural.intersection;
        return result;
    }

    result.stable_in_range = native_stable_in_range_pvs_promotion(sample);
    result.intersection = result.stable_in_range.intersection;
    return result;
}

void emit_sample(
    const char* reason,
    uint64_t reason_count,
    bool distance_evaluated_by_guest,
    const CullSample& sample
) {
    const uint64_t pvs_total =
        g_pvs_reject_count.load(std::memory_order_relaxed);
    const uint64_t distance_total =
        g_distance_reject_count.load(std::memory_order_relaxed);
    std::fprintf(
        stderr,
        "BUMBLE_OBJECT_CULL stage=reject_sample reason=%s"
        " reason_count=%" PRIu64 " pvs_reject_total=%" PRIu64
        " distance_reject_total=%" PRIu64
        " sample_policy=first_four_then_powers_of_two"
        " object=0x%08" PRIX32 " category=%" PRIu32 " mode=%" PRIu8
        " explicit_object_mask=0x%02" PRIX8
        " resolved_object_mask=0x%02" PRIX8
        " camera_mask=0x%02" PRIX8 " camera=0x%08" PRIX32
        " camera_midpoint_x=%.6f camera_midpoint_z=%.6f"
        " object_x=%.6f object_z=%.6f bound=%.6f"
        " distance_squared=%.6f base_threshold_squared=%.6f"
        " threshold_squared=%.6f aspect_threshold_squared=%.6f"
        " expanded_threshold_squared=%.6f"
        " distance_evaluated_by_guest=%d original_distance_pass=%d"
        " aspect_distance_pass=%d expanded_distance_pass=%d"
        " horizontal_expansion_scale=%.6f"
        " fog_distance_expansion_scale=%.6f"
        " distance_expansion_scale=%.6f"
        " target_aspect=%.6f fog_scale=%.6f"
        " matrix_count=%" PRId32 " matrix_capacity=%" PRIu32
        " matrix_slots_remaining=%" PRIu32
        " guest_registers_mutated=0 guest_memory_mutated=0\n",
        reason,
        reason_count,
        pvs_total,
        distance_total,
        sample.object,
        sample.category,
        sample.mode,
        sample.explicit_object_mask,
        sample.resolved_object_mask,
        sample.camera_mask,
        sample.camera,
        static_cast<double>(sample.camera_midpoint_x),
        static_cast<double>(sample.camera_midpoint_z),
        static_cast<double>(sample.object_x),
        static_cast<double>(sample.object_z),
        static_cast<double>(sample.bound),
        static_cast<double>(sample.distance_squared),
        static_cast<double>(sample.base_threshold_squared),
        static_cast<double>(sample.threshold_squared),
        static_cast<double>(sample.aspect_threshold_squared),
        static_cast<double>(sample.expanded_threshold_squared),
        distance_evaluated_by_guest ? 1 : 0,
        sample.original_distance_pass ? 1 : 0,
        sample.aspect_distance_pass ? 1 : 0,
        sample.expanded_distance_pass ? 1 : 0,
        sample.horizontal_expansion_scale,
        sample.fog_distance_expansion_scale,
        sample.distance_expansion_scale,
        sample.target_aspect,
        static_cast<double>(sample.fog_scale),
        sample.matrix_count,
        sample.matrix_capacity,
        sample.matrix_slots_remaining
    );
    std::fflush(stderr);
}

void emit_weapon_projectile_visibility_promotion(
    const char* stage,
    uint64_t count,
    const CullSample& sample,
    uint32_t promoted_intersection,
    float promoted_threshold_squared
) {
    std::fprintf(
        stderr,
        "BUMBLE_OBJECT_CULL stage=%s count=%" PRIu64
        " object=0x%08" PRIX32 " descriptor=0x%08" PRIX32
        " category=%" PRIu32 " mode=%" PRIu8
        " resolved_object_mask=0x%02" PRIX8
        " camera_mask=0x%02" PRIX8
        " promoted_intersection=0x%02" PRIX32
        " distance_squared=%.6f promoted_threshold_squared=%.6f"
        " matrix_count=%" PRId32 " matrix_capacity=%" PRIu32
        " policy=weapon_visible_for_collision_owned_lifetime"
        " gpu_frustum_depth_owned=1 guest_memory_mutated=0\n",
        stage,
        count,
        sample.object,
        sample.descriptor,
        sample.category,
        sample.mode,
        sample.resolved_object_mask,
        sample.camera_mask,
        promoted_intersection,
        static_cast<double>(sample.distance_squared),
        static_cast<double>(promoted_threshold_squared),
        sample.matrix_count,
        sample.matrix_capacity
    );
    std::fflush(stderr);
}

void emit_structural_shell_pvs_visibility_promotion(
    uint64_t promotion_count,
    const CullSample& sample,
    const StructuralPvsPromotion& promotion
) {
    std::fprintf(
        stderr,
        "BUMBLE_OBJECT_CULL stage=structural_shell_pvs_promoted"
        " promotion_count=%" PRIu64
        " sample_policy=first_four_then_powers_of_two"
        " object=0x%08" PRIX32 " category=%" PRIu32
        " mode=%" PRIu8 " bound=%.6f"
        " resolved_object_mask=0x%02" PRIX8
        " camera_mask=0x%02" PRIX8
        " promoted_intersection=0x%02" PRIX32
        " object_x=%.6f object_z=%.6f"
        " distance_squared=%.6f expanded_threshold_squared=%.6f"
        " matrix_count=%" PRId32 " matrix_capacity=%" PRIu32
        " matrix_slots_remaining=%" PRIu32
        " pvs_policy=large_category13_mode1_bound_ge_1600_all_sectors"
        " distance_policy=large_category13_mode1_exact_no_fog_lerp_1_to_16"
        " static_shell_depth_owned=1 camera_sector_dependency_removed=1"
        " guest_registers_mutated=1 mutated_register=r2"
        " guest_memory_mutated=0\n",
        promotion_count,
        sample.object,
        sample.category,
        sample.mode,
        static_cast<double>(sample.bound),
        sample.resolved_object_mask,
        sample.camera_mask,
        promotion.intersection,
        static_cast<double>(sample.object_x),
        static_cast<double>(sample.object_z),
        static_cast<double>(sample.distance_squared),
        static_cast<double>(sample.expanded_threshold_squared),
        sample.matrix_count,
        sample.matrix_capacity,
        sample.matrix_slots_remaining
    );
    std::fflush(stderr);
}

void emit_stable_in_range_pvs_visibility_promotion(
    uint64_t promotion_count,
    const CullSample& sample,
    const StableInRangePvsPromotion& promotion
) {
    std::fprintf(
        stderr,
        "BUMBLE_OBJECT_CULL stage=stable_in_range_pvs_promoted"
        " promotion_count=%" PRIu64
        " sample_policy=first_four_then_powers_of_two"
        " object=0x%08" PRIX32 " category=%" PRIu32
        " mode=%" PRIu8 " bound=%.6f"
        " resolved_object_mask=0x%02" PRIX8
        " camera_mask=0x%02" PRIX8
        " promoted_intersection=0x%02" PRIX32
        " object_x=%.6f object_z=%.6f"
        " horizontal_expansion_scale=%.6f target_aspect=%.6f"
        " distance_squared=%.6f expanded_threshold_squared=%.6f"
        " matrix_count=%" PRId32 " matrix_capacity=%" PRIu32
        " matrix_slots_remaining=%" PRIu32
        " pvs_policy=extended_arena_all_in_range_sectors"
        " gpu_frustum_depth_owned=1 camera_sector_dependency_removed=1"
        " authored_arena_preserved=1 matrix_capacity_preserved=1"
        " guest_registers_mutated=1 mutated_register=r2"
        " guest_memory_mutated=0\n",
        promotion_count,
        sample.object,
        sample.category,
        sample.mode,
        static_cast<double>(sample.bound),
        sample.resolved_object_mask,
        sample.camera_mask,
        promotion.intersection,
        static_cast<double>(sample.object_x),
        static_cast<double>(sample.object_z),
        sample.horizontal_expansion_scale,
        sample.target_aspect,
        static_cast<double>(sample.distance_squared),
        static_cast<double>(sample.expanded_threshold_squared),
        sample.matrix_count,
        sample.matrix_capacity,
        sample.matrix_slots_remaining
    );
    std::fflush(stderr);
}

void emit_distance_visibility_promotion(
    uint64_t promotion_count,
    const CullSample& sample,
    const char* pvs_policy
) {
    std::fprintf(
        stderr,
        "BUMBLE_OBJECT_CULL stage=distance_threshold_scaled"
        " promotion_count=%" PRIu64
        " sample_policy=first_four_then_powers_of_two"
        " object=0x%08" PRIX32 " category=%" PRIu32 " mode=%" PRIu8
        " resolved_object_mask=0x%02" PRIX8
        " camera_mask=0x%02" PRIX8 " camera=0x%08" PRIX32
        " distance_squared=%.6f base_threshold_squared=%.6f"
        " original_threshold_squared=%.6f"
        " scaled_threshold_squared=%.6f"
        " aspect_threshold_squared=%.6f"
        " horizontal_expansion_scale=%.6f"
        " fog_distance_expansion_scale=%.6f"
        " distance_expansion_scale=%.6f"
        " target_aspect=%.6f fog_scale=%.6f"
        " matrix_count=%" PRId32 " matrix_capacity=%" PRIu32
        " matrix_slots_remaining=%" PRIu32
        " pvs_policy=%s bound_contribution_policy=preserved"
        " distance_policy=large_category13_mode1_lerp_1_to_16_else_lerp_1_to_8"
        " guest_registers_mutated=1 mutated_register=f0"
        " guest_memory_mutated=0\n",
        promotion_count,
        sample.object,
        sample.category,
        sample.mode,
        sample.resolved_object_mask,
        sample.camera_mask,
        sample.camera,
        static_cast<double>(sample.distance_squared),
        static_cast<double>(sample.base_threshold_squared),
        static_cast<double>(sample.threshold_squared),
        static_cast<double>(sample.expanded_threshold_squared),
        static_cast<double>(sample.aspect_threshold_squared),
        sample.horizontal_expansion_scale,
        sample.fog_distance_expansion_scale,
        sample.distance_expansion_scale,
        sample.target_aspect,
        static_cast<double>(sample.fog_scale),
        sample.matrix_count,
        sample.matrix_capacity,
        sample.matrix_slots_remaining,
        pvs_policy
    );
    std::fflush(stderr);
}

} // namespace

void bumble::object_cull_telemetry::set_diagnostics_enabled(bool enabled) {
    g_diagnostics_enabled.store(enabled, std::memory_order_release);
}

bumble::object_cull_telemetry::PerformanceSample
bumble::object_cull_telemetry::consume_performance_sample() {
    return {
        g_performance_sample_count.exchange(0u, std::memory_order_acq_rel),
        g_performance_sample_nanoseconds.exchange(0u, std::memory_order_acq_rel),
    };
}

extern "C" void bumble_object_cull_pvs_reject_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!diagnostics_enabled()) {
        return;
    }
    CullSample sample{};
    if (!capture_common_sample(rdram, context, sample)) {
        return;
    }

    const uint32_t mask_intersection = static_cast<uint32_t>(
        sample.resolved_object_mask & sample.camera_mask
    );
    if (low_guest_address(context->r2) != mask_intersection ||
        mask_intersection != 0u) {
        return;
    }

    const uint64_t count = g_pvs_reject_count.fetch_add(
        1u,
        std::memory_order_relaxed
    ) + 1u;
    if (should_emit_sample(count)) {
        emit_sample("pvs", count, false, sample);
    }
}

extern "C" void bumble_expand_static_geometry_pvs(
    uint8_t* rdram,
    recomp_context* context
) {
    g_cull_sample_cache = {};
    if (context == nullptr) {
        return;
    }
    g_cull_sample_cache.ready = true;
    g_cull_sample_cache.object = low_guest_address(context->r17);
    g_cull_sample_cache.camera = low_guest_address(context->r19);
    g_cull_sample_cache.category = low_guest_address(context->r21);
    g_cull_sample_cache.resolved_mask = low_guest_address(context->r18);
    g_cull_sample_cache.valid = capture_common_sample(
        rdram,
        context,
        g_cull_sample_cache.sample
    );
    if (!g_cull_sample_cache.valid) {
        return;
    }
    const CullSample& sample = g_cull_sample_cache.sample;

    const uint32_t authored_intersection = static_cast<uint32_t>(
        sample.resolved_object_mask & sample.camera_mask
    );
    if (authored_intersection != 0u ||
        low_guest_address(context->r2) != authored_intersection) {
        return;
    }

    const NativePvsPromotion promotion = native_pvs_promotion(sample);
    if (promotion.intersection == 0u) {
        return;
    }

    context->r2 = static_cast<gpr>(promotion.intersection);
    if (promotion.projectile.collision_lifetime_owned) {
        if (!diagnostics_enabled()) {
            return;
        }
        const uint64_t count =
            g_projectile_pvs_visibility_promotion_count.fetch_add(
                1u,
                std::memory_order_relaxed
            ) + 1u;
        if (should_emit_sample(count)) {
            emit_weapon_projectile_visibility_promotion(
                "weapon_projectile_pvs_promoted",
                count,
                sample,
                promotion.intersection,
                sample.threshold_squared
            );
        }
        return;
    }
    if (promotion.structural.all_sectors) {
        if (!diagnostics_enabled()) {
            return;
        }
        const uint64_t count =
            g_structural_shell_pvs_visibility_promotion_count.fetch_add(
                1u,
                std::memory_order_relaxed
            ) + 1u;
        if (should_emit_sample(count)) {
            emit_structural_shell_pvs_visibility_promotion(
                count,
                sample,
                promotion.structural
            );
        }
        return;
    }

    if (promotion.stable_in_range.all_sectors && diagnostics_enabled()) {
        const uint64_t count =
            g_stable_in_range_pvs_visibility_promotion_count.fetch_add(
                1u,
                std::memory_order_relaxed
            ) + 1u;
        if (should_emit_sample(count)) {
            emit_stable_in_range_pvs_visibility_promotion(
                count,
                sample,
                promotion.stable_in_range
            );
        }
    }
}

extern "C" void bumble_scale_object_distance_threshold(
    uint8_t* rdram,
    recomp_context* context
) {
    CullSample sample{};
    bool valid_sample = false;
    if (cull_sample_cache_enabled() && cached_sample_matches(context)) {
        valid_sample = g_cull_sample_cache.valid;
        if (valid_sample) {
            sample = g_cull_sample_cache.sample;
        }
        g_cull_sample_cache.ready = false;
    } else {
        valid_sample = capture_common_sample(rdram, context, sample);
    }
    if (!valid_sample) {
        return;
    }

    const uint32_t authored_intersection = static_cast<uint32_t>(
        sample.resolved_object_mask & sample.camera_mask
    );
    const uint32_t live_intersection = low_guest_address(context->r2);
    const NativePvsPromotion native_promotion = native_pvs_promotion(sample);
    const bool authored_pvs_pass =
        authored_intersection != 0u &&
        live_intersection == authored_intersection;
    const bool native_pvs_pass =
        authored_intersection == 0u &&
        native_promotion.intersection != 0u &&
        live_intersection == native_promotion.intersection;
    if ((!authored_pvs_pass && !native_pvs_pass) ||
        !matrix_arena_has_capacity(sample.matrix_count)) {
        return;
    }

    const float guest_distance_squared = context->f4.fl;
    const float original_threshold_squared = context->f0.fl;
    const float base_threshold_squared = context->f28.fl;
    if (!std::isfinite(guest_distance_squared) ||
        !std::isfinite(original_threshold_squared) ||
        !std::isfinite(base_threshold_squared) ||
        base_threshold_squared < 0.0f ||
        original_threshold_squared < base_threshold_squared ||
        guest_distance_squared < original_threshold_squared) {
        return;
    }

    if (sample.weapon_projectile) {
        const float promoted_threshold_squared = std::nextafter(
            guest_distance_squared,
            std::numeric_limits<float>::infinity()
        );
        if (!std::isfinite(promoted_threshold_squared) ||
            !(promoted_threshold_squared > guest_distance_squared)) {
            return;
        }
        context->f0.fl = promoted_threshold_squared;
        if (diagnostics_enabled()) {
            const uint64_t count =
                g_projectile_distance_visibility_promotion_count.fetch_add(
                    1u,
                    std::memory_order_relaxed
                ) + 1u;
            if (should_emit_sample(count)) {
                emit_weapon_projectile_visibility_promotion(
                    "weapon_projectile_distance_promoted",
                    count,
                    sample,
                    live_intersection,
                    promoted_threshold_squared
                );
            }
        }
        return;
    }

    const double distance_scale_squared =
        sample.distance_expansion_scale * sample.distance_expansion_scale;
    const double scaled_threshold_double =
        static_cast<double>(original_threshold_squared) +
        static_cast<double>(base_threshold_squared) *
            (distance_scale_squared - 1.0);
    if (!std::isfinite(scaled_threshold_double) ||
        scaled_threshold_double >
            static_cast<double>(std::numeric_limits<float>::max())) {
        return;
    }
    const float scaled_threshold_squared =
        static_cast<float>(scaled_threshold_double);
    if (!(scaled_threshold_squared > original_threshold_squared) ||
        !(guest_distance_squared < scaled_threshold_squared)) {
        return;
    }

    sample.distance_squared = guest_distance_squared;
    sample.threshold_squared = original_threshold_squared;
    sample.expanded_threshold_squared = scaled_threshold_squared;
    sample.original_distance_pass = false;
    sample.expanded_distance_pass = true;
    sample.aspect_distance_pass =
        guest_distance_squared < sample.aspect_threshold_squared;
    context->f0.fl = scaled_threshold_squared;

    if (!diagnostics_enabled()) {
        return;
    }

    if (sample.structural_distance_policy &&
        sample.fog_scale == kExactNoFogScale) {
        const double previous_scale_squared =
            kPreviousNoFogStructuralDistanceExpansionScale *
            kPreviousNoFogStructuralDistanceExpansionScale;
        const double previous_threshold_double =
            static_cast<double>(original_threshold_squared) +
            static_cast<double>(base_threshold_squared) *
                (previous_scale_squared - 1.0);
        if (std::isfinite(previous_threshold_double) &&
            static_cast<double>(guest_distance_squared) >=
                previous_threshold_double &&
            static_cast<double>(guest_distance_squared) <
                static_cast<double>(scaled_threshold_squared)) {
            bool expected = false;
            if (g_far_structural_visibility_promotion_logged
                    .compare_exchange_strong(
                        expected,
                        true,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire
                    )) {
                std::fprintf(
                    stderr,
                    "BUMBLE_OBJECT_CULL stage=far_structural_promoted"
                    " object=0x%08" PRIX32
                    " category=%" PRIu32 " mode=%" PRIu8
                    " bound=%.6f distance_squared=%.6f"
                    " previous_scale=%.6f previous_threshold_squared=%.6f"
                    " current_scale=%.6f current_threshold_squared=%.6f"
                    " matrix_count=%" PRId32
                    " matrix_capacity=%" PRIu32
                    " policy=large_category13_mode1_bound_ge_1600"
                    " pvs_preserved=1 capacity_guard_preserved=1"
                    " guest_registers_mutated=1 mutated_register=f0"
                    " guest_memory_mutated=0\n",
                    sample.object,
                    sample.category,
                    sample.mode,
                    static_cast<double>(sample.bound),
                    static_cast<double>(guest_distance_squared),
                    kPreviousNoFogStructuralDistanceExpansionScale,
                    previous_threshold_double,
                    sample.distance_expansion_scale,
                    static_cast<double>(scaled_threshold_squared),
                    sample.matrix_count,
                    sample.matrix_capacity
                );
                std::fflush(stderr);
            }
        }
    }

    const uint64_t count = g_distance_visibility_promotion_count.fetch_add(
        1u,
        std::memory_order_relaxed
    ) + 1u;
    if (should_emit_sample(count)) {
        emit_distance_visibility_promotion(
            count,
            sample,
            authored_pvs_pass
                ? "authored_intersection"
                : (native_promotion.structural.all_sectors
                    ? "native_structural_shell_all_sectors"
                    : "native_extended_arena_all_in_range_sectors")
        );
    }
}

extern "C" void bumble_object_cull_distance_reject_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!diagnostics_enabled()) {
        return;
    }
    CullSample sample{};
    if (!capture_common_sample(rdram, context, sample) ||
        (sample.resolved_object_mask & sample.camera_mask) == 0u) {
        return;
    }

    const float guest_distance_squared = context->f4.fl;
    const float guest_threshold_squared = context->f0.fl;
    if (guest_distance_squared < guest_threshold_squared) {
        return;
    }
    sample.distance_squared = guest_distance_squared;
    sample.threshold_squared = guest_threshold_squared;
    sample.original_distance_pass = false;
    sample.aspect_distance_pass =
        guest_distance_squared < sample.aspect_threshold_squared;
    sample.expanded_distance_pass =
        guest_distance_squared < sample.expanded_threshold_squared;

    const uint64_t count = g_distance_reject_count.fetch_add(
        1u,
        std::memory_order_relaxed
    ) + 1u;
    if (should_emit_sample(count)) {
        emit_sample("distance", count, true, sample);
    }
}

extern "C" void bumble_guard_object_matrix_capacity(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr ||
        !visibility_capacity_guard_enabled()) {
        return;
    }

    const int32_t matrix_count = read_s32(rdram, kSharedMatrixCount);
    const uint32_t original_branch_value = low_guest_address(context->r2);
    if (matrix_arena_has_capacity(matrix_count) ||
        original_branch_value == 0u) {
        return;
    }

    context->r2 = 0;

    if (!diagnostics_enabled()) {
        return;
    }
    const uint32_t object = low_guest_address(context->r17);
    uint32_t mode = UINT32_MAX;
    if (guest_rdram_range(object, 0x86u, 4u)) {
        mode = read_u8(rdram, object + 0x85u);
    }

    const uint64_t count = g_object_capacity_suppression_count.fetch_add(
        1u,
        std::memory_order_relaxed
    ) + 1u;
    if (should_emit_sample(count)) {
        emit_capacity_suppression(
            "generic_object",
            count,
            matrix_count,
            object,
            low_guest_address(context->r21),
            mode,
            original_branch_value,
            0u
        );
    }
}

extern "C" void bumble_guard_post_object_matrix_capacity(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr ||
        !visibility_capacity_guard_enabled()) {
        return;
    }

    const uint32_t original_type = low_guest_address(context->r3);
    if (original_type != 1u) {
        return;
    }
    const int32_t matrix_count = read_s32(rdram, kSharedMatrixCount);
    if (matrix_arena_has_capacity(matrix_count)) {
        return;
    }

    context->r3 = 2;
    if (!diagnostics_enabled()) {
        return;
    }
    const uint64_t count =
        g_post_object_capacity_suppression_count.fetch_add(
            1u,
            std::memory_order_relaxed
        ) + 1u;
    if (should_emit_sample(count)) {
        emit_capacity_suppression(
            "post_object_record",
            count,
            matrix_count,
            low_guest_address(context->r16),
            low_guest_address(context->r19),
            original_type,
            original_type,
            2u
        );
    }
}

extern "C" void bumble_matrix_arena_world_pass_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr ||
        !visibility_capacity_guard_enabled() || !diagnostics_enabled()) {
        return;
    }

    const int32_t matrix_count = read_s32(rdram, kSharedMatrixCount);
    int32_t observed_max =
        g_max_world_pass_matrix_count.load(std::memory_order_relaxed);
    while (matrix_count > observed_max &&
           !g_max_world_pass_matrix_count.compare_exchange_weak(
               observed_max,
               matrix_count,
               std::memory_order_relaxed,
               std::memory_order_relaxed
           )) {
    }
    const int32_t maximum_matrix_count =
        g_max_world_pass_matrix_count.load(std::memory_order_relaxed);
    const uint64_t pass_count = g_matrix_world_pass_count.fetch_add(
        1u,
        std::memory_order_relaxed
    ) + 1u;
    const uint32_t matrix_capacity = active_matrix_arena_capacity();
    const bool overflow = matrix_count >
        static_cast<int32_t>(matrix_capacity);
    const uint64_t overflow_count = overflow
        ? g_matrix_world_pass_overflow_count.fetch_add(
              1u,
              std::memory_order_relaxed
          ) + 1u
        : g_matrix_world_pass_overflow_count.load(std::memory_order_relaxed);
    if (!should_emit_sample(pass_count) &&
        !(overflow && should_emit_sample(overflow_count))) {
        return;
    }

    std::fprintf(
        stderr,
        "BUMBLE_MATRIX_ARENA stage=world_pass_complete"
        " pass_count=%" PRIu64
        " sample_policy=first_four_then_powers_of_two"
        " matrix_count=%" PRId32 " maximum_matrix_count=%" PRId32
        " matrix_capacity=%" PRIu32 " at_capacity=%d overflow=%d"
        " overflow_count=%" PRIu64
        " guest_registers_mutated=0 guest_memory_mutated=0\n",
        pass_count,
        matrix_count,
        maximum_matrix_count,
        matrix_capacity,
        matrix_count == static_cast<int32_t>(matrix_capacity) ? 1 : 0,
        overflow ? 1 : 0,
        overflow_count
    );
    std::fflush(stderr);
}
