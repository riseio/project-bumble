#include "native_modern_controls.hpp"

#include "funcs.h"
#include "native_checkpoint_bridge.hpp"
#include "native_menu_actions.hpp"
#include "native_widescreen.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace {

bool aim_telemetry_all_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv(
            "BUMBLE_RT64_PROBE_AIM_TELEMETRY_ALL"
        );
        return value != nullptr && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

bool runtime_telemetry_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BUMBLE_RT64_RUNTIME_TELEMETRY");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled || aim_telemetry_all_enabled();
}

constexpr uint32_t kPlayerOneOwnerSlot = 0x800E91D4u;
constexpr uint32_t kPlayerOneCamera = 0x800E9254u;
constexpr uint32_t kPlayerOneHealth = 0x800E92D8u;
constexpr uint32_t kPlayerVtable = 0x800445E8u;
constexpr uint32_t kMissileVtable = 0x80044DB8u;
constexpr uint32_t kGuidedMissilePointer = 0x800CD730u;
constexpr uint32_t kTeleportVtable = 0x80046A90u;
constexpr uint32_t kWebBoxVtable = 0x800466D8u;
constexpr uint32_t kTeleportCollisionCategory = 0x00800000u;
constexpr uint32_t kCollisionActorLookup = 0x800E2408u;
constexpr uint32_t kCollisionActorLookupMask = 0x1FFu;
constexpr uint32_t kPostObjectRecordCount = 0x800CC8C4u;
constexpr uint32_t kPostObjectRecordBase = 0x80102C30u;
constexpr uint32_t kPostObjectRecordSize = 0x14u;
constexpr uint32_t kPostObjectRecordCapacity = 0xB4u;
constexpr uint32_t kDisplayListCursor = 0x80035F30u;
constexpr uint32_t kTeleportTransformInput = 0x800CE4E8u;
constexpr uint32_t kTeleportDisplayList = 0x040246F8u;
constexpr uint32_t kExtendedGbiOpcode = 0x64u;
constexpr uint32_t kTeleportDisabledTintMarker = 0x000036u;
constexpr uint32_t kTeleportDisabledRecordType = 2u;
constexpr uint32_t kGfxCommandBytes = 8u;
// func_8005598C emits matrix, transform/setup DL, Teleport model DL, and pop.
constexpr uint32_t kTeleportOriginalCommandBytes = 4u * kGfxCommandBytes;
constexpr uint32_t kFrontendObject = 0x800FFF80u;
constexpr uint32_t kFrontendBriefingPhase = 0x18u;
constexpr uint32_t kFrontendGameplayPhase = 0x19u;
constexpr uint32_t kFrontendPhaseOffset = 0x00u;
constexpr uint32_t kFrontendTransitionOffset = 0x40u;
constexpr uint32_t kFrontendDescriptorTable = 0x800FE570u;
constexpr uint32_t kMissionCardDescriptor = 0x800FD458u;
constexpr uint32_t kMissionCardDescriptorFlags = 0x00000513u;
constexpr uint32_t kBriefingDescriptor = 0x800FD3F0u;
constexpr uint32_t kBriefingDescriptorFlags = 0x00000023u;
constexpr uint32_t kFrontendDescriptorFlagsOffset = 0x1Cu;
constexpr uint32_t kFrontendDescriptorItemOffset = 0x38u;
constexpr uint32_t kTranslatedCurrentPad = 0x80035F00u;
constexpr uint32_t kPlayerNormalizedPad = 0x80038340u;
constexpr uint32_t kPauseMenuVisible = 0x80100124u;
constexpr uint32_t kFlyingState = 2u;
constexpr uint32_t kAirborneRecoveryState = 3u;
constexpr uint32_t kAirborneCollisionState = 4u;
constexpr uint32_t kGroundedEntryState = 5u;
constexpr uint32_t kGroundedFirstState = 6u;
constexpr uint32_t kGroundedLastState = 7u;
constexpr uint32_t kButtonA = 0x8000u;
constexpr uint32_t kButtonB = 0x4000u;
constexpr uint16_t kCDown = 0x0004u;
constexpr float kMinimumSensitivity = 0.01f;
constexpr float kMaximumSensitivity = 2.0f;
constexpr float kDefaultSensitivity = 0.15f;
constexpr float kMaximumPitch = 80.0f;
constexpr float kMovementSpeed = 2.5f;
constexpr float kSprintMovementMultiplier = 2.0f;
constexpr float kBasisScale = 0.25f;
constexpr float kIntegratorScale = 4.0f;
constexpr float kBuckAngleRadiansPerDegree = 0.01744999922811985f;
constexpr int64_t kMaximumQueuedLookCounts = 2000;
constexpr uint32_t kLandingInputBufferUpdates = 12u;
// B8 is decremented before testing 60..82 at 0x8005A8B0.
constexpr uint32_t kQuickFlipEntryWindowFirst = 61u;
constexpr uint32_t kQuickFlipEntryWindowLast = 83u;
constexpr uint32_t kQuickFlipTimeoutUpdates = 120u;
constexpr float kPi = 3.14159265358979323846f;
constexpr uint32_t kBarrelRollDurationUpdates = 36u;
constexpr uint32_t kBarrelRollCooldownUpdates = 30u;
constexpr float kBarrelRollTotalDistance = 252.0f;
constexpr uint32_t kLoopDeLoopDurationUpdates = 95u;
constexpr uint32_t kLoopDeLoopCooldownUpdates = 45u;
constexpr float kLoopDeLoopRadius = 64.0f;
constexpr float kLoopDeLoopForwardAdvance = 120.0f;
constexpr uint32_t kQuickTurnDurationUpdates = 32u;
constexpr float kQuickTurnHeadingDegrees = 180.0f;
constexpr auto kTeleportCooldownDuration = std::chrono::seconds(10);

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

enum class ActiveManeuver : uint32_t {
    None = 0u,
    BarrelRoll = 2u,
    LoopDeLoop = 3u,
};

struct WebBoxContact {
    uint32_t actor = 0u;
    uint16_t id = 0u;
};
thread_local std::array<WebBoxContact, kCollisionActorLookupMask + 1u>
    g_web_box_contacts{};
thread_local size_t g_web_box_contact_count = 0u;
thread_local uint32_t g_web_box_player = 0u;
thread_local uint16_t g_web_box_player_id = 0u;

enum class QuickFlipStage : uint32_t {
    Idle = 0u,
    AwaitFlipWindow = 1u,
    AwaitGuestCommit = 2u,
};

std::atomic_bool g_enabled{false};
std::atomic_bool g_replay_automation{false};
std::atomic_bool g_window_focused{false};
std::atomic_bool g_mouse_captured{false};
std::atomic_bool g_gameplay_active{false};
std::atomic_bool g_pause_menu_active{false};
std::atomic_bool g_resync_aim{true};
std::atomic_bool g_gameplay_arm_deferred_logged{false};
std::atomic_int64_t g_pending_look_x{0};
std::atomic_int64_t g_pending_look_y{0};
std::atomic_bool g_primary_fire{false};
std::atomic_bool g_primary_fire_require_release{false};
std::atomic_bool g_frontend_confirm_pending{false};
std::atomic_bool g_frontend_confirm_held{false};
std::atomic_bool g_frontend_confirm_edge{false};
std::atomic_bool g_frontend_confirm_require_release{true};
std::atomic_bool g_frontend_confirm_latched{false};
std::atomic_bool g_mission_card_accepted{false};
std::atomic_bool g_cutscene_skip_held{false};
std::atomic_bool g_cutscene_skip_edge{false};
std::atomic_bool g_cutscene_skip_require_release{false};
std::atomic_bool g_takeoff_land_held{false};
std::atomic_bool g_takeoff_land_edge{false};
std::atomic_bool g_takeoff_land_require_release{true};
std::atomic_bool g_loop_de_loop_held{false};
std::atomic_bool g_loop_de_loop_edge{false};
std::atomic_bool g_loop_de_loop_require_release{true};
std::atomic_bool g_quick_flip_held{false};
std::atomic_bool g_quick_flip_edge{false};
std::atomic_bool g_quick_flip_require_release{true};
std::atomic_bool g_sprint_held{false};
std::atomic_bool g_sprint_require_release{true};
std::atomic_bool g_barrel_roll_held{false};
std::atomic_bool g_barrel_roll_edge{false};
std::atomic_bool g_barrel_roll_require_release{true};
std::atomic_uint32_t g_quick_flip_stage{
    static_cast<uint32_t>(QuickFlipStage::Idle)
};
std::atomic_uint32_t g_quick_flip_actor{0u};
std::atomic_uint32_t g_quick_flip_timeout_updates{0u};
std::atomic_uint32_t g_active_maneuver{
    static_cast<uint32_t>(ActiveManeuver::None)
};
std::atomic_uint32_t g_maneuver_actor{0u};
std::atomic_uint32_t g_maneuver_updates_remaining{0u};
std::atomic_uint32_t g_maneuver_direction_x{0u};
std::atomic_uint32_t g_maneuver_direction_y{0u};
std::atomic_uint32_t g_maneuver_direction_z{0u};
std::atomic_uint32_t g_maneuver_up_x{0u};
std::atomic_uint32_t g_maneuver_up_y{0u};
std::atomic_uint32_t g_maneuver_up_z{0u};
std::atomic_uint32_t g_barrel_roll_cooldown_updates{0u};
std::atomic_uint32_t g_loop_de_loop_cooldown_updates{0u};
std::atomic_int32_t g_last_strafe_direction{1};
std::atomic_uint32_t g_guided_missile_actor{0u};
std::atomic_uint32_t g_guided_missile_player_actor{0u};
std::atomic_uint64_t g_guided_missile_aim_update_count{0u};
std::atomic_uint32_t g_barrel_roll_visual_actor{0u};
std::atomic_uint32_t g_barrel_roll_visual_angle_bits{0u};
std::atomic_int32_t g_barrel_roll_visual_direction{1};
std::atomic_uint64_t g_barrel_roll_visual_scope_count{0u};
std::atomic_uint32_t g_quick_turn_actor{0u};
std::atomic_uint32_t g_quick_turn_updates_completed{0u};
std::atomic_uint32_t g_quick_turn_start_yaw_bits{0u};
std::atomic_bool g_quick_turn_active{false};
std::mutex g_stunt_visual_mutex;
uint32_t g_stunt_visual_actor = 0u;
std::array<Vec3, 3> g_stunt_visual_basis{};
std::array<float, 3> g_stunt_visual_euler{};
bool g_stunt_visual_quick_flip = false;
std::atomic_uint64_t g_stunt_visual_scope_count{0u};
std::atomic_uint32_t g_manual_land_request_actor{0};
std::atomic_uint32_t g_manual_land_buffer_updates_remaining{0};
std::atomic_uint32_t g_manual_landing_authorized_actor{0};
std::atomic_uint32_t g_manual_takeoff_request_actor{0};
std::atomic_int32_t g_movement_forward{0};
std::atomic_int32_t g_movement_strafe{0};
std::atomic_uint64_t g_player_aim_update_count{0};
std::atomic_uint64_t g_movement_frame_count{0};
std::atomic_uint64_t g_active_player_frame_count{0};
std::atomic_uint64_t g_teleport_completion_count{0};
std::atomic_uint64_t g_teleport_suppression_count{0};
std::atomic_uint32_t g_teleport_cooldown_actor{0};
std::atomic_uint32_t g_teleport_cooldown_pair_id{0};
std::atomic_int64_t g_teleport_cooldown_deadline_ns{0};
std::atomic_int64_t g_teleport_visual_logged_deadline_ns{0};
std::atomic_uint64_t g_teleport_visual_scope_count{0};
std::atomic_uint64_t g_automatic_landing_suppressed_count{0};
std::atomic_uint64_t g_automatic_landing_store_suppressed_count{0};
std::atomic_uint64_t g_automatic_grounded_entry_suppressed_count{0};
std::atomic_uint64_t g_airborne_collision_rollback_count{0};
std::atomic_uint64_t g_airborne_collision_recovery_restore_count{0};
std::atomic_uint64_t g_ground_contact_rollback_count{0};
std::atomic_uint64_t g_airborne_floor_hold_count{0};
std::atomic_uint64_t g_airborne_idle_hold_count{0};
std::atomic_uint64_t g_airborne_landing_gear_retraction_count{0};
std::atomic_uint64_t g_manual_land_obstacle_rejection_count{0};
std::atomic_uint64_t g_automatic_ground_exit_count{0};
std::atomic_uint32_t g_player_update_actor{0};
std::atomic_uint32_t g_player_update_entry_state{0};
std::atomic_uint32_t g_airborne_collision_finalize_actor{0};
std::atomic_uint32_t g_surface_glide_actor{0};
std::atomic_uint32_t g_surface_glide_height_y{0};
std::atomic_uint32_t g_manual_grounded_entry_actor{0};
std::atomic_uint32_t g_manual_takeoff_injection_actor{0};
std::atomic_uint32_t g_manual_takeoff_animation_actor{0};
std::atomic_uint32_t g_automatic_takeoff_pending_actor{0};
std::atomic_uint32_t g_automatic_takeoff_animation_actor{0};
std::atomic_bool g_movement_checkpoint_valid{false};
std::atomic_uint32_t g_movement_checkpoint_actor{0};
std::atomic_uint32_t g_movement_checkpoint_x{0};
std::atomic_uint32_t g_movement_checkpoint_y{0};
std::atomic_uint32_t g_movement_checkpoint_z{0};
std::atomic_uint32_t g_movement_target_x{0};
std::atomic_uint32_t g_movement_target_y{0};
std::atomic_uint32_t g_movement_target_z{0};
std::atomic_bool g_collision_safe_anchor_valid{false};
std::atomic_uint32_t g_collision_safe_anchor_actor{0};
std::atomic_uint32_t g_collision_safe_anchor_x{0};
std::atomic_uint32_t g_collision_safe_anchor_y{0};
std::atomic_uint32_t g_collision_safe_anchor_z{0};
std::atomic_bool g_landing_anchor_valid{false};
std::atomic_uint32_t g_landing_anchor_actor{0};
std::atomic_uint32_t g_landing_anchor_x{0};
std::atomic_uint32_t g_landing_anchor_y{0};
std::atomic_uint32_t g_landing_anchor_z{0};
std::atomic_bool g_airborne_idle_anchor_valid{false};
std::atomic_uint32_t g_airborne_idle_anchor_actor{0};
std::atomic_uint32_t g_airborne_idle_anchor_x{0};
std::atomic_uint32_t g_airborne_idle_anchor_y{0};
std::atomic_uint32_t g_airborne_idle_anchor_z{0};

std::mutex g_aim_mutex;
float g_look_sensitivity = 0.15f;
bool g_invert_look_y = false;
bool g_aim_ready = false;
uint32_t g_aim_actor = 0;
std::atomic_bool g_aim_input_consumed_this_update{false};
Vec3 g_aim_right{};
Vec3 g_aim_up{};
Vec3 g_aim_forward{};
float g_aim_pitch = 0.0f;
float g_aim_yaw = 0.0f;

thread_local uint32_t g_pending_teleport_visual_actor = 0u;
thread_local uint32_t g_pending_teleport_visual_record_count = 0u;
thread_local bool g_teleport_visual_scope_active = false;
thread_local uint32_t g_teleport_visual_expected_end_cursor = 0u;
thread_local uint32_t g_barrel_roll_saved_actor = 0u;
thread_local std::array<Vec3, 3> g_barrel_roll_saved_basis{};

gpr guest_address(uint32_t address) {
    return static_cast<gpr>(static_cast<int32_t>(address));
}

uint32_t guest_u32(gpr value) {
    return static_cast<uint32_t>(value);
}

bool valid_guest_pointer(uint32_t address, uint32_t size) {
    return address >= 0x80000000u && address <= 0x84000000u - size;
}

uint32_t read_u32(uint8_t* rdram, uint32_t address) {
    return static_cast<uint32_t>(MEM_W(0, guest_address(address)));
}

void write_u32(uint8_t* rdram, uint32_t address, uint32_t value) {
    (void)rdram;
    MEM_W(0, guest_address(address)) = static_cast<int32_t>(value);
}

uint16_t read_u16(uint8_t* rdram, uint32_t address) {
    return static_cast<uint16_t>(MEM_HU(0, guest_address(address)));
}

int64_t monotonic_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

bool exact_mission_card_is_live(uint8_t* rdram) {
    return rdram != nullptr &&
        read_u32(rdram, kFrontendObject + kFrontendPhaseOffset) ==
            kFrontendGameplayPhase &&
        read_u32(rdram, kFrontendObject + kFrontendTransitionOffset) == 0u &&
        read_u32(
            rdram,
            kFrontendDescriptorTable +
                kFrontendGameplayPhase * sizeof(uint32_t)
        ) == kMissionCardDescriptor &&
        read_u32(
            rdram,
            kMissionCardDescriptor + kFrontendDescriptorFlagsOffset
        ) == kMissionCardDescriptorFlags &&
        read_u32(
            rdram,
            kMissionCardDescriptor + kFrontendDescriptorItemOffset
        ) == 0u;
}

bool exact_briefing_staging_is_live(uint8_t* rdram) {
    return rdram != nullptr &&
        read_u32(rdram, kFrontendObject + kFrontendPhaseOffset) ==
            kFrontendBriefingPhase &&
        read_u32(
            rdram,
            kFrontendDescriptorTable +
                kFrontendBriefingPhase * sizeof(uint32_t)
        ) == kBriefingDescriptor &&
        read_u32(
            rdram,
            kBriefingDescriptor + kFrontendDescriptorFlagsOffset
        ) == kBriefingDescriptorFlags &&
        read_u32(
            rdram,
            kBriefingDescriptor + kFrontendDescriptorItemOffset
        ) == 0u;
}

float float_from_word(uint32_t word) {
    return std::bit_cast<float>(word);
}

uint32_t word_from_float(float value) {
    return std::bit_cast<uint32_t>(value);
}

Vec3 add(const Vec3& left, const Vec3& right) {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

Vec3 multiply(const Vec3& value, float scale) {
    return {value.x * scale, value.y * scale, value.z * scale};
}

float dot(const Vec3& left, const Vec3& right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

Vec3 cross(const Vec3& left, const Vec3& right) {
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

float length(const Vec3& value) {
    return std::sqrt(dot(value, value));
}

bool all_finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.z);
}

bool normalize(const Vec3& value, Vec3& result) {
    const float magnitude = length(value);
    if (!std::isfinite(magnitude) || magnitude < 0.0001f) {
        return false;
    }
    result = multiply(value, 1.0f / magnitude);
    return true;
}

float half_cosine_progress(uint32_t completed, uint32_t duration) {
    if (duration == 0u || completed == 0u) {
        return 0.0f;
    }
    if (completed >= duration) {
        return 1.0f;
    }
    const float phase = static_cast<float>(completed) /
        static_cast<float>(duration);
    const float sine = std::sin(phase * kPi * 0.5f);
    return sine * sine;
}

float half_sine_velocity_progress(uint32_t completed, uint32_t duration) {
    if (duration == 0u || completed == 0u) {
        return 0.0f;
    }
    if (completed >= duration) {
        return 1.0f;
    }
    const float phase = static_cast<float>(completed) /
        static_cast<float>(duration);
    return 0.5f * (1.0f - std::cos(phase * kPi));
}

float curved_step_distance(
    float total_distance,
    uint32_t completed,
    uint32_t duration
) {
    return total_distance * (
        half_cosine_progress(completed, duration) -
        half_cosine_progress(completed - 1u, duration)
    );
}

float half_sine_velocity_step_distance(
    float total_distance,
    uint32_t completed,
    uint32_t duration
) {
    return total_distance * (
        half_sine_velocity_progress(completed, duration) -
        half_sine_velocity_progress(completed - 1u, duration)
    );
}

bool build_scaled_basis_from_euler(
    float pitch_degrees,
    float yaw_degrees,
    float roll_degrees,
    std::array<Vec3, 3>& basis
) {
    if (!std::isfinite(pitch_degrees) || !std::isfinite(yaw_degrees) ||
        !std::isfinite(roll_degrees)) {
        return false;
    }

    const float pitch = pitch_degrees * kBuckAngleRadiansPerDegree;
    const float yaw = yaw_degrees * kBuckAngleRadiansPerDegree;
    const float roll = roll_degrees * kBuckAngleRadiansPerDegree;
    const float sine_pitch = std::sin(pitch);
    const float cosine_pitch = std::cos(pitch);
    const float sine_yaw = std::sin(yaw);
    const float cosine_yaw = std::cos(yaw);
    const float sine_roll = std::sin(roll);
    const float cosine_roll = std::cos(roll);

    const Vec3 right{cosine_yaw, 0.0f, sine_yaw};
    const Vec3 up{
        sine_yaw * sine_pitch,
        cosine_pitch,
        -cosine_yaw * sine_pitch,
    };
    const Vec3 forward{
        -cosine_pitch * sine_yaw,
        sine_pitch,
        cosine_pitch * cosine_yaw,
    };
    basis = {
        multiply(
            add(multiply(right, cosine_roll), multiply(up, sine_roll)),
            kBasisScale
        ),
        multiply(
            add(multiply(up, cosine_roll), multiply(right, -sine_roll)),
            kBasisScale
        ),
        multiply(forward, kBasisScale),
    };
    return all_finite(basis[0]) && all_finite(basis[1]) &&
        all_finite(basis[2]);
}

Vec3 read_actor_vector(uint8_t* rdram, uint32_t actor, uint32_t offset) {
    return {
        float_from_word(read_u32(rdram, actor + offset + 0u)),
        float_from_word(read_u32(rdram, actor + offset + 4u)),
        float_from_word(read_u32(rdram, actor + offset + 8u)),
    };
}

void write_actor_vector(
    uint8_t* rdram,
    uint32_t actor,
    uint32_t offset,
    const Vec3& value
) {
    MEM_W(offset + 0u, guest_address(actor)) = word_from_float(value.x);
    MEM_W(offset + 4u, guest_address(actor)) = word_from_float(value.y);
    MEM_W(offset + 8u, guest_address(actor)) = word_from_float(value.z);
}

void invalidate_movement_checkpoint() {
    g_movement_checkpoint_valid.store(false, std::memory_order_release);
}

void capture_movement_checkpoint(uint8_t* rdram, uint32_t actor) {
    const Vec3 position = read_actor_vector(rdram, actor, 0x40u);
    const Vec3 forward_basis = read_actor_vector(rdram, actor, 0x30u);
    const float speed = float_from_word(read_u32(rdram, actor + 0x5Cu));
    const Vec3 target = add(
        position,
        multiply(forward_basis, -speed * kIntegratorScale)
    );
    if (!all_finite(position) || !all_finite(forward_basis) ||
        !std::isfinite(speed) || !all_finite(target)) {
        invalidate_movement_checkpoint();
        return;
    }

    g_movement_checkpoint_actor.store(actor, std::memory_order_relaxed);
    g_movement_checkpoint_x.store(
        word_from_float(position.x),
        std::memory_order_relaxed
    );
    g_movement_checkpoint_y.store(
        word_from_float(position.y),
        std::memory_order_relaxed
    );
    g_movement_checkpoint_z.store(
        word_from_float(position.z),
        std::memory_order_relaxed
    );
    g_movement_target_x.store(
        word_from_float(target.x),
        std::memory_order_relaxed
    );
    g_movement_target_y.store(
        word_from_float(target.y),
        std::memory_order_relaxed
    );
    g_movement_target_z.store(
        word_from_float(target.z),
        std::memory_order_relaxed
    );
    g_movement_checkpoint_valid.store(true, std::memory_order_release);
}

bool consume_movement_checkpoint(
    uint32_t actor,
    Vec3& position,
    Vec3* target = nullptr
) {
    if (!g_movement_checkpoint_valid.exchange(
            false,
            std::memory_order_acq_rel)) {
        return false;
    }
    if (g_movement_checkpoint_actor.load(std::memory_order_acquire) != actor) {
        return false;
    }

    position = {
        float_from_word(
            g_movement_checkpoint_x.load(std::memory_order_relaxed)
        ),
        float_from_word(
            g_movement_checkpoint_y.load(std::memory_order_relaxed)
        ),
        float_from_word(
            g_movement_checkpoint_z.load(std::memory_order_relaxed)
        ),
    };
    if (!all_finite(position)) {
        return false;
    }
    if (target != nullptr) {
        *target = {
            float_from_word(g_movement_target_x.load(std::memory_order_relaxed)),
            float_from_word(g_movement_target_y.load(std::memory_order_relaxed)),
            float_from_word(g_movement_target_z.load(std::memory_order_relaxed)),
        };
        if (!all_finite(*target)) {
            return false;
        }
    }
    return true;
}

bool restore_movement_checkpoint(uint8_t* rdram, uint32_t actor) {
    Vec3 position{};
    if (!consume_movement_checkpoint(actor, position)) {
        return false;
    }
    write_actor_vector(rdram, actor, 0x40u, position);
    return true;
}

bool resolve_floor_contact_from_checkpoint(
    uint8_t* rdram,
    uint32_t actor,
    bool& downward_clamped
) {
    downward_clamped = false;
    Vec3 checkpoint{};
    if (!consume_movement_checkpoint(actor, checkpoint)) {
        return false;
    }
    Vec3 integrated = read_actor_vector(rdram, actor, 0x40u);
    if (!all_finite(integrated)) {
        write_actor_vector(rdram, actor, 0x40u, checkpoint);
        return false;
    }

    if (integrated.y < checkpoint.y) {
        integrated.y = checkpoint.y;
        write_actor_vector(rdram, actor, 0x40u, integrated);
        downward_clamped = true;
    }
    return true;
}

void clear_surface_glide(uint32_t actor = 0u) {
    if (actor == 0u) {
        g_surface_glide_actor.store(0u, std::memory_order_release);
        return;
    }
    uint32_t expected_actor = actor;
    g_surface_glide_actor.compare_exchange_strong(
        expected_actor,
        0u,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
}

bool read_surface_glide_height(uint32_t actor, float& height) {
    if (g_surface_glide_actor.load(std::memory_order_acquire) != actor) {
        return false;
    }
    height = float_from_word(
        g_surface_glide_height_y.load(std::memory_order_acquire)
    );
    return std::isfinite(height);
}

bool retain_surface_glide_height(uint8_t* rdram, uint32_t actor) {
    const Vec3 position = read_actor_vector(rdram, actor, 0x40u);
    if (!all_finite(position)) {
        return false;
    }

    float retained_height = position.y;
    float previous_height = 0.0f;
    if (read_surface_glide_height(actor, previous_height)) {
        retained_height = std::max(retained_height, previous_height);
    }
    g_surface_glide_height_y.store(
        word_from_float(retained_height),
        std::memory_order_release
    );
    g_surface_glide_actor.store(actor, std::memory_order_release);
    return true;
}

bool enforce_surface_glide_height(uint8_t* rdram, uint32_t actor) {
    float minimum_height = 0.0f;
    if (!read_surface_glide_height(actor, minimum_height)) {
        return false;
    }
    Vec3 position = read_actor_vector(rdram, actor, 0x40u);
    if (!all_finite(position) || position.y >= minimum_height) {
        return false;
    }
    position.y = minimum_height;
    write_actor_vector(rdram, actor, 0x40u, position);
    return true;
}

void invalidate_collision_safe_anchor() {
    g_collision_safe_anchor_valid.store(false, std::memory_order_release);
}

void capture_collision_safe_anchor(uint8_t* rdram, uint32_t actor) {
    const Vec3 position = read_actor_vector(rdram, actor, 0x40u);
    if (!all_finite(position)) {
        invalidate_collision_safe_anchor();
        return;
    }
    g_collision_safe_anchor_actor.store(actor, std::memory_order_relaxed);
    g_collision_safe_anchor_x.store(
        word_from_float(position.x), std::memory_order_relaxed
    );
    g_collision_safe_anchor_y.store(
        word_from_float(position.y), std::memory_order_relaxed
    );
    g_collision_safe_anchor_z.store(
        word_from_float(position.z), std::memory_order_relaxed
    );
    g_collision_safe_anchor_valid.store(true, std::memory_order_release);
}

bool read_collision_safe_anchor(uint8_t* rdram, uint32_t actor, Vec3& position) {
    if (!g_collision_safe_anchor_valid.load(std::memory_order_acquire) ||
        g_collision_safe_anchor_actor.load(std::memory_order_acquire) != actor) {
        return false;
    }
    position = {
        float_from_word(
            g_collision_safe_anchor_x.load(std::memory_order_relaxed)
        ),
        float_from_word(
            g_collision_safe_anchor_y.load(std::memory_order_relaxed)
        ),
        float_from_word(
            g_collision_safe_anchor_z.load(std::memory_order_relaxed)
        ),
    };
    if (!all_finite(position)) {
        return false;
    }
    float minimum_height = 0.0f;
    if (read_surface_glide_height(actor, minimum_height) &&
        position.y < minimum_height) {
        position.y = minimum_height;
    }
    return true;
}

bool restore_collision_safe_anchor(uint8_t* rdram, uint32_t actor) {
    Vec3 position{};
    if (!read_collision_safe_anchor(rdram, actor, position)) {
        return false;
    }
    write_actor_vector(rdram, actor, 0x40u, position);
    return true;
}

void invalidate_landing_anchor() {
    g_landing_anchor_valid.store(false, std::memory_order_release);
}

void capture_landing_anchor(uint8_t* rdram, uint32_t actor) {
    const Vec3 position = read_actor_vector(rdram, actor, 0x40u);
    if (!all_finite(position)) {
        invalidate_landing_anchor();
        return;
    }

    g_landing_anchor_actor.store(actor, std::memory_order_relaxed);
    g_landing_anchor_x.store(word_from_float(position.x), std::memory_order_relaxed);
    g_landing_anchor_y.store(word_from_float(position.y), std::memory_order_relaxed);
    g_landing_anchor_z.store(word_from_float(position.z), std::memory_order_relaxed);
    g_landing_anchor_valid.store(true, std::memory_order_release);
}

bool restore_landing_anchor(uint8_t* rdram, uint32_t actor) {
    if (!g_landing_anchor_valid.exchange(false, std::memory_order_acq_rel) ||
        g_landing_anchor_actor.load(std::memory_order_acquire) != actor) {
        return false;
    }

    const Vec3 position{
        float_from_word(g_landing_anchor_x.load(std::memory_order_relaxed)),
        float_from_word(g_landing_anchor_y.load(std::memory_order_relaxed)),
        float_from_word(g_landing_anchor_z.load(std::memory_order_relaxed)),
    };
    if (!all_finite(position)) {
        return false;
    }
    write_actor_vector(rdram, actor, 0x40u, position);
    return true;
}

void invalidate_airborne_idle_anchor() {
    g_airborne_idle_anchor_valid.store(false, std::memory_order_release);
}

void capture_airborne_idle_anchor(uint8_t* rdram, uint32_t actor) {
    const Vec3 position = read_actor_vector(rdram, actor, 0x40u);
    if (!all_finite(position)) {
        invalidate_airborne_idle_anchor();
        return;
    }

    g_airborne_idle_anchor_actor.store(actor, std::memory_order_relaxed);
    g_airborne_idle_anchor_x.store(
        word_from_float(position.x),
        std::memory_order_relaxed
    );
    g_airborne_idle_anchor_y.store(
        word_from_float(position.y),
        std::memory_order_relaxed
    );
    g_airborne_idle_anchor_z.store(
        word_from_float(position.z),
        std::memory_order_relaxed
    );
    g_airborne_idle_anchor_valid.store(true, std::memory_order_release);
}

bool restore_airborne_idle_anchor(uint8_t* rdram, uint32_t actor) {
    if (!g_airborne_idle_anchor_valid.load(std::memory_order_acquire) ||
        g_airborne_idle_anchor_actor.load(std::memory_order_acquire) != actor) {
        return false;
    }

    const Vec3 position{
        float_from_word(
            g_airborne_idle_anchor_x.load(std::memory_order_relaxed)
        ),
        float_from_word(
            g_airborne_idle_anchor_y.load(std::memory_order_relaxed)
        ),
        float_from_word(
            g_airborne_idle_anchor_z.load(std::memory_order_relaxed)
        ),
    };
    if (!all_finite(position)) {
        invalidate_airborne_idle_anchor();
        return false;
    }
    write_actor_vector(rdram, actor, 0x40u, position);
    return true;
}

void clear_pending_look() {
    g_pending_look_x.exchange(0, std::memory_order_acq_rel);
    g_pending_look_y.exchange(0, std::memory_order_acq_rel);
}

void clear_manual_flight_actions() {
    g_takeoff_land_held.store(false, std::memory_order_release);
    g_takeoff_land_edge.store(false, std::memory_order_release);
    g_takeoff_land_require_release.store(true, std::memory_order_release);
    g_loop_de_loop_held.store(false, std::memory_order_release);
    g_loop_de_loop_edge.store(false, std::memory_order_release);
    g_loop_de_loop_require_release.store(true, std::memory_order_release);
    g_quick_flip_held.store(false, std::memory_order_release);
    g_quick_flip_edge.store(false, std::memory_order_release);
    g_quick_flip_require_release.store(true, std::memory_order_release);
    g_sprint_held.store(false, std::memory_order_release);
    g_sprint_require_release.store(true, std::memory_order_release);
    g_barrel_roll_held.store(false, std::memory_order_release);
    g_barrel_roll_edge.store(false, std::memory_order_release);
    g_barrel_roll_require_release.store(true, std::memory_order_release);
    g_quick_flip_stage.store(
        static_cast<uint32_t>(QuickFlipStage::Idle),
        std::memory_order_release
    );
    g_quick_flip_actor.store(0u, std::memory_order_release);
    g_quick_flip_timeout_updates.store(0u, std::memory_order_release);
    g_active_maneuver.store(
        static_cast<uint32_t>(ActiveManeuver::None),
        std::memory_order_release
    );
    g_maneuver_actor.store(0u, std::memory_order_release);
    g_maneuver_updates_remaining.store(0u, std::memory_order_release);
    g_barrel_roll_cooldown_updates.store(0u, std::memory_order_release);
    g_loop_de_loop_cooldown_updates.store(0u, std::memory_order_release);
    g_barrel_roll_visual_actor.store(0u, std::memory_order_release);
    g_barrel_roll_visual_angle_bits.store(
        word_from_float(0.0f),
        std::memory_order_release
    );
    g_quick_turn_actor.store(0u, std::memory_order_release);
    g_quick_turn_updates_completed.store(0u, std::memory_order_release);
    g_quick_turn_start_yaw_bits.store(
        word_from_float(0.0f),
        std::memory_order_release
    );
    g_quick_turn_active.store(false, std::memory_order_release);
    {
        std::scoped_lock visual_lock(g_stunt_visual_mutex);
        g_stunt_visual_actor = 0u;
        g_stunt_visual_basis = {};
        g_stunt_visual_euler = {};
        g_stunt_visual_quick_flip = false;
    }
    g_manual_land_request_actor.store(0u, std::memory_order_release);
    g_manual_land_buffer_updates_remaining.store(0u, std::memory_order_release);
    g_manual_landing_authorized_actor.store(0u, std::memory_order_release);
    g_manual_takeoff_request_actor.store(0u, std::memory_order_release);
    g_manual_grounded_entry_actor.store(0, std::memory_order_release);
    g_manual_takeoff_injection_actor.store(0, std::memory_order_release);
    g_manual_takeoff_animation_actor.store(0, std::memory_order_release);
    g_automatic_takeoff_pending_actor.store(0, std::memory_order_release);
    g_automatic_takeoff_animation_actor.store(0, std::memory_order_release);
    invalidate_movement_checkpoint();
    invalidate_landing_anchor();
    invalidate_airborne_idle_anchor();
}

void update_edge_action_button(
    bool pressed,
    std::atomic_bool& held,
    std::atomic_bool& edge,
    std::atomic_bool& require_release
) {
    if (!bumble::modern_controls::enabled()) {
        held.store(false, std::memory_order_release);
        edge.store(false, std::memory_order_release);
        require_release.store(true, std::memory_order_release);
        return;
    }
    if (!bumble::modern_controls::window_focused()) {
        held.store(false, std::memory_order_release);
        edge.store(false, std::memory_order_release);
        require_release.store(true, std::memory_order_release);
        return;
    }
    if (!pressed) {
        held.store(false, std::memory_order_release);
        require_release.store(false, std::memory_order_release);
        return;
    }

    const bool was_held = held.exchange(true, std::memory_order_acq_rel);
    if (!bumble::modern_controls::gameplay_input_active()) {
        require_release.store(true, std::memory_order_release);
        return;
    }
    if (require_release.load(std::memory_order_acquire)) {
        return;
    }
    if (!was_held) {
        edge.store(true, std::memory_order_release);
    }
}

void decrement_update_counter(std::atomic_uint32_t& counter) {
    uint32_t remaining = counter.load(std::memory_order_acquire);
    while (remaining > 0u &&
        !counter.compare_exchange_weak(
            remaining,
            remaining - 1u,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
    }
}

bool begin_quick_turn(uint32_t actor);
bool advance_quick_turn(uint32_t actor);

void cancel_quick_turn() {
    g_quick_turn_active.store(false, std::memory_order_release);
    g_quick_turn_actor.store(0u, std::memory_order_release);
    g_quick_turn_updates_completed.store(0u, std::memory_order_release);
}

bool quick_turn_is_active_for(uint32_t actor) {
    return g_quick_turn_active.load(std::memory_order_acquire) &&
        g_quick_turn_actor.load(std::memory_order_acquire) == actor;
}

void clear_stunt_visual(uint32_t actor = 0u) {
    std::scoped_lock lock(g_stunt_visual_mutex);
    if (actor != 0u && g_stunt_visual_actor != actor) {
        return;
    }
    g_stunt_visual_actor = 0u;
    g_stunt_visual_basis = {};
    g_stunt_visual_euler = {};
    g_stunt_visual_quick_flip = false;
}

bool restore_guest_stunt_pose(uint8_t* rdram, uint32_t actor) {
    std::array<Vec3, 3> basis{};
    std::array<float, 3> euler{};
    {
        std::scoped_lock lock(g_stunt_visual_mutex);
        if (g_stunt_visual_actor != actor) {
            return false;
        }
        basis = g_stunt_visual_basis;
        euler = g_stunt_visual_euler;
    }
    if (!all_finite(basis[0]) || !all_finite(basis[1]) ||
        !all_finite(basis[2]) || !std::isfinite(euler[0]) ||
        !std::isfinite(euler[1]) || !std::isfinite(euler[2])) {
        clear_stunt_visual(actor);
        return false;
    }
    write_actor_vector(rdram, actor, 0x10u, basis[0]);
    write_actor_vector(rdram, actor, 0x20u, basis[1]);
    write_actor_vector(rdram, actor, 0x30u, basis[2]);
    write_u32(rdram, actor + 0x50u, word_from_float(euler[0]));
    write_u32(rdram, actor + 0x54u, word_from_float(euler[1]));
    write_u32(rdram, actor + 0x58u, word_from_float(euler[2]));
    return true;
}

void clear_quick_flip_sequence() {
    g_quick_flip_stage.store(
        static_cast<uint32_t>(QuickFlipStage::Idle),
        std::memory_order_release
    );
    g_quick_flip_actor.store(0u, std::memory_order_release);
    g_quick_flip_timeout_updates.store(0u, std::memory_order_release);
}

void cancel_active_maneuver() {
    g_active_maneuver.store(
        static_cast<uint32_t>(ActiveManeuver::None),
        std::memory_order_release
    );
    g_maneuver_actor.store(0u, std::memory_order_release);
    g_maneuver_updates_remaining.store(0u, std::memory_order_release);
    g_barrel_roll_visual_actor.store(0u, std::memory_order_release);
    g_barrel_roll_visual_angle_bits.store(
        word_from_float(0.0f),
        std::memory_order_release
    );
}

bool live_contact_actor(uint8_t* rdram, uint32_t actor, uint16_t id,
    uint32_t descriptor) {
    const uint32_t entry = kCollisionActorLookup +
        (static_cast<uint32_t>(id) & kCollisionActorLookupMask) * 8u;
    return valid_guest_pointer(actor, 0x90u) &&
        read_u16(rdram, entry) == id && read_u32(rdram, entry + 4u) == actor &&
        read_u16(rdram, actor + 0x7Eu) == id &&
        read_u32(rdram, actor + 0x88u) == descriptor;
}

bool live_web_box_restraint(uint8_t* rdram, uint32_t player) {
    if (g_web_box_contact_count == 0u) {
        return false;
    }
    if (player != g_web_box_player ||
        read_u32(rdram, kPlayerOneOwnerSlot) != player ||
        !live_contact_actor(rdram, player, g_web_box_player_id, kPlayerVtable)) {
        g_web_box_contact_count = 0u;
        return false;
    }
    const Vec3 position = read_actor_vector(rdram, player, 0x40u);
    size_t kept = 0u;
    for (size_t index = 0u; index < g_web_box_contact_count; ++index) {
        const WebBoxContact contact = g_web_box_contacts[index];
        if (!live_contact_actor(rdram, contact.actor, contact.id, kWebBoxVtable)) {
            continue;
        }
        const Vec3 box = read_actor_vector(rdram, contact.actor, 0x40u);
        if (std::abs(position.x - box.x) <= 240.0f &&
            std::abs(position.z - box.z) <= 240.0f &&
            position.y - box.y > -240.0f && position.y - box.y < 240.0f) {
            g_web_box_contacts[kept++] = contact;
        }
    }
    g_web_box_contact_count = kept;
    return kept != 0u;
}

void inject_normalized_quick_flip_press(uint8_t* rdram) {
    (void)rdram;
    const uint16_t pressed = static_cast<uint16_t>(
        MEM_HU(2, guest_address(kPlayerNormalizedPad))
    );
    MEM_H(2, guest_address(kPlayerNormalizedPad)) =
        static_cast<int16_t>(pressed | kCDown);
}

bool quick_flip_sequence_active() {
    return g_quick_flip_stage.load(std::memory_order_acquire) !=
        static_cast<uint32_t>(QuickFlipStage::Idle);
}

void process_quick_flip_action(
    uint8_t* rdram,
    uint32_t actor,
    uint32_t state
) {
    const bool pressed_edge =
        g_quick_flip_edge.exchange(false, std::memory_order_acq_rel);
    const ActiveManeuver maneuver = static_cast<ActiveManeuver>(
        g_active_maneuver.load(std::memory_order_acquire)
    );
    if (state != kFlyingState || maneuver != ActiveManeuver::None) {
        if (pressed_edge) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=modern_quick_flip_rejected"
                " actor=0x%08" PRIX32 " state=%" PRIu32
                " reason=%s\n",
                actor,
                state,
                state != kFlyingState ? "not_flying" : "maneuver_active"
            );
            std::fflush(stderr);
        }
        clear_quick_flip_sequence();
        if (state != kFlyingState) {
            cancel_quick_turn();
        }
        return;
    }

    if (quick_flip_sequence_active() &&
        g_quick_flip_actor.load(std::memory_order_acquire) != actor) {
        clear_quick_flip_sequence();
    }

    const uint32_t flip_flag = read_u32(rdram, actor + 0xB4u);
    const uint16_t loop_timer = read_u16(rdram, actor + 0xB8u);
    const QuickFlipStage stage = static_cast<QuickFlipStage>(
        g_quick_flip_stage.load(std::memory_order_acquire)
    );
    if (stage == QuickFlipStage::AwaitGuestCommit) {
        if (flip_flag != 0u) {
            clear_quick_flip_sequence();
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=modern_quick_flip_committed"
                " actor=0x%08" PRIX32 " guest_flip_flag=%" PRIu32
                " guest_loop_timer=%u source=original_player_update\n",
                actor,
                flip_flag,
                static_cast<unsigned>(loop_timer)
            );
            std::fflush(stderr);
            return;
        }
        uint32_t commit_timeout =
            g_quick_flip_timeout_updates.load(std::memory_order_acquire);
        while (commit_timeout > 0u &&
            !g_quick_flip_timeout_updates.compare_exchange_weak(
                commit_timeout,
                commit_timeout - 1u,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
        }
        if (commit_timeout <= 1u) {
            clear_quick_flip_sequence();
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=modern_quick_flip_timeout"
                " actor=0x%08" PRIX32
                " state=%" PRIu32
                " loop_timer=%u reason=guest_commit_missing"
                " direct_state_write=0\n",
                actor,
                state,
                static_cast<unsigned>(loop_timer)
            );
            std::fflush(stderr);
        }
        return;
    }
    if (pressed_edge) {
        if (flip_flag != 0u) {
            clear_quick_flip_sequence();
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=modern_quick_flip_rejected"
                " actor=0x%08" PRIX32 " state=%" PRIu32
                " reason=quick_flip_already_active\n",
                actor,
                state
            );
            std::fflush(stderr);
            return;
        }
        g_quick_flip_actor.store(actor, std::memory_order_relaxed);
        g_quick_flip_timeout_updates.store(
            kQuickFlipTimeoutUpdates,
            std::memory_order_relaxed
        );
        g_quick_flip_stage.store(
            static_cast<uint32_t>(QuickFlipStage::AwaitFlipWindow),
            std::memory_order_release
        );
    }

    if (!quick_flip_sequence_active()) {
        return;
    }
    if (flip_flag != 0u) {
        clear_quick_flip_sequence();
        return;
    }

    if (loop_timer == 0u) {
        inject_normalized_quick_flip_press(rdram);
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_quick_flip_started"
            " actor=0x%08" PRIX32
            " source=normalized_pressed_c_down"
            " original_loop_animation=1 direct_state_write=0\n",
            actor
        );
        std::fflush(stderr);
    } else if (loop_timer >= kQuickFlipEntryWindowFirst &&
        loop_timer <= kQuickFlipEntryWindowLast) {
        inject_normalized_quick_flip_press(rdram);
        const bool camera_turn_started = begin_quick_turn(actor);
        g_quick_flip_timeout_updates.store(4u, std::memory_order_relaxed);
        g_quick_flip_stage.store(
            static_cast<uint32_t>(QuickFlipStage::AwaitGuestCommit),
            std::memory_order_release
        );
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_quick_flip_triggered"
            " actor=0x%08" PRIX32 " entry_loop_timer=%u"
            " guest_window_after_decrement=%u"
            " source=normalized_pressed_c_down direct_state_write=0"
            " camera_turn_started=%d camera_curve=half_cosine"
            " camera_heading_delta=180.000\n",
            actor,
            static_cast<unsigned>(loop_timer),
            static_cast<unsigned>(loop_timer - 1u),
            camera_turn_started ? 1 : 0
        );
        std::fflush(stderr);
        return;
    }

    uint32_t timeout =
        g_quick_flip_timeout_updates.load(std::memory_order_acquire);
    while (timeout > 0u &&
        !g_quick_flip_timeout_updates.compare_exchange_weak(
            timeout,
            timeout - 1u,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
    }
    if (timeout <= 1u) {
        clear_quick_flip_sequence();
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_quick_flip_timeout"
            " actor=0x%08" PRIX32 " state=%" PRIu32
            " loop_timer=%u direct_state_write=0\n",
            actor,
            state,
            static_cast<unsigned>(loop_timer)
        );
        std::fflush(stderr);
    }
}

bool begin_active_maneuver(
    uint32_t actor,
    ActiveManeuver maneuver
) {
    Vec3 direction{};
    Vec3 loop_up{};
    int32_t roll_direction = 1;
    {
        std::scoped_lock lock(g_aim_mutex);
        if (g_resync_aim.load(std::memory_order_acquire) ||
            !g_aim_ready || g_aim_actor != actor) {
            return false;
        }

        if (maneuver == ActiveManeuver::LoopDeLoop) {
            if (!normalize(multiply(g_aim_forward, -1.0f), direction)) {
                return false;
            }
            const Vec3 orthogonal_up = add(
                g_aim_up,
                multiply(direction, -dot(g_aim_up, direction))
            );
            if (!normalize(orthogonal_up, loop_up)) {
                return false;
            }
        } else if (maneuver == ActiveManeuver::BarrelRoll) {
            const float strafe =
                static_cast<float>(
                    g_movement_strafe.load(std::memory_order_acquire)
                ) / 32767.0f;
            if (std::abs(strafe) >= 0.2f) {
                roll_direction = strafe > 0.0f ? 1 : -1;
            } else {
                roll_direction = g_last_strafe_direction.load(
                    std::memory_order_acquire
                );
                if (roll_direction == 0) {
                    roll_direction = 1;
                }
            }
            // Capsule strafe uses +aim_right; guest movement uses the opposite sign.
            if (!normalize(
                    multiply(
                        g_aim_right,
                        static_cast<float>(roll_direction)
                    ),
                    direction)) {
                return false;
            }
        } else {
            return false;
        }
    }

    g_maneuver_direction_x.store(
        word_from_float(direction.x),
        std::memory_order_relaxed
    );
    g_maneuver_direction_y.store(
        word_from_float(direction.y),
        std::memory_order_relaxed
    );
    g_maneuver_direction_z.store(
        word_from_float(direction.z),
        std::memory_order_relaxed
    );
    g_maneuver_up_x.store(
        word_from_float(loop_up.x),
        std::memory_order_relaxed
    );
    g_maneuver_up_y.store(
        word_from_float(loop_up.y),
        std::memory_order_relaxed
    );
    g_maneuver_up_z.store(
        word_from_float(loop_up.z),
        std::memory_order_relaxed
    );
    g_maneuver_actor.store(actor, std::memory_order_relaxed);
    if (maneuver == ActiveManeuver::BarrelRoll) {
        g_barrel_roll_visual_direction.store(
            roll_direction,
            std::memory_order_relaxed
        );
        g_barrel_roll_visual_angle_bits.store(
            word_from_float(0.0f),
            std::memory_order_relaxed
        );
        g_barrel_roll_visual_actor.store(actor, std::memory_order_relaxed);
        g_maneuver_updates_remaining.store(
            kBarrelRollDurationUpdates,
            std::memory_order_relaxed
        );
        g_barrel_roll_cooldown_updates.store(
            kBarrelRollDurationUpdates + kBarrelRollCooldownUpdates,
            std::memory_order_release
        );
    } else if (maneuver == ActiveManeuver::LoopDeLoop) {
        g_maneuver_updates_remaining.store(
            kLoopDeLoopDurationUpdates,
            std::memory_order_relaxed
        );
        g_loop_de_loop_cooldown_updates.store(
            kLoopDeLoopDurationUpdates + kLoopDeLoopCooldownUpdates,
            std::memory_order_release
        );
    }
    g_active_maneuver.store(
        static_cast<uint32_t>(maneuver),
        std::memory_order_release
    );

    if (maneuver == ActiveManeuver::LoopDeLoop) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_loop_de_loop_started"
            " actor=0x%08" PRIX32
            " duration_updates=%" PRIu32 " cooldown_after_updates=%" PRIu32
            " radius=%.3f vertical_excursion=%.3f forward_advance=%.3f"
            " curve=half_cosine_path"
            " forward=(%.4f,%.4f,%.4f) up=(%.4f,%.4f,%.4f)"
            " collision=guest_bsp_integrator invulnerability=0 teleport=0\n",
            actor,
            kLoopDeLoopDurationUpdates,
            kLoopDeLoopCooldownUpdates,
            static_cast<double>(kLoopDeLoopRadius),
            static_cast<double>(kLoopDeLoopRadius * 2.0f),
            static_cast<double>(kLoopDeLoopForwardAdvance),
            static_cast<double>(direction.x),
            static_cast<double>(direction.y),
            static_cast<double>(direction.z),
            static_cast<double>(loop_up.x),
            static_cast<double>(loop_up.y),
            static_cast<double>(loop_up.z)
        );
        std::fflush(stderr);
        return true;
    }

    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=modern_barrel_roll_started"
        " actor=0x%08" PRIX32
        " duration_updates=%" PRIu32 " cooldown_after_updates=%" PRIu32
        " total_distance=%.3f curve=half_sine_velocity"
        " direction=(%.4f,%.4f,%.4f)"
        " collision=guest_bsp_integrator invulnerability=0 teleport=0\n",
        actor,
        kBarrelRollDurationUpdates,
        kBarrelRollCooldownUpdates,
        static_cast<double>(kBarrelRollTotalDistance),
        static_cast<double>(direction.x),
        static_cast<double>(direction.y),
        static_cast<double>(direction.z)
    );
    std::fflush(stderr);
    return true;
}

void suppress_normalized_loop_press(uint8_t* rdram) {
    (void)rdram;
    const uint16_t held = static_cast<uint16_t>(
        MEM_HU(0, guest_address(kPlayerNormalizedPad))
    );
    const uint16_t pressed = static_cast<uint16_t>(
        MEM_HU(2, guest_address(kPlayerNormalizedPad))
    );
    MEM_H(0, guest_address(kPlayerNormalizedPad)) =
        static_cast<int16_t>(held & ~kCDown);
    MEM_H(2, guest_address(kPlayerNormalizedPad)) =
        static_cast<int16_t>(pressed & ~kCDown);
}

void process_new_maneuver_actions(
    uint8_t* rdram,
    uint32_t actor,
    uint32_t state
) {
    decrement_update_counter(g_barrel_roll_cooldown_updates);
    decrement_update_counter(g_loop_de_loop_cooldown_updates);
    const bool web_restrained = live_web_box_restraint(rdram, actor);
    if (web_restrained && g_active_maneuver.load(std::memory_order_acquire) ==
            static_cast<uint32_t>(ActiveManeuver::BarrelRoll)) {
        cancel_active_maneuver();
    }

    if (g_active_maneuver.load(std::memory_order_acquire) ==
            static_cast<uint32_t>(ActiveManeuver::None) &&
        g_maneuver_updates_remaining.load(std::memory_order_acquire) == 0u &&
        g_barrel_roll_visual_actor.load(std::memory_order_acquire) != 0u) {
        g_barrel_roll_visual_actor.store(0u, std::memory_order_release);
        g_barrel_roll_visual_angle_bits.store(
            word_from_float(0.0f),
            std::memory_order_release
        );
    }

    const bool roll_edge =
        g_barrel_roll_edge.exchange(false, std::memory_order_acq_rel);
    const bool loop_edge =
        g_loop_de_loop_edge.exchange(false, std::memory_order_acq_rel);

    const ActiveManeuver active = static_cast<ActiveManeuver>(
        g_active_maneuver.load(std::memory_order_acquire)
    );
    const uint32_t active_actor =
        g_maneuver_actor.load(std::memory_order_acquire);
    if (active != ActiveManeuver::None &&
        (state != kFlyingState || active_actor != actor)) {
        cancel_active_maneuver();
    }

    if (!roll_edge && !loop_edge) {
        return;
    }

    const ActiveManeuver current = static_cast<ActiveManeuver>(
        g_active_maneuver.load(std::memory_order_acquire)
    );
    const bool stunt_busy = read_u16(rdram, actor + 0xB8u) != 0u ||
        read_u32(rdram, actor + 0xB4u) != 0u ||
        quick_flip_sequence_active();
    if (state != kFlyingState || current != ActiveManeuver::None ||
        stunt_busy) {
        if (loop_edge) {
            suppress_normalized_loop_press(rdram);
        }
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_maneuver_rejected"
            " actor=0x%08" PRIX32 " state=%" PRIu32
            " barrel_roll=%d loop_de_loop=%d reason=%s\n",
            actor,
            state,
            roll_edge ? 1 : 0,
            loop_edge ? 1 : 0,
            state != kFlyingState ? "not_flying" :
                (current != ActiveManeuver::None
                    ? "maneuver_active"
                    : "original_stunt_active")
        );
        std::fflush(stderr);
        return;
    }

    if (loop_edge &&
        g_loop_de_loop_cooldown_updates.load(std::memory_order_acquire) == 0u) {
        if (begin_active_maneuver(actor, ActiveManeuver::LoopDeLoop)) {
            return;
        }
    }
    if (roll_edge && !web_restrained &&
        g_barrel_roll_cooldown_updates.load(std::memory_order_acquire) == 0u) {
        if (begin_active_maneuver(actor, ActiveManeuver::BarrelRoll)) {
            if (loop_edge) {
                suppress_normalized_loop_press(rdram);
            }
            return;
        }
    }
    if (loop_edge) {
        suppress_normalized_loop_press(rdram);
    }

    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=modern_maneuver_rejected"
        " actor=0x%08" PRIX32 " state=%" PRIu32
        " barrel_roll=%d loop_de_loop=%d reason=%s"
        " barrel_roll_cooldown=%" PRIu32
        " loop_de_loop_cooldown=%" PRIu32 "\n",
        actor,
        state,
        roll_edge ? 1 : 0,
        loop_edge ? 1 : 0,
        (roll_edge &&
                g_barrel_roll_cooldown_updates.load(
                    std::memory_order_acquire
                ) != 0u) ||
            (loop_edge &&
                g_loop_de_loop_cooldown_updates.load(
                    std::memory_order_acquire
                ) != 0u)
            ? "cooldown"
            : web_restrained && roll_edge ? "web_box_contact" : "aim_not_ready",
        g_barrel_roll_cooldown_updates.load(std::memory_order_acquire),
        g_loop_de_loop_cooldown_updates.load(std::memory_order_acquire)
    );
    std::fflush(stderr);
}

void clear_frontend_confirm_action(bool clear_latch) {
    g_frontend_confirm_pending.store(false, std::memory_order_release);
    g_frontend_confirm_held.store(false, std::memory_order_release);
    g_frontend_confirm_edge.store(false, std::memory_order_release);
    g_frontend_confirm_require_release.store(true, std::memory_order_release);
    if (clear_latch) {
        g_frontend_confirm_latched.store(false, std::memory_order_release);
    }
}

void add_saturated(std::atomic_int64_t& destination, int64_t delta) {
    int64_t observed = destination.load(std::memory_order_relaxed);
    while (true) {
        const int64_t desired = std::clamp(
            observed + delta,
            -kMaximumQueuedLookCounts,
            kMaximumQueuedLookCounts
        );
        if (destination.compare_exchange_weak(
                observed,
                desired,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return;
        }
    }
}

bool living_player_one_actor_is_owned(uint8_t* rdram, uint32_t actor) {
    // At zero health, release flight control so Buck can fall and enter the death states.
    if (rdram == nullptr) return false;
    const float health = float_from_word(read_u32(rdram, kPlayerOneHealth));
    return std::isfinite(health) && health > 0.0f &&
        valid_guest_pointer(actor, 0xFCu) &&
        read_u32(rdram, kPlayerOneOwnerSlot) == actor &&
        read_u32(rdram, actor + 0x88u) == kPlayerVtable &&
        MEM_BU(0xF4, guest_address(actor)) == 0u;
}

uint32_t exact_guided_missile_for_player(
    uint8_t* rdram,
    uint32_t player_actor
) {
    if (rdram == nullptr ||
        !living_player_one_actor_is_owned(rdram, player_actor)) {
        return 0u;
    }

    const uint32_t missile = read_u32(rdram, kGuidedMissilePointer);
    if (!valid_guest_pointer(missile, 0xA0u) ||
        read_u32(rdram, missile + 0x88u) != kMissileVtable ||
        read_u32(rdram, missile + 0x8Cu) != 1u ||
        read_u32(rdram, missile + 0x9Cu) != 1u ||
        read_u16(rdram, missile + 0x80u) !=
            read_u16(rdram, player_actor + 0x7Eu)) {
        return 0u;
    }
    return missile;
}

void begin_guided_missile_control(
    uint32_t player_actor,
    uint32_t missile_actor
) {
    const uint32_t previous = g_guided_missile_actor.exchange(
        missile_actor,
        std::memory_order_acq_rel
    );
    g_guided_missile_player_actor.store(
        player_actor,
        std::memory_order_release
    );
    if (previous == missile_actor) {
        return;
    }

    clear_manual_flight_actions();
    cancel_active_maneuver();
    g_resync_aim.store(true, std::memory_order_release);
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=modern_guided_missile_control_attached"
        " player=0x%08" PRIX32 " missile=0x%08" PRIX32
        " player_motion=hover camera_owner=game_guided_solver"
        " steering=modern_look\n",
        player_actor,
        missile_actor
    );
    std::fflush(stderr);
}

void finish_guided_missile_control_if_stale(
    uint8_t* rdram,
    uint32_t player_actor
) {
    const uint32_t tracked_player = g_guided_missile_player_actor.load(
        std::memory_order_acquire
    );
    const uint32_t tracked_missile = g_guided_missile_actor.load(
        std::memory_order_acquire
    );
    if (tracked_missile == 0u || tracked_player != player_actor ||
        exact_guided_missile_for_player(rdram, player_actor) != 0u) {
        return;
    }

    g_guided_missile_actor.store(0u, std::memory_order_release);
    g_guided_missile_player_actor.store(0u, std::memory_order_release);
    clear_pending_look();
    g_resync_aim.store(true, std::memory_order_release);
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=modern_guided_missile_control_released"
        " player=0x%08" PRIX32 " missile=0x%08" PRIX32
        " player_aim_resync=1 stale_missile_delta_discarded=1\n",
        player_actor,
        tracked_missile
    );
    std::fflush(stderr);
}

bool exact_teleport_actor_is_live(uint8_t* rdram, uint32_t actor) {
    return valid_guest_pointer(actor, 0x98u) &&
        read_u32(rdram, actor + 0x88u) == kTeleportVtable &&
        (read_u32(rdram, actor + 0x64u) & kTeleportCollisionCategory) != 0u;
}

bool teleport_actor_is_temporarily_disabled(
    uint8_t* rdram,
    uint32_t actor,
    int64_t& deadline_ns
) {
    if (!bumble::modern_controls::enabled() || rdram == nullptr ||
        !g_gameplay_active.load(std::memory_order_acquire) ||
        !exact_teleport_actor_is_live(rdram, actor) ||
        actor != g_teleport_cooldown_actor.load(std::memory_order_acquire) ||
        read_u32(rdram, actor + 0x90u) !=
            g_teleport_cooldown_pair_id.load(std::memory_order_acquire)) {
        return false;
    }

    deadline_ns = g_teleport_cooldown_deadline_ns.load(
        std::memory_order_acquire
    );
    return deadline_ns > 0 && monotonic_now_ns() < deadline_ns;
}

bool append_teleport_visual_marker(
    uint8_t* rdram,
    bool disabled,
    uint32_t required_bytes_after_marker
) {
    if (rdram == nullptr || !valid_guest_pointer(kDisplayListCursor, 4u)) {
        return false;
    }

    const uint32_t cursor = read_u32(rdram, kDisplayListCursor);
    const uint32_t arena_base =
        bumble::widescreen::display_list_arena_base();
    const uint32_t arena_end =
        bumble::widescreen::display_list_arena_end();
    const uint32_t required_bytes =
        kGfxCommandBytes + required_bytes_after_marker;
    if (arena_base == 0u || arena_end <= arena_base ||
        cursor < arena_base || cursor > arena_end ||
        arena_end - cursor < required_bytes ||
        !valid_guest_pointer(cursor, kGfxCommandBytes)) {
        return false;
    }

    write_u32(
        rdram,
        cursor,
        (kExtendedGbiOpcode << 24) | kTeleportDisabledTintMarker
    );
    write_u32(rdram, cursor + 4u, disabled ? 1u : 0u);
    write_u32(rdram, kDisplayListCursor, cursor + kGfxCommandBytes);
    return true;
}

uint32_t teleport_actor_from_collision_record(
    uint8_t* rdram,
    uint32_t collision_record
) {
    if (rdram == nullptr || !valid_guest_pointer(collision_record, 0x10u) ||
        read_u32(rdram, collision_record + 0x08u) !=
            kTeleportCollisionCategory) {
        return 0u;
    }

    const uint16_t collision_id = read_u16(rdram, collision_record + 0x0Cu);
    const uint32_t entry = kCollisionActorLookup +
        (static_cast<uint32_t>(collision_id) & kCollisionActorLookupMask) * 8u;
    if (read_u16(rdram, entry) != collision_id) {
        return 0u;
    }
    const uint32_t actor = read_u32(rdram, entry + 0x04u);
    return exact_teleport_actor_is_live(rdram, actor) ? actor : 0u;
}

bool gameplay_control_owner_active() {
    return g_window_focused.load(std::memory_order_acquire) ||
        g_replay_automation.load(std::memory_order_acquire);
}

bool player_one_actor_is_owned(uint8_t* rdram, uint32_t actor) {
    return living_player_one_actor_is_owned(rdram, actor) &&
        read_u32(rdram, actor + 0xF8u) == kPlayerOneCamera;
}

bool state_supports_modern_control(uint32_t state) {
    return state == kFlyingState || state == kAirborneCollisionState ||
        state == kGroundedEntryState ||
        state == 6u || state == 7u;
}

bool is_grounded_state(uint32_t state) {
    return state >= kGroundedEntryState && state <= kGroundedLastState;
}

bool modern_player_hook_is_owned(
    uint8_t* rdram,
    uint32_t actor,
    uint32_t expected_state = 0u
) {
    if (!bumble::modern_controls::enabled() ||
        !g_gameplay_active.load(std::memory_order_acquire) ||
        !player_one_actor_is_owned(rdram, actor)) {
        return false;
    }
    return expected_state == 0u ||
        read_u32(rdram, actor + 0x8Cu) == expected_state;
}

bool modern_state2_update_is_owned(uint8_t* rdram, uint32_t actor) {
    return living_player_one_actor_is_owned(rdram, actor) &&
        bumble::modern_controls::enabled() &&
        g_gameplay_active.load(std::memory_order_acquire) &&
        g_player_update_actor.load(std::memory_order_acquire) == actor &&
        g_player_update_entry_state.load(std::memory_order_acquire) ==
            kFlyingState;
}

void prepare_buffered_landing_request_for_update(
    uint32_t actor,
    uint32_t state
) {
    const uint32_t stale_authorization =
        g_manual_landing_authorized_actor.exchange(
            0u,
            std::memory_order_acq_rel
        );
    if (stale_authorization != 0u) {
        invalidate_landing_anchor();
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_manual_land_authorization_expired"
            " actor=0x%08" PRIX32 " authorization_actor=0x%08" PRIX32
            " reason=previous_player_update_did_not_commit\n",
            actor,
            stale_authorization
        );
        std::fflush(stderr);
    }

    const uint32_t request_actor =
        g_manual_land_request_actor.load(std::memory_order_acquire);
    const uint32_t remaining =
        g_manual_land_buffer_updates_remaining.load(std::memory_order_acquire);
    if (request_actor == 0u ||
        (request_actor == actor && state == kFlyingState && remaining > 0u)) {
        return;
    }

    uint32_t expected_actor = request_actor;
    if (!g_manual_land_request_actor.compare_exchange_strong(
            expected_actor,
            0u,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }
    g_manual_land_buffer_updates_remaining.store(0u, std::memory_order_release);
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=modern_manual_land_request_expired"
        " actor=0x%08" PRIX32 " request_actor=0x%08" PRIX32
        " observed_state=%" PRIu32 " remaining_updates=%" PRIu32
        " reason=actor_or_state_changed position_write=0\n",
        actor,
        request_actor,
        state,
        remaining
    );
    std::fflush(stderr);
}

uint32_t age_buffered_landing_request(uint32_t actor) {
    if (g_manual_land_request_actor.load(std::memory_order_acquire) != actor) {
        return 0u;
    }

    uint32_t remaining =
        g_manual_land_buffer_updates_remaining.load(std::memory_order_acquire);
    while (remaining > 0u) {
        const uint32_t next = remaining - 1u;
        if (!g_manual_land_buffer_updates_remaining.compare_exchange_weak(
                remaining,
                next,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            continue;
        }
        if (next == 0u) {
            uint32_t expected_actor = actor;
            const bool expired =
                g_manual_land_request_actor.compare_exchange_strong(
                    expected_actor,
                    0u,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire
                );
            if (expired) {
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=modern_manual_land_request_expired"
                    " actor=0x%08" PRIX32
                    " reason=input_buffer_timeout remaining_updates=0"
                    " position_write=0 maximum_snap_distance=0\n",
                    actor
                );
                std::fflush(stderr);
            }
        }
        return next;
    }
    return 0u;
}

void consume_manual_flight_edge(uint32_t actor, uint32_t state) {
    if (!g_takeoff_land_edge.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    g_manual_landing_authorized_actor.store(0u, std::memory_order_release);
    invalidate_landing_anchor();
    if (state == kFlyingState) {
        g_manual_takeoff_request_actor.store(0u, std::memory_order_release);
        g_manual_land_buffer_updates_remaining.store(
            kLandingInputBufferUpdates,
            std::memory_order_relaxed
        );
        g_manual_land_request_actor.store(actor, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_manual_land_press_received"
            " actor=0x%08" PRIX32 " state=%" PRIu32
            " decision_pc=0x8005B14C"
            " policy=buffer_until_native_capsule_support"
            " buffer_updates=%" PRIu32
            " maximum_snap_distance=0 position_write=0\n",
            actor,
            state,
            kLandingInputBufferUpdates
        );
        std::fflush(stderr);
    } else if (is_grounded_state(state)) {
        g_manual_land_request_actor.store(0u, std::memory_order_release);
        g_manual_land_buffer_updates_remaining.store(
            0u,
            std::memory_order_release
        );
        g_manual_takeoff_request_actor.store(actor, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_manual_takeoff_armed"
            " actor=0x%08" PRIX32 " state=%" PRIu32 "\n",
            actor,
            state
        );
        std::fflush(stderr);
    } else {
        g_manual_land_request_actor.store(0u, std::memory_order_release);
        g_manual_land_buffer_updates_remaining.store(
            0u,
            std::memory_order_release
        );
        g_manual_takeoff_request_actor.store(0u, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_manual_flight_action_noop"
            " actor=0x%08" PRIX32 " state=%" PRIu32
            " reason=state_not_landable_or_grounded\n",
            actor,
            state
        );
        std::fflush(stderr);
    }
}

void clear_stale_manual_flight_requests(uint32_t actor, uint32_t state) {
    bool cleared_takeoff = false;
    if (!is_grounded_state(state)) {
        uint32_t expected_actor = actor;
        cleared_takeoff = g_manual_takeoff_request_actor.compare_exchange_strong(
            expected_actor,
            0u,
            std::memory_order_acq_rel
        );
        if (cleared_takeoff) {
            g_manual_takeoff_injection_actor.store(0u, std::memory_order_release);
        }
    }
    if (cleared_takeoff) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_manual_flight_request_cleared"
            " actor=0x%08" PRIX32 " state=%" PRIu32
            " land=0 takeoff=1 reason=state_transition\n",
            actor,
            state
        );
        std::fflush(stderr);
    }
}

void rebuild_aim_vectors_from_angles() {
    const float pitch = g_aim_pitch * kBuckAngleRadiansPerDegree;
    const float yaw = g_aim_yaw * kBuckAngleRadiansPerDegree;
    const float sine_pitch = std::sin(pitch);
    const float cosine_pitch = std::cos(pitch);
    const float sine_yaw = std::sin(yaw);
    const float cosine_yaw = std::cos(yaw);

    // Match func_80052C28's roll-zero basis.
    g_aim_right = {cosine_yaw, 0.0f, sine_yaw};
    g_aim_up = {
        sine_yaw * sine_pitch,
        cosine_pitch,
        -cosine_yaw * sine_pitch,
    };
    g_aim_forward = {
        -cosine_pitch * sine_yaw,
        sine_pitch,
        cosine_pitch * cosine_yaw,
    };
}

bool begin_quick_turn(uint32_t actor) {
    std::scoped_lock lock(g_aim_mutex);
    if (g_resync_aim.load(std::memory_order_acquire) || !g_aim_ready ||
        g_aim_actor != actor || !std::isfinite(g_aim_yaw)) {
        return false;
    }
    g_quick_turn_start_yaw_bits.store(
        word_from_float(g_aim_yaw),
        std::memory_order_relaxed
    );
    g_quick_turn_updates_completed.store(0u, std::memory_order_relaxed);
    g_quick_turn_actor.store(actor, std::memory_order_relaxed);
    g_quick_turn_active.store(true, std::memory_order_release);
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=modern_quick_turn_camera_started"
        " actor=0x%08" PRIX32 " duration_updates=%" PRIu32
        " start_yaw=%.3f heading_delta=%.3f"
        " curve=half_cosine camera_snap=0\n",
        actor,
        kQuickTurnDurationUpdates,
        static_cast<double>(g_aim_yaw),
        static_cast<double>(kQuickTurnHeadingDegrees)
    );
    std::fflush(stderr);
    return true;
}

bool advance_quick_turn(uint32_t actor) {
    if (!quick_turn_is_active_for(actor)) {
        return false;
    }

    uint32_t completed = g_quick_turn_updates_completed.load(
        std::memory_order_acquire
    );
    while (completed < kQuickTurnDurationUpdates &&
        !g_quick_turn_updates_completed.compare_exchange_weak(
            completed,
            completed + 1u,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
    }
    if (completed >= kQuickTurnDurationUpdates) {
        return true;
    }

    const uint32_t next_completed = completed + 1u;
    const float progress = half_cosine_progress(
        next_completed,
        kQuickTurnDurationUpdates
    );
    const float start_yaw = float_from_word(
        g_quick_turn_start_yaw_bits.load(std::memory_order_acquire)
    );
    if (!std::isfinite(start_yaw) || !std::isfinite(progress)) {
        cancel_quick_turn();
        return false;
    }
    {
        std::scoped_lock lock(g_aim_mutex);
        if (g_resync_aim.load(std::memory_order_acquire) || !g_aim_ready ||
            g_aim_actor != actor) {
            cancel_quick_turn();
            return false;
        }
        g_aim_yaw = std::remainder(
            start_yaw + kQuickTurnHeadingDegrees * progress,
            360.0f
        );
        rebuild_aim_vectors_from_angles();
    }

    if (next_completed == 1u ||
        next_completed == kQuickTurnDurationUpdates / 2u ||
        next_completed == kQuickTurnDurationUpdates) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_quick_turn_camera_step"
            " actor=0x%08" PRIX32 " step=%" PRIu32 "/%" PRIu32
            " progress=%.6f yaw=%.3f curve=half_cosine\n",
            actor,
            next_completed,
            kQuickTurnDurationUpdates,
            static_cast<double>(progress),
            static_cast<double>(std::remainder(
                start_yaw + kQuickTurnHeadingDegrees * progress,
                360.0f
            ))
        );
        std::fflush(stderr);
    }

    if (next_completed == kQuickTurnDurationUpdates) {
        g_quick_turn_active.store(false, std::memory_order_release);
        g_quick_turn_actor.store(0u, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_quick_turn_camera_completed"
            " actor=0x%08" PRIX32 " completed_updates=%" PRIu32
            " heading_delta=%.3f curve=half_cosine camera_snap=0\n",
            actor,
            kQuickTurnDurationUpdates,
            static_cast<double>(kQuickTurnHeadingDegrees)
        );
        std::fflush(stderr);
    }
    return true;
}

bool initialize_aim_from_actor_angles(uint8_t* rdram, uint32_t actor) {
    g_aim_ready = false;
    const float authored_pitch = float_from_word(
        read_u32(rdram, actor + 0x50u)
    );
    const float authored_yaw = float_from_word(
        read_u32(rdram, actor + 0x54u)
    );
    if (!std::isfinite(authored_pitch) || !std::isfinite(authored_yaw)) {
        return false;
    }

    g_aim_actor = actor;
    g_aim_pitch = std::clamp(
        authored_pitch,
        -kMaximumPitch,
        kMaximumPitch
    );
    g_aim_yaw = std::remainder(authored_yaw, 360.0f);
    rebuild_aim_vectors_from_angles();
    g_aim_ready = true;
    g_resync_aim.store(false, std::memory_order_release);
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=modern_aim_seeded"
        " actor=0x%08" PRIX32
        " source=authoritative_post_orientation_euler"
        " pitch=%.3f yaw=%.3f authored_pitch=%.3f"
        " transitional_anchor_used=0\n",
        actor,
        static_cast<double>(g_aim_pitch),
        static_cast<double>(g_aim_yaw),
        static_cast<double>(authored_pitch)
    );
    std::fflush(stderr);
    return true;
}

bool update_aim_from_pending_input(uint32_t actor) {
    if (g_resync_aim.load(std::memory_order_acquire) ||
        !g_aim_ready || g_aim_actor != actor) {
        g_resync_aim.store(true, std::memory_order_release);
        return false;
    }

    const int32_t delta_x = static_cast<int32_t>(
        g_pending_look_x.exchange(0, std::memory_order_acq_rel)
    );
    const int32_t delta_y = static_cast<int32_t>(
        g_pending_look_y.exchange(0, std::memory_order_acq_rel)
    );
    if (delta_x == 0 && delta_y == 0) {
        return true;
    }

    // The integrator subtracts its basis vector, so positive yaw turns toward +X.
    const float yaw_delta = static_cast<float>(delta_x) * g_look_sensitivity;
    const float pitch_sign = g_invert_look_y ? -1.0f : 1.0f;
    const float requested_pitch_delta =
        static_cast<float>(delta_y) * g_look_sensitivity * pitch_sign;
    const float next_pitch = std::clamp(
        g_aim_pitch + requested_pitch_delta,
        -kMaximumPitch,
        kMaximumPitch
    );
    g_aim_pitch = next_pitch;
    g_aim_yaw = std::remainder(g_aim_yaw + yaw_delta, 360.0f);
    rebuild_aim_vectors_from_angles();

    const uint64_t count =
        g_player_aim_update_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (runtime_telemetry_enabled() &&
        (aim_telemetry_all_enabled() || count <= 5 || count % 120 == 0)) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_player_aim_update count=%" PRIu64
            " actor=0x%08" PRIX32 " delta_x=%d delta_y=%d"
            " yaw=%.3f pitch=%.3f\n",
            count,
            actor,
            delta_x,
            delta_y,
            static_cast<double>(g_aim_yaw),
            static_cast<double>(g_aim_pitch)
        );
        std::fflush(stderr);
    }
    return true;
}

void write_aim_frame(uint8_t* rdram, uint32_t actor, bool write_basis) {
    if (write_basis) {
        write_actor_vector(
            rdram,
            actor,
            0x10u,
            multiply(g_aim_right, kBasisScale)
        );
        write_actor_vector(
            rdram,
            actor,
            0x20u,
            multiply(g_aim_up, kBasisScale)
        );
        write_actor_vector(
            rdram,
            actor,
            0x30u,
            multiply(g_aim_forward, kBasisScale)
        );
    }
    MEM_W(0x50, guest_address(actor)) = word_from_float(g_aim_pitch);
    MEM_W(0x54, guest_address(actor)) = word_from_float(g_aim_yaw);
    MEM_W(0x58, guest_address(actor)) = word_from_float(0.0f);
}

bool adopt_authored_teleport_aim(uint8_t* rdram, uint32_t actor) {
    const float pitch = float_from_word(read_u32(rdram, actor + 0x50u));
    const float yaw = float_from_word(read_u32(rdram, actor + 0x54u));
    if (!std::isfinite(pitch) || !std::isfinite(yaw)) {
        return false;
    }

    clear_pending_look();
    std::scoped_lock lock(g_aim_mutex);
    g_aim_actor = actor;
    g_aim_pitch = std::clamp(pitch, -kMaximumPitch, kMaximumPitch);
    g_aim_yaw = std::remainder(yaw, 360.0f);
    rebuild_aim_vectors_from_angles();
    g_aim_ready = true;
    g_resync_aim.store(false, std::memory_order_release);
    return true;
}

bool rebase_after_authored_portal_exit(
    uint8_t* rdram,
    uint32_t actor,
    uint32_t destination_teleport,
    uint32_t pair_id
) {
    g_web_box_contact_count = 0u;
    clear_manual_flight_actions();
    invalidate_movement_checkpoint();
    invalidate_collision_safe_anchor();
    invalidate_landing_anchor();
    invalidate_airborne_idle_anchor();
    clear_surface_glide(actor);
    capture_collision_safe_anchor(rdram, actor);
    capture_airborne_idle_anchor(rdram, actor);

    const bool aim_adopted = adopt_authored_teleport_aim(rdram, actor);
    const int64_t deadline_ns = monotonic_now_ns() +
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            kTeleportCooldownDuration
        ).count();
    g_teleport_suppression_count.store(0, std::memory_order_release);
    g_teleport_cooldown_pair_id.store(pair_id, std::memory_order_release);
    g_teleport_cooldown_deadline_ns.store(deadline_ns, std::memory_order_release);
    g_teleport_cooldown_actor.store(
        destination_teleport,
        std::memory_order_release
    );
    return aim_adopted;
}

float decode_movement_axis(const std::atomic_int32_t& value) {
    return static_cast<float>(value.load(std::memory_order_acquire)) / 32767.0f;
}

} // namespace

bool bumble::modern_controls::restore_recent_collision_safe_position(
    uint8_t* rdram,
    uint32_t actor
) {
    return rdram != nullptr && actor != 0u &&
        restore_collision_safe_anchor(rdram, actor);
}

void bumble::modern_controls::rebase_after_authored_portal_transfer(
    uint8_t* rdram,
    uint32_t actor,
    uint32_t source_teleport,
    uint32_t destination_teleport,
    uint32_t pair_id
) {
    if (!enabled() || !g_gameplay_active.load(std::memory_order_acquire) ||
        rdram == nullptr ||
        !living_player_one_actor_is_owned(rdram, actor) ||
        !exact_teleport_actor_is_live(rdram, source_teleport) ||
        !exact_teleport_actor_is_live(rdram, destination_teleport) ||
        source_teleport == destination_teleport ||
        read_u32(rdram, source_teleport + 0x90u) != pair_id ||
        read_u32(rdram, destination_teleport + 0x90u) != pair_id) {
        return;
    }
    const uint32_t destination_yaw_word = read_u32(
        rdram,
        destination_teleport + 0x54u
    );
    if (destination_yaw_word != read_u32(rdram, actor + 0x54u) ||
        !all_finite(read_actor_vector(rdram, actor, 0x40u))) {
        return;
    }

    const bool aim_adopted = rebase_after_authored_portal_exit(
        rdram,
        actor,
        destination_teleport,
        pair_id
    );
    const uint64_t completion = g_teleport_completion_count.fetch_add(
        1,
        std::memory_order_relaxed
    ) + 1;
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=modern_teleport_completed"
        " count=%" PRIu64 " player=0x%08" PRIX32
        " source=0x%08" PRIX32 " destination=0x%08" PRIX32
        " pair_id=%" PRIu32 " exit_yaw=%.3f aim_adopted=%d"
        " cooldown_ms=10000 anchors_rebased=1"
        " owner=settled_player_update\n",
        completion,
        actor,
        source_teleport,
        destination_teleport,
        pair_id,
        static_cast<double>(float_from_word(destination_yaw_word)),
        aim_adopted ? 1 : 0
    );
    std::fflush(stderr);
}

void bumble::modern_controls::rebase_after_mission_checkpoint_restore(
    uint8_t* rdram,
    uint32_t actor,
    uint32_t destination_teleport,
    uint32_t pair_id
) {
    if (!enabled() || rdram == nullptr ||
        !living_player_one_actor_is_owned(rdram, actor)) {
        return;
    }

    clear_pending_look();
    g_primary_fire.store(false, std::memory_order_release);
    g_primary_fire_require_release.store(true, std::memory_order_release);
    g_movement_forward.store(0, std::memory_order_release);
    g_movement_strafe.store(0, std::memory_order_release);
    const bool portal_checkpoint = destination_teleport != 0u;
    const bool aim_adopted = rebase_after_authored_portal_exit(
        rdram,
        actor,
        destination_teleport,
        pair_id
    );
    if (!portal_checkpoint) {
        g_teleport_suppression_count.store(0, std::memory_order_release);
        g_teleport_cooldown_pair_id.store(0u, std::memory_order_release);
        g_teleport_cooldown_deadline_ns.store(0, std::memory_order_release);
        g_teleport_cooldown_actor.store(0u, std::memory_order_release);
    }
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=modern_checkpoint_controls_rebased"
        " checkpoint_kind=%s player=0x%08" PRIX32
        " destination=0x%08" PRIX32
        " pair_id=%" PRIu32 " aim_adopted=%d cooldown_ms=%d"
        " movement_released=1 fire_release_required=1\n",
        portal_checkpoint ? "portal" : "objective",
        actor,
        destination_teleport,
        pair_id,
        aim_adopted ? 1 : 0,
        portal_checkpoint ? 10000 : 0
    );
    std::fflush(stderr);
}

extern "C" void bumble_observe_web_box_contact(
    uint8_t* rdram, recomp_context* context
) {
    if (rdram == nullptr || context == nullptr ||
        !g_enabled.load(std::memory_order_acquire)) {
        return;
    }
    const uint32_t player = static_cast<uint32_t>(context->r18);
    const uint32_t box = static_cast<uint32_t>(context->r2);
    if (!valid_guest_pointer(player, 0x90u) || !valid_guest_pointer(box, 0x90u) ||
        read_u32(rdram, kPlayerOneOwnerSlot) != player) {
        return;
    }
    const uint16_t player_id = read_u16(rdram, player + 0x7Eu);
    const uint16_t box_id = read_u16(rdram, box + 0x7Eu);
    if (!live_contact_actor(rdram, player, player_id, kPlayerVtable) ||
        !live_contact_actor(rdram, box, box_id, kWebBoxVtable)) {
        return;
    }
    if (player != g_web_box_player || player_id != g_web_box_player_id) {
        g_web_box_contact_count = 0u;
        g_web_box_player = player;
        g_web_box_player_id = player_id;
    }
    (void)live_web_box_restraint(rdram, player);
    bool recorded = false;
    for (size_t index = 0u; index < g_web_box_contact_count; ++index) {
        recorded |= g_web_box_contacts[index].actor == box &&
            g_web_box_contacts[index].id == box_id;
    }
    if (!recorded && g_web_box_contact_count < g_web_box_contacts.size()) {
        g_web_box_contacts[g_web_box_contact_count++] = {box, box_id};
    }
    if (g_maneuver_actor.load(std::memory_order_acquire) == player &&
        g_active_maneuver.load(std::memory_order_acquire) ==
            static_cast<uint32_t>(ActiveManeuver::BarrelRoll)) {
        cancel_active_maneuver();
    }
    g_barrel_roll_edge.store(false, std::memory_order_release);
}

extern "C" void bumble_forget_web_box_contact(uint32_t box) {
    size_t kept = 0u;
    for (size_t index = 0u; index < g_web_box_contact_count; ++index) {
        if (g_web_box_contacts[index].actor != box) {
            g_web_box_contacts[kept++] = g_web_box_contacts[index];
        }
    }
    g_web_box_contact_count = kept;
}

extern "C" void bumble_clear_web_box_contacts() {
    g_web_box_contact_count = 0u;
    g_web_box_player = 0u;
    g_web_box_player_id = 0u;
}

void bumble::modern_controls::configure(
    bool enabled_value,
    float look_sensitivity,
    bool invert_look_y
) {
    std::scoped_lock lock(g_aim_mutex);
    if (!std::isfinite(look_sensitivity)) {
        look_sensitivity = kDefaultSensitivity;
    }
    g_look_sensitivity = std::clamp(
        look_sensitivity,
        kMinimumSensitivity,
        kMaximumSensitivity
    );
    g_invert_look_y = invert_look_y;
    g_aim_ready = false;
    g_aim_actor = 0;
    g_aim_input_consumed_this_update.store(false, std::memory_order_release);
    clear_pending_look();
    g_primary_fire.store(false, std::memory_order_release);
    g_primary_fire_require_release.store(false, std::memory_order_release);
    clear_frontend_confirm_action(true);
    g_mission_card_accepted.store(false, std::memory_order_release);
    g_cutscene_skip_held.store(false, std::memory_order_release);
    g_cutscene_skip_edge.store(false, std::memory_order_release);
    g_cutscene_skip_require_release.store(false, std::memory_order_release);
    clear_manual_flight_actions();
    g_guided_missile_actor.store(0u, std::memory_order_release);
    g_guided_missile_player_actor.store(0u, std::memory_order_release);
    g_movement_forward.store(0, std::memory_order_release);
    g_movement_strafe.store(0, std::memory_order_release);
    g_resync_aim.store(true, std::memory_order_release);
    g_gameplay_arm_deferred_logged.store(false, std::memory_order_release);
    g_mouse_captured.store(false, std::memory_order_release);
    g_gameplay_active.store(false, std::memory_order_release);
    g_pause_menu_active.store(false, std::memory_order_release);
    g_replay_automation.store(false, std::memory_order_release);
    g_window_focused.store(false, std::memory_order_release);
    g_player_aim_update_count.store(0, std::memory_order_release);
    g_movement_frame_count.store(0, std::memory_order_release);
    g_active_player_frame_count.store(0, std::memory_order_release);
    g_teleport_completion_count.store(0, std::memory_order_release);
    g_teleport_suppression_count.store(0, std::memory_order_release);
    g_teleport_cooldown_pair_id.store(0, std::memory_order_release);
    g_teleport_cooldown_deadline_ns.store(0, std::memory_order_release);
    g_teleport_cooldown_actor.store(0, std::memory_order_release);
    g_teleport_visual_logged_deadline_ns.store(0, std::memory_order_release);
    g_teleport_visual_scope_count.store(0, std::memory_order_release);
    g_barrel_roll_visual_scope_count.store(0, std::memory_order_release);
    g_stunt_visual_scope_count.store(0, std::memory_order_release);
    g_pending_teleport_visual_actor = 0u;
    g_pending_teleport_visual_record_count = 0u;
    g_teleport_visual_scope_active = false;
    g_teleport_visual_expected_end_cursor = 0u;
    g_automatic_landing_suppressed_count.store(0, std::memory_order_release);
    g_automatic_landing_store_suppressed_count.store(
        0,
        std::memory_order_release
    );
    g_automatic_grounded_entry_suppressed_count.store(
        0,
        std::memory_order_release
    );
    g_airborne_collision_rollback_count.store(0, std::memory_order_release);
    g_airborne_collision_recovery_restore_count.store(
        0,
        std::memory_order_release
    );
    g_ground_contact_rollback_count.store(0, std::memory_order_release);
    g_airborne_floor_hold_count.store(0, std::memory_order_release);
    g_airborne_idle_hold_count.store(0, std::memory_order_release);
    g_airborne_landing_gear_retraction_count.store(
        0,
        std::memory_order_release
    );
    g_manual_land_obstacle_rejection_count.store(0, std::memory_order_release);
    g_automatic_ground_exit_count.store(0, std::memory_order_release);
    g_player_update_actor.store(0, std::memory_order_release);
    g_player_update_entry_state.store(0, std::memory_order_release);
    g_airborne_collision_finalize_actor.store(0, std::memory_order_release);
    clear_surface_glide();
    invalidate_collision_safe_anchor();
    g_manual_grounded_entry_actor.store(0, std::memory_order_release);
    g_enabled.store(enabled_value, std::memory_order_release);
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=modern_controls_configured enabled=%d"
        " look_sensitivity=%.3f invert_look_y=%d scheme=actor_aim_wasd_movement\n",
        enabled_value ? 1 : 0,
        static_cast<double>(g_look_sensitivity),
        invert_look_y ? 1 : 0
    );
    std::fflush(stderr);
}

bool bumble::modern_controls::enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

void bumble::modern_controls::set_replay_automation(bool enabled_value) {
    const bool active = enabled() && enabled_value;
    g_replay_automation.store(active, std::memory_order_release);
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=modern_replay_automation_configured"
        " active=%d physical_input_suppressed=1 focus_spoofed=0\n",
        active ? 1 : 0
    );
    std::fflush(stderr);
}

bool bumble::modern_controls::replay_automation_enabled() {
    return g_replay_automation.load(std::memory_order_acquire);
}

bool bumble::modern_controls::gameplay_input_active() {
    return enabled() && g_gameplay_active.load(std::memory_order_acquire) &&
        !g_pause_menu_active.load(std::memory_order_acquire);
}

bool bumble::modern_controls::pause_menu_active() {
    return enabled() &&
        g_pause_menu_active.load(std::memory_order_acquire);
}

void bumble::modern_controls::set_window_focused(bool focused) {
    const bool previous = g_window_focused.exchange(
        focused,
        std::memory_order_acq_rel
    );
    if (previous == focused) {
        return;
    }
    if (!focused) {
        g_primary_fire.store(false, std::memory_order_release);
        clear_frontend_confirm_action(false);
        g_cutscene_skip_held.store(false, std::memory_order_release);
        g_cutscene_skip_edge.store(false, std::memory_order_release);
        g_cutscene_skip_require_release.store(true, std::memory_order_release);
        clear_manual_flight_actions();
        set_movement_input(0.0f, 0.0f);
        clear_pending_look();
        g_resync_aim.store(true, std::memory_order_release);
    }
}

bool bumble::modern_controls::window_focused() {
    return g_window_focused.load(std::memory_order_acquire);
}

void bumble::modern_controls::set_mouse_capture(bool captured) {
    const bool active = enabled() && captured && window_focused();
    const bool previous = g_mouse_captured.exchange(active, std::memory_order_acq_rel);
    if (active == previous) {
        return;
    }
    clear_pending_look();
    if (active) {
        g_resync_aim.store(true, std::memory_order_release);
    }
    if (!active) {
        set_movement_input(0.0f, 0.0f);
    }
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=modern_mouse_capture active=%d\n",
        active ? 1 : 0
    );
    std::fflush(stderr);
}

bool bumble::modern_controls::mouse_captured() {
    return g_mouse_captured.load(std::memory_order_acquire);
}

void bumble::modern_controls::add_raw_mouse_delta(
    int32_t delta_x,
    int32_t delta_y
) {
    if (!enabled() || !window_focused() || !mouse_captured()) {
        return;
    }
    add_saturated(g_pending_look_x, delta_x);
    add_saturated(g_pending_look_y, delta_y);
}

void bumble::modern_controls::add_controller_look(float axis_x, float axis_y) {
    if (!enabled() || !window_focused()) {
        return;
    }
    constexpr float kControllerCountsPerPoll = 20.0f;
    const float sensitivity_compensation =
        kDefaultSensitivity / g_look_sensitivity;
    const float invert_y_compensation = g_invert_look_y ? -1.0f : 1.0f;
    add_saturated(
        g_pending_look_x,
        static_cast<int32_t>(std::lround(
            axis_x * kControllerCountsPerPoll * sensitivity_compensation
        ))
    );
    add_saturated(
        g_pending_look_y,
        static_cast<int32_t>(std::lround(
            axis_y * kControllerCountsPerPoll * sensitivity_compensation *
                invert_y_compensation
        ))
    );
}

void bumble::modern_controls::set_primary_fire(bool pressed) {
    if (!pressed) {
        g_primary_fire.store(false, std::memory_order_release);
        g_primary_fire_require_release.store(false, std::memory_order_release);
        return;
    }
    if (!enabled() || !window_focused() ||
        g_primary_fire_require_release.load(std::memory_order_acquire)) {
        g_primary_fire.store(false, std::memory_order_release);
        return;
    }
    g_primary_fire.store(true, std::memory_order_release);
}

bool bumble::modern_controls::primary_fire_pressed() {
    return enabled() && window_focused() &&
        g_primary_fire.load(std::memory_order_acquire);
}

bool bumble::modern_controls::frontend_confirm_pending() {
    return enabled() && gameplay_input_active() &&
        g_frontend_confirm_pending.load(std::memory_order_acquire);
}

void bumble::modern_controls::set_frontend_confirm_pressed(bool pressed) {
    if (!enabled()) {
        clear_frontend_confirm_action(true);
        return;
    }
    if (!window_focused()) {
        g_frontend_confirm_held.store(false, std::memory_order_release);
        g_frontend_confirm_edge.store(false, std::memory_order_release);
        g_frontend_confirm_require_release.store(true, std::memory_order_release);
        return;
    }
    if (!pressed) {
        g_frontend_confirm_held.store(false, std::memory_order_release);
        g_frontend_confirm_require_release.store(false, std::memory_order_release);
        return;
    }

    const bool was_held = g_frontend_confirm_held.exchange(
        true,
        std::memory_order_acq_rel
    );
    if (!frontend_confirm_pending()) {
        g_frontend_confirm_require_release.store(true, std::memory_order_release);
        return;
    }
    if (g_frontend_confirm_require_release.load(std::memory_order_acquire)) {
        return;
    }
    if (!was_held) {
        g_frontend_confirm_edge.store(true, std::memory_order_release);
    }
}

void bumble::modern_controls::set_cutscene_skip_pressed(bool pressed) {
    if (!pressed) {
        g_cutscene_skip_held.store(false, std::memory_order_release);
        g_cutscene_skip_require_release.store(false, std::memory_order_release);
        return;
    }

    const bool was_held = g_cutscene_skip_held.exchange(
        true,
        std::memory_order_acq_rel
    );
    if (!was_held && !g_cutscene_skip_require_release.load(
            std::memory_order_acquire)) {
        g_cutscene_skip_edge.store(true, std::memory_order_release);
    }
}

void bumble::modern_controls::set_takeoff_land_pressed(bool pressed) {
    if (!enabled()) {
        clear_manual_flight_actions();
        return;
    }
    if (!window_focused()) {
        g_takeoff_land_held.store(false, std::memory_order_release);
        return;
    }
    if (!pressed) {
        g_takeoff_land_held.store(false, std::memory_order_release);
        g_takeoff_land_require_release.store(false, std::memory_order_release);
        return;
    }

    const bool was_held = g_takeoff_land_held.exchange(
        true,
        std::memory_order_acq_rel
    );
    if (!gameplay_input_active()) {
        // Clear held actions across focus and menu transitions.
        g_takeoff_land_require_release.store(true, std::memory_order_release);
        return;
    }
    if (frontend_confirm_pending()) {
        g_takeoff_land_require_release.store(true, std::memory_order_release);
        return;
    }
    if (g_takeoff_land_require_release.load(std::memory_order_acquire)) {
        return;
    }
    if (!was_held) {
        g_takeoff_land_edge.store(true, std::memory_order_release);
    }
}

void bumble::modern_controls::set_loop_de_loop_pressed(bool pressed) {
    update_edge_action_button(
        pressed,
        g_loop_de_loop_held,
        g_loop_de_loop_edge,
        g_loop_de_loop_require_release
    );
}

void bumble::modern_controls::set_quick_flip_pressed(bool pressed) {
    update_edge_action_button(
        pressed,
        g_quick_flip_held,
        g_quick_flip_edge,
        g_quick_flip_require_release
    );
}

void bumble::modern_controls::set_sprint_pressed(bool pressed) {
    if (!enabled() || !window_focused()) {
        g_sprint_held.store(false, std::memory_order_release);
        g_sprint_require_release.store(true, std::memory_order_release);
        return;
    }
    if (!pressed) {
        g_sprint_held.store(false, std::memory_order_release);
        g_sprint_require_release.store(false, std::memory_order_release);
        return;
    }
    if (!gameplay_input_active()) {
        g_sprint_held.store(false, std::memory_order_release);
        g_sprint_require_release.store(true, std::memory_order_release);
        return;
    }
    g_sprint_held.store(
        !g_sprint_require_release.load(std::memory_order_acquire),
        std::memory_order_release
    );
}

void bumble::modern_controls::set_barrel_roll_pressed(bool pressed) {
    update_edge_action_button(
        pressed,
        g_barrel_roll_held,
        g_barrel_roll_edge,
        g_barrel_roll_require_release
    );
}

void bumble::modern_controls::set_movement_input(float forward, float strafe) {
    forward = std::clamp(forward, -1.0f, 1.0f);
    strafe = std::clamp(strafe, -1.0f, 1.0f);
    const float magnitude = std::hypot(forward, strafe);
    if (magnitude > 1.0f) {
        forward /= magnitude;
        strafe /= magnitude;
    }
    g_movement_forward.store(
        static_cast<int32_t>(std::lround(forward * 32767.0f)),
        std::memory_order_release
    );
    g_movement_strafe.store(
        static_cast<int32_t>(std::lround(strafe * 32767.0f)),
        std::memory_order_release
    );
    if (std::abs(strafe) >= 0.2f) {
        g_last_strafe_direction.store(
            strafe > 0.0f ? 1 : -1,
            std::memory_order_release
        );
    }
}

bool bumble::modern_controls::query_spatial_intent(
    uint8_t* rdram,
    uint32_t actor,
    uint32_t state,
    SpatialIntent& intent
) {
    intent = {};
    if (rdram == nullptr ||
        !modern_player_hook_is_owned(rdram, actor, state) ||
        !gameplay_control_owner_active() ||
        (state != kFlyingState && !is_grounded_state(state))) {
        return false;
    }

    const uint32_t guided_missile =
        exact_guided_missile_for_player(rdram, actor);
    if (guided_missile != 0u) {
        begin_guided_missile_control(actor, guided_missile);
        return true;
    }

    ActiveManeuver maneuver = static_cast<ActiveManeuver>(
        g_active_maneuver.load(std::memory_order_acquire)
    );
    if (maneuver == ActiveManeuver::BarrelRoll &&
        live_web_box_restraint(rdram, actor)) {
        cancel_active_maneuver();
        maneuver = ActiveManeuver::None;
    }
    if (maneuver != ActiveManeuver::None) {
        if (state != kFlyingState ||
            g_maneuver_actor.load(std::memory_order_acquire) != actor) {
            cancel_active_maneuver();
        } else {
            const Vec3 direction{
                float_from_word(
                    g_maneuver_direction_x.load(std::memory_order_acquire)
                ),
                float_from_word(
                    g_maneuver_direction_y.load(std::memory_order_acquire)
                ),
                float_from_word(
                    g_maneuver_direction_z.load(std::memory_order_acquire)
                ),
            };
            const Vec3 loop_up{
                float_from_word(
                    g_maneuver_up_x.load(std::memory_order_acquire)
                ),
                float_from_word(
                    g_maneuver_up_y.load(std::memory_order_acquire)
                ),
                float_from_word(
                    g_maneuver_up_z.load(std::memory_order_acquire)
                ),
            };
            uint32_t remaining =
                g_maneuver_updates_remaining.load(std::memory_order_acquire);
            const bool loop = maneuver == ActiveManeuver::LoopDeLoop;
            if (!all_finite(direction) ||
                (loop && !all_finite(loop_up)) || remaining == 0u) {
                cancel_active_maneuver();
            } else {
                while (remaining > 0u &&
                    !g_maneuver_updates_remaining.compare_exchange_weak(
                        remaining,
                        remaining - 1u,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                }
                if (remaining == 0u) {
                    cancel_active_maneuver();
                } else {
                    const bool roll =
                        maneuver == ActiveManeuver::BarrelRoll;
                    const uint32_t duration = roll
                        ? kBarrelRollDurationUpdates
                        : kLoopDeLoopDurationUpdates;
                    const uint32_t completed =
                        duration - remaining + 1u;
                    const float progress = roll
                        ? half_sine_velocity_progress(completed, duration)
                        : half_cosine_progress(completed, duration);
                    Vec3 delta{};
                    float distance = 0.0f;
                    if (loop) {
                        const float previous_progress = half_cosine_progress(
                            completed - 1u,
                            duration
                        );
                        const float previous_angle =
                            previous_progress * 2.0f * kPi;
                        const float current_angle =
                            progress * 2.0f * kPi;
                        const float previous_forward =
                            kLoopDeLoopRadius * std::sin(previous_angle) +
                            kLoopDeLoopForwardAdvance * previous_progress;
                        const float current_forward =
                            kLoopDeLoopRadius * std::sin(current_angle) +
                            kLoopDeLoopForwardAdvance * progress;
                        const float previous_up = kLoopDeLoopRadius *
                            (1.0f - std::cos(previous_angle));
                        const float current_up = kLoopDeLoopRadius *
                            (1.0f - std::cos(current_angle));
                        delta = add(
                            multiply(
                                direction,
                                current_forward - previous_forward
                            ),
                            multiply(loop_up, current_up - previous_up)
                        );
                        distance = length(delta);
                    } else {
                        distance = half_sine_velocity_step_distance(
                            kBarrelRollTotalDistance,
                            completed,
                            duration
                        );
                        delta = multiply(direction, distance);
                    }
                    intent.delta_x = delta.x;
                    intent.delta_y = delta.y;
                    intent.delta_z = delta.z;
                    intent.speed = distance;
                    intent.maneuver_progress = progress;
                    intent.maneuver_step = completed;
                    intent.maneuver_steps_total = duration;
                    intent.barrel_roll_active = roll;
                    intent.loop_de_loop_active = loop;

                    if (roll) {
                        const int32_t roll_direction =
                            g_barrel_roll_visual_direction.load(
                                std::memory_order_acquire
                            );
                        // Model roll has the opposite handedness to world-space strafe.
                        const float angle =
                            -static_cast<float>(roll_direction) *
                            360.0f * progress;
                        g_barrel_roll_visual_angle_bits.store(
                            word_from_float(angle),
                            std::memory_order_release
                        );
                        g_barrel_roll_visual_actor.store(
                            actor,
                            std::memory_order_release
                        );
                    }

                    if (remaining == 1u) {
                        g_active_maneuver.store(
                            static_cast<uint32_t>(ActiveManeuver::None),
                            std::memory_order_release
                        );
                        g_maneuver_actor.store(
                            0u,
                            std::memory_order_release
                        );
                        std::fprintf(
                            stderr,
                            "BUMBLE_RT64_PROBE stage=%s"
                            " actor=0x%08" PRIX32
                            " completed_updates=%" PRIu32
                            " curve=%s collision=guest_bsp_integrator"
                            " total_requested_distance=%.3f"
                            " forward_advance=%.3f\n",
                            roll
                                ? "modern_barrel_roll_completed"
                                : "modern_loop_de_loop_completed",
                            actor,
                            duration,
                            loop
                                ? "half_cosine_path"
                                : "half_sine_velocity",
                            static_cast<double>(
                                roll ? kBarrelRollTotalDistance : 0.0f
                            ),
                            static_cast<double>(
                                loop ? kLoopDeLoopForwardAdvance : 0.0f
                            )
                        );
                        std::fflush(stderr);
                    }
                    return true;
                }
            }
        }
    }

    float forward_axis = decode_movement_axis(g_movement_forward);
    float strafe_axis = decode_movement_axis(g_movement_strafe);
    const float magnitude = std::hypot(forward_axis, strafe_axis);
    if (magnitude > 1.0f) {
        forward_axis /= magnitude;
        strafe_axis /= magnitude;
    }

    {
        std::scoped_lock lock(g_aim_mutex);
        if (g_resync_aim.load(std::memory_order_acquire) ||
            !g_aim_ready || g_aim_actor != actor) {
            return false;
        }

        Vec3 movement_forward = g_aim_forward;
        Vec3 movement_right = g_aim_right;
        if (is_grounded_state(state)) {
            movement_forward.y = 0.0f;
            movement_right.y = 0.0f;
            Vec3 normalized_forward{};
            Vec3 normalized_right{};
            if (!normalize(movement_forward, normalized_forward) ||
                !normalize(movement_right, normalized_right)) {
                return false;
            }
            movement_forward = normalized_forward;
            movement_right = normalized_right;
        }

        Vec3 direction = add(
            multiply(movement_forward, forward_axis),
            multiply(movement_right, -strafe_axis)
        );
        Vec3 normalized_direction{};
        if (magnitude >= 0.001f && normalize(direction, normalized_direction)) {
            const float speed = kMovementSpeed *
                (g_sprint_held.load(std::memory_order_acquire) &&
                 !g_sprint_require_release.load(std::memory_order_acquire)
                    ? kSprintMovementMultiplier
                    : 1.0f);
            const Vec3 delta = multiply(
                normalized_direction,
                -speed * std::min(magnitude, 1.0f)
            );
            intent.delta_x = delta.x;
            intent.delta_y = delta.y;
            intent.delta_z = delta.z;
            intent.speed = speed * std::min(magnitude, 1.0f);
        }
    }

    intent.landing_requested = state == kFlyingState &&
        g_manual_land_request_actor.load(std::memory_order_acquire) == actor &&
        g_manual_land_buffer_updates_remaining.load(
            std::memory_order_acquire
        ) > 0u;
    intent.takeoff_requested = is_grounded_state(state) &&
        g_manual_takeoff_request_actor.load(std::memory_order_acquire) == actor;
    return true;
}

void bumble::modern_controls::finish_spatial_landing_attempt(
    uint32_t actor,
    bool landed
) {
    if (actor == 0u ||
        g_manual_land_request_actor.load(std::memory_order_acquire) != actor) {
        return;
    }

    if (!landed) {
        age_buffered_landing_request(actor);
        return;
    }

    uint32_t expected_actor = actor;
    if (!g_manual_land_request_actor.compare_exchange_strong(
            expected_actor,
            0u,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }
    g_manual_land_buffer_updates_remaining.store(0u, std::memory_order_release);
    g_manual_landing_authorized_actor.store(0u, std::memory_order_release);
    g_manual_grounded_entry_actor.store(actor, std::memory_order_release);
    invalidate_movement_checkpoint();
    invalidate_collision_safe_anchor();
    invalidate_airborne_idle_anchor();
    clear_surface_glide(actor);
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=modern_native_landing_committed"
        " actor=0x%08" PRIX32
        " support=current_capsule_contact input_buffer_consumed=1"
        " remote_snap=0\n",
        actor
    );
    std::fflush(stderr);
}

bool bumble::modern_controls::set_automation_aim(
    float pitch_degrees,
    float yaw_degrees
) {
    if (!enabled() || !replay_automation_enabled() ||
        !std::isfinite(pitch_degrees) || !std::isfinite(yaw_degrees)) {
        return false;
    }
    std::scoped_lock lock(g_aim_mutex);
    if (g_resync_aim.load(std::memory_order_acquire) ||
        !g_aim_ready || g_aim_actor == 0u) {
        return false;
    }
    g_aim_pitch = std::clamp(
        pitch_degrees,
        -kMaximumPitch,
        kMaximumPitch
    );
    g_aim_yaw = std::remainder(yaw_degrees, 360.0f);
    rebuild_aim_vectors_from_angles();
    return true;
}

uint64_t bumble::modern_controls::player_aim_update_count() {
    return g_player_aim_update_count.load(std::memory_order_acquire);
}

uint64_t bumble::modern_controls::movement_frame_count() {
    return g_movement_frame_count.load(std::memory_order_acquire);
}

uint64_t bumble::modern_controls::active_player_frame_count() {
    return g_active_player_frame_count.load(std::memory_order_acquire);
}

extern "C" void bumble_prepare_modern_teleport_visual_record(
    uint8_t* rdram,
    recomp_context* context
) {
    g_pending_teleport_visual_actor = 0u;
    g_pending_teleport_visual_record_count = 0u;
    if (rdram == nullptr || context == nullptr) {
        return;
    }

    const uint32_t actor = guest_u32(context->r4);
    int64_t deadline_ns = 0;
    if (!teleport_actor_is_temporarily_disabled(
            rdram,
            actor,
            deadline_ns
        )) {
        return;
    }

    const uint32_t record_count = read_u32(rdram, kPostObjectRecordCount);
    if (record_count >= kPostObjectRecordCapacity) {
        return;
    }
    g_pending_teleport_visual_actor = actor;
    g_pending_teleport_visual_record_count = record_count;

    const int64_t previous_deadline =
        g_teleport_visual_logged_deadline_ns.exchange(
            deadline_ns,
            std::memory_order_acq_rel
        );
    if (previous_deadline != deadline_ns) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_teleport_visual_disabled"
            " teleport=0x%08" PRIX32 " tint=yellow duration_ms=10000\n",
            actor
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_tag_modern_teleport_visual_record(
    uint8_t* rdram,
    recomp_context* context
) {
    (void)context;
    const uint32_t actor = g_pending_teleport_visual_actor;
    const uint32_t record_count = g_pending_teleport_visual_record_count;
    g_pending_teleport_visual_actor = 0u;
    g_pending_teleport_visual_record_count = 0u;
    if (rdram == nullptr || actor == 0u ||
        record_count >= kPostObjectRecordCapacity ||
        read_u32(rdram, kPostObjectRecordCount) != record_count + 1u) {
        return;
    }

    const uint32_t record =
        kPostObjectRecordBase + record_count * kPostObjectRecordSize;
    if (!valid_guest_pointer(record, kPostObjectRecordSize) ||
        read_u32(rdram, record + 0x04u) != 0u ||
        read_u32(rdram, record + 0x08u) != kTeleportTransformInput ||
        read_u32(rdram, record + 0x0Cu) != kTeleportDisplayList ||
        read_u32(rdram, record + 0x10u) != 0x3F800000u) {
        return;
    }

    write_u32(
        rdram,
        record + 0x04u,
        kTeleportDisabledRecordType
    );
}

extern "C" void bumble_begin_modern_teleport_visual_scope(
    uint8_t* rdram,
    recomp_context* context
) {
    g_teleport_visual_scope_active = false;
    g_teleport_visual_expected_end_cursor = 0u;
    if (rdram == nullptr || context == nullptr ||
        guest_u32(context->r3) != kTeleportDisabledRecordType) {
        return;
    }

    const uint32_t record = guest_u32(context->r16);
    if (!valid_guest_pointer(record, kPostObjectRecordSize) ||
        read_u32(rdram, record + 0x08u) != kTeleportTransformInput ||
        read_u32(rdram, record + 0x0Cu) != kTeleportDisplayList ||
        read_u32(rdram, record + 0x10u) != 0x3F800000u) {
        return;
    }

    context->r3 = 0;
    const uint32_t cursor = read_u32(rdram, kDisplayListCursor);
    if (!append_teleport_visual_marker(
            rdram,
            true,
            kTeleportOriginalCommandBytes + kGfxCommandBytes
        )) {
        return;
    }
    g_teleport_visual_scope_active = true;
    g_teleport_visual_expected_end_cursor =
        cursor + kGfxCommandBytes + kTeleportOriginalCommandBytes;
}

extern "C" void bumble_end_modern_teleport_visual_scope(
    uint8_t* rdram,
    recomp_context* context
) {
    (void)context;
    if (!g_teleport_visual_scope_active) {
        return;
    }
    g_teleport_visual_scope_active = false;

    const uint32_t cursor = rdram != nullptr
        ? read_u32(rdram, kDisplayListCursor)
        : 0u;
    const bool ordered =
        cursor == g_teleport_visual_expected_end_cursor;
    g_teleport_visual_expected_end_cursor = 0u;
    if (!ordered ||
        !append_teleport_visual_marker(rdram, false, 0u)) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_teleport_visual_scope_failed"
            " cursor=0x%08" PRIX32 " ordered=%d fallback=green\n",
            cursor,
            ordered ? 1 : 0
        );
        std::fflush(stderr);
        return;
    }

    const uint64_t scope = g_teleport_visual_scope_count.fetch_add(
        1u,
        std::memory_order_relaxed
    ) + 1u;
    if (scope == 1u) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_teleport_visual_scope"
            " count=%" PRIu64 " tint=yellow marker_balanced=1\n",
            scope
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_gate_modern_player_teleport(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!bumble::modern_controls::enabled() || rdram == nullptr ||
        context == nullptr ||
        !g_gameplay_active.load(std::memory_order_acquire)) {
        return;
    }

    const uint32_t actor = guest_u32(context->r18);
    if (!living_player_one_actor_is_owned(rdram, actor) ||
        guest_u32(context->r3) != kTeleportCollisionCategory ||
        guest_u32(context->r2) != kTeleportCollisionCategory) {
        return;
    }
    const uint32_t collision_record = guest_u32(context->r22);
    const uint32_t source_teleport = teleport_actor_from_collision_record(
        rdram,
        collision_record
    );
    const uint32_t cooling_teleport = g_teleport_cooldown_actor.load(
        std::memory_order_acquire
    );
    if (source_teleport == 0u || source_teleport != cooling_teleport ||
        read_u32(rdram, source_teleport + 0x90u) !=
            g_teleport_cooldown_pair_id.load(std::memory_order_acquire)) {
        return;
    }

    const int64_t now_ns = monotonic_now_ns();
    const int64_t deadline_ns = g_teleport_cooldown_deadline_ns.load(
        std::memory_order_acquire
    );
    if (now_ns >= deadline_ns) {
        uint32_t expected_actor = cooling_teleport;
        if (g_teleport_cooldown_actor.compare_exchange_strong(
                expected_actor,
                0u,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            g_teleport_cooldown_pair_id.store(0u, std::memory_order_release);
            g_teleport_cooldown_deadline_ns.store(0, std::memory_order_release);
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=modern_teleport_cooldown_expired"
                " player=0x%08" PRIX32 " teleport=0x%08" PRIX32
                " duration_ms=10000\n",
                actor,
                cooling_teleport
            );
            std::fflush(stderr);
        }
        return;
    }

    context->r3 = 0;
    const uint64_t suppression = g_teleport_suppression_count.fetch_add(
        1,
        std::memory_order_relaxed
    ) + 1;
    if (suppression == 1u) {
        const int64_t remaining_ms = std::max<int64_t>(
            1,
            (deadline_ns - now_ns + 999999) / 1000000
        );
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_teleport_reentry_suppressed"
            " player=0x%08" PRIX32 " teleport=0x%08" PRIX32
            " collision_id=%" PRIu16 " remaining_ms=%" PRId64 "\n",
            actor,
            cooling_teleport,
            read_u16(rdram, collision_record + 0x0Cu),
            remaining_ms
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_complete_modern_player_teleport(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr) {
        return;
    }
    bumble::native_checkpoint::capture_portal_mission_checkpoint(
        rdram,
        context
    );
}

extern "C" void bumble_update_modern_player_aim(
    uint8_t* rdram,
    recomp_context* context
) {
    invalidate_movement_checkpoint();
    g_airborne_collision_finalize_actor.store(0u, std::memory_order_release);
    g_aim_input_consumed_this_update.store(false, std::memory_order_release);
    g_player_update_actor.store(0, std::memory_order_release);
    g_player_update_entry_state.store(0, std::memory_order_release);
    if (!bumble::modern_controls::enabled()) {
        return;
    }
    const uint32_t actor = guest_u32(context->r4);
    if (!g_gameplay_active.load(std::memory_order_acquire)) {
        const bool owned_player = player_one_actor_is_owned(rdram, actor);
        const uint32_t entry_state = owned_player
            ? read_u32(rdram, actor + 0x8Cu)
            : 0xFFFFFFFFu;
        const uint32_t frontend_phase = rdram != nullptr
            ? read_u32(rdram, kFrontendObject + kFrontendPhaseOffset)
            : 0xFFFFFFFFu;
        const bool accepted_confirm_fallback =
            g_frontend_confirm_latched.load(std::memory_order_acquire) &&
            frontend_phase == kFrontendGameplayPhase &&
            owned_player && state_supports_modern_control(entry_state);
        if (accepted_confirm_fallback) {
            g_mission_card_accepted.store(true, std::memory_order_release);
            g_gameplay_arm_deferred_logged.exchange(
                false,
                std::memory_order_acq_rel
            );
            clear_pending_look();
            clear_manual_flight_actions();
            g_movement_forward.store(0, std::memory_order_release);
            g_movement_strafe.store(0, std::memory_order_release);
            {
                std::scoped_lock lock(g_aim_mutex);
                g_aim_ready = false;
                g_aim_actor = 0u;
            }
            g_resync_aim.store(true, std::memory_order_release);
            const bool carried_primary_fire = g_primary_fire.exchange(
                false,
                std::memory_order_acq_rel
            );
            if (carried_primary_fire) {
                g_primary_fire_require_release.store(
                    true,
                    std::memory_order_release
                );
            }
            g_gameplay_active.store(true, std::memory_order_release);
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE"
                " stage=modern_gameplay_input_armed_fallback"
                " source=confirmed_phase19_player_update actor=0x%08" PRIX32
                " state=%" PRIu32 " aim_seed_pending=1"
                " transitional_anchor_used=0"
                " carried_fire_suppressed=%d\n",
                actor,
                entry_state,
                carried_primary_fire ? 1 : 0
            );
            std::fflush(stderr);
        } else {
            clear_pending_look();
            g_resync_aim.store(true, std::memory_order_release);
            return;
        }
    }
    if (living_player_one_actor_is_owned(rdram, actor)) {
        const uint32_t entry_state = read_u32(rdram, actor + 0x8Cu);
        prepare_buffered_landing_request_for_update(actor, entry_state);
        g_player_update_actor.store(actor, std::memory_order_release);
        g_player_update_entry_state.store(
            entry_state,
            std::memory_order_release
        );

        if (entry_state == kGroundedEntryState &&
            g_manual_grounded_entry_actor.load(std::memory_order_acquire) ==
                actor) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=modern_manual_landing_animation_started"
                " actor=0x%08" PRIX32
                " entry_state=%" PRIu32 " target_state=%" PRIu32 "\n",
                actor,
                entry_state,
                kGroundedFirstState
            );
            std::fflush(stderr);
        }
        uint32_t expected_actor = actor;
        if (entry_state == kFlyingState &&
            g_manual_takeoff_animation_actor.compare_exchange_strong(
                expected_actor,
                0u,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=modern_manual_takeoff_animation_started"
                " actor=0x%08" PRIX32 " entry_state=%" PRIu32 "\n",
                actor,
                entry_state
            );
            std::fflush(stderr);
        }
        expected_actor = actor;
        if (entry_state == kFlyingState &&
            g_automatic_takeoff_animation_actor.compare_exchange_strong(
                expected_actor,
                0u,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE"
                " stage=modern_automatic_takeoff_animation_started"
                " actor=0x%08" PRIX32 " entry_state=%" PRIu32 "\n",
                actor,
                entry_state
            );
            std::fflush(stderr);
        }

        uint32_t authorized_actor =
            g_manual_grounded_entry_actor.load(std::memory_order_acquire);
        if (authorized_actor != 0u &&
            (authorized_actor != actor || entry_state != kGroundedEntryState)) {
            g_manual_grounded_entry_actor.compare_exchange_strong(
                authorized_actor,
                0u,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            );
            invalidate_landing_anchor();
        }
    }
    if (!gameplay_control_owner_active()) {
        return;
    }
    if (!player_one_actor_is_owned(rdram, actor)) {
        if (rdram != nullptr && read_u32(rdram, kPlayerOneOwnerSlot) == actor) {
            clear_pending_look();
            clear_manual_flight_actions();
            g_resync_aim.store(true, std::memory_order_release);
        }
        return;
    }
    uint32_t state = read_u32(rdram, actor + 0x8Cu);
    const uint32_t guided_missile =
        exact_guided_missile_for_player(rdram, actor);
    if (guided_missile != 0u) {
        begin_guided_missile_control(actor, guided_missile);
        return;
    }
    finish_guided_missile_control_if_stale(rdram, actor);

    const bool stunt_live_at_entry = state == kFlyingState &&
        (read_u16(rdram, actor + 0xB8u) != 0u ||
         read_u32(rdram, actor + 0xB4u) != 0u);
    if (stunt_live_at_entry) {
        restore_guest_stunt_pose(rdram, actor);
    }
    consume_manual_flight_edge(actor, state);
    state = read_u32(rdram, actor + 0x8Cu);
    clear_stale_manual_flight_requests(actor, state);
    process_quick_flip_action(rdram, actor, state);
    process_new_maneuver_actions(rdram, actor, state);
    if (advance_quick_turn(actor)) {
        clear_pending_look();
        return;
    }
    const bool original_stunt_active =
        read_u16(rdram, actor + 0xB8u) != 0u ||
        read_u32(rdram, actor + 0xB4u) != 0u;
    const bool original_stunt_pressed =
        (read_u16(rdram, kPlayerNormalizedPad + 2u) & kCDown) != 0u;
    const bool host_loop_active =
        static_cast<ActiveManeuver>(
            g_active_maneuver.load(std::memory_order_acquire)
        ) == ActiveManeuver::LoopDeLoop &&
        g_maneuver_actor.load(std::memory_order_acquire) == actor;
    if (state == kFlyingState &&
        (original_stunt_active || original_stunt_pressed) &&
        !host_loop_active) {
        return;
    }
    if (state == kAirborneRecoveryState) {
        return;
    }
    if (!state_supports_modern_control(state)) {
        clear_pending_look();
        g_resync_aim.store(true, std::memory_order_release);
        return;
    }

    std::scoped_lock lock(g_aim_mutex);
    if (g_resync_aim.load(std::memory_order_acquire) ||
        !g_aim_ready || g_aim_actor != actor) {
        return;
    }
    if (!update_aim_from_pending_input(actor)) {
        return;
    }
    g_aim_input_consumed_this_update.store(true, std::memory_order_release);
    const uint64_t frame =
        g_active_player_frame_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (runtime_telemetry_enabled() &&
        (frame == 1 || (frame % 600) == 0)) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_player_attached frame=%" PRIu64
            " actor=0x%08" PRIX32 " state=%" PRIu32 "\n",
            frame,
            actor,
            state
        );
        std::fflush(stderr);
    }
}

extern "C" uint32_t bumble_apply_modern_guided_missile_aim(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr ||
        !bumble::modern_controls::enabled() ||
        !g_gameplay_active.load(std::memory_order_acquire) ||
        !gameplay_control_owner_active()) {
        return 0u;
    }

    const uint32_t missile_actor = guest_u32(context->r16);
    const uint32_t player_actor = read_u32(rdram, kPlayerOneOwnerSlot);
    if (exact_guided_missile_for_player(rdram, player_actor) != missile_actor) {
        return 0u;
    }
    begin_guided_missile_control(player_actor, missile_actor);

    const int32_t delta_x = static_cast<int32_t>(
        g_pending_look_x.exchange(0, std::memory_order_acq_rel)
    );
    const int32_t delta_y = static_cast<int32_t>(
        g_pending_look_y.exchange(0, std::memory_order_acq_rel)
    );
    float sensitivity = 0.0f;
    float pitch_sign = 1.0f;
    {
        std::scoped_lock lock(g_aim_mutex);
        sensitivity = g_look_sensitivity;
        pitch_sign = g_invert_look_y ? -1.0f : 1.0f;
    }

    const float authored_pitch = float_from_word(
        read_u32(rdram, missile_actor + 0x50u)
    );
    const float authored_yaw = float_from_word(
        read_u32(rdram, missile_actor + 0x54u)
    );
    if (!std::isfinite(authored_pitch) || !std::isfinite(authored_yaw) ||
        !std::isfinite(sensitivity)) {
        return 0u;
    }

    const float pitch = std::clamp(
        authored_pitch +
            static_cast<float>(delta_y) * sensitivity * pitch_sign,
        -kMaximumPitch,
        kMaximumPitch
    );
    const float yaw = std::remainder(
        authored_yaw + static_cast<float>(delta_x) * sensitivity,
        360.0f
    );
    write_u32(rdram, missile_actor + 0x50u, word_from_float(pitch));
    write_u32(rdram, missile_actor + 0x54u, word_from_float(yaw));

    const uint64_t count = g_guided_missile_aim_update_count.fetch_add(
        1u,
        std::memory_order_relaxed
    ) + 1u;
    if (runtime_telemetry_enabled() &&
        (count <= 5u || count % 120u == 0u)) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_guided_missile_aim_update"
            " count=%" PRIu64 " player=0x%08" PRIX32
            " missile=0x%08" PRIX32 " delta_x=%" PRId32
            " delta_y=%" PRId32 " yaw=%.3f pitch=%.3f"
            " legacy_stick_bypassed=1 camera_owner=game_guided_solver\n",
            count,
            player_actor,
            missile_actor,
            delta_x,
            delta_y,
            static_cast<double>(yaw),
            static_cast<double>(pitch)
        );
        std::fflush(stderr);
    }
    return 1u;
}

extern "C" void bumble_begin_modern_barrel_roll_visual(
    uint8_t* rdram,
    recomp_context* context
) {
    g_barrel_roll_saved_actor = 0u;
    if (rdram == nullptr || context == nullptr) {
        return;
    }

    const uint32_t actor = guest_u32(context->r17);
    if (actor == 0u || !living_player_one_actor_is_owned(rdram, actor)) {
        return;
    }

    const Vec3 right = read_actor_vector(rdram, actor, 0x10u);
    const Vec3 up = read_actor_vector(rdram, actor, 0x20u);
    const Vec3 forward = read_actor_vector(rdram, actor, 0x30u);
    if (!all_finite(right) || !all_finite(up) || !all_finite(forward)) {
        return;
    }

    std::array<Vec3, 3> stunt_basis{};
    bool stunt_visual = false;
    bool stunt_quick_flip = false;
    {
        std::scoped_lock lock(g_stunt_visual_mutex);
        if (g_stunt_visual_actor == actor) {
            stunt_basis = g_stunt_visual_basis;
            stunt_quick_flip = g_stunt_visual_quick_flip;
            stunt_visual = all_finite(stunt_basis[0]) &&
                all_finite(stunt_basis[1]) && all_finite(stunt_basis[2]);
        }
    }
    if (stunt_visual) {
        g_barrel_roll_saved_basis = {right, up, forward};
        g_barrel_roll_saved_actor = actor;
        write_actor_vector(rdram, actor, 0x10u, stunt_basis[0]);
        write_actor_vector(rdram, actor, 0x20u, stunt_basis[1]);
        write_actor_vector(rdram, actor, 0x30u, stunt_basis[2]);
        const uint64_t count = g_stunt_visual_scope_count.fetch_add(
            1u,
            std::memory_order_relaxed
        ) + 1u;
        if (runtime_telemetry_enabled() &&
            (count == 1u || (count % 120u) == 0u)) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=modern_original_stunt_model_visual"
                " count=%" PRIu64 " actor=0x%08" PRIX32 " kind=%s"
                " scope=model_matrix_only camera_rotation=0"
                " capsule_rotation=0\n",
                count,
                actor,
                stunt_quick_flip ? "quick_flip" : "loop_de_loop"
            );
            std::fflush(stderr);
        }
        return;
    }

    if (actor !=
        g_barrel_roll_visual_actor.load(std::memory_order_acquire)) {
        return;
    }
    float angle = float_from_word(
        g_barrel_roll_visual_angle_bits.load(std::memory_order_acquire)
    );
    if (!std::isfinite(angle)) {
        return;
    }
    angle = std::remainder(angle, 360.0f);
    if (std::abs(angle) < 0.001f) {
        return;
    }

    const float radians = angle * kBuckAngleRadiansPerDegree;
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    const Vec3 rolled_right = add(
        multiply(right, cosine),
        multiply(up, sine)
    );
    const Vec3 rolled_up = add(
        multiply(up, cosine),
        multiply(right, -sine)
    );
    if (!all_finite(rolled_right) || !all_finite(rolled_up)) {
        return;
    }

    g_barrel_roll_saved_basis = {right, up, forward};
    g_barrel_roll_saved_actor = actor;
    write_actor_vector(rdram, actor, 0x10u, rolled_right);
    write_actor_vector(rdram, actor, 0x20u, rolled_up);
    write_actor_vector(rdram, actor, 0x30u, forward);
    const uint64_t count = g_barrel_roll_visual_scope_count.fetch_add(
        1u,
        std::memory_order_relaxed
    ) + 1u;
    if (runtime_telemetry_enabled() &&
        (count == 1u || (count % 120u) == 0u)) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_barrel_roll_visual"
            " count=%" PRIu64 " actor=0x%08" PRIX32
            " angle=%.3f scope=model_matrix_only"
            " camera_rotation=0 capsule_rotation=0\n",
            count,
            actor,
            static_cast<double>(angle)
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_end_modern_barrel_roll_visual(
    uint8_t* rdram,
    recomp_context* context
) {
    (void)context;
    const uint32_t actor = g_barrel_roll_saved_actor;
    g_barrel_roll_saved_actor = 0u;
    if (rdram == nullptr || actor == 0u) {
        return;
    }

    write_actor_vector(rdram, actor, 0x10u, g_barrel_roll_saved_basis[0]);
    write_actor_vector(rdram, actor, 0x20u, g_barrel_roll_saved_basis[1]);
    write_actor_vector(rdram, actor, 0x30u, g_barrel_roll_saved_basis[2]);
}

extern "C" void bumble_resolve_modern_airborne_collision(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t actor = guest_u32(context->r17);
    const uint32_t original_collision_result = guest_u32(context->r2);
    if (!modern_state2_update_is_owned(rdram, actor) ||
        !living_player_one_actor_is_owned(rdram, actor)) {
        return;
    }

    if (original_collision_result == 0u) {
        capture_collision_safe_anchor(rdram, actor);
        return;
    }

    // Consume the existing query result; rerunning it requires unavailable call state.
    // Resolve from the checkpoint, before temporary side-probe offsets.
    Vec3 checkpoint{};
    Vec3 checkpoint_target{};
    const bool movement_path_resolved = consume_movement_checkpoint(
        actor,
        checkpoint,
        &checkpoint_target
    );

    Vec3 requested_delta{};
    Vec3 resolved_position = read_actor_vector(rdram, actor, 0x40u);
    bool fallback_anchor_restored = false;
    Vec3 certified_clear_anchor{};
    const bool certified_clear_anchor_available = read_collision_safe_anchor(
        rdram,
        actor,
        certified_clear_anchor
    );
    bool certified_anchor_retained = false;

    if (movement_path_resolved) {
        requested_delta = add(
            checkpoint_target,
            multiply(checkpoint, -1.0f)
        );

        resolved_position = checkpoint;
        if (certified_clear_anchor_available) {
            resolved_position = certified_clear_anchor;
            certified_anchor_retained = true;
        }
        float minimum_height = 0.0f;
        if (read_surface_glide_height(actor, minimum_height) &&
            resolved_position.y < minimum_height) {
            resolved_position.y = minimum_height;
        }
        write_actor_vector(rdram, actor, 0x40u, resolved_position);
    } else {
        fallback_anchor_restored =
            restore_collision_safe_anchor(rdram, actor);
        resolved_position = read_actor_vector(rdram, actor, 0x40u);
    }

    bool aim_frame_restored = false;
    {
        std::scoped_lock lock(g_aim_mutex);
        if (g_aim_ready && g_aim_actor == actor) {
            write_aim_frame(rdram, actor, true);
            aim_frame_restored = true;
        }
    }

    MEM_W(0x5C, guest_address(actor)) = word_from_float(0.0f);
    g_airborne_collision_finalize_actor.store(actor, std::memory_order_release);
    context->r2 = 0u;

    const bool full_rollback = movement_path_resolved ||
        fallback_anchor_restored;
    const uint64_t count = g_airborne_collision_rollback_count.fetch_add(
        1,
        std::memory_order_relaxed
    ) + 1;
    const float resolved_distance = movement_path_resolved
        ? length(add(resolved_position, multiply(checkpoint, -1.0f)))
        : 0.0f;
    if (runtime_telemetry_enabled() &&
        (aim_telemetry_all_enabled() || count == 1 || count % 120 == 0)) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_airborne_collision_slide"
            " count=%" PRIu64 " actor=0x%08" PRIX32
            " source_pc=0x8005AF20 original_collision_result=0x%08" PRIX32
            " authoritative_collision_consumed=1"
            " resolution=collision_certified_full_rollback"
            " movement_path_resolved=%d"
            " requested_delta=(%.4f,%.4f,%.4f)"
            " resolved_position=(%.4f,%.4f,%.4f)"
            " resolved_distance=%.6f full_rollback=%d"
            " fallback_anchor_restored=%d aim_frame_restored=%d"
            " certified_clear_anchor_available=%d"
            " certified_anchor_retained=%d"
            " vertical_recoil_discarded=1 classification=side_or_object"
            " state4_suppressed=1 resolved_transform_retained=1\n",
            count,
            actor,
            original_collision_result,
            movement_path_resolved ? 1 : 0,
            static_cast<double>(requested_delta.x),
            static_cast<double>(requested_delta.y),
            static_cast<double>(requested_delta.z),
            static_cast<double>(resolved_position.x),
            static_cast<double>(resolved_position.y),
            static_cast<double>(resolved_position.z),
            static_cast<double>(resolved_distance),
            full_rollback ? 1 : 0,
            fallback_anchor_restored ? 1 : 0,
            aim_frame_restored ? 1 : 0,
            certified_clear_anchor_available ? 1 : 0,
            certified_anchor_retained ? 1 : 0
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_gate_manual_landing(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t actor = guest_u32(context->r17);
    if (!modern_state2_update_is_owned(rdram, actor)) {
        invalidate_movement_checkpoint();
        return;
    }

    const uint32_t state = read_u32(rdram, actor + 0x8Cu);
    const bool manual_land_requested =
        g_manual_land_request_actor.load(std::memory_order_acquire) == actor;
    const bool bottom_contact = guest_u32(context->r2) != 0u;
    const bool side_or_object_contact =
        g_airborne_collision_finalize_actor.load(std::memory_order_acquire) ==
            actor;
    if (!bottom_contact && state == kFlyingState) {
        clear_surface_glide(actor);
    }

    if (manual_land_requested) {
        g_manual_landing_authorized_actor.store(0u, std::memory_order_release);
        invalidate_landing_anchor();

        if (state != kFlyingState || side_or_object_contact) {
            const uint32_t remaining = age_buffered_landing_request(actor);
            invalidate_movement_checkpoint();
            context->r2 = 0;
            const uint64_t count =
                g_manual_land_obstacle_rejection_count.fetch_add(
                    1,
                    std::memory_order_relaxed
                ) + 1;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=modern_manual_land_obstacle_rejected"
                " count=%" PRIu64 " actor=0x%08" PRIX32
                " source_pc=0x8005B14C observed_state=%" PRIu32
                " required_state=%" PRIu32
                " side_or_object_contact=%d request_consumed=0"
                " request_retained=%d remaining_updates=%" PRIu32
                " position_write=0 position_snap=0\n",
                count,
                actor,
                state,
                kFlyingState,
                side_or_object_contact ? 1 : 0,
                remaining > 0u ? 1 : 0,
                remaining
            );
            std::fflush(stderr);
            return;
        }

        if (!bottom_contact) {
            const uint32_t remaining = age_buffered_landing_request(actor);
            invalidate_movement_checkpoint();
            context->r2 = 0;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=modern_manual_land_buffered"
                " actor=0x%08" PRIX32 " state=%" PRIu32
                " reason=no_current_support request_consumed=0"
                " request_retained=%d remaining_updates=%" PRIu32
                " position_write=0 maximum_snap_distance=0\n",
                actor,
                state,
                remaining > 0u ? 1 : 0,
                remaining
            );
            std::fflush(stderr);
            return;
        }

        uint32_t expected_actor = actor;
        const bool request_consumed =
            g_manual_land_request_actor.compare_exchange_strong(
                expected_actor,
                0u,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            );
        g_manual_land_buffer_updates_remaining.store(
            0u,
            std::memory_order_release
        );
        if (!request_consumed) {
            // The token can be revoked concurrently; leave the integrated position unchanged.
            invalidate_movement_checkpoint();
            context->r2 = 0;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=modern_manual_land_noop"
                " actor=0x%08" PRIX32 " state=%" PRIu32
                " reason=request_revoked request_consumed=0"
                " position_write=0 maximum_snap_distance=0\n",
                actor,
                state
            );
            std::fflush(stderr);
            return;
        }

        const bool movement_rolled_back = restore_movement_checkpoint(
            rdram,
            actor
        );
        const Vec3 supported_position = read_actor_vector(rdram, actor, 0x40u);
        if (!movement_rolled_back || !all_finite(supported_position)) {
            context->r2 = 0;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=modern_manual_land_noop"
                " actor=0x%08" PRIX32 " state=%" PRIu32
                " reason=missing_precontact_checkpoint request_consumed=%d"
                " deferred_landing=0 position_snap=0\n",
                actor,
                state,
                request_consumed ? 1 : 0
            );
            std::fflush(stderr);
            return;
        }

        MEM_W(0x5C, guest_address(actor)) = word_from_float(0.0f);
        context->r2 = 1;
        capture_landing_anchor(rdram, actor);
        g_manual_landing_authorized_actor.store(actor, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_manual_land_surface_selected"
            " actor=0x%08" PRIX32 " state=%" PRIu32
            " source_pc=0x8005B14C"
            " position=(%.3f,%.3f,%.3f)"
            " collision_owner=game_owned_current_bottom_probe"
            " current_support=1 movement_rollback=1"
            " maximum_snap_distance=0\n",
            actor,
            state,
            static_cast<double>(supported_position.x),
            static_cast<double>(supported_position.y),
            static_cast<double>(supported_position.z)
        );
        std::fflush(stderr);
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_manual_land_authorized"
            " actor=0x%08" PRIX32
            " source_pc=0x8005B14C observed_state=%" PRIu32
            " target_state=%" PRIu32
            " support_position=(%.3f,%.3f,%.3f)"
            " current_support=1 input_buffer_consumed=1 position_snap=0"
            " forced_original_branch=1"
            " landing_anchor=1\n",
            actor,
            state,
            kGroundedEntryState,
            static_cast<double>(supported_position.x),
            static_cast<double>(supported_position.y),
            static_cast<double>(supported_position.z)
        );
        std::fflush(stderr);
        return;
    }

    if (side_or_object_contact || state != kFlyingState) {
        invalidate_movement_checkpoint();
        context->r2 = 0;
        return;
    }

    if (context->r2 == 0) {
        invalidate_movement_checkpoint();
        return;
    }

    bool downward_clamped = false;
    const bool checkpoint_resolved = resolve_floor_contact_from_checkpoint(
        rdram,
        actor,
        downward_clamped
    );
    if (!checkpoint_resolved) {
        restore_airborne_idle_anchor(rdram, actor);
    }
    const bool glide_height_retained = retain_surface_glide_height(rdram, actor);
    const uint64_t rollback_count = g_ground_contact_rollback_count.fetch_add(
        1,
        std::memory_order_relaxed
    ) + 1;
    if (runtime_telemetry_enabled() &&
        (aim_telemetry_all_enabled() || rollback_count == 1 ||
         rollback_count % 120 == 0)) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_ground_contact_slide"
            " count=%" PRIu64 " actor=0x%08" PRIX32
            " source_pc=0x8005B14C manual_land=%d"
            " downward_clamped=%d upward_motion_preserved=%d"
            " planar_displacement_preserved=%d glide_height_retained=%d\n",
            rollback_count,
            actor,
            manual_land_requested ? 1 : 0,
            downward_clamped ? 1 : 0,
            checkpoint_resolved && !downward_clamped ? 1 : 0,
            checkpoint_resolved ? 1 : 0,
            glide_height_retained ? 1 : 0
        );
        std::fflush(stderr);
    }

    context->r2 = 0;
    const Vec3 held_position = read_actor_vector(rdram, actor, 0x40u);
    const uint64_t floor_count = g_airborne_floor_hold_count.fetch_add(
        1,
        std::memory_order_relaxed
    ) + 1;
    if (runtime_telemetry_enabled() &&
        (aim_telemetry_all_enabled() || floor_count == 1 ||
         floor_count % 120 == 0)) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_airborne_floor_slide"
            " count=%" PRIu64 " actor=0x%08" PRIX32
            " source_pc=0x8005B14C position=(%.3f,%.3f,%.3f)"
            " speed=preserved checkpoint_height=restored\n",
            floor_count,
            actor,
            static_cast<double>(held_position.x),
            static_cast<double>(held_position.y),
            static_cast<double>(held_position.z)
        );
        std::fflush(stderr);
    }
    const uint64_t count = g_automatic_landing_suppressed_count.fetch_add(
        1,
        std::memory_order_relaxed
    ) + 1;
    if (runtime_telemetry_enabled() &&
        (count == 1 || count % 120 == 0)) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_automatic_landing_suppressed"
            " count=%" PRIu64 " actor=0x%08" PRIX32
            " observed_state=%" PRIu32
            " source_pc=0x8005B14C fallback_pc=0x8005B1B8"
            " collision_response=original_non_landing\n",
            count,
            actor,
            state
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_retract_modern_airborne_landing_gear(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t actor = guest_u32(context->r17);
    if (!modern_state2_update_is_owned(rdram, actor) ||
        !living_player_one_actor_is_owned(rdram, actor)) {
        return;
    }
    const uint32_t state = read_u32(rdram, actor + 0x8Cu);
    if (state != kFlyingState && state != kAirborneCollisionState) {
        return;
    }

    const uint32_t previous_frame = MEM_BU(0xB1, guest_address(actor));
    if (previous_frame == 0u) {
        return;
    }
    MEM_B(0xB1, guest_address(actor)) = 0;
    const uint64_t count =
        g_airborne_landing_gear_retraction_count.fetch_add(
            1,
            std::memory_order_relaxed
        ) + 1;
    if (runtime_telemetry_enabled() &&
        (aim_telemetry_all_enabled() || count == 1 || count % 120 == 0)) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_airborne_landing_gear_retracted"
            " count=%" PRIu64 " actor=0x%08" PRIX32
            " source_pc=0x8005B28C state=%" PRIu32
            " previous_frame=%" PRIu32 " grounded_commit=0\n",
            count,
            actor,
            state,
            previous_frame
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_stabilize_modern_airborne_collision_recovery(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t actor = guest_u32(context->r17);
    if (!bumble::modern_controls::enabled() ||
        !g_gameplay_active.load(std::memory_order_acquire) ||
        g_player_update_actor.load(std::memory_order_acquire) != actor ||
        !living_player_one_actor_is_owned(rdram, actor)) {
        return;
    }

    uint32_t expected_actor = actor;
    const bool collision_finalized =
        g_airborne_collision_finalize_actor.compare_exchange_strong(
            expected_actor,
            0u,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        );
    const uint32_t entry_state =
        g_player_update_entry_state.load(std::memory_order_acquire);
    const uint32_t live_state = read_u32(rdram, actor + 0x8Cu);
    if (!collision_finalized || entry_state != kFlyingState ||
        live_state != kFlyingState) {
        return;
    }

    MEM_W(0x5C, guest_address(actor)) = word_from_float(0.0f);
    bool aim_frame_restored = false;
    {
        std::scoped_lock lock(g_aim_mutex);
        if (g_aim_ready && g_aim_actor == actor) {
            write_aim_frame(rdram, actor, true);
            aim_frame_restored = true;
        }
    }
    invalidate_airborne_idle_anchor();
    const uint64_t count =
        g_airborne_collision_recovery_restore_count.fetch_add(
            1,
            std::memory_order_relaxed
        ) + 1;
    if (runtime_telemetry_enabled() &&
        (aim_telemetry_all_enabled() || count == 1 || count % 120 == 0)) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE"
            " stage=modern_airborne_collision_recovery_stabilized"
            " count=%" PRIu64 " actor=0x%08" PRIX32
            " source_pc=0x8005C644 entry_state=%" PRIu32
            " retained_state=%" PRIu32
            " collision_finalized=%d authored_state_preserved=1"
            " aim_frame_restored=%d vertical_recoil_discarded=1"
            " resolved_transform_retained=%d"
            " state_write=0 speed_cleared=1\n",
            count,
            actor,
            entry_state,
            kFlyingState,
            collision_finalized ? 1 : 0,
            aim_frame_restored ? 1 : 0,
            collision_finalized ? 1 : 0
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_guard_manual_landing_state_store(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t actor = guest_u32(context->r17);
    if (!modern_state2_update_is_owned(rdram, actor) ||
        guest_u32(context->r2) != kGroundedEntryState) {
        return;
    }

    uint32_t authorized_actor = actor;
    const bool store_authorized =
        g_manual_landing_authorized_actor.compare_exchange_strong(
            authorized_actor,
            0u,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        );
    if (store_authorized) {
        g_manual_grounded_entry_actor.store(actor, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_manual_land_committed"
            " actor=0x%08" PRIX32
            " source_pc=0x8005B168 target_state=%" PRIu32
            " current_support_proven=1 position_snap=0\n",
            actor,
            kGroundedEntryState
        );
        std::fflush(stderr);
        return;
    }

    const uint32_t observed_state = read_u32(rdram, actor + 0x8Cu);
    const uint32_t retained_state = observed_state == kAirborneCollisionState
        ? kAirborneCollisionState
        : kFlyingState;
    context->r2 = retained_state;
    invalidate_landing_anchor();
    const uint64_t count =
        g_automatic_landing_store_suppressed_count.fetch_add(
            1,
            std::memory_order_relaxed
        ) + 1;
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=modern_automatic_landing_store_suppressed"
        " count=%" PRIu64 " actor=0x%08" PRIX32
        " source_pc=0x8005B168 attempted_state=%" PRIu32
        " observed_state=%" PRIu32 " retained_state=%" PRIu32
        " next_automatic_pc=0x8005B6A4\n",
        count,
        actor,
        kGroundedEntryState,
        observed_state,
        retained_state
    );
    std::fflush(stderr);
}

extern "C" void bumble_guard_manual_grounded_entry_state_store(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t actor = guest_u32(context->r17);
    if (!bumble::modern_controls::enabled() ||
        !g_gameplay_active.load(std::memory_order_acquire) ||
        g_player_update_actor.load(std::memory_order_acquire) != actor ||
        g_player_update_entry_state.load(std::memory_order_acquire) !=
            kGroundedEntryState ||
        !living_player_one_actor_is_owned(rdram, actor) ||
        read_u32(rdram, actor + 0x8Cu) != kGroundedEntryState ||
        guest_u32(context->r2) != kGroundedFirstState) {
        return;
    }

    uint32_t authorized_actor = actor;
    if (g_manual_grounded_entry_actor.compare_exchange_strong(
            authorized_actor,
            0u,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        const bool landing_anchor_restored = restore_landing_anchor(rdram, actor);
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_manual_grounded_entry_committed"
            " actor=0x%08" PRIX32
            " source_pc=0x8005B6A4 entry_state=%" PRIu32
            " target_state=%" PRIu32 " landing_anchor_restored=%d\n",
            actor,
            kGroundedEntryState,
            kGroundedFirstState,
            landing_anchor_restored ? 1 : 0
        );
        std::fflush(stderr);
        return;
    }

    context->r2 = kFlyingState;
    invalidate_landing_anchor();
    const uint64_t count =
        g_automatic_grounded_entry_suppressed_count.fetch_add(
            1,
            std::memory_order_relaxed
        ) + 1;
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=modern_automatic_grounded_entry_suppressed"
        " count=%" PRIu64 " actor=0x%08" PRIX32
        " source_pc=0x8005B6A4 entry_state=%" PRIu32
        " attempted_state=%" PRIu32 " retained_state=%" PRIu32
        " preceding_guard_pc=0x8005B168\n",
        count,
        actor,
        kGroundedEntryState,
        kGroundedFirstState,
        kFlyingState
    );
    std::fflush(stderr);
}

extern "C" void bumble_observe_automatic_ground_exit(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t actor = guest_u32(context->r17);
    if (!modern_player_hook_is_owned(rdram, actor)) {
        return;
    }
    const uint32_t state = read_u32(rdram, actor + 0x8Cu);
    if (state < kGroundedFirstState || state > kGroundedLastState ||
        guest_u32(context->r2) != kFlyingState) {
        return;
    }

    g_automatic_takeoff_pending_actor.store(actor, std::memory_order_release);
    const uint64_t count = g_automatic_ground_exit_count.fetch_add(
        1,
        std::memory_order_relaxed
    ) + 1;
    if (runtime_telemetry_enabled() &&
        (count == 1 || count % 120 == 0)) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_automatic_ground_exit"
            " count=%" PRIu64 " actor=0x%08" PRIX32
            " source_pc=0x8005BE44 prior_state=%" PRIu32
            " target_state=%" PRIu32 " guest_mutation=0\n",
            count,
            actor,
            state,
            kFlyingState
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_observe_automatic_ground_exit_commit(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t actor = guest_u32(context->r17);
    uint32_t expected_actor = actor;
    if (!g_automatic_takeoff_pending_actor.compare_exchange_strong(
            expected_actor,
            0u,
            std::memory_order_acq_rel,
            std::memory_order_acquire) ||
        !living_player_one_actor_is_owned(rdram, actor) ||
        read_u32(rdram, actor + 0x8Cu) != kFlyingState) {
        return;
    }

    uint32_t manual_actor = actor;
    g_manual_takeoff_request_actor.compare_exchange_strong(
        manual_actor,
        0u,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
    g_manual_takeoff_injection_actor.store(0u, std::memory_order_release);
    clear_surface_glide(actor);
    invalidate_collision_safe_anchor();
    invalidate_landing_anchor();
    invalidate_airborne_idle_anchor();
    invalidate_movement_checkpoint();
    g_automatic_takeoff_animation_actor.store(
        actor,
        std::memory_order_release
    );
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=modern_automatic_ground_exit_committed"
        " actor=0x%08" PRIX32
        " source_pc=0x8005BE48 observed_pc=0x8005C008"
        " target_state=%" PRIu32 "\n",
        actor,
        kFlyingState
    );
    std::fflush(stderr);
}

extern "C" void bumble_apply_manual_takeoff_press(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t actor = guest_u32(context->r17);
    if (!modern_player_hook_is_owned(rdram, actor)) {
        return;
    }
    const uint32_t state = read_u32(rdram, actor + 0x8Cu);
    if ((state < kGroundedEntryState || state > kGroundedLastState) ||
        g_manual_takeoff_request_actor.load(std::memory_order_acquire) !=
            actor) {
        return;
    }

    context->r2 = guest_u32(context->r2) | kButtonA;
    g_manual_takeoff_injection_actor.store(actor, std::memory_order_release);
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=modern_manual_takeoff_injected"
        " actor=0x%08" PRIX32
        " source_pc=0x8005C0D8 state=%" PRIu32 "\n",
        actor,
        state
    );
    std::fflush(stderr);
}

extern "C" void bumble_bypass_manual_takeoff_state7_exclusion(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t actor = guest_u32(context->r17);
    if (g_manual_takeoff_injection_actor.load(std::memory_order_acquire) !=
            actor ||
        g_manual_takeoff_request_actor.load(std::memory_order_acquire) !=
            actor ||
        !living_player_one_actor_is_owned(rdram, actor) ||
        read_u32(rdram, actor + 0x8Cu) != 7u ||
        guest_u32(context->r3) != 7u) {
        return;
    }

    context->r3 = kGroundedFirstState;
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=modern_manual_takeoff_state7_unblocked"
        " actor=0x%08" PRIX32
        " source_pc=0x8005C0E8 compared_state=7 effective_state=%" PRIu32
        " guest_state_mutation=0\n",
        actor,
        kGroundedFirstState
    );
    std::fflush(stderr);
}

extern "C" void bumble_bypass_manual_takeoff_resource_gate(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t actor = guest_u32(context->r17);
    if (g_manual_takeoff_injection_actor.load(std::memory_order_acquire) !=
            actor ||
        g_manual_takeoff_request_actor.load(std::memory_order_acquire) !=
            actor ||
        !living_player_one_actor_is_owned(rdram, actor)) {
        return;
    }

    if (!(context->f2.fl > 0.0f)) {
        context->f2.fl = 1.0f;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_manual_takeoff_gate_unblocked"
            " actor=0x%08" PRIX32
            " source_pc=0x8005C114 compare_value=1.000"
            " guest_record_mutation=0\n",
            actor
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_observe_manual_takeoff_commit(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t actor = guest_u32(context->r17);
    uint32_t expected_actor = actor;
    if (!g_manual_takeoff_injection_actor.compare_exchange_strong(
            expected_actor,
            0u,
            std::memory_order_acq_rel,
            std::memory_order_acquire) ||
        !living_player_one_actor_is_owned(rdram, actor)) {
        return;
    }

    const uint32_t state = read_u32(rdram, actor + 0x8Cu);
    if (state != kFlyingState) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_manual_takeoff_deferred"
            " actor=0x%08" PRIX32
            " observed_pc=0x8005C1AC observed_state=%" PRIu32
            " request_retained=1\n",
            actor,
            state
        );
        std::fflush(stderr);
        return;
    }

    uint32_t request_actor = actor;
    const bool request_consumed =
        g_manual_takeoff_request_actor.compare_exchange_strong(
            request_actor,
            0u,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        );
    clear_surface_glide(actor);
    invalidate_collision_safe_anchor();
    invalidate_landing_anchor();
    g_manual_land_request_actor.store(0u, std::memory_order_release);
        g_manual_land_buffer_updates_remaining.store(
            0u,
            std::memory_order_release
        );
    g_manual_landing_authorized_actor.store(0u, std::memory_order_release);
    invalidate_airborne_idle_anchor();
    invalidate_movement_checkpoint();
    g_manual_takeoff_animation_actor.store(actor, std::memory_order_release);
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=modern_manual_takeoff_committed"
        " actor=0x%08" PRIX32
        " source_pc=0x8005C1A8 observed_pc=0x8005C1AC"
        " target_state=%" PRIu32 " request_consumed=%d"
        " original_impulse=1\n",
        actor,
        kFlyingState,
        request_consumed ? 1 : 0
    );
    std::fflush(stderr);
}

extern "C" void bumble_apply_modern_player_aim(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!bumble::modern_controls::enabled()) {
        return;
    }
    if (!g_gameplay_active.load(std::memory_order_acquire)) {
        return;
    }
    const uint32_t actor = guest_u32(context->r17);
    if (!living_player_one_actor_is_owned(rdram, actor)) {
        return;
    }
    if (exact_guided_missile_for_player(rdram, actor) != 0u) {
        return;
    }
    const uint32_t state = read_u32(rdram, actor + 0x8Cu);
    if (!state_supports_modern_control(state)) {
        return;
    }

    const uint16_t loop_timer = read_u16(rdram, actor + 0xB8u);
    const uint32_t flip_flag = read_u32(rdram, actor + 0xB4u);
    if (loop_timer != 0u || flip_flag != 0u) {
        return;
    }

    std::scoped_lock lock(g_aim_mutex);
    if (g_resync_aim.load(std::memory_order_acquire) ||
        !g_aim_ready || g_aim_actor != actor) {
        if (!initialize_aim_from_actor_angles(rdram, actor)) {
            return;
        }
    }
    if (quick_turn_is_active_for(actor)) {
        clear_pending_look();
        write_aim_frame(rdram, actor, true);
        return;
    }
    if (!g_aim_input_consumed_this_update.exchange(
            true,
            std::memory_order_acq_rel)) {
        if (!update_aim_from_pending_input(actor)) {
            return;
        }
    }
    write_aim_frame(rdram, actor, true);
}

extern "C" void bumble_finalize_modern_player_aim(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!bumble::modern_controls::enabled() ||
        !g_gameplay_active.load(std::memory_order_acquire) ||
        rdram == nullptr || context == nullptr) {
        return;
    }
    const uint32_t actor = guest_u32(context->r17);
    if (!living_player_one_actor_is_owned(rdram, actor)) {
        return;
    }
    if (exact_guided_missile_for_player(rdram, actor) != 0u) {
        clear_stunt_visual(actor);
        return;
    }
    const uint32_t state = read_u32(rdram, actor + 0x8Cu);
    if (!state_supports_modern_control(state)) {
        clear_stunt_visual(actor);
        return;
    }

    const uint16_t loop_timer = read_u16(rdram, actor + 0xB8u);
    const uint32_t flip_flag = read_u32(rdram, actor + 0xB4u);
    const bool stunt_live = state == kFlyingState &&
        (loop_timer != 0u || flip_flag != 0u);
    if (!stunt_live) {
        bool released = false;
        bool released_quick_flip = false;
        {
            std::scoped_lock visual_lock(g_stunt_visual_mutex);
            if (g_stunt_visual_actor == actor) {
                released = true;
                released_quick_flip = g_stunt_visual_quick_flip;
                g_stunt_visual_actor = 0u;
                g_stunt_visual_basis = {};
                g_stunt_visual_euler = {};
                g_stunt_visual_quick_flip = false;
            }
        }
        if (released) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=modern_original_stunt_pose_released"
                " actor=0x%08" PRIX32 " kind=%s"
                " camera_snap=0 model_pose_isolated=1\n",
                actor,
                released_quick_flip ? "quick_flip" : "loop_de_loop"
            );
            std::fflush(stderr);
        }
        bumble_apply_modern_player_aim(rdram, context);
        return;
    }

    const float pitch = float_from_word(read_u32(rdram, actor + 0x50u));
    const float yaw = float_from_word(read_u32(rdram, actor + 0x54u));
    const float roll = float_from_word(read_u32(rdram, actor + 0x58u));
    std::array<Vec3, 3> stunt_basis{};
    if (!build_scaled_basis_from_euler(
            pitch,
            yaw,
            roll,
            stunt_basis)) {
        clear_stunt_visual(actor);
        return;
    }

    bool first_pose = false;
    bool quick_flip_visual = flip_flag != 0u ||
        (quick_flip_sequence_active() &&
         g_quick_flip_actor.load(std::memory_order_acquire) == actor) ||
        quick_turn_is_active_for(actor);
    {
        std::scoped_lock visual_lock(g_stunt_visual_mutex);
        first_pose = g_stunt_visual_actor != actor;
        if (g_stunt_visual_actor == actor) {
            quick_flip_visual =
                g_stunt_visual_quick_flip || quick_flip_visual;
        }
        g_stunt_visual_actor = actor;
        g_stunt_visual_basis = stunt_basis;
        g_stunt_visual_euler = {pitch, yaw, roll};
        g_stunt_visual_quick_flip = quick_flip_visual;
    }

    float host_pitch = 0.0f;
    float host_yaw = 0.0f;
    {
        std::scoped_lock aim_lock(g_aim_mutex);
        if (g_resync_aim.load(std::memory_order_acquire) || !g_aim_ready ||
            g_aim_actor != actor) {
            return;
        }
        host_pitch = g_aim_pitch;
        host_yaw = g_aim_yaw;
        write_aim_frame(rdram, actor, true);
    }
    if (first_pose) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_original_stunt_pose_isolated"
            " actor=0x%08" PRIX32 " kind=%s loop_timer=%u"
            " guest_pitch=%.3f guest_yaw=%.3f guest_roll=%.3f"
            " host_pitch=%.3f host_yaw=%.3f"
            " scope=model_matrix_only camera_snap=0"
            " collision_orientation=host\n",
            actor,
            quick_flip_visual ? "quick_flip" : "loop_de_loop",
            static_cast<unsigned>(loop_timer),
            static_cast<double>(pitch),
            static_cast<double>(yaw),
            static_cast<double>(roll),
            static_cast<double>(host_pitch),
            static_cast<double>(host_yaw)
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_prepare_modern_player_movement(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!bumble::modern_controls::enabled()) {
        return;
    }
    if (!g_gameplay_active.load(std::memory_order_acquire)) {
        return;
    }
    const uint32_t actor = guest_u32(context->r17);
    if (!living_player_one_actor_is_owned(rdram, actor)) {
        return;
    }
    if (exact_guided_missile_for_player(rdram, actor) != 0u) {
        return;
    }
    const uint32_t state = read_u32(rdram, actor + 0x8Cu);
    if (!state_supports_modern_control(state)) {
        return;
    }

    if (state == kFlyingState) {
        enforce_surface_glide_height(rdram, actor);
    }

    if (state == kAirborneCollisionState) {
        invalidate_airborne_idle_anchor();
        return;
    }

    float forward_axis = decode_movement_axis(g_movement_forward);
    float strafe_axis = decode_movement_axis(g_movement_strafe);
    const float magnitude = std::hypot(forward_axis, strafe_axis);
    if (magnitude > 1.0f) {
        forward_axis /= magnitude;
        strafe_axis /= magnitude;
    }

    const bool maneuver_movement_pending = state == kFlyingState &&
        g_maneuver_actor.load(std::memory_order_acquire) == actor &&
        static_cast<ActiveManeuver>(
            g_active_maneuver.load(std::memory_order_acquire)
        ) != ActiveManeuver::None;
    if (state == kFlyingState && magnitude < 0.001f &&
        !maneuver_movement_pending) {
        const float previous_speed = float_from_word(
            read_u32(rdram, actor + 0x5Cu)
        );
        bool anchor_restored = restore_airborne_idle_anchor(rdram, actor);
        if (!anchor_restored) {
            capture_airborne_idle_anchor(rdram, actor);
            anchor_restored = restore_airborne_idle_anchor(rdram, actor);
        }
        MEM_W(0x5C, guest_address(actor)) = word_from_float(0.0f);
        const uint64_t idle_count = g_airborne_idle_hold_count.fetch_add(
            1,
            std::memory_order_relaxed
        ) + 1;
        if (runtime_telemetry_enabled() &&
            (idle_count == 1 || idle_count % 120 == 0)) {
            const uint32_t position_x = read_u32(rdram, actor + 0x40u);
            const uint32_t position_y = read_u32(rdram, actor + 0x44u);
            const uint32_t position_z = read_u32(rdram, actor + 0x48u);
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=modern_airborne_idle_hold"
                " count=%" PRIu64 " actor=0x%08" PRIX32
                " position_bits=(%08" PRIX32 ",%08" PRIX32 ",%08" PRIX32 ")"
                " previous_speed=%.6f held_speed=0.000000"
                " idle_anchor_restored=%d focus_owned=%d camera_owned=%d\n",
                idle_count,
                actor,
                position_x,
                position_y,
                position_z,
                static_cast<double>(previous_speed),
                anchor_restored ? 1 : 0,
                gameplay_control_owner_active() ? 1 : 0,
                player_one_actor_is_owned(rdram, actor) ? 1 : 0
            );
            std::fflush(stderr);
        }
    } else {
        invalidate_airborne_idle_anchor();
    }

    if (state == kFlyingState && modern_state2_update_is_owned(rdram, actor)) {
        capture_movement_checkpoint(rdram, actor);
        Vec3 safe_anchor{};
        if (!read_collision_safe_anchor(rdram, actor, safe_anchor)) {
            capture_collision_safe_anchor(rdram, actor);
        }
    }
    if (!gameplay_control_owner_active() ||
        !player_one_actor_is_owned(rdram, actor)) {
        return;
    }

    bumble::modern_controls::SpatialIntent spatial_intent{};
    if (!bumble::modern_controls::query_spatial_intent(
            rdram,
            actor,
            state,
            spatial_intent)) {
        return;
    }

    Vec3 aim_right{};
    Vec3 movement_up_axis{};
    {
        std::scoped_lock lock(g_aim_mutex);
        if (g_resync_aim.load(std::memory_order_acquire) ||
            !g_aim_ready || g_aim_actor != actor) {
            return;
        }
        aim_right = g_aim_right;
        movement_up_axis = g_aim_up;
    }

    Vec3 movement_direction{
        -spatial_intent.delta_x,
        -spatial_intent.delta_y,
        -spatial_intent.delta_z,
    };
    float requested_speed = spatial_intent.speed;
    if (!std::isfinite(requested_speed) || requested_speed < 0.0f) {
        return;
    }
    if (requested_speed <= 0.0f) {
        requested_speed = length(movement_direction);
    }
    const bool grounded = is_grounded_state(state);
    if (grounded) {
        movement_up_axis = {0.0f, 1.0f, 0.0f};
    }

    bool surface_glide_projected = false;
    const bool maneuver_active = spatial_intent.loop_de_loop_active ||
        spatial_intent.barrel_roll_active;
    if (!maneuver_active && state == kFlyingState &&
        g_surface_glide_actor.load(std::memory_order_acquire) == actor &&
        spatial_intent.delta_y < 0.0f) {
        movement_direction.y = 0.0f;
        requested_speed = length(movement_direction);
        movement_up_axis = {0.0f, 1.0f, 0.0f};
        surface_glide_projected = true;
    }
    Vec3 normalized_movement{};
    if (requested_speed < 0.001f ||
        !normalize(movement_direction, normalized_movement)) {
        write_aim_frame(rdram, actor, true);
        MEM_W(0x5C, guest_address(actor)) = word_from_float(0.0f);
        return;
    }

    Vec3 movement_right{};
    Vec3 movement_up{};
    if (maneuver_active) {
        const Vec3 projected_right = add(
            aim_right,
            multiply(
                normalized_movement,
                -dot(aim_right, normalized_movement)
            )
        );
        if (!normalize(projected_right, movement_right)) {
            movement_right = cross(movement_up_axis, normalized_movement);
        }
    } else {
        movement_right = cross(movement_up_axis, normalized_movement);
    }
    if (!normalize(movement_right, movement_right) ||
        !normalize(cross(normalized_movement, movement_right), movement_up)) {
        return;
    }
    write_actor_vector(
        rdram,
        actor,
        0x10u,
        multiply(movement_right, kBasisScale)
    );
    write_actor_vector(
        rdram,
        actor,
        0x20u,
        multiply(movement_up, kBasisScale)
    );
    write_actor_vector(
        rdram,
        actor,
        0x30u,
        multiply(normalized_movement, kBasisScale)
    );
    MEM_W(0x5C, guest_address(actor)) = word_from_float(requested_speed);
    if (state == kFlyingState && modern_state2_update_is_owned(rdram, actor)) {
        capture_movement_checkpoint(rdram, actor);
    }

    const uint64_t count =
        g_movement_frame_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (runtime_telemetry_enabled() &&
        (aim_telemetry_all_enabled() || count <= 5 || count % 120 == 0)) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_movement_frame count=%" PRIu64
            " actor=0x%08" PRIX32
            " forward=%.3f strafe=%.3f speed=%.3f surface=%s"
            " motion=%s collision=guest_bsp_integrator\n",
            count,
            actor,
            static_cast<double>(forward_axis),
            static_cast<double>(strafe_axis),
            static_cast<double>(requested_speed),
            grounded ? "grounded_planar" :
                (surface_glide_projected ? "airborne_surface_glide" : "flight_3d"),
            spatial_intent.loop_de_loop_active ? "loop_de_loop" :
                (spatial_intent.barrel_roll_active ? "barrel_roll" :
                    (g_sprint_held.load(std::memory_order_acquire) &&
                     !g_sprint_require_release.load(std::memory_order_acquire)
                        ? "forward_dash"
                        : "ordinary"))
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_mark_modern_gameplay_active(
    uint8_t* rdram,
    recomp_context* context
) {
    if (bumble::native_checkpoint::apply_pending_mission_restart(
            rdram,
            context)) {
        return;
    }
    bumble::menu_actions::observe_pause_menu(rdram);
    if (!bumble::modern_controls::enabled()) {
        return;
    }

    const bool pause_menu_visible = rdram != nullptr &&
        read_u32(rdram, kPauseMenuVisible) != 0u;
    const bool pause_menu_was_visible = g_pause_menu_active.exchange(
        pause_menu_visible,
        std::memory_order_acq_rel
    );
    if (pause_menu_visible != pause_menu_was_visible) {
        clear_pending_look();
        clear_manual_flight_actions();
        bumble::modern_controls::set_movement_input(0.0f, 0.0f);
        g_resync_aim.store(true, std::memory_order_release);
        g_aim_input_consumed_this_update.store(false, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_pause_input_context"
            " menu_active=%d was_active=%d"
            " wasd_owner=%s enter_owner=%s mouse_capture_target=%s\n",
            pause_menu_visible ? 1 : 0,
            pause_menu_was_visible ? 1 : 0,
            pause_menu_visible ? "guest_menu_stick" : "modern_movement",
            pause_menu_visible ? "guest_a" : "guest_start",
            pause_menu_visible ? "released" : "gameplay"
        );
        std::fflush(stderr);
    }
    if (pause_menu_visible) {
        return;
    }

    const uint32_t frontend_phase = rdram != nullptr
        ? read_u32(rdram, kFrontendObject + kFrontendPhaseOffset)
        : 0xFFFFFFFFu;
    const bool briefing_owns_player = exact_briefing_staging_is_live(rdram);
    const bool mission_card_owns_player = exact_mission_card_is_live(rdram) &&
        !g_mission_card_accepted.load(std::memory_order_acquire);
    if (briefing_owns_player || mission_card_owns_player) {
        g_gameplay_active.store(false, std::memory_order_release);
        clear_pending_look();
        clear_manual_flight_actions();
        bumble::modern_controls::set_movement_input(0.0f, 0.0f);
        g_resync_aim.store(true, std::memory_order_release);
        if (!g_gameplay_arm_deferred_logged.exchange(
                true,
                std::memory_order_acq_rel)) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=modern_gameplay_input_deferred"
                " source=frontend_staging phase=0x%08" PRIX32
                " briefing=%d mission_card=%d guest_mutation=0\n",
                frontend_phase,
                briefing_owns_player ? 1 : 0,
                mission_card_owns_player ? 1 : 0
            );
            std::fflush(stderr);
        }
        return;
    }

    const bool was_active =
        g_gameplay_active.load(std::memory_order_acquire);
    if (!was_active) {
        const bool was_deferred = g_gameplay_arm_deferred_logged.exchange(
            false,
            std::memory_order_acq_rel
        );
        clear_pending_look();
        clear_manual_flight_actions();
        const uint32_t actor = rdram != nullptr
            ? read_u32(rdram, kPlayerOneOwnerSlot)
            : 0u;
        {
            std::scoped_lock lock(g_aim_mutex);
            g_aim_ready = false;
            g_aim_actor = 0u;
        }
        g_resync_aim.store(true, std::memory_order_release);
        g_aim_input_consumed_this_update.store(false, std::memory_order_release);

        const bool carried_primary_fire = g_primary_fire.exchange(
            false,
            std::memory_order_acq_rel
        );
        if (carried_primary_fire) {
            g_primary_fire_require_release.store(
                true,
                std::memory_order_release
            );
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE"
                " stage=modern_primary_fire_handoff_suppressed"
                " source=mission_card_confirm require_release=1\n"
            );
            std::fflush(stderr);
        }
        g_gameplay_active.store(true, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_gameplay_input_armed"
            " source=gameplay_pause_update deferred_until_card_accept=%d"
            " aim_seed_pending=1 transitional_anchor_used=0"
            " actor=0x%08" PRIX32 "\n",
            was_deferred ? 1 : 0,
            actor
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_mark_modern_gameplay_inactive(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!bumble::modern_controls::enabled()) {
        return;
    }

    const uint32_t live_object = guest_u32(context->r19);
    const uint32_t phase = valid_guest_pointer(live_object, 0x88u)
        ? read_u32(rdram, live_object + kFrontendPhaseOffset)
        : 0xFFFFFFFFu;
    if (live_object != kFrontendObject || phase == kFrontendGameplayPhase) {
        return;
    }

    const bool was_active = g_gameplay_active.exchange(
        false,
        std::memory_order_acq_rel
    );
    g_pause_menu_active.store(false, std::memory_order_release);
    clear_pending_look();
    clear_manual_flight_actions();
    bumble::modern_controls::set_movement_input(0.0f, 0.0f);
    g_resync_aim.store(true, std::memory_order_release);
    g_aim_input_consumed_this_update.store(false, std::memory_order_release);
    {
        std::scoped_lock lock(g_aim_mutex);
        g_aim_ready = false;
        g_aim_actor = 0u;
    }
    g_gameplay_arm_deferred_logged.store(false, std::memory_order_release);
    if (was_active) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_gameplay_input_disarmed"
            " source=frontend_menu_update current_phase=0x%08" PRIX32
            " requested_phase=0x%08" PRIX32 "\n",
            phase,
            read_u32(rdram, live_object + 0x84u)
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_apply_modern_translated_confirm(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr) {
        return;
    }

    const bool exact_briefing = exact_briefing_staging_is_live(rdram);
    if (!exact_briefing) {
        g_cutscene_skip_edge.store(false, std::memory_order_release);
    } else if (g_cutscene_skip_edge.exchange(
                   false,
                   std::memory_order_acq_rel)) {
        const uint16_t pressed_before = MEM_HU(
            2,
            guest_address(kTranslatedCurrentPad)
        );
        const uint16_t converted_pressed = static_cast<uint16_t>(
            (pressed_before & ~kButtonB) | kButtonA
        );
        MEM_H(2, guest_address(kTranslatedCurrentPad)) =
            static_cast<int16_t>(converted_pressed);
        g_frontend_confirm_pending.store(false, std::memory_order_release);
        g_frontend_confirm_edge.store(false, std::memory_order_release);
        g_frontend_confirm_latched.store(false, std::memory_order_release);
        g_mission_card_accepted.store(false, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_cutscene_skip_committed"
            " pc=0x800523B8 phase=0x%08" PRIX32
            " source=menu_back_as_pressed_a pressed_before=0x%04" PRIX16
            " pressed_after=0x%04" PRIX16
            " held_a_write=0 player_pad_write=0 one_frame=1\n",
            kFrontendBriefingPhase,
            pressed_before,
            converted_pressed
        );
        std::fflush(stderr);
        return;
    }

    if (!exact_mission_card_is_live(rdram) ||
        g_frontend_confirm_latched.load(std::memory_order_acquire) ||
        !g_frontend_confirm_edge.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    const uint16_t held_before = MEM_HU(
        0,
        guest_address(kTranslatedCurrentPad)
    );
    const uint16_t pressed_before = MEM_HU(
        2,
        guest_address(kTranslatedCurrentPad)
    );
    const uint16_t player_held = MEM_HU(
        0,
        guest_address(kPlayerNormalizedPad)
    );
    const uint16_t player_pressed = MEM_HU(
        2,
        guest_address(kPlayerNormalizedPad)
    );
    if ((held_before & kButtonA) != 0u ||
        (player_held & kButtonA) != 0u ||
        (player_pressed & kButtonA) != 0u) {
        return;
    }

    MEM_H(2, guest_address(kTranslatedCurrentPad)) = static_cast<int16_t>(
        pressed_before | kButtonA
    );
    g_frontend_confirm_pending.store(false, std::memory_order_release);
    g_frontend_confirm_latched.store(true, std::memory_order_release);
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=modern_translated_confirm_committed"
        " pc=0x800523B8 phase=0x%08" PRIX32
        " current_pad=0x%08" PRIX32
        " held_before=0x%04" PRIX16
        " pressed_before=0x%04" PRIX16
        " pressed_after=0x%04" PRIX32
        " player_held=0x%04" PRIX16
        " player_pressed=0x%04" PRIX16
        " held_a_write=0 player_pad_write=0 one_frame=1\n",
        kFrontendGameplayPhase,
        kTranslatedCurrentPad,
        held_before,
        pressed_before,
        static_cast<uint32_t>(pressed_before | kButtonA),
        player_held,
        player_pressed
    );
    std::fflush(stderr);
}

extern "C" void bumble_apply_modern_frontend_confirm(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!bumble::modern_controls::enabled() || rdram == nullptr ||
        context == nullptr) {
        return;
    }

    const uint32_t live_object = guest_u32(context->r19);
    const uint32_t live_state = guest_u32(context->r22);
    const uint32_t descriptor = guest_u32(context->r18);
    const uint32_t phase = valid_guest_pointer(live_object, 0x88u)
        ? read_u32(rdram, live_object + kFrontendPhaseOffset)
        : 0xFFFFFFFFu;

    if (!bumble::modern_controls::enabled()) {
        return;
    }

    if (phase != kFrontendGameplayPhase) {
        g_frontend_confirm_pending.store(false, std::memory_order_release);
        g_frontend_confirm_edge.store(false, std::memory_order_release);
        g_frontend_confirm_latched.store(false, std::memory_order_release);
        g_mission_card_accepted.store(false, std::memory_order_release);
        return;
    }

    const bool exact_mission_card = live_object == kFrontendObject &&
        live_state == kFrontendGameplayPhase &&
        descriptor == kMissionCardDescriptor &&
        read_u32(
            rdram,
            kFrontendDescriptorTable +
                kFrontendGameplayPhase * sizeof(uint32_t)
        ) == kMissionCardDescriptor &&
        read_u32(rdram, kFrontendObject + kFrontendTransitionOffset) == 0u &&
        read_u32(
            rdram,
            descriptor + kFrontendDescriptorFlagsOffset
        ) == kMissionCardDescriptorFlags &&
        read_u32(
            rdram,
            descriptor + kFrontendDescriptorItemOffset
        ) == 0u;
    if (!exact_mission_card) {
        g_frontend_confirm_pending.store(false, std::memory_order_release);
        g_frontend_confirm_edge.store(false, std::memory_order_release);
        return;
    }

    if ((guest_u32(context->r2) & kButtonA) != 0u) {
        g_frontend_confirm_pending.store(false, std::memory_order_release);
        g_frontend_confirm_edge.store(false, std::memory_order_release);
        g_frontend_confirm_latched.store(true, std::memory_order_release);
        g_mission_card_accepted.store(true, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_mission_card_accepted"
            " pc=0x800AB72C phase=0x%08" PRIX32
            " requested_phase=0x%08" PRIX32
            " camera_release=next_gameplay_pause_update guest_mutation=0\n",
            phase,
            read_u32(rdram, live_object + 0x84u)
        );
        std::fflush(stderr);
        return;
    }
    if (g_frontend_confirm_latched.load(std::memory_order_acquire)) {
        g_frontend_confirm_pending.store(false, std::memory_order_release);
        g_frontend_confirm_edge.store(false, std::memory_order_release);
        return;
    }

    g_frontend_confirm_edge.store(true, std::memory_order_release);
    const bool was_pending = g_frontend_confirm_pending.exchange(
        true,
        std::memory_order_acq_rel
    );
    if (!was_pending) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=modern_frontend_confirm_available"
            " pc=0x800AB72C phase=0x%08" PRIX32
            " descriptor=0x%08" PRIX32
            " source=automatic_gameplay_handoff guest_pad_write=0\n",
            phase,
            descriptor
        );
        std::fflush(stderr);
    }
}
