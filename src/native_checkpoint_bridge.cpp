#include "native_checkpoint_bridge.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include "funcs.h"
#include "native_campaign_levels.hpp"
#include "native_campaign_level_observer.hpp"
#include "native_gameplay_options.hpp"
#include "native_graphics_options.hpp"
#include "native_modern_controls.hpp"
#include "native_startup_flow.hpp"
#include "native_weapon_system.hpp"
#include "recomp.h"

namespace {

constexpr uint32_t kRawPadBase = 0x800CC874u;
constexpr uint32_t kRawPadStride = 6u;
constexpr uint32_t kCurrentPadBase = 0x80035F00u;
constexpr uint32_t kCurrentPadStride = 8u;
constexpr uint32_t kNormalizedPadBase = 0x80038340u;
constexpr uint32_t kNormalizedPadStride = 8u;
constexpr uint32_t kPlayerOneOwnerSlot = 0x800E91D4u;
constexpr uint32_t kPlayerTwoOwnerSlot = 0x800E91D8u;
constexpr uint32_t kWorldCameraActiveAddress = 0x800E91E0u;
constexpr uint32_t kGuidedCameraOwnerAddress = 0x800CD730u;
constexpr uint32_t kPlayerVtable = 0x800445E8u;
constexpr uint32_t kPlayerActorUpdate = 0x80059E98u;
constexpr uint32_t kPlayerStateTwo = 2u;
constexpr uint32_t kScaledXConstantAddress = 0x80044214u;
constexpr uint32_t kStickYConstantAddress = 0x80044218u;
constexpr uint32_t kVerticalBiasConstantAddress = 0x8004421Cu;
constexpr uint32_t kMinimumControlAddress = 0x80044220u;
constexpr uint32_t kMaximumControlAddress = 0x80044224u;
constexpr uint32_t kOrientationConstantAddress = 0x80044244u;
constexpr uint32_t kIntegratorScaleAddress = 0x80043C60u;
constexpr uint32_t kScaledXConstantWord = 0x3F666666u;
constexpr uint32_t kStickYConstantWord = 0x3F333333u;
constexpr uint32_t kVerticalBiasConstantWord = 0x43160000u;
constexpr uint32_t kMinimumControlWord = 0xC2200000u;
constexpr uint32_t kMaximumControlWord = 0x42200000u;
constexpr uint32_t kOrientationConstantWord = 0x3C343958u;
constexpr uint64_t kIntegratorScaleWord = UINT64_C(0x4010000000000000);
constexpr uint32_t kCurrentLevelIndexAddress = 0x800E9640u;
constexpr uint32_t kLevelRecordTable = bumble::campaign_levels::kRecordTable;
constexpr uint32_t kLevelRecordSize = bumble::campaign_levels::kRecordSize;
constexpr uint32_t kMissionOneLevelIndex = 1u;
constexpr uint32_t kMainMenuFrontendState = 0x0Cu;
constexpr uint32_t kLevelSelectFrontendState = 0x0Eu;
constexpr uint32_t kMissionOneLevelRecord = 0x800EA004u;
constexpr uint32_t kMissionOneIdWord = 0x30315368u;
constexpr uint32_t kMissionOneLoadOffset = 0x00005DC0u;
constexpr uint32_t kMissionOneScriptBase = 0x8034B000u;
constexpr uint32_t kMissionOneDelayAddress = 0x8034B008u;
constexpr uint32_t kMissionOneCounterAddress = 0x8034B010u;
constexpr uint32_t kMissionOneDelayGateNext = 0x8034B034u;
constexpr uint32_t kMissionOneCounterGateNext = 0x8034B07Cu;
constexpr uint32_t kMissionOneTerminalCallback = 0x8034B0D8u;
constexpr uint32_t kMissionOneTerminalRawOperand = 0x00005DD0u;
constexpr uint32_t kMissionOneTerminalOwnerCount = 13u;
constexpr uint32_t kMissionOneInstallerCount = 11u;
constexpr uint32_t kMissionOneFlyingEnemySelector = 0x00000008u;
constexpr uint32_t kMissionOneWeaponPickupSelector = 0x00020000u;
constexpr uint32_t kMissionOneWeaponPickupVtable = 0x80049238u;
constexpr uint32_t kMissionOneWaveTriggerRecord = 0x8034C59Cu;
constexpr uint32_t kMissionOneWaveTriggerSelector = 0x00400000u;
constexpr uint32_t kMissionOneWaveTriggerVtable = 0x80046A20u;
constexpr uint32_t kMissionOneWaveTriggerCallback = 0x8034B1A0u;
constexpr uint32_t kMissionOneFactoryTriggerRecord = 0x8034B2F4u;
constexpr uint32_t kMissionOneFactoryTriggerVtable = 0x80047900u;
constexpr uint32_t kMissionOneFactoryTriggerCollisionCallback = 0x8007DDF4u;
constexpr uint32_t kCollisionActorLookup = 0x800E2408u;
constexpr uint32_t kCollisionActorLookupCount = 0x200u;
constexpr uint32_t kTeleportVtable = 0x80046A90u;
constexpr uint32_t kTeleportCollisionCategory = 0x00800000u;
constexpr uint32_t kWeaponStateBase = 0x800E92C0u;
constexpr uint32_t kWeaponStateStride = 0x54u;
constexpr uint32_t kPlayerHealthOffset = 0x18u;
constexpr uint32_t kPlayerLivesAddress = 0x800E9650u;
constexpr uint32_t kPlayerMaximumHealthAddress = 0x800E9678u;
constexpr std::array<uint32_t, 4> kPlayerPositionAddresses{
    0x800E93B4u,
    0x800E93C0u,
    0x800E93CCu,
    0x800E93D8u,
};
constexpr uint32_t kWaterRecoveryFrames = 30u;
constexpr uint32_t kWeaponAmmoOffset = 0x24u;
constexpr uint32_t kObjectListBase = 0x800D7460u;
constexpr uint32_t kObjectListStride = 0xC8Cu;
constexpr uint32_t kObjectListCount = 14u;
constexpr uint32_t kParticleParentVtable = 0x800488C0u;
constexpr uint32_t kParticleActorVtable = 0x800489A0u;
constexpr uint32_t kSelectedWeaponOffset = 0x58u;
constexpr uint32_t kWidescreenHudWeaponCount = 11u;
constexpr uint32_t kWidescreenHudValidationAmmo = 99u;

constexpr uint32_t configured_ammo_pickup_grant(
    uint32_t authored_grant,
    bool doubled
) {
    return doubled ? authored_grant * 2u : authored_grant;
}

static_assert(configured_ammo_pickup_grant(10u, false) == 10u);
static_assert(configured_ammo_pickup_grant(10u, true) == 20u);

constexpr uint64_t configured_mission_time_seconds(
    uint32_t minutes,
    uint32_t seconds,
    bool doubled
) {
    const uint64_t authored = static_cast<uint64_t>(minutes) * 60u + seconds;
    return doubled ? authored * 2u : authored;
}

static_assert(configured_mission_time_seconds(5u, 30u, false) == 330u);
static_assert(configured_mission_time_seconds(5u, 30u, true) == 660u);

constexpr float configured_enemy_awareness_distance_component(
    float squared_component,
    bool doubled
) {
    return doubled ? squared_component * 0.25f : squared_component;
}

static_assert(configured_enemy_awareness_distance_component(400.0f, false) == 400.0f);
static_assert(configured_enemy_awareness_distance_component(400.0f, true) == 100.0f);

constexpr bool hostile_combat_vtable(uint32_t vtable) {
    switch (vtable) {
    case 0x80043F08u: // Ant
    case 0x80043F40u: // Ant nest
    case 0x800452D0u: // Bug Tank nest
    case 0x800470B0u: // Mini Bumble
    case 0x80047258u: // Cranefly
    case 0x80047428u: // Dragonfly
    case 0x80047858u: // Wasp portal
    case 0x800478C8u: // Wasp
    case 0x80047900u: // Wasp nest
    case 0x80047BD8u: // Web Spider
    case 0x80047DA8u: // Hoverfly
    case 0x80047E50u: // Hoverfly nest
    case 0x80047E88u: // Block Bug
    case 0x800480A8u: // Killerpiller
    case 0x80049608u: // Golden Flea
    case 0x80049640u: // Grab Bug
    case 0x80049678u: // Grab Bug nest
    case 0x80049B18u: // Weevil
    case 0x8004A080u: // Spotter
    case 0x8004A1F0u: // Water Beetle
    case 0x8004A308u: // Louse gun
    case 0x8004B170u: // Commander
    case 0x8004B390u: // Scorpion
    case 0x8004B678u: // Chain Moth
    case 0x8004B9F0u: // Zeppelin
    case 0x8004C360u: // Mantis
    case 0x8004C550u: // Wood Wasp
        return true;
    default:
        return false;
    }
}

constexpr bool enemy_spawner_vtable(uint32_t vtable) {
    switch (vtable) {
    case 0x80043F40u: // Ant nest
    case 0x800452D0u: // Bug Tank nest
    case 0x80047900u: // Wasp nest
    case 0x80047E50u: // Hoverfly nest
    case 0x80049678u: // Grab Bug nest
        return true;
    default:
        return false;
    }
}

constexpr uint32_t kDestroyedSpawnerState = 9u;

static_assert(hostile_combat_vtable(0x800478C8u));
static_assert(hostile_combat_vtable(0x8004C360u));
static_assert(!hostile_combat_vtable(kPlayerVtable));
static_assert(!hostile_combat_vtable(0x80049908u)); // Transporter
static_assert(!hostile_combat_vtable(0x80047818u)); // Wasp wave controller
static_assert(!hostile_combat_vtable(0x80045260u)); // Acid-drop hazard

constexpr uint64_t kWidescreenHudCycleStartFrame = 60u;
constexpr uint32_t kGameplayFrontendPhase = 0x19u;
constexpr uint32_t kBriefingFrontendPhase = 0x18u;
constexpr uint32_t kPauseMenuVisibleAddress = 0x80100124u;
constexpr uint8_t kStarterWeapon = 0u;
constexpr uint8_t kPlasmaWeapon = 1u;
constexpr uint8_t kGrenadeWeapon = 2u;
constexpr uint8_t kSmallHealthPickup = 12u;
constexpr uint8_t kFullHealthPickup = 13u;
constexpr uint16_t kRawCDownButton = 0x0004u;
constexpr std::array<uint32_t, kMissionOneInstallerCount>
    kMissionOneInstallerRecords{
        0x8034B2F4u,
        0x8034B4FCu,
        0x8034B6F4u,
        0x8034B72Cu,
        0x8034B8ECu,
        0x8034B95Cu,
        0x8034BAACu,
        0x8034BBFCu,
        0x8034BC6Cu,
        0x8034C05Cu,
        0x8034C0CCu,
    };
constexpr uint32_t kMissionOneCallbackRelocationAddress = 0x800D7360u;
constexpr uint32_t kFrontendLiveObject = 0x800FFF80u;
constexpr uint32_t kFrontendCurrentLevelOffset = 0x10u;
constexpr uint32_t kMissionCompleteFrontendState = 0x1Au;
constexpr uint32_t kMissionTwoLevelIndex = 2u;
constexpr uint32_t kMissionTwoLevelRecord = 0x800EA028u;
constexpr uint32_t kMissionTwoIdWord = 0x30325261u;
constexpr uint32_t kMissionTwoLoadOffset = 0x0000A960u;
constexpr uint32_t kFrontendDescriptorAddress = 0x800FE5A8u;
constexpr uint32_t kDefaultFrontendDescriptor = 0x800FD388u;
constexpr uint32_t kLevelSelectFrontendDescriptor = 0x800FD320u;
constexpr uint32_t kLevelSelectEnabledAddress = 0x800F598Du;
constexpr uint32_t kLevelSelectCheatProgressAddress = 0x80107A58u;
constexpr uint32_t kLevelSelectCheatFlagAddress = 0x80107A64u;
constexpr uint32_t kLevelSelectCheatSequenceAddress = 0x800FE79Cu;
constexpr uint32_t kMissionSelectorIndexAddress = 0x80107A68u;
constexpr uint32_t kMissionSelectorPreviousIndexAddress = 0x80107A6Cu;
constexpr uint32_t kMissionSelectorNumberAddress = 0x80107A70u;
constexpr uint16_t kDpadMask = 0x0F00u;
constexpr uint16_t kAButton = 0x8000u;
constexpr uint16_t kBButton = 0x4000u;
constexpr uint16_t kZButton = 0x2000u;
constexpr uint16_t kStartButton = 0x1000u;
constexpr std::array<uint32_t, 15> kMissionCheckpointPoseOffsets{
    0x10u, 0x14u, 0x18u,
    0x20u, 0x24u, 0x28u,
    0x30u, 0x34u, 0x38u,
    0x40u, 0x44u, 0x48u,
    0x50u, 0x54u, 0x58u,
};
constexpr std::array<uint16_t, 12> kLevelSelectCheatSequence{
    0x2100u,
    0x2400u,
    0x2400u,
    0x2100u,
    0x0100u,
    0x0800u,
    0x0400u,
    0x0200u,
    0x0200u,
    0x0800u,
    0x0100u,
    0x0100u,
};

std::atomic_uint32_t g_raw_transition_count{0};
std::array<std::atomic_uint32_t, 2> g_last_raw_signatures{};
std::atomic_uint32_t g_normalized_edge_count{0};
std::array<std::atomic_uint64_t, 2> g_last_normalized_signatures{};
std::atomic_uint32_t g_frontend_confirm_count{0};
std::atomic_uint32_t g_frontend_phase_transition_count{0};
std::atomic_uint32_t g_last_frontend_phase{0xFFFFFFFFu};
std::atomic_uint32_t g_last_accepted_frontend_phase{0xFFFFFFFFu};
std::atomic_uint32_t g_last_frontend_descriptor{0};
std::atomic_uint32_t g_last_frontend_descriptor_item{0xFFFFFFFFu};
std::atomic_uint32_t g_level_select_cheat_progress{0};
std::atomic_bool g_level_select_cheat_complete{false};
std::atomic_bool g_mission2_selector_initialized{false};
std::atomic_bool g_mission2_selected{false};
std::atomic_bool g_mission2_selection_committed{false};
std::atomic_uint32_t g_campaign_selector_index{0};
std::atomic_uint32_t g_campaign_selection_committed_index{0};
std::atomic_bool g_campaign_grid_active{false};
std::atomic_uint32_t g_campaign_grid_selected_slot{0u};
std::atomic_uint32_t g_campaign_grid_progress_level{1u};
std::atomic_uint32_t g_campaign_grid_live_object{0u};
std::atomic_bool g_campaign_grid_input_latched{false};
std::atomic_bool g_campaign_grid_back_hovered{false};
std::atomic_uint32_t g_campaign_player_level_observed{0};
std::atomic_bool g_mission1_player_observed{false};
std::atomic_bool g_mission2_player_observed{false};
std::atomic_bool g_player_checkpoint_candidate_logged{false};
std::atomic_uint32_t g_campaign_level_observed_mask{0};
std::atomic_bool g_mission1_completion_replay_enabled{false};
std::atomic_bool g_mission1_withhold_one_owner{false};
std::atomic_uint16_t g_mission1_buttons_requested{0u};
std::atomic_uint32_t g_mission1_installer_count{0};
std::atomic_uint32_t g_mission1_installer_candidate_count{0};
std::atomic_uint32_t g_mission1_registered_owner_count{0};
std::atomic_uint32_t g_mission1_terminal_count{0};
std::atomic_uint32_t g_mission1_damage_event_count{0};
std::atomic_uint32_t g_mission1_child_probe_candidate_count{0};
std::atomic_bool g_mission1_special_installer_candidate_logged{false};
std::atomic_uint32_t g_mission1_counter_value{kMissionOneTerminalOwnerCount};
std::atomic_uint32_t g_mission1_delay_value{20u};
std::atomic_bool g_mission1_counter_zero_gate_observed{false};
std::atomic_bool g_mission1_delay_zero_gate_observed{false};
std::atomic_bool g_mission1_success_callback_observed{false};
std::atomic_bool g_mission1_success_frontend_observed{false};
std::atomic_bool g_mission1_level_increment_observed{false};
std::atomic_bool g_mission1_profile_save_observed{false};
std::atomic_bool g_mission1_profile_reopen_observed{false};
std::atomic_uint64_t g_mission1_driver_frame{0};
std::atomic_bool g_widescreen_hud_validation_enabled{false};
std::atomic_bool g_widescreen_hud_inventory_granted{false};
std::atomic_uint64_t g_widescreen_hud_player_frame{0u};
std::atomic_uint32_t g_widescreen_hud_last_selected{UINT32_MAX};
std::atomic_uint32_t g_widescreen_hud_capture_request{0u};
std::atomic_uint32_t g_widescreen_hud_capture_complete_mask{0u};
std::atomic_bool g_two_player_state2_ready{false};
std::atomic_uint32_t g_two_player_ready_candidate_count{0};
std::atomic_bool g_two_player_ready_logged{false};
std::atomic_bool g_two_player_checkpoint_observed{false};
std::atomic_bool g_two_player_positive_control_observed{false};
std::atomic_bool g_half_player_health_active{false};
std::atomic_uint32_t g_pending_mission_restart{0u};
uint8_t* g_player_health_rdram = nullptr;
float g_authored_player_maximum_health = 0.0f;
float g_applied_player_maximum_health = 0.0f;
uint32_t g_health_gameplay_actor = 0u;
uint32_t g_health_gameplay_level = UINT32_MAX;
std::unordered_map<uint32_t, bool> g_enemy_damage_suppress_next;
bool g_gameplay_modifiers_live = false;
bool g_enemy_health_setting = false;

struct WaterMissionStart {
    uint8_t* rdram = nullptr;
    uint32_t actor = 0u;
    uint32_t level = UINT32_MAX;
    std::array<uint32_t, 3> position_words{};
    bool valid = false;
};

WaterMissionStart g_water_mission_start{};

enum class MissionCheckpointKind : uint8_t {
    Portal,
    Objective,
};

enum class MissionCheckpointRestoreTrigger : uint8_t {
    PauseMenu,
    MissionFailure,
};

struct MissionCheckpoint {
    uint8_t* rdram = nullptr;
    uint32_t actor = 0u;
    uint32_t level = UINT32_MAX;
    MissionCheckpointKind kind = MissionCheckpointKind::Portal;
    uint32_t source_teleport = 0u;
    uint32_t destination_teleport = 0u;
    uint32_t pair_id = 0u;
    std::array<uint32_t, kMissionCheckpointPoseOffsets.size()> pose_words{};
    uint32_t exit_speed_word = 0u;
    uint32_t collision_category = 0u;
    uint32_t health_word = 0u;
    std::array<uint32_t, kWidescreenHudWeaponCount> ammo_words{};
    uint32_t selected_weapon = 0u;
    uint64_t generation = 0u;
    bool valid = false;
};

struct PendingPortalMissionCheckpoint {
    uint8_t* rdram = nullptr;
    uint32_t actor = 0u;
    uint32_t level = UINT32_MAX;
    uint32_t source_teleport = 0u;
    uint32_t destination_teleport = 0u;
    uint32_t pair_id = 0u;
    bool valid = false;
};

std::mutex g_mission_checkpoint_mutex;
MissionCheckpoint g_mission_checkpoint{};
PendingPortalMissionCheckpoint g_pending_portal_checkpoint{};
std::atomic_bool g_mission_checkpoint_available{false};
std::atomic_uint64_t g_mission_checkpoint_generation{0u};

struct PlayerCheatState {
    uint32_t actor = 0u;
    uint32_t owned_weapon_mask = 0u;
    bool all_weapons_logged = false;
    bool unlimited_ammo_logged = false;
    bool unlimited_health_logged = false;
    uint32_t water_recovery_frames = 0u;
    float water_recovery_health = 0.0f;
};

std::array<PlayerCheatState, 2> g_player_cheat_states{};

enum class TwoPlayerStage : uint32_t {
    Unarmed,
    WaitingCallback,
    WaitingDispatch,
    WaitingControlSample,
    WaitingScaledControls,
    WaitingOrientationSample,
    WaitingOrientationPost,
    WaitingIntegratorPre,
    WaitingIntegratorPost,
    Complete,
};

struct NormalizedPad {
    uint16_t held = 0;
    uint16_t pressed = 0;
    int16_t stick_x = 0;
    int16_t stick_y = 0;
};

struct PlayerOwners {
    uint32_t player_one = 0;
    uint32_t player_two = 0;
    uint32_t player_one_vtable = 0;
    uint32_t player_two_vtable = 0;
    uint8_t player_one_index = 0xFFu;
    uint8_t player_two_index = 0xFFu;
    bool distinct_nonzero = false;
    bool metadata_matches = false;
};

struct TwoPlayerChain {
    bool armed = false;
    bool positive_control = false;
    int16_t expected_stick_y = 0;
    TwoPlayerStage stage = TwoPlayerStage::Unarmed;
    uint32_t actor = 0;
    int16_t stick_x = 0;
    uint32_t vertical_bias_word = 0;
    uint32_t expected_scaled_x_word = 0;
    uint32_t expected_scaled_y_word = 0;
    uint32_t orientation_before_word = 0;
    uint32_t expected_orientation_word = 0;
    std::array<uint32_t, 3> basis_words{};
    std::array<uint32_t, 3> position_before_words{};
    std::array<uint32_t, 3> expected_position_words{};
    uint32_t speed_word = 0;
    uint64_t integrator_scale_word = 0;
};

std::mutex g_two_player_mutex;
TwoPlayerChain g_two_player_chain{};

struct MissionOneTarget {
    uint32_t actor = 0;
    uint32_t source_pc = 0;
    uint32_t generation = 0;
    bool terminal_observed = false;
    uint64_t retry_after_frame = 0;
};

struct MissionOnePickup {
    uint32_t actor = 0;
    uint32_t record = 0;
    uint8_t weapon = 0;
    bool consumed = false;
    uint64_t retry_after_frame = 0;
};

struct MissionOneHealthPickup {
    uint32_t actor = 0;
    uint32_t record = 0;
    uint8_t subtype = 0;
    bool consumed = false;
    uint64_t retry_after_frame = 0;
};

struct MissionOneDetour {
    bool active = false;
    uint32_t actor = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct MissionOneTrigger {
    uint32_t actor = 0;
    uint32_t record = 0;
    bool activation_logged = false;
};

struct MissionOneWavePrerequisite {
    uint32_t actor = 0;
    uint64_t retry_after_frame = 0;
};

std::mutex g_mission1_mutex;
std::array<uint32_t, kMissionOneInstallerCount> g_mission1_installer_records{};
std::array<MissionOneTarget, kMissionOneTerminalOwnerCount>
    g_mission1_targets{};
std::array<MissionOnePickup, 3> g_mission1_pickups{};
std::array<MissionOneHealthPickup, 16> g_mission1_health_pickups{};
MissionOneDetour g_mission1_detour{};
MissionOneTrigger g_mission1_wave_trigger{};
MissionOneTrigger g_mission1_factory_trigger{};
std::array<MissionOneWavePrerequisite, 32>
    g_mission1_wave_prerequisites{};
bool g_mission1_wave_prerequisites_cleared_logged = false;
uint32_t g_mission1_withheld_actor = 0;
uint32_t g_mission1_last_driver_target = 0;
uint64_t g_mission1_last_terminal_frame = 0;
uint64_t g_mission1_attack_start_frame = 0;
int32_t g_mission1_attack_last_health = INT32_MAX;
uint64_t g_mission1_navigation_progress_frame = 0;
float g_mission1_navigation_best_distance = INFINITY;
float g_mission1_navigation_anchor_x = NAN;
float g_mission1_navigation_anchor_y = NAN;
float g_mission1_navigation_anchor_z = NAN;
float g_mission1_last_player_health = NAN;

struct MissionOneTerminalInvocation {
    bool active = false;
    uint32_t actor = 0;
    uint32_t caller_pc = 0;
    uint32_t counter_before = 0;
};

thread_local std::array<MissionOneTerminalInvocation, 32>
    g_mission1_terminal_invocation_stack{};
thread_local size_t g_mission1_terminal_invocation_depth = 0u;
thread_local size_t g_mission1_terminal_invocation_overflow = 0u;
thread_local uint32_t g_mission1_pending_terminal_caller_pc = 0u;
thread_local bool g_mission1_profile_save_active = false;
thread_local uint32_t g_mission1_profile_save_slot = UINT32_MAX;
thread_local bool g_mission1_profile_reopen_active = false;
thread_local uint32_t g_mission1_profile_reopen_slot = UINT32_MAX;
std::atomic_uint32_t g_performance_active_actors{0u};
std::atomic_uint32_t g_performance_rendered_actors{0u};
std::atomic_uint32_t g_performance_rendered_actors_current{0u};
std::atomic_uint32_t g_performance_particle_actors{0u};
std::array<
    std::atomic_uint32_t,
    bumble::weapon_system::kActorFamilyCount
> g_performance_weapon_actors{};
std::atomic_uint32_t g_performance_player_x{0u};
std::atomic_uint32_t g_performance_player_y{0u};
std::atomic_uint32_t g_performance_player_z{0u};
std::atomic_uint32_t g_performance_player_yaw{0u};

bool performance_reporting_enabled() {
    static const bool enabled = [] {
        const char* detail =
            std::getenv("BUMBLE_RT64_FRAME_PACING_DETAIL");
        const char* profile = std::getenv("RT64_PROFILE_OUTPUT");
        const auto requested = [](const char* value) {
            return value != nullptr && value[0] != '\0' && value[0] != '0';
        };
        return requested(detail) || requested(profile);
    }();
    return enabled;
}

gpr guest_address(uint32_t address) {
    return static_cast<gpr>(static_cast<int32_t>(address));
}

uint32_t guest_u32(gpr value) {
    return static_cast<uint32_t>(value);
}

bool valid_guest_pointer(uint32_t address, uint32_t size) {
    return address >= 0x80000000u && address <= 0x84000000u - size;
}

uint32_t pad_signature(uint16_t buttons, int8_t stick_x, int8_t stick_y) {
    return (static_cast<uint32_t>(buttons) << 16) |
        (static_cast<uint32_t>(static_cast<uint8_t>(stick_x)) << 8) |
        static_cast<uint32_t>(static_cast<uint8_t>(stick_y));
}

uint64_t current_pad_transition_signature(const NormalizedPad& pad) {
    return (static_cast<uint64_t>(pad.held) << 32) |
        (static_cast<uint64_t>(static_cast<uint16_t>(pad.stick_x)) << 16) |
        static_cast<uint64_t>(static_cast<uint16_t>(pad.stick_y));
}

uint32_t read_u32(uint8_t* rdram, uint32_t address) {
    return static_cast<uint32_t>(MEM_W(0, guest_address(address)));
}

bool settled_gameplay_is_live(uint8_t* rdram) {
    // +0 is current state; +0x84 is the requested next state and may be stale.
    // Do not save or restore during a pending transition.
    return rdram != nullptr &&
        read_u32(rdram, kFrontendLiveObject + 0x00u) == kGameplayFrontendPhase &&
        read_u32(rdram, kFrontendLiveObject + 0x40u) == 0u;
}

void write_u32(uint8_t* rdram, uint32_t address, uint32_t value) {
    (void)rdram;
    MEM_W(0, guest_address(address)) = static_cast<int32_t>(value);
}

uint32_t mission_restart_value(
    bumble::native_checkpoint::MissionRestart restart
) {
    return restart == bumble::native_checkpoint::MissionRestart::Beginning
        ? 1u
        : 2u;
}

struct LevelRecordIdentity {
    uint32_t address = 0;
    uint32_t id_word = 0;
    uint32_t load_offset = 0;
    uint32_t record_word_18 = 0;
    uint32_t record_word_1c = 0;
    uint32_t record_word_20 = 0;
    bool valid = false;
};

LevelRecordIdentity read_level_record_identity(uint8_t* rdram, uint32_t level_index) {
    const uint64_t address = static_cast<uint64_t>(kLevelRecordTable) +
        static_cast<uint64_t>(level_index) * kLevelRecordSize;
    if (address > UINT32_MAX ||
        !valid_guest_pointer(static_cast<uint32_t>(address), kLevelRecordSize)) {
        return {};
    }

    const uint32_t record = static_cast<uint32_t>(address);
    return {
        record,
        read_u32(rdram, record),
        read_u32(rdram, record + 0x14u),
        read_u32(rdram, record + 0x18u),
        read_u32(rdram, record + 0x1Cu),
        read_u32(rdram, record + 0x20u),
        true,
    };
}

bool level_record_matches(
    const LevelRecordIdentity& identity,
    uint32_t expected_address,
    uint32_t expected_id_word,
    uint32_t expected_load_offset
) {
    return identity.valid && identity.address == expected_address &&
        identity.id_word == expected_id_word &&
        identity.load_offset == expected_load_offset;
}

bool campaign_record_matches(
    uint8_t* rdram,
    const LevelRecordIdentity& identity,
    const bumble::campaign_levels::Record& expected
) {
    if (!identity.valid ||
        identity.address != bumble::campaign_levels::record_address(
            expected.level_index
        ) ||
        identity.load_offset != expected.load_offset ||
        identity.record_word_18 != expected.record_word_18 ||
        identity.record_word_1c != expected.record_word_1c ||
        identity.record_word_20 != expected.record_word_20) {
        return false;
    }

    for (uint32_t index = 0u;
         index < bumble::campaign_levels::kIdentifierSize;
         ++index) {
        const uint8_t expected_byte = index < expected.identifier.size()
            ? static_cast<uint8_t>(expected.identifier[index])
            : 0u;
        if (MEM_BU(index, guest_address(identity.address)) != expected_byte) {
            return false;
        }
    }
    return true;
}

const char* campaign_observation_site_name(
    bumble::native_campaign_level::ObservationSite site
) {
    using Site = bumble::native_campaign_level::ObservationSite;
    switch (site) {
    case Site::PlayerUpdate:
        return "player_update";
    case Site::GraphicsTaskSubmit:
        return "graphics_task_submit";
    }
    return "unknown";
}

uint32_t campaign_observation_pc(
    bumble::native_campaign_level::ObservationSite site
) {
    using Site = bumble::native_campaign_level::ObservationSite;
    switch (site) {
    case Site::PlayerUpdate:
        return kPlayerActorUpdate;
    case Site::GraphicsTaskSubmit:
        return 0x80054E84u;
    }
    return 0u;
}

NormalizedPad read_normalized_pad(uint8_t* rdram, uint32_t player_index) {
    const gpr pad = guest_address(kNormalizedPadBase + player_index * kNormalizedPadStride);
    return {
        MEM_HU(0, pad),
        MEM_HU(2, pad),
        MEM_H(4, pad),
        MEM_H(6, pad),
    };
}

bool normalized_pad_is_neutral(const NormalizedPad& pad) {
    return pad.held == 0 && pad.pressed == 0 &&
        pad.stick_x == 0 && pad.stick_y == 0;
}

PlayerOwners read_player_owners(uint8_t* rdram) {
    PlayerOwners owners{};
    owners.player_one = read_u32(rdram, kPlayerOneOwnerSlot);
    owners.player_two = read_u32(rdram, kPlayerTwoOwnerSlot);
    owners.distinct_nonzero = owners.player_one != 0 && owners.player_two != 0 &&
        owners.player_one != owners.player_two;
    if (!owners.distinct_nonzero ||
        !valid_guest_pointer(owners.player_one, 0xF5u) ||
        !valid_guest_pointer(owners.player_two, 0xF5u)) {
        return owners;
    }

    owners.player_one_vtable = read_u32(rdram, owners.player_one + 0x88u);
    owners.player_two_vtable = read_u32(rdram, owners.player_two + 0x88u);
    owners.player_one_index = MEM_BU(0xF4, guest_address(owners.player_one));
    owners.player_two_index = MEM_BU(0xF4, guest_address(owners.player_two));
    owners.metadata_matches = owners.player_one_vtable == kPlayerVtable &&
        owners.player_two_vtable == kPlayerVtable &&
        owners.player_one_index == 0u && owners.player_two_index == 1u;
    return owners;
}

float float_from_word(uint32_t word) {
    return std::bit_cast<float>(word);
}

bool reviewed_hostile_actor(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t& actor
) {
    if (rdram == nullptr || context == nullptr) {
        return false;
    }
    actor = guest_u32(context->r4);
    return valid_guest_pointer(actor, 0x8Cu) &&
        hostile_combat_vtable(read_u32(rdram, actor + 0x88u));
}

bool enemy_awareness_actor(uint8_t* rdram, recomp_context* context) {
    uint32_t actor = 0u;
    return bumble::graphics_options::double_enemy_awareness_enabled() &&
        reviewed_hostile_actor(rdram, context, actor);
}

bool player_position_target(gpr target) {
    const uint32_t address = guest_u32(target);
    return std::find(
        kPlayerPositionAddresses.begin(),
        kPlayerPositionAddresses.end(),
        address
    ) != kPlayerPositionAddresses.end();
}

bool exact_player_actor(
    uint8_t* rdram,
    uint32_t actor,
    uint8_t& player_index
) {
    if (rdram == nullptr || actor == 0u ||
        !valid_guest_pointer(actor, 0xF5u) ||
        read_u32(rdram, actor + 0x88u) != kPlayerVtable) {
        return false;
    }
    player_index = MEM_BU(0xF4, guest_address(actor));
    if (player_index > 1u) {
        return false;
    }
    const uint32_t owner_slot = player_index == 0u
        ? kPlayerOneOwnerSlot
        : kPlayerTwoOwnerSlot;
    return read_u32(rdram, owner_slot) == actor;
}

bool exact_teleport_actor(uint8_t* rdram, uint32_t actor) {
    return rdram != nullptr && valid_guest_pointer(actor, 0x98u) &&
        read_u32(rdram, actor + 0x88u) == kTeleportVtable &&
        (read_u32(rdram, actor + 0x64u) & kTeleportCollisionCategory) != 0u;
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

    const uint16_t collision_id = static_cast<uint16_t>(
        MEM_HU(0, guest_address(collision_record + 0x0Cu))
    );
    const uint32_t entry = kCollisionActorLookup +
        (static_cast<uint32_t>(collision_id) &
            (kCollisionActorLookupCount - 1u)) * 8u;
    if (static_cast<uint16_t>(MEM_HU(0, guest_address(entry))) !=
            collision_id) {
        return 0u;
    }
    const uint32_t actor = read_u32(rdram, entry + 0x04u);
    return exact_teleport_actor(rdram, actor) ? actor : 0u;
}

const char* mission_checkpoint_kind_name(MissionCheckpointKind kind) {
    switch (kind) {
    case MissionCheckpointKind::Portal:
        return "portal";
    case MissionCheckpointKind::Objective:
        return "objective";
    }
    return "unknown";
}

const char* mission_checkpoint_restore_trigger_name(
    MissionCheckpointRestoreTrigger trigger
) {
    switch (trigger) {
    case MissionCheckpointRestoreTrigger::PauseMenu:
        return "pause_menu";
    case MissionCheckpointRestoreTrigger::MissionFailure:
        return "mission_failure";
    }
    return "unknown";
}

void invalidate_mission_checkpoint_locked() {
    g_mission_checkpoint = MissionCheckpoint{};
    g_pending_portal_checkpoint = PendingPortalMissionCheckpoint{};
    g_mission_checkpoint_available.store(false, std::memory_order_release);
}

uint32_t maximum_health_word(uint8_t* rdram) {
    uint32_t word = read_u32(rdram, kPlayerMaximumHealthAddress);
    const float value = float_from_word(word);
    if (!std::isfinite(value) || value <= 0.0f || value > 100000.0f) {
        word = UINT32_C(0x42C80000); // 100.0f
    }
    return word;
}

constexpr float honeycomb_cell_damage(float maximum, uint32_t cell_count) {
    return maximum / static_cast<float>(cell_count);
}

static_assert(honeycomb_cell_damage(80.0f, 10u) == 8.0f);
static_assert(honeycomb_cell_damage(40.0f, 5u) == 8.0f);

bool restore_water_recovery_position(uint8_t* rdram, uint32_t actor) {
    if (bumble::modern_controls::restore_recent_collision_safe_position(
            rdram,
            actor)) {
        return true;
    }
    if (!g_water_mission_start.valid || g_water_mission_start.rdram != rdram ||
        g_water_mission_start.actor != actor ||
        g_water_mission_start.level !=
            read_u32(rdram, kCurrentLevelIndexAddress)) {
        return false;
    }
    write_u32(rdram, actor + 0x40u, g_water_mission_start.position_words[0]);
    write_u32(rdram, actor + 0x44u, g_water_mission_start.position_words[1]);
    write_u32(rdram, actor + 0x48u, g_water_mission_start.position_words[2]);
    return true;
}

void resume_after_water_recovery(uint8_t* rdram, uint32_t actor) {
    write_u32(rdram, actor + 0x50u, 0u);
    write_u32(rdram, actor + 0x58u, 0u);
    write_u32(rdram, actor + 0x5Cu, 0u);
    write_u32(rdram, actor + 0x90u, 0u);
    write_u32(rdram, actor + 0x8Cu, kPlayerStateTwo);
}

uint32_t apply_configured_player_maximum(
    uint8_t* rdram,
    float loaded_maximum
) {
    if (g_player_health_rdram != rdram) {
        g_player_health_rdram = rdram;
        g_authored_player_maximum_health = 0.0f;
        g_applied_player_maximum_health = 0.0f;
        g_health_gameplay_actor = 0u;
        g_health_gameplay_level = UINT32_MAX;
    }
    const float comparison_scale = std::max(1.0f, loaded_maximum);
    if (!(g_authored_player_maximum_health > 0.0f) ||
        std::abs(loaded_maximum - g_applied_player_maximum_health) >
            comparison_scale * 0.0001f) {
        g_authored_player_maximum_health = loaded_maximum;
    }

    const bool half =
        bumble::graphics_options::half_player_health_enabled();
    const float applied_maximum = g_authored_player_maximum_health *
        (half ? 0.5f : 1.0f);
    const uint32_t applied_word = std::bit_cast<uint32_t>(applied_maximum);
    write_u32(rdram, kPlayerMaximumHealthAddress, applied_word);
    g_applied_player_maximum_health = applied_maximum;
    g_half_player_health_active.store(half, std::memory_order_release);
    return applied_word;
}

void apply_player_cheats(uint8_t* rdram, recomp_context* context) {
    if (rdram == nullptr || context == nullptr) {
        return;
    }
    const bool all_weapons = bumble::graphics_options::all_weapons_enabled();
    const bool unlimited_ammo =
        bumble::graphics_options::unlimited_ammo_enabled();
    const bool unlimited_health =
        bumble::graphics_options::unlimited_health_enabled();
    const bool half_health =
        g_half_player_health_active.load(std::memory_order_acquire);
    const bool health_setting_pending =
        bumble::graphics_options::half_player_health_enabled() != half_health;
    if (!all_weapons && !unlimited_ammo && !unlimited_health && !half_health &&
        !health_setting_pending) {
        return;
    }

    const uint32_t actor = guest_u32(context->r4);
    uint8_t player_index = 0xFFu;
    if (!exact_player_actor(rdram, actor, player_index)) {
        return;
    }
    PlayerCheatState& state = g_player_cheat_states[player_index];
    if (state.actor != actor) {
        state = PlayerCheatState{};
        state.actor = actor;
    }

    const uint32_t weapon_state =
        kWeaponStateBase + player_index * kWeaponStateStride;
    if (player_index == 0u &&
        g_last_frontend_phase.load(std::memory_order_acquire) ==
            kGameplayFrontendPhase) {
        const uint32_t level = read_u32(rdram, kCurrentLevelIndexAddress);
        if (g_health_gameplay_actor != actor ||
            g_health_gameplay_level != level) {
            g_health_gameplay_actor = actor;
            g_health_gameplay_level = level;
            if (health_setting_pending) {
                const uint32_t maximum_word = apply_configured_player_maximum(
                    rdram,
                    float_from_word(maximum_health_word(rdram))
                );
                const float current_health = float_from_word(read_u32(
                    rdram,
                    weapon_state + kPlayerHealthOffset
                ));
                const float maximum_health = float_from_word(maximum_word);
                if (std::isfinite(current_health) &&
                    current_health > maximum_health) {
                    write_u32(
                        rdram,
                        weapon_state + kPlayerHealthOffset,
                        maximum_word
                    );
                }
            }
        }
    }
    for (uint32_t weapon = 0u; weapon < kWidescreenHudWeaponCount; ++weapon) {
        const uint32_t ammo_address =
            weapon_state + kWeaponAmmoOffset + weapon * sizeof(uint32_t);
        if (read_u32(rdram, ammo_address) != 0u) {
            state.owned_weapon_mask |= 1u << weapon;
        }
    }

    const uint32_t full_weapon_mask =
        (1u << kWidescreenHudWeaponCount) - 1u;
    if (all_weapons && state.owned_weapon_mask != full_weapon_mask) {
        for (uint32_t weapon = 0u; weapon < kWidescreenHudWeaponCount; ++weapon) {
            if ((state.owned_weapon_mask & (1u << weapon)) == 0u) {
                write_u32(
                    rdram,
                    weapon_state + kWeaponAmmoOffset +
                        weapon * sizeof(uint32_t),
                    kWidescreenHudValidationAmmo
                );
            }
        }
        state.owned_weapon_mask = full_weapon_mask;
    }
    if (unlimited_ammo) {
        for (uint32_t weapon = 0u; weapon < kWidescreenHudWeaponCount; ++weapon) {
            if ((state.owned_weapon_mask & (1u << weapon)) == 0u) {
                continue;
            }
            const uint32_t ammo_address =
                weapon_state + kWeaponAmmoOffset + weapon * sizeof(uint32_t);
            if (read_u32(rdram, ammo_address) < kWidescreenHudValidationAmmo) {
                write_u32(
                    rdram,
                    ammo_address,
                    kWidescreenHudValidationAmmo
                );
            }
        }
    }
    if (unlimited_health) {
        write_u32(
            rdram,
            weapon_state + kPlayerHealthOffset,
            maximum_health_word(rdram)
        );
    } else if (half_health) {
        const uint32_t maximum_word = maximum_health_word(rdram);
        const float current_health = float_from_word(read_u32(
            rdram,
            weapon_state + kPlayerHealthOffset
        ));
        const float maximum_health = float_from_word(maximum_word);
        if (std::isfinite(current_health) && current_health > maximum_health) {
            write_u32(
                rdram,
                weapon_state + kPlayerHealthOffset,
                maximum_word
            );
        }
    }

    const bool should_log =
        (all_weapons && !state.all_weapons_logged) ||
        (unlimited_ammo && !state.unlimited_ammo_logged) ||
        (unlimited_health && !state.unlimited_health_logged);
    if (should_log) {
        std::fprintf(
            stderr,
            "BUMBLE_CHEATS stage=player_applied actor=0x%08" PRIX32
            " player_index=%u all_weapons=%d unlimited_ammo=%d"
            " unlimited_health=%d owned_mask=0x%08" PRIX32 "\n",
            actor,
            static_cast<unsigned>(player_index),
            all_weapons ? 1 : 0,
            unlimited_ammo ? 1 : 0,
            unlimited_health ? 1 : 0,
            state.owned_weapon_mask
        );
        std::fflush(stderr);
    }
    state.all_weapons_logged |= all_weapons;
    state.unlimited_ammo_logged |= unlimited_ammo;
    state.unlimited_health_logged |= unlimited_health;
}

size_t campaign_slot_for_progress(uint32_t progress_level) {
    size_t slot = 0u;
    for (size_t index = 0u;
         index < bumble::campaign_levels::kMissionSelectLevelIndices.size();
         ++index) {
        if (bumble::campaign_levels::kMissionSelectLevelIndices[index] >
            progress_level) {
            break;
        }
        slot = index;
    }
    return slot;
}

bool campaign_grid_identity(uint8_t* rdram, uint32_t live_object) {
    return bumble::graphics_options::interactive_menu_enabled() &&
        valid_guest_pointer(live_object, 0x88u) &&
        read_u32(rdram, kFrontendDescriptorAddress) ==
            kLevelSelectFrontendDescriptor &&
        MEM_BU(0, guest_address(kLevelSelectEnabledAddress)) == 1u;
}

bool campaign_level_unlocked(uint32_t level) {
    return bumble::graphics_options::unlock_all_levels_enabled() ||
        level <= g_campaign_grid_progress_level.load(std::memory_order_acquire);
}

bool mission1_script_loaded(uint8_t* rdram) {
    return rdram != nullptr &&
        read_u32(rdram, kMissionOneScriptBase) == 0x80096918u;
}

bool mission1_live(uint8_t* rdram) {
    return mission1_script_loaded(rdram) &&
        read_u32(rdram, kCurrentLevelIndexAddress) == kMissionOneLevelIndex;
}

bool mission1_terminal_caller(uint32_t caller_pc) {
    return caller_pc == 0x80078EC8u || caller_pc == 0x8009930Cu ||
        caller_pc == 0x8007B5DCu || caller_pc == 0x8007C6B0u ||
        caller_pc == 0x8007CE74u;
}

MissionOneTerminalInvocation* active_mission1_terminal_invocation() {
    for (size_t depth = g_mission1_terminal_invocation_depth;
         depth > 0u; --depth) {
        MissionOneTerminalInvocation& invocation =
            g_mission1_terminal_invocation_stack[depth - 1u];
        if (invocation.active) {
            return &invocation;
        }
    }
    return nullptr;
}

bool mission1_profile_valid(uint8_t* rdram, uint32_t buffer) {
    if (!valid_guest_pointer(buffer, 0x40u)) {
        return false;
    }
    uint8_t checksum = 0;
    for (uint32_t index = 0; index < 0x3Fu; ++index) {
        checksum ^= MEM_BU(index, guest_address(buffer));
    }
    return MEM_BU(0x20, guest_address(buffer)) == 2u &&
        MEM_BU(0x3F, guest_address(buffer)) == checksum;
}

bool mission1_profile_buffers_equal(
    uint8_t* rdram,
    uint32_t left,
    uint32_t right
) {
    if (!valid_guest_pointer(left, 0x40u) ||
        !valid_guest_pointer(right, 0x40u)) {
        return false;
    }
    for (uint32_t index = 0; index < 0x40u; ++index) {
        if (MEM_BU(index, guest_address(left)) !=
            MEM_BU(index, guest_address(right))) {
            return false;
        }
    }
    return true;
}

bool register_mission1_target_locked(
    uint32_t actor,
    uint32_t source_pc,
    bool allow_withheld_selection
) {
    if (!valid_guest_pointer(actor, 0x74u)) {
        return false;
    }
    MissionOneTarget* reused_terminal_generation = nullptr;
    for (MissionOneTarget& target : g_mission1_targets) {
        if (target.actor == actor) {
            if (!target.terminal_observed) {
                return false;
            }
            reused_terminal_generation = &target;
            break;
        }
    }
    for (MissionOneTarget& target : g_mission1_targets) {
        if (&target != reused_terminal_generation && target.actor != 0u) {
            continue;
        }
        const bool reused = &target == reused_terminal_generation;
        target.actor = actor;
        target.source_pc = source_pc;
        target.generation = reused ? target.generation + 1u : 1u;
        target.terminal_observed = false;
        target.retry_after_frame = 0u;
        const uint32_t count =
            g_mission1_registered_owner_count.fetch_add(
                1,
                std::memory_order_acq_rel
            ) + 1u;
        if (allow_withheld_selection &&
            g_mission1_withhold_one_owner.load(std::memory_order_acquire) &&
            g_mission1_withheld_actor == 0u) {
            g_mission1_withheld_actor = actor;
        }
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=game_mission1_terminal_owner_registered"
            " count=%" PRIu32 " expected=13 source_pc=0x%08" PRIX32
            " actor=0x%08" PRIX32 " callback=0x8034B0D8"
            " generation=%" PRIu32 " reused_terminal_generation=%d"
            " withheld=%d guest_mutation=0\n",
            count,
            source_pc,
            actor,
            target.generation,
            reused ? 1 : 0,
            actor == g_mission1_withheld_actor ? 1 : 0
        );
        std::fflush(stderr);
        return true;
    }
    return false;
}

bool mark_mission1_terminal_locked(uint32_t actor) {
    for (MissionOneTarget& target : g_mission1_targets) {
        if (target.actor != actor) {
            continue;
        }
        if (target.terminal_observed) {
            return false;
        }
        target.terminal_observed = true;
        return true;
    }
    if (register_mission1_target_locked(actor, 0u, false)) {
        for (MissionOneTarget& target : g_mission1_targets) {
            if (target.actor == actor) {
                target.terminal_observed = true;
                return true;
            }
        }
    }
    return false;
}

bool register_mission1_pickup_locked(
    uint8_t* rdram,
    uint32_t actor,
    uint32_t record,
    uint8_t weapon
) {
    if ((weapon != kPlasmaWeapon && weapon != kGrenadeWeapon) ||
        !valid_guest_pointer(actor, 0x8Cu) ||
        read_u32(rdram, actor + 0x88u) != kMissionOneWeaponPickupVtable) {
        return false;
    }
    for (const MissionOnePickup& pickup : g_mission1_pickups) {
        if (pickup.actor == actor) {
            return false;
        }
    }
    for (MissionOnePickup& pickup : g_mission1_pickups) {
        if (pickup.actor != 0u) {
            continue;
        }
        pickup.actor = actor;
        pickup.record = record;
        pickup.weapon = weapon;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=game_mission1_weapon_pickup_registered"
            " record=0x%08" PRIX32 " actor=0x%08" PRIX32
            " selector=0x%08" PRIX32 " subtype=%u"
            " collision_path=func_80058500 guest_mutation=0\n",
            record,
            actor,
            kMissionOneWeaponPickupSelector,
            static_cast<unsigned>(weapon)
        );
        std::fflush(stderr);
        return true;
    }
    return false;
}

bool register_mission1_health_pickup_locked(
    uint8_t* rdram,
    uint32_t actor,
    uint32_t record,
    uint8_t subtype
) {
    if ((subtype != kSmallHealthPickup && subtype != kFullHealthPickup) ||
        !valid_guest_pointer(actor, 0x8Cu) ||
        read_u32(rdram, actor + 0x88u) != kMissionOneWeaponPickupVtable) {
        return false;
    }
    for (const MissionOneHealthPickup& pickup : g_mission1_health_pickups) {
        if (pickup.actor == actor) {
            return false;
        }
    }
    for (MissionOneHealthPickup& pickup : g_mission1_health_pickups) {
        if (pickup.actor != 0u) {
            continue;
        }
        pickup.actor = actor;
        pickup.record = record;
        pickup.subtype = subtype;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=game_mission1_health_pickup_registered"
            " record=0x%08" PRIX32 " actor=0x%08" PRIX32
            " selector=0x%08" PRIX32 " subtype=%u"
            " collision_path=func_80058500 guest_mutation=0\n",
            record,
            actor,
            kMissionOneWeaponPickupSelector,
            static_cast<unsigned>(subtype)
        );
        std::fflush(stderr);
        return true;
    }
    return false;
}

struct MissionOneDriverTarget {
    uint32_t actor = 0;
    bool pickup = false;
    bool health_pickup = false;
    bool waypoint = false;
    bool wave_prerequisite = false;
    bool wave_trigger = false;
    bool factory_trigger = false;
    bool cruise = false;
    bool altitude_staging = false;
    bool collision_center_aim = false;
    uint8_t pickup_subtype = 0;
    uint8_t desired_weapon = kStarterWeapon;
    int32_t health = 0;
    float distance = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;
    float dz = 0.0f;
    float player_x = 0.0f;
    float player_y = 0.0f;
    float player_z = 0.0f;
    float target_x = 0.0f;
    float target_y = 0.0f;
    float target_z = 0.0f;
};

MissionOneDriverTarget choose_mission1_driver_target(
    uint8_t* rdram,
    uint32_t player,
    uint64_t frame,
    uint32_t plasma_ammo,
    uint32_t grenade_ammo,
    float player_health
) {
    const float player_x = float_from_word(read_u32(rdram, player + 0x40u));
    const float player_y = float_from_word(read_u32(rdram, player + 0x44u));
    const float player_z = float_from_word(read_u32(rdram, player + 0x48u));
    if (!std::isfinite(player_x) || !std::isfinite(player_y) ||
        !std::isfinite(player_z)) {
        return {};
    }

    const auto make_candidate = [=](
        uint32_t actor,
        bool pickup,
        bool health_pickup,
        uint8_t pickup_subtype,
        uint8_t desired_weapon,
        int32_t health
    ) {
        MissionOneDriverTarget candidate{};
        if (!valid_guest_pointer(actor, 0x4Cu)) {
            return candidate;
        }
        float target_x =
            float_from_word(read_u32(rdram, actor + 0x40u));
        float target_y =
            float_from_word(read_u32(rdram, actor + 0x44u));
        float target_z =
            float_from_word(read_u32(rdram, actor + 0x48u));
        if (!std::isfinite(target_x) || !std::isfinite(target_y) ||
            !std::isfinite(target_z)) {
            return candidate;
        }
        constexpr uint32_t kTransporterVtable = 0x80049908u;
        bool collision_center_aim = false;
        if (!pickup && valid_guest_pointer(actor, 0x8Cu) &&
            read_u32(rdram, actor + 0x88u) == kTransporterVtable) {
            const float basis_20_x =
                float_from_word(read_u32(rdram, actor + 0x20u));
            const float basis_20_y =
                float_from_word(read_u32(rdram, actor + 0x24u));
            const float basis_20_z =
                float_from_word(read_u32(rdram, actor + 0x28u));
            const float basis_30_x =
                float_from_word(read_u32(rdram, actor + 0x30u));
            const float basis_30_y =
                float_from_word(read_u32(rdram, actor + 0x34u));
            const float basis_30_z =
                float_from_word(read_u32(rdram, actor + 0x38u));
            if (std::isfinite(basis_20_x) &&
                std::isfinite(basis_20_y) &&
                std::isfinite(basis_20_z) &&
                std::isfinite(basis_30_x) &&
                std::isfinite(basis_30_y) &&
                std::isfinite(basis_30_z)) {
                target_x += basis_20_x * -60.0f + basis_30_x * -190.0f;
                target_y += basis_20_y * -60.0f + basis_30_y * -190.0f;
                target_z += basis_20_z * -60.0f + basis_30_z * -190.0f;
                collision_center_aim = true;
            }
        }
        const float dx = target_x - player_x;
        const float dy = target_y - player_y;
        const float dz = target_z - player_z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(distance) || distance < 0.01f) {
            return candidate;
        }
        candidate.actor = actor;
        candidate.pickup = pickup;
        candidate.health_pickup = health_pickup;
        candidate.pickup_subtype = pickup_subtype;
        candidate.desired_weapon = desired_weapon;
        candidate.health = health;
        candidate.collision_center_aim = collision_center_aim;
        candidate.distance = distance;
        candidate.dx = dx;
        candidate.dy = dy;
        candidate.dz = dz;
        candidate.player_x = player_x;
        candidate.player_y = player_y;
        candidate.player_z = player_z;
        candidate.target_x = target_x;
        candidate.target_y = target_y;
        candidate.target_z = target_z;
        return candidate;
    };

    std::scoped_lock lock(g_mission1_mutex);
    const auto choose_pickup = [&](uint8_t weapon) {
        MissionOneDriverTarget best{};
        for (const MissionOnePickup& pickup : g_mission1_pickups) {
            if (pickup.actor == 0u || pickup.weapon != weapon ||
                pickup.consumed || pickup.retry_after_frame > frame ||
                !valid_guest_pointer(pickup.actor, 0x8Cu) ||
                read_u32(rdram, pickup.actor + 0x88u) !=
                    kMissionOneWeaponPickupVtable) {
                continue;
            }
            MissionOneDriverTarget candidate = make_candidate(
                pickup.actor,
                true,
                false,
                pickup.weapon,
                pickup.weapon,
                0
            );
            if (candidate.actor == 0u) {
                continue;
            }
            if (candidate.actor == g_mission1_last_driver_target) {
                return candidate;
            }
            if (best.actor == 0u || candidate.distance < best.distance) {
                best = candidate;
            }
        }
        return best;
    };

    const auto choose_health_pickup = [&]() {
        MissionOneDriverTarget best{};
        for (const MissionOneHealthPickup& pickup :
             g_mission1_health_pickups) {
            if (pickup.actor == 0u || pickup.consumed ||
                pickup.retry_after_frame > frame ||
                !valid_guest_pointer(pickup.actor, 0x8Cu) ||
                read_u32(rdram, pickup.actor + 0x88u) !=
                    kMissionOneWeaponPickupVtable) {
                continue;
            }
            MissionOneDriverTarget candidate = make_candidate(
                pickup.actor,
                true,
                true,
                pickup.subtype,
                kStarterWeapon,
                0
            );
            if (candidate.actor == 0u) {
                continue;
            }
            if (candidate.actor == g_mission1_last_driver_target) {
                return candidate;
            }
            if (best.actor == 0u || candidate.distance < best.distance) {
                best = candidate;
            }
        }
        return best;
    };

    if (g_mission1_detour.active) {
        const MissionOneDetour detour = g_mission1_detour;
        int32_t health = 0;
        bool target_live = false;
        for (const MissionOneTarget& target : g_mission1_targets) {
            if (target.actor == detour.actor && !target.terminal_observed &&
                valid_guest_pointer(target.actor, 0x74u) &&
                read_u32(rdram, target.actor + 0x70u) ==
                    kMissionOneTerminalCallback) {
                health = static_cast<int32_t>(
                    MEM_H(0, guest_address(target.actor + 0x7Cu))
                );
                target_live = health > 0;
                break;
            }
        }
        if (target_live) {
            const uint8_t desired_weapon = health > 3 && grenade_ammo > 0u
                ? kGrenadeWeapon
                : (plasma_ammo > 0u ? kPlasmaWeapon : kStarterWeapon);
            MissionOneDriverTarget candidate = make_candidate(
                detour.actor,
                false,
                false,
                0u,
                desired_weapon,
                health
            );
            candidate.waypoint = true;
            candidate.target_x = detour.x;
            candidate.target_y = detour.y;
            candidate.target_z = detour.z;
            candidate.dx = detour.x - player_x;
            candidate.dy = detour.y - player_y;
            candidate.dz = detour.z - player_z;
            candidate.distance = std::sqrt(
                candidate.dx * candidate.dx +
                candidate.dy * candidate.dy +
                candidate.dz * candidate.dz
            );
            if (std::isfinite(candidate.distance) &&
                candidate.distance > 75.0f) {
                return candidate;
            }
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=mission1_completion_detour_reached"
                " actor=0x%08" PRIX32 " waypoint_xyz=%.3f,%.3f,%.3f"
                " player_xyz=%.3f,%.3f,%.3f input_only=1\n",
                detour.actor,
                static_cast<double>(detour.x),
                static_cast<double>(detour.y),
                static_cast<double>(detour.z),
                static_cast<double>(player_x),
                static_cast<double>(player_y),
                static_cast<double>(player_z)
            );
            std::fflush(stderr);
        }
        g_mission1_detour = {};
        g_mission1_last_driver_target = 0u;
    }

    if (std::isfinite(player_health) && player_health < 65.0f) {
        MissionOneDriverTarget pickup = choose_health_pickup();
        if (pickup.actor != 0u) {
            return pickup;
        }
    }

    if (plasma_ammo == 0u &&
        g_mission1_terminal_count.load(std::memory_order_acquire) >= 1u) {
        MissionOneDriverTarget pickup = choose_pickup(kPlasmaWeapon);
        if (pickup.actor != 0u && pickup.distance <= 1400.0f) {
            return pickup;
        }
    }

    if (g_mission1_wave_trigger.actor != 0u ||
        g_mission1_factory_trigger.actor != 0u) {
        const uint32_t wave_actor = g_mission1_wave_trigger.actor;
        if (valid_guest_pointer(wave_actor, 0xA8u) &&
            read_u32(rdram, wave_actor + 0x88u) ==
                kMissionOneWaveTriggerVtable) {
            const uint32_t wave_state = read_u32(rdram, wave_actor + 0x8Cu);
            if (wave_state == 0u) {
                const uint32_t list_root =
                    read_u32(rdram, wave_actor + 0x60u);
                std::array<uint32_t, 32> visited{};
                MissionOneDriverTarget best{};
                MissionOneDriverTarget sticky{};
                size_t list_count = 0u;
                uint32_t node = valid_guest_pointer(list_root, 4u)
                    ? read_u32(rdram, list_root)
                    : 0u;
                for (size_t member_index = 0u;
                     member_index < visited.size() &&
                     valid_guest_pointer(node, 0xA8u);
                     ++member_index) {
                    bool cycle = false;
                    for (size_t seen = 0u; seen < member_index; ++seen) {
                        if (visited[seen] == node) {
                            cycle = true;
                            break;
                        }
                    }
                    if (cycle) {
                        break;
                    }
                    visited[member_index] = node;
                    ++list_count;

                    if (node != wave_actor) {
                        MissionOneWavePrerequisite* observed = nullptr;
                        for (MissionOneWavePrerequisite& prerequisite :
                             g_mission1_wave_prerequisites) {
                            if (prerequisite.actor == node) {
                                observed = &prerequisite;
                                break;
                            }
                            if (observed == nullptr &&
                                prerequisite.actor == 0u) {
                                observed = &prerequisite;
                            }
                        }

                        const uint32_t vtable =
                            read_u32(rdram, node + 0x88u);
                        const uint32_t callback =
                            read_u32(rdram, node + 0x70u);
                        const uint32_t state =
                            read_u32(rdram, node + 0x8Cu);
                        const int32_t health = static_cast<int32_t>(
                            static_cast<int16_t>(
                                MEM_H(0, guest_address(node + 0x7Cu))
                            )
                        );
                        if (observed != nullptr && observed->actor == 0u) {
                            observed->actor = node;
                            std::fprintf(
                                stderr,
                                "BUMBLE_RT64_PROBE"
                                " stage=game_mission1_wave_prerequisite_registered"
                                " monitor=0x%08" PRIX32
                                " root=0x%08" PRIX32
                                " member_index=%zu actor=0x%08" PRIX32
                                " vtable=0x%08" PRIX32
                                " callback=0x%08" PRIX32
                                " state=%" PRIu32 " health=%d"
                                " actor_xyz=%.3f,%.3f,%.3f"
                                " selector_category=0x00400000"
                                " guest_mutation=0\n",
                                wave_actor,
                                list_root,
                                member_index,
                                node,
                                vtable,
                                callback,
                                state,
                                health,
                                static_cast<double>(float_from_word(
                                    read_u32(rdram, node + 0x40u)
                                )),
                                static_cast<double>(float_from_word(
                                    read_u32(rdram, node + 0x44u)
                                )),
                                static_cast<double>(float_from_word(
                                    read_u32(rdram, node + 0x48u)
                                ))
                            );
                            std::fflush(stderr);
                        }

                        const bool retry_ready = observed == nullptr ||
                            observed->retry_after_frame <= frame;
                        if (retry_ready) {
                            const uint8_t desired_weapon =
                                health > 3 && grenade_ammo > 0u
                                ? kGrenadeWeapon
                                : (plasma_ammo > 0u
                                    ? kPlasmaWeapon
                                    : kStarterWeapon);
                            MissionOneDriverTarget candidate = make_candidate(
                                node,
                                false,
                                false,
                                0u,
                                desired_weapon,
                                std::max(health, 1)
                            );
                            candidate.wave_prerequisite =
                                candidate.actor != 0u;
                            if (candidate.actor != 0u) {
                                if (candidate.actor ==
                                    g_mission1_last_driver_target) {
                                    sticky = candidate;
                                }
                                if (best.actor == 0u ||
                                    candidate.distance < best.distance) {
                                    best = candidate;
                                }
                            }
                        }
                    }
                    node = read_u32(rdram, node + 0x04u);
                }

                MissionOneDriverTarget prerequisite =
                    sticky.actor != 0u ? sticky : best;
                if (prerequisite.actor != 0u) {
                    if (prerequisite.desired_weapon == kGrenadeWeapon &&
                        grenade_ammo == 0u) {
                        MissionOneDriverTarget pickup =
                            choose_pickup(kGrenadeWeapon);
                        if (pickup.actor != 0u) {
                            return pickup;
                        }
                        prerequisite.desired_weapon = plasma_ammo > 0u
                            ? kPlasmaWeapon
                            : kStarterWeapon;
                    }
                    return prerequisite;
                }

                if (list_count == 1u &&
                    !g_mission1_wave_prerequisites_cleared_logged) {
                    g_mission1_wave_prerequisites_cleared_logged = true;
                    std::fprintf(
                        stderr,
                        "BUMBLE_RT64_PROBE"
                        " stage=game_mission1_wave_prerequisites_cleared"
                        " monitor=0x%08" PRIX32
                        " root=0x%08" PRIX32
                        " list_count=1"
                        " activation_wait=next_64_frame_monitor_check"
                        " guest_mutation=0\n",
                        wave_actor,
                        list_root
                    );
                    std::fflush(stderr);
                }
            }
            if (wave_state != 0u &&
                !g_mission1_wave_trigger.activation_logged) {
                g_mission1_wave_trigger.activation_logged = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=game_mission1_wave_trigger_activated"
                    " record=0x%08" PRIX32 " actor=0x%08" PRIX32
                    " state=%" PRIu32 " callback=0x%08" PRIX32
                    " callback_effect=spawn_selector8_wave"
                    " guest_mutation=game_owned\n",
                    g_mission1_wave_trigger.record,
                    wave_actor,
                    wave_state,
                    kMissionOneWaveTriggerCallback
                );
                std::fflush(stderr);
            }
        }

        const uint32_t factory_actor = g_mission1_factory_trigger.actor;
        if (valid_guest_pointer(factory_actor, 0xA8u) &&
            read_u32(rdram, factory_actor + 0x88u) ==
                kMissionOneFactoryTriggerVtable) {
            const uint32_t factory_state =
                read_u32(rdram, factory_actor + 0x8Cu);
            if (factory_state == 0u) {
                MissionOneDriverTarget factory = make_candidate(
                    factory_actor,
                    false,
                    false,
                    0u,
                    kStarterWeapon,
                    0
                );
                factory.factory_trigger = factory.actor != 0u;
                return factory;
            }
            if (!g_mission1_factory_trigger.activation_logged) {
                g_mission1_factory_trigger.activation_logged = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=game_mission1_factory_trigger_activated"
                    " record=0x%08" PRIX32 " actor=0x%08" PRIX32
                    " state=%" PRIu32 " spawn_count=%" PRIu32
                    " collision_callback=0x%08" PRIX32
                    " guest_mutation=game_owned\n",
                    g_mission1_factory_trigger.record,
                    factory_actor,
                    factory_state,
                    read_u32(rdram, factory_actor + 0x98u),
                    kMissionOneFactoryTriggerCollisionCallback
                );
                std::fflush(stderr);
            }
        }
    }

    MissionOneDriverTarget best_low_health{};
    MissionOneDriverTarget sticky_low_health{};
    MissionOneDriverTarget best_any{};
    MissionOneDriverTarget sticky_any{};
    for (const MissionOneTarget& target : g_mission1_targets) {
        if (target.actor == 0u || target.terminal_observed ||
            target.actor == g_mission1_withheld_actor ||
            target.retry_after_frame > frame ||
            !valid_guest_pointer(target.actor, 0x74u) ||
            read_u32(rdram, target.actor + 0x70u) !=
                kMissionOneTerminalCallback) {
            continue;
        }
        const int32_t health = static_cast<int32_t>(
            MEM_H(0, guest_address(target.actor + 0x7Cu))
        );
        if (health <= 0) {
            continue;
        }
        const uint8_t desired_weapon = health > 3 && grenade_ammo > 0u
            ? kGrenadeWeapon
            : (plasma_ammo > 0u ? kPlasmaWeapon : kStarterWeapon);
        MissionOneDriverTarget candidate = make_candidate(
            target.actor,
            false,
            false,
            0u,
            desired_weapon,
            health
        );
        if (candidate.actor == 0u) {
            continue;
        }
        if (target.actor == g_mission1_last_driver_target) {
            sticky_any = candidate;
            if (health <= 3) {
                sticky_low_health = candidate;
            }
        }
        if (best_any.actor == 0u || candidate.distance < best_any.distance) {
            best_any = candidate;
        }
        if (health <= 3 && (best_low_health.actor == 0u ||
                           candidate.distance < best_low_health.distance)) {
            best_low_health = candidate;
        }
    }

    if (sticky_low_health.actor != 0u) {
        return sticky_low_health;
    }
    if (best_low_health.actor != 0u) {
        return best_low_health;
    }

    if (grenade_ammo == 0u) {
        MissionOneDriverTarget pickup = choose_pickup(kGrenadeWeapon);
        if (pickup.actor != 0u) {
            return pickup;
        }
    }
    MissionOneDriverTarget enemy =
        sticky_any.actor != 0u ? sticky_any : best_any;
    if (enemy.actor != 0u) {
        return enemy;
    }

    return {};
}

uint32_t float_to_word(float value) {
    return std::bit_cast<uint32_t>(value);
}

uint32_t rounded_float_mul(uint32_t left_word, uint32_t right_word) {
    volatile float rounded = float_from_word(left_word) * float_from_word(right_word);
    return float_to_word(rounded);
}

uint32_t rounded_float_add(uint32_t left_word, uint32_t right_word) {
    volatile float rounded = float_from_word(left_word) + float_from_word(right_word);
    return float_to_word(rounded);
}

uint32_t signed_value_float_word(int16_t value) {
    volatile float rounded = static_cast<float>(value);
    return float_to_word(rounded);
}

struct ControlExpectation {
    uint32_t scaled_x_constant_word = 0;
    uint32_t stick_y_constant_word = 0;
    uint32_t vertical_bias_constant_word = 0;
    uint32_t minimum_word = 0;
    uint32_t maximum_word = 0;
    uint32_t scaled_x_word = 0;
    uint32_t scaled_y_word = 0;
    uint32_t scaled_y_input_word = 0;
    uint32_t vertical_bias_component_word = 0;
    uint32_t unclamped_y_word = 0;
    bool constants_match = false;
};

ControlExpectation expected_scaled_controls(
    uint8_t* rdram,
    int16_t stick_x,
    int16_t stick_y,
    uint32_t vertical_bias_word
) {
    const uint32_t scaled_x_constant = read_u32(rdram, kScaledXConstantAddress);
    const uint32_t stick_y_constant = read_u32(rdram, kStickYConstantAddress);
    const uint32_t vertical_bias_constant = read_u32(rdram, kVerticalBiasConstantAddress);
    const uint32_t minimum = read_u32(rdram, kMinimumControlAddress);
    const uint32_t maximum = read_u32(rdram, kMaximumControlAddress);

    ControlExpectation result{};
    result.scaled_x_constant_word = scaled_x_constant;
    result.stick_y_constant_word = stick_y_constant;
    result.vertical_bias_constant_word = vertical_bias_constant;
    result.minimum_word = minimum;
    result.maximum_word = maximum;
    result.constants_match = scaled_x_constant == kScaledXConstantWord &&
        stick_y_constant == kStickYConstantWord &&
        vertical_bias_constant == kVerticalBiasConstantWord &&
        minimum == kMinimumControlWord && maximum == kMaximumControlWord;
    result.scaled_x_word = rounded_float_mul(
        signed_value_float_word(stick_x),
        scaled_x_constant
    );
    result.scaled_y_input_word = rounded_float_mul(
        signed_value_float_word(stick_y),
        stick_y_constant
    );
    result.vertical_bias_component_word = rounded_float_mul(
        vertical_bias_word,
        vertical_bias_constant
    );
    result.unclamped_y_word = rounded_float_add(
        result.scaled_y_input_word,
        result.vertical_bias_component_word
    );
    const float unclamped = float_from_word(result.unclamped_y_word);
    if (unclamped < float_from_word(minimum)) {
        result.scaled_y_word = minimum;
    } else if (float_from_word(maximum) < unclamped) {
        result.scaled_y_word = maximum;
    } else {
        result.scaled_y_word = result.unclamped_y_word;
    }
    return result;
}

uint32_t expected_orientation(
    int16_t stick_y,
    uint32_t orientation_before_word,
    uint32_t constant_word
) {
    const uint32_t delta = rounded_float_mul(
        signed_value_float_word(stick_y),
        constant_word
    );
    return rounded_float_add(orientation_before_word, delta);
}

uint32_t expected_integrator_position(
    uint32_t basis_word,
    uint32_t speed_word,
    uint32_t position_word,
    double scale
) {
    const uint32_t product_word = rounded_float_mul(basis_word, speed_word);
    volatile double displacement = static_cast<double>(float_from_word(product_word)) * scale;
    volatile double result = static_cast<double>(float_from_word(position_word)) - displacement;
    volatile float rounded = static_cast<float>(result);
    return float_to_word(rounded);
}

const char* two_player_stage_name(TwoPlayerStage stage) {
    switch (stage) {
    case TwoPlayerStage::Unarmed: return "unarmed";
    case TwoPlayerStage::WaitingCallback: return "waiting_callback";
    case TwoPlayerStage::WaitingDispatch: return "waiting_dispatch";
    case TwoPlayerStage::WaitingControlSample: return "waiting_control_sample";
    case TwoPlayerStage::WaitingScaledControls: return "waiting_scaled_controls";
    case TwoPlayerStage::WaitingOrientationSample: return "waiting_orientation_sample";
    case TwoPlayerStage::WaitingOrientationPost: return "waiting_orientation_post";
    case TwoPlayerStage::WaitingIntegratorPre: return "waiting_integrator_pre";
    case TwoPlayerStage::WaitingIntegratorPost: return "waiting_integrator_post";
    case TwoPlayerStage::Complete: return "complete";
    }
    return "invalid";
}

void reject_two_player_chain_locked(
    const char* reason,
    uint32_t pc,
    uint32_t actor
) {
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_two_player_chain_rejected"
        " pc=0x%08" PRIX32 " actor=0x%08" PRIX32
        " expected_stage=%s reason=%s\n",
        pc,
        actor,
        two_player_stage_name(g_two_player_chain.stage),
        reason
    );
    std::fflush(stderr);
    g_two_player_chain.armed = false;
    g_two_player_chain.stage = TwoPlayerStage::Unarmed;
}

bool chain_identity_matches(
    uint8_t* rdram,
    uint32_t actor,
    PlayerOwners* owners_out,
    NormalizedPad* player_one_pad_out
) {
    const PlayerOwners owners = read_player_owners(rdram);
    const NormalizedPad player_one_pad = read_normalized_pad(rdram, 0u);
    if (owners_out != nullptr) {
        *owners_out = owners;
    }
    if (player_one_pad_out != nullptr) {
        *player_one_pad_out = player_one_pad;
    }
    if (!owners.distinct_nonzero || !owners.metadata_matches ||
        actor == 0 || actor != owners.player_two ||
        actor != g_two_player_chain.actor ||
        !valid_guest_pointer(actor, 0xF5u) ||
        read_u32(rdram, actor + 0x88u) != kPlayerVtable ||
        MEM_BU(0xF4, guest_address(actor)) != 1u ||
        read_u32(rdram, actor + 0x8Cu) != kPlayerStateTwo ||
        !normalized_pad_is_neutral(player_one_pad)) {
        return false;
    }
    return true;
}

} // namespace

void bumble::native_campaign_level::observe(
    uint8_t* rdram,
    ObservationSite site
) {
    if (rdram == nullptr) {
        return;
    }

    const uint32_t observation_pc = campaign_observation_pc(site);
    if (observation_pc == 0u) {
        return;
    }

    const uint32_t level_index = read_u32(rdram, kCurrentLevelIndexAddress);
    const bumble::campaign_levels::Record* expected =
        bumble::campaign_levels::find(level_index);
    if (expected == nullptr) {
        return;
    }

    const LevelRecordIdentity identity = read_level_record_identity(
        rdram,
        level_index
    );
    if (!campaign_record_matches(rdram, identity, *expected)) {
        return;
    }

    const uint32_t level_bit = 1u << level_index;
    const uint32_t previous_mask = g_campaign_level_observed_mask.fetch_or(
        level_bit,
        std::memory_order_acq_rel
    );
    if ((previous_mask & level_bit) != 0u) {
        return;
    }

    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_campaign_level_identity_observed"
        " schema=1 identity_contract=full_0x24"
        " observation_site=%s observation_pc=0x%08" PRIX32
        " level_index_address=0x800E9640 level_index=%" PRIu32
        " level_record=0x%08" PRIX32
        " level_identifier=\"%.*s\" level_id_word=0x%08" PRIX32
        " level_load_offset=0x%08" PRIX32
        " level_record_word_18=0x%08" PRIX32
        " level_record_word_1c=0x%08" PRIX32
        " level_record_word_20=0x%08" PRIX32
        " observed_level_index_mask=0x%08" PRIX32 "\n",
        campaign_observation_site_name(site),
        observation_pc,
        level_index,
        identity.address,
        static_cast<int>(expected->identifier.size()),
        expected->identifier.data(),
        identity.id_word,
        identity.load_offset,
        identity.record_word_18,
        identity.record_word_1c,
        identity.record_word_20,
        previous_mask | level_bit
    );
    std::fflush(stderr);
}

extern "C" void buck_raw_pad_return_probe(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_apply_player_maximum_health(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr) {
        return;
    }
    const float loaded_maximum = float_from_word(context->f0.u32l);
    if (!std::isfinite(loaded_maximum) || loaded_maximum <= 0.0f ||
        loaded_maximum > 100000.0f) {
        return;
    }

    const uint32_t applied_word = apply_configured_player_maximum(
        rdram,
        loaded_maximum
    );
    context->f0.u32l = applied_word;
}

extern "C" void bumble_apply_ammo_pickup_amount(
    uint8_t*,
    recomp_context* context
) {
    if (context == nullptr ||
        !bumble::graphics_options::double_ammo_pickups_enabled()) {
        return;
    }
    context->r16 = static_cast<gpr>(configured_ammo_pickup_grant(
        guest_u32(context->r16),
        true
    ));
}

extern "C" void bumble_apply_mission_time_limit(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr || guest_u32(context->r2) != 1u ||
        !bumble::graphics_options::double_mission_time_limits_enabled()) {
        return;
    }

    const uint32_t timer = guest_u32(context->r4);
    if (!valid_guest_pointer(timer, 0x34u) || read_u32(rdram, timer) == 0u ||
        read_u32(rdram, timer + 0x24u) == 1u) {
        return;
    }

    const uint64_t total = configured_mission_time_seconds(
        read_u32(rdram, timer + 0x28u),
        read_u32(rdram, timer + 0x2Cu),
        true
    );
    write_u32(rdram, timer + 0x28u, static_cast<uint32_t>(total / 60u));
    write_u32(rdram, timer + 0x2Cu, static_cast<uint32_t>(total % 60u));
}

extern "C" void bumble_gameplay_modifier_tick(uint8_t* rdram) {
    const bool gameplay = settled_gameplay_is_live(rdram);
    const bool enemy_health =
        bumble::graphics_options::double_enemy_health_enabled();
    if (!gameplay || !g_gameplay_modifiers_live ||
        enemy_health != g_enemy_health_setting) {
        g_enemy_damage_suppress_next.clear();
    }
    g_gameplay_modifiers_live = gameplay;
    g_enemy_health_setting = enemy_health;

    if (!performance_reporting_enabled() || !gameplay) {
        return;
    }
    uint32_t active_actors = 0u;
    uint32_t particle_actors = 0u;
    std::array<
        uint32_t,
        bumble::weapon_system::kActorFamilyCount
    > weapon_actors{};
    for (uint32_t list = 0u; list < kObjectListCount; ++list) {
        uint32_t actor = read_u32(
            rdram,
            kObjectListBase + list * kObjectListStride
        );
        uint32_t traversed = 0u;
        while (actor != 0u && traversed++ < 4096u &&
               valid_guest_pointer(actor, 0x8Cu)) {
            ++active_actors;
            const uint32_t vtable = read_u32(rdram, actor + 0x88u);
            if (vtable == kParticleParentVtable ||
                vtable == kParticleActorVtable) {
                ++particle_actors;
            }
            const bumble::weapon_system::ActorFamily weapon_family =
                bumble::weapon_system::actor_family_for_descriptor(vtable);
            if (weapon_family !=
                bumble::weapon_system::ActorFamily::Count) {
                ++weapon_actors[static_cast<size_t>(weapon_family)];
            }
            actor = read_u32(rdram, actor + 0x04u);
        }
    }
    g_performance_active_actors.store(active_actors, std::memory_order_release);
    g_performance_particle_actors.store(
        particle_actors,
        std::memory_order_release
    );
    for (size_t family = 0u; family < weapon_actors.size(); ++family) {
        g_performance_weapon_actors[family].store(
            weapon_actors[family],
            std::memory_order_release
        );
    }
    g_performance_rendered_actors.store(
        g_performance_rendered_actors_current.exchange(
            0u,
            std::memory_order_acq_rel
        ),
        std::memory_order_release
    );
}

extern "C" void bumble_record_rendered_actor(void) {
    if (performance_reporting_enabled()) {
        g_performance_rendered_actors_current.fetch_add(
            1u,
            std::memory_order_relaxed
        );
    }
}

extern "C" void bumble_enemy_spawner_retire_collision(
    uint8_t* rdram,
    uint32_t actor
) {
    actor = guest_u32(actor);
    if (rdram == nullptr || !valid_guest_pointer(actor, 0x8Cu) ||
        !enemy_spawner_vtable(read_u32(rdram, actor + 0x88u)) ||
        read_u32(rdram, actor + 0x8Cu) != kDestroyedSpawnerState) {
        return;
    }

    const uint32_t collision_flags = read_u32(rdram, actor + 0x64u);
    const uint32_t collision_mask = read_u32(rdram, actor + 0x68u);
    if ((collision_flags | collision_mask) == 0u) {
        return;
    }
    write_u32(rdram, actor + 0x64u, 0u);
    write_u32(rdram, actor + 0x68u, 0u);
    std::fprintf(
        stderr,
        "BUMBLE_COLLISION stage=destroyed_spawner_retired"
        " actor=0x%08" PRIX32 " vtable=0x%08" PRIX32
        " flags=0x%08" PRIX32 " mask=0x%08" PRIX32 "\n",
        actor,
        read_u32(rdram, actor + 0x88u),
        collision_flags,
        collision_mask
    );
    std::fflush(stderr);
}

extern "C" void bumble_scale_enemy_awareness_planar_distance(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!enemy_awareness_actor(rdram, context) ||
        !player_position_target(context->r5)) {
        return;
    }
    const uint32_t output = guest_u32(context->r7);
    if (!valid_guest_pointer(output, sizeof(uint32_t))) {
        return;
    }
    const float scaled = configured_enemy_awareness_distance_component(
        float_from_word(read_u32(rdram, output)),
        true
    );
    write_u32(rdram, output, std::bit_cast<uint32_t>(scaled));
}

extern "C" void bumble_scale_enemy_awareness_target_distance(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!enemy_awareness_actor(rdram, context) ||
        !player_position_target(context->r5)) {
        return;
    }
    context->f2.fl = configured_enemy_awareness_distance_component(
        context->f2.fl,
        true
    );
    context->f0.fl = configured_enemy_awareness_distance_component(
        context->f0.fl,
        true
    );
}

extern "C" void bumble_scale_enemy_awareness_player_distance(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!enemy_awareness_actor(rdram, context)) {
        return;
    }
    context->f2.fl = configured_enemy_awareness_distance_component(
        context->f2.fl,
        true
    );
    context->f0.fl = configured_enemy_awareness_distance_component(
        context->f0.fl,
        true
    );
}

extern "C" void bumble_apply_enemy_health_damage(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t actor
) {
    if (rdram == nullptr || context == nullptr || !g_gameplay_modifiers_live ||
        !g_enemy_health_setting) {
        return;
    }
    actor = guest_u32(actor);
    if (!valid_guest_pointer(actor, 0x8Cu)) {
        return;
    }
    if (!hostile_combat_vtable(read_u32(rdram, actor + 0x88u))) {
        return;
    }

    bool& suppress =
        g_enemy_damage_suppress_next.try_emplace(actor, true).first->second;
    if (suppress) {
        context->r2 = static_cast<gpr>(guest_u32(context->r2) + 1u);
    }
    suppress = !suppress;
}

extern "C" void bumble_cancel_life_decrement(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr) {
        return;
    }
    context->r2 = static_cast<gpr>(read_u32(rdram, kPlayerLivesAddress));
}

extern "C" void bumble_remove_lives_gate(
    uint8_t*,
    recomp_context* context
) {
    if (context == nullptr) {
        return;
    }
    context->r2 = 1;
}

extern "C" void bumble_apply_unlimited_health_gate(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr ||
        !bumble::graphics_options::unlimited_health_enabled()) {
        return;
    }
    const uint32_t health_word = maximum_health_word(rdram);
    write_u32(
        rdram,
        kWeaponStateBase + kPlayerHealthOffset,
        health_word
    );
    // Health is already loaded into f2; update the register as well as memory.
    context->f2.u32l = health_word;
}

extern "C" void bumble_apply_honeycomb_water_rescue(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr) {
        return;
    }
    const uint32_t actor = guest_u32(context->r17);
    uint8_t player_index = 0xFFu;
    if (!exact_player_actor(rdram, actor, player_index)) {
        return;
    }
    PlayerCheatState& state = g_player_cheat_states[player_index];
    if (state.actor != actor) {
        state = PlayerCheatState{};
        state.actor = actor;
    }
    if (!bumble::graphics_options::honeycomb_water_rescue_enabled()) {
        state.water_recovery_frames = 0u;
        return;
    }

    const uint32_t weapon_state =
        kWeaponStateBase + player_index * kWeaponStateStride;
    const bool recovering = state.water_recovery_frames > 0u;
    if (recovering) {
        const float health = float_from_word(read_u32(
            rdram,
            weapon_state + kPlayerHealthOffset
        ));
        if (std::isfinite(health)) {
            if (health < state.water_recovery_health) {
                write_u32(
                    rdram,
                    weapon_state + kPlayerHealthOffset,
                    std::bit_cast<uint32_t>(state.water_recovery_health)
                );
            } else {
                state.water_recovery_health = health;
            }
        }
        --state.water_recovery_frames;
    }

    if (read_u32(rdram, actor + 0x8Cu) != 10u) {
        return;
    }
    if (recovering) {
        if (restore_water_recovery_position(rdram, actor)) {
            resume_after_water_recovery(rdram, actor);
        }
        return;
    }

    const float maximum = float_from_word(read_u32(
        rdram,
        kPlayerMaximumHealthAddress
    ));
    const float health = float_from_word(read_u32(
        rdram,
        weapon_state + kPlayerHealthOffset
    ));
    const uint32_t cells =
        bumble::graphics_options::half_player_health_enabled() ? 5u : 10u;
    if (!std::isfinite(maximum) || maximum <= 0.0f ||
        !std::isfinite(health) || health <= 0.0f) {
        return;
    }

    const float damage = honeycomb_cell_damage(maximum, cells);
    const float recovered_health = std::max(0.0f, health - damage);
    if (recovered_health <= 0.0f) {
        write_u32(rdram, weapon_state + kPlayerHealthOffset, 0u);
        return;
    }
    if (!restore_water_recovery_position(rdram, actor)) {
        return;
    }
    write_u32(
        rdram,
        weapon_state + kPlayerHealthOffset,
        std::bit_cast<uint32_t>(recovered_health)
    );

    resume_after_water_recovery(rdram, actor);
    state.water_recovery_frames = kWaterRecoveryFrames;
    state.water_recovery_health = recovered_health;
    std::fprintf(
        stderr,
        "BUMBLE_WATER stage=honeycomb_rescue"
        " actor=0x%08" PRIX32 " player_index=%u health=%.3f"
        " damage=%.3f recovery_frames=%u\n",
        actor,
        static_cast<unsigned>(player_index),
        static_cast<double>(recovered_health),
        static_cast<double>(damage),
        kWaterRecoveryFrames
    );
    std::fflush(stderr);
}

extern "C" void bumble_capture_campaign_level_progress(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr) {
        return;
    }
    const uint32_t live_object = guest_u32(context->r4);
    if (!valid_guest_pointer(live_object, 0x88u)) {
        return;
    }
    const uint32_t progress_level =
        bumble::graphics_options::campaign_unlocked_level();
    g_campaign_grid_progress_level.store(
        progress_level,
        std::memory_order_release
    );
    g_campaign_grid_live_object.store(live_object, std::memory_order_release);
}

extern "C" void bumble_initialize_campaign_level_grid(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr) {
        return;
    }
    const uint32_t live_object = guest_u32(context->r18);
    if (!campaign_grid_identity(rdram, live_object)) {
        return;
    }
    const uint32_t progress_level = g_campaign_grid_progress_level.load(
        std::memory_order_acquire
    );
    const size_t selected_slot = campaign_slot_for_progress(progress_level);
    const uint32_t selected_level =
        bumble::campaign_levels::kMissionSelectLevelIndices[selected_slot];
    const auto* record = bumble::campaign_levels::find(selected_level);
    if (record == nullptr) {
        return;
    }

    write_u32(rdram, kMissionSelectorIndexAddress, selected_level);
    write_u32(rdram, kMissionSelectorPreviousIndexAddress, 0u);
    write_u32(
        rdram,
        kMissionSelectorNumberAddress,
        bumble::campaign_levels::displayed_mission_number(*record)
    );
    write_u32(
        rdram,
        live_object + kFrontendCurrentLevelOffset,
        selected_level
    );
    g_campaign_selector_index.store(selected_level, std::memory_order_release);
    g_campaign_grid_selected_slot.store(
        static_cast<uint32_t>(selected_slot),
        std::memory_order_release
    );
    g_campaign_grid_live_object.store(live_object, std::memory_order_release);
    g_campaign_grid_input_latched.store(false, std::memory_order_release);
    g_campaign_grid_back_hovered.store(false, std::memory_order_release);
    g_campaign_selection_committed_index.store(0u, std::memory_order_release);
    g_campaign_grid_active.store(true, std::memory_order_release);
    bumble::graphics_options::set_campaign_grid_pointer_active(true);
    std::fprintf(
        stderr,
        "BUMBLE_CAMPAIGN_GRID stage=initialized missions=%zu columns=2"
        " rows_per_column=%" PRIu32 " progress_level=%" PRIu32
        " selected_slot=%zu selected_level=%" PRIu32 "\n",
        bumble::campaign_levels::kMissionSelectLevelIndices.size(),
        bumble::campaign_levels::kMissionGridRows,
        progress_level,
        selected_slot,
        selected_level
    );
    std::fflush(stderr);
}

extern "C" void bumble_handle_campaign_level_grid_input(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr ||
        !g_campaign_grid_active.load(std::memory_order_acquire)) {
        return;
    }
    const uint32_t live_object = guest_u32(context->r4);
    if (live_object != g_campaign_grid_live_object.load(
            std::memory_order_acquire) ||
        !campaign_grid_identity(rdram, live_object)) {
        return;
    }
    if (read_u32(rdram, live_object + 0x40u) != 0u) {
        return;
    }

    const gpr pad = guest_address(kCurrentPadBase);
    const int16_t stick_x = MEM_H(4, pad);
    const int16_t stick_y = MEM_H(6, pad);
    const bool neutral = stick_x > -15 && stick_x < 15 &&
        stick_y > -15 && stick_y < 15;
    bool latched = g_campaign_grid_input_latched.load(std::memory_order_acquire);
    uint32_t slot = std::min<uint32_t>(
        g_campaign_grid_selected_slot.load(std::memory_order_acquire),
        static_cast<uint32_t>(
            bumble::campaign_levels::kMissionSelectLevelIndices.size() - 1u
        )
    );
    const uint32_t previous_slot = slot;
    bool pointer_click = false;
    uint32_t pointer_slot = slot;
    const bool pointer_selected =
        bumble::graphics_options::consume_campaign_grid_pointer(
            pointer_slot,
            pointer_click
        );
    const bool pointer_back = pointer_selected &&
        pointer_slot == bumble::campaign_levels::kMissionSelectLevelIndices.size();
    if (pointer_selected) {
        latched = false;
        g_campaign_grid_back_hovered.store(pointer_back, std::memory_order_release);
        if (!pointer_back) {
            slot = pointer_slot;
        }
    } else if (!latched) {
        uint32_t column = slot / bumble::campaign_levels::kMissionGridRows;
        uint32_t row = slot % bumble::campaign_levels::kMissionGridRows;
        if (std::abs(static_cast<int>(stick_x)) >=
                std::abs(static_cast<int>(stick_y)) &&
            (stick_x >= 41 || stick_x <= -41)) {
            column = column == 0u ? 1u : 0u;
            latched = true;
        } else if (stick_y >= 41) {
            row = row == 0u ? 0u : row - 1u;
            latched = true;
        } else if (stick_y <= -41) {
            ++row;
            latched = true;
        }
        row = std::min(row,
            bumble::campaign_levels::mission_grid_column_size(column) - 1u);
        slot = column * bumble::campaign_levels::kMissionGridRows + row;
        if (latched) {
            g_campaign_grid_back_hovered.store(false, std::memory_order_release);
        }
    } else if (neutral) {
        latched = false;
    }
    g_campaign_grid_input_latched.store(latched, std::memory_order_release);

    MEM_H(4, pad) = 0;
    MEM_H(6, pad) = 0;
    if (pointer_click && pointer_back) {
        MEM_H(2, pad) = static_cast<int16_t>(MEM_HU(2, pad) | kBButton);
    } else if (pointer_click) {
        MEM_H(2, pad) = static_cast<int16_t>(MEM_HU(2, pad) | kAButton);
    }

    const uint32_t selected_level =
        bumble::campaign_levels::kMissionSelectLevelIndices[slot];
    const auto* record = bumble::campaign_levels::find(selected_level);
    if (record == nullptr) {
        return;
    }
    if (slot != previous_slot) {
        const uint32_t previous_level =
            bumble::campaign_levels::kMissionSelectLevelIndices[previous_slot];
        write_u32(rdram, kMissionSelectorPreviousIndexAddress, previous_level);
        write_u32(rdram, kMissionSelectorIndexAddress, selected_level);
        write_u32(
            rdram,
            kMissionSelectorNumberAddress,
            bumble::campaign_levels::displayed_mission_number(*record)
        );
        write_u32(
            rdram,
            live_object + kFrontendCurrentLevelOffset,
            selected_level
        );
        g_campaign_grid_selected_slot.store(slot, std::memory_order_release);
        g_campaign_selector_index.store(selected_level, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_CAMPAIGN_GRID stage=selection_changed"
            " slot=%" PRIu32 " level=%" PRIu32 " unlocked=%d\n",
            slot,
            selected_level,
            campaign_level_unlocked(selected_level) ? 1 : 0
        );
        std::fflush(stderr);
    }

    const uint32_t progress_level = g_campaign_grid_progress_level.load(
        std::memory_order_acquire
    );
    const uint16_t pressed = MEM_HU(2, pad);
    if ((pressed & kBButton) != 0u) {
        MEM_H(2, pad) = static_cast<int16_t>(pressed & ~kBButton);
        write_u32(rdram, live_object + 0x84u, kMainMenuFrontendState);
        write_u32(rdram, live_object + 0x40u, 1u);
        osGetTime_recomp(rdram, context);
        write_u32(rdram, live_object + 0x68u, guest_u32(context->r2));
        write_u32(rdram, live_object + 0x6Cu, guest_u32(context->r3));
        bumble::graphics_options::request_play_menu_return();
        bumble::native_checkpoint::retire_campaign_grid();
        std::fprintf(
            stderr,
            "BUMBLE_CAMPAIGN_GRID stage=back_requested"
            " source=menu_back target=play_menu\n"
        );
        std::fflush(stderr);
        return;
    }
    if (!campaign_level_unlocked(selected_level)) {
        if ((pressed & kAButton) != 0u) {
            MEM_H(2, pad) = static_cast<int16_t>(pressed & ~kAButton);
            std::fprintf(
                stderr,
                "BUMBLE_CAMPAIGN_GRID stage=locked_level_rejected"
                " slot=%" PRIu32 " level=%" PRIu32
                " progress_level=%" PRIu32 "\n",
                slot,
                selected_level,
                progress_level
            );
            std::fflush(stderr);
        }
    }
}

extern "C" void bumble_commit_campaign_level_grid(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr ||
        !g_campaign_grid_active.load(std::memory_order_acquire)) {
        return;
    }
    const uint32_t selected_level = read_u32(
        rdram,
        kMissionSelectorIndexAddress
    );
    const uint16_t pressed = MEM_HU(2, guest_address(kCurrentPadBase));
    if ((pressed & kAButton) == 0u || guest_u32(context->r4) != selected_level ||
        !campaign_level_unlocked(selected_level)) {
        return;
    }
    g_campaign_selection_committed_index.store(
        selected_level,
        std::memory_order_release
    );
    bumble::native_checkpoint::retire_campaign_grid();
    std::fprintf(
        stderr,
        "BUMBLE_CAMPAIGN_GRID stage=selection_committed level=%" PRIu32
        " route=guest_original\n",
        selected_level
    );
    std::fflush(stderr);
}

extern "C" void buck_raw_pad_return_probe(uint8_t* rdram, recomp_context*) {
    for (uint32_t controller = 0; controller < 2u; ++controller) {
        const gpr pad = guest_address(kRawPadBase + controller * kRawPadStride);
        const uint16_t buttons = MEM_HU(0, pad);
        const int8_t stick_x = MEM_B(2, pad);
        const int8_t stick_y = MEM_B(3, pad);
        const uint32_t signature = pad_signature(buttons, stick_x, stick_y);
        const uint32_t previous = g_last_raw_signatures[controller].exchange(
            signature,
            std::memory_order_acq_rel
        );
        if (signature == previous) {
            continue;
        }

        const uint32_t count =
            g_raw_transition_count.fetch_add(1, std::memory_order_relaxed) + 1;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=game_raw_pad_transition count=%" PRIu32
            " pc=0x80051EF4 word=0x00008821 controller=%" PRIu32
            " buttons=0x%04X stick_x=%d stick_y=%d\n",
            count,
            controller,
            static_cast<unsigned>(buttons),
            static_cast<int>(stick_x),
            static_cast<int>(stick_y)
        );
        std::fflush(stderr);
    }
}

extern "C" void buck_controller_init_probe(uint8_t* rdram, recomp_context*) {
    const gpr pattern_address = guest_address(0x800CC838u);
    const gpr status_address = guest_address(0x800CC854u);
    const uint8_t pattern = MEM_BU(0, pattern_address);
    const uint16_t type = MEM_HU(0, status_address);
    const uint8_t status = MEM_BU(2, status_address);
    const uint8_t error = MEM_BU(3, status_address);
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_controller_init"
        " pc=0x80051E00 pattern=0x%02X controller0_type=0x%04X"
        " controller0_status=0x%02X controller0_error=0x%02X\n",
        static_cast<unsigned>(pattern),
        static_cast<unsigned>(type),
        static_cast<unsigned>(status),
        static_cast<unsigned>(error)
    );
    std::fflush(stderr);
}

extern "C" void buck_normalized_pad_probe(uint8_t* rdram, recomp_context*) {
    for (uint32_t controller = 0; controller < 2u; ++controller) {
        const gpr current = guest_address(kCurrentPadBase + controller * kCurrentPadStride);
        const NormalizedPad current_pad{
            MEM_HU(0, current),
            MEM_HU(2, current),
            MEM_H(4, current),
            MEM_H(6, current),
        };
        const NormalizedPad translated = read_normalized_pad(rdram, controller);
        const uint64_t signature = current_pad_transition_signature(current_pad);
        const uint64_t previous = g_last_normalized_signatures[controller].exchange(
            signature,
            std::memory_order_acq_rel
        );
        if (signature == previous && current_pad.pressed == 0) {
            continue;
        }

        const uint32_t count =
            g_normalized_edge_count.fetch_add(1, std::memory_order_relaxed) + 1;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=game_normalized_pad count=%" PRIu32
            " pc=0x800523B8 word=0x24040005 controller=%" PRIu32
            " held=0x%04X pressed=0x%04X stick_x=%d stick_y=%d"
            " translated_held=0x%04X translated_pressed=0x%04X"
            " translated_x=%d translated_y=%d\n",
            count,
            controller,
            static_cast<unsigned>(current_pad.held),
            static_cast<unsigned>(current_pad.pressed),
            static_cast<int>(current_pad.stick_x),
            static_cast<int>(current_pad.stick_y),
            static_cast<unsigned>(translated.held),
            static_cast<unsigned>(translated.pressed),
            static_cast<int>(translated.stick_x),
            static_cast<int>(translated.stick_y)
        );
        std::fflush(stderr);
    }
}

extern "C" void buck_frontend_confirm_probe(uint8_t* rdram, recomp_context* context) {
    const uint32_t live_object = guest_u32(context->r19);
    const uint32_t descriptor = guest_u32(context->r18);
    const uint32_t descriptor_item = valid_guest_pointer(descriptor, 0x68u)
        ? static_cast<uint32_t>(MEM_W(0x38, guest_address(descriptor)))
        : 0xFFFFFFFFu;
    g_last_frontend_descriptor.store(descriptor, std::memory_order_release);
    g_last_frontend_descriptor_item.store(descriptor_item, std::memory_order_release);
    const uint32_t phase = valid_guest_pointer(live_object, 0x88u)
        ? static_cast<uint32_t>(MEM_W(0x00, guest_address(live_object)))
        : 0xFFFFFFFFu;
    if (phase != kLevelSelectFrontendState &&
        g_campaign_grid_active.load(std::memory_order_acquire)) {
        bumble::native_checkpoint::retire_campaign_grid();
    }
    const uint32_t previous_phase =
        g_last_frontend_phase.exchange(phase, std::memory_order_acq_rel);
    if (phase != previous_phase) {
        const uint32_t transition =
            g_frontend_phase_transition_count.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint16_t pressed = MEM_HU(2, guest_address(kCurrentPadBase));
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=game_frontend_phase_transition count=%" PRIu32
            " pc=0x800AB72C previous=0x%08" PRIX32 " phase=0x%08" PRIX32
            " live_object=0x%08" PRIX32 " descriptor=0x%08" PRIX32
            " pressed=0x%04X\n",
            transition,
            previous_phase,
            phase,
            live_object,
            descriptor,
            static_cast<unsigned>(pressed)
        );
        std::fflush(stderr);
    }

    const uint16_t accepted_mask = static_cast<uint16_t>(guest_u32(context->r2));
    if ((accepted_mask & 0x8000u) == 0) {
        return;
    }

    const gpr current = guest_address(kCurrentPadBase);
    const uint16_t held = MEM_HU(0, current);
    const uint16_t pressed = MEM_HU(2, current);
    const int8_t stick_x = MEM_B(4, current);
    const int8_t stick_y = MEM_B(6, current);
    const uint32_t object_40 = valid_guest_pointer(live_object, 0x88u)
        ? static_cast<uint32_t>(MEM_W(0x40, guest_address(live_object)))
        : 0xFFFFFFFFu;
    const uint32_t object_68 = valid_guest_pointer(live_object, 0x88u)
        ? static_cast<uint32_t>(MEM_W(0x68, guest_address(live_object)))
        : 0xFFFFFFFFu;
    const uint32_t object_6c = valid_guest_pointer(live_object, 0x88u)
        ? static_cast<uint32_t>(MEM_W(0x6C, guest_address(live_object)))
        : 0xFFFFFFFFu;
    const uint32_t descriptor_flags = valid_guest_pointer(descriptor, 0x68u)
        ? static_cast<uint32_t>(MEM_W(0x1C, guest_address(descriptor)))
        : 0xFFFFFFFFu;
    const int16_t value_60 = valid_guest_pointer(descriptor, 0x68u)
        ? MEM_H(0x60, guest_address(descriptor))
        : static_cast<int16_t>(-1);
    const int16_t value_62 = valid_guest_pointer(descriptor, 0x68u)
        ? MEM_H(0x62, guest_address(descriptor))
        : static_cast<int16_t>(-1);
    const int16_t value_64 = valid_guest_pointer(descriptor, 0x68u)
        ? MEM_H(0x64, guest_address(descriptor))
        : static_cast<int16_t>(-1);

    g_last_accepted_frontend_phase.store(phase, std::memory_order_relaxed);
    const uint32_t count = g_frontend_confirm_count.fetch_add(1, std::memory_order_release) + 1;
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_frontend_a_accepted count=%" PRIu32
        " pc=0x800AB72C word=0x104000AE mask=0x%04X held=0x%04X pressed=0x%04X"
        " stick_x=%d stick_y=%d live_object=0x%08" PRIX32
        " descriptor=0x%08" PRIX32 " phase=0x%08" PRIX32
        " object_40=0x%08" PRIX32 " object_68=0x%08" PRIX32
        " object_6c=0x%08" PRIX32 " descriptor_flags=0x%08" PRIX32
        " descriptor_item=0x%08" PRIX32 " value_60=%d value_62=%d value_64=%d\n",
        count,
        static_cast<unsigned>(accepted_mask),
        static_cast<unsigned>(held),
        static_cast<unsigned>(pressed),
        static_cast<int>(stick_x),
        static_cast<int>(stick_y),
        live_object,
        descriptor,
        phase,
        object_40,
        object_68,
        object_6c,
        descriptor_flags,
        descriptor_item,
        static_cast<int>(value_60),
        static_cast<int>(value_62),
        static_cast<int>(value_64)
    );
    std::fflush(stderr);
}

extern "C" void buck_mission1_installer_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!g_mission1_completion_replay_enabled.load(
            std::memory_order_acquire)) {
        return;
    }
    const uint32_t record = guest_u32(context->r5);
    const uint32_t callback_base = guest_u32(context->r3);
    const uint32_t actor_owner = guest_u32(context->r2);
    const uint32_t callback_relocation = read_u32(
        rdram,
        kMissionOneCallbackRelocationAddress
    );
    const uint32_t effective_callback = callback_base +
        static_cast<uint32_t>(callback_relocation << 2u);
    const uint32_t candidate_actor = valid_guest_pointer(actor_owner, 0xA4u)
        ? read_u32(rdram, actor_owner + 0xA0u)
        : 0u;
    const uint32_t candidate_count =
        g_mission1_installer_candidate_count.fetch_add(
            1u,
            std::memory_order_acq_rel
        ) + 1u;
    if (candidate_count <= 256u && valid_guest_pointer(record, 0x34u)) {
        const bool actor_valid = valid_guest_pointer(candidate_actor, 0xA8u);
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=game_mission1_installer_candidate"
            " count=%" PRIu32 " pc=0x800922C8 record=0x%08" PRIX32
            " selector=0x%08" PRIX32 " flags20=0x%08" PRIX32
            " flags24=0x%08" PRIX32 " argument30=0x%08" PRIX32
            " actor=0x%08" PRIX32 " vtable=0x%08" PRIX32
            " actor_xyz=%.3f,%.3f,%.3f callback_base=0x%08" PRIX32
            " effective_callback=0x%08" PRIX32 " observer_only=1\n",
            candidate_count,
            record,
            read_u32(rdram, record + 0x28u),
            read_u32(rdram, record + 0x20u),
            read_u32(rdram, record + 0x24u),
            read_u32(rdram, record + 0x30u),
            candidate_actor,
            actor_valid ? read_u32(rdram, candidate_actor + 0x88u) : 0u,
            actor_valid
                ? static_cast<double>(float_from_word(
                    read_u32(rdram, candidate_actor + 0x40u)))
                : 0.0,
            actor_valid
                ? static_cast<double>(float_from_word(
                    read_u32(rdram, candidate_actor + 0x44u)))
                : 0.0,
            actor_valid
                ? static_cast<double>(float_from_word(
                    read_u32(rdram, candidate_actor + 0x48u)))
                : 0.0,
            callback_base,
            effective_callback
        );
        std::fflush(stderr);
    }
    const bool mission_one_live =
        read_u32(rdram, kCurrentLevelIndexAddress) == kMissionOneLevelIndex &&
        read_u32(rdram, kMissionOneLevelRecord) == kMissionOneIdWord;
    if (mission_one_live && valid_guest_pointer(record, 0x34u) &&
        valid_guest_pointer(candidate_actor, 0x8Cu)) {
        const uint32_t selector = read_u32(rdram, record + 0x28u);
        const uint32_t vtable = read_u32(rdram, candidate_actor + 0x88u);
        const uint8_t subtype = static_cast<uint8_t>(
            read_u32(rdram, record + 0x24u) & 0x1Fu
        );
        if (record == kMissionOneWaveTriggerRecord &&
            selector == kMissionOneWaveTriggerSelector &&
            read_u32(rdram, record + 0x20u) == 0x02u &&
            vtable == kMissionOneWaveTriggerVtable &&
            effective_callback == kMissionOneWaveTriggerCallback) {
            std::scoped_lock lock(g_mission1_mutex);
            if (g_mission1_wave_trigger.actor == 0u) {
                g_mission1_wave_trigger.actor = candidate_actor;
                g_mission1_wave_trigger.record = record;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=game_mission1_wave_trigger_registered"
                    " record=0x%08" PRIX32 " actor=0x%08" PRIX32
                    " selector=0x%08" PRIX32 " flags20=0x00000002"
                    " flags24=0x%08" PRIX32 " vtable=0x%08" PRIX32
                    " state=%" PRIu32 " callback=0x%08" PRIX32
                    " actor_xyz=%.3f,%.3f,%.3f"
                    " activation_condition=collision_list_count_one"
                    " guest_mutation=0\n",
                    record,
                    candidate_actor,
                    selector,
                    read_u32(rdram, record + 0x24u),
                    vtable,
                    read_u32(rdram, candidate_actor + 0x8Cu),
                    effective_callback,
                    static_cast<double>(float_from_word(
                        read_u32(rdram, candidate_actor + 0x40u))),
                    static_cast<double>(float_from_word(
                        read_u32(rdram, candidate_actor + 0x44u))),
                    static_cast<double>(float_from_word(
                        read_u32(rdram, candidate_actor + 0x48u)))
                );
                std::fflush(stderr);
            }
        } else if (record == kMissionOneFactoryTriggerRecord &&
            selector == kMissionOneFlyingEnemySelector &&
            read_u32(rdram, record + 0x20u) == 0x82u &&
            vtable == kMissionOneFactoryTriggerVtable &&
            effective_callback == kMissionOneTerminalCallback) {
            std::scoped_lock lock(g_mission1_mutex);
            if (g_mission1_factory_trigger.actor == 0u) {
                g_mission1_factory_trigger.actor = candidate_actor;
                g_mission1_factory_trigger.record = record;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=game_mission1_factory_trigger_registered"
                    " record=0x%08" PRIX32 " actor=0x%08" PRIX32
                    " selector=0x%08" PRIX32 " flags20=0x00000082"
                    " vtable=0x%08" PRIX32 " state=%" PRIu32
                    " spawn_count=%" PRIu32
                    " actor_xyz=%.3f,%.3f,%.3f"
                    " collision_callback=0x%08" PRIX32
                    " guest_mutation=0\n",
                    record,
                    candidate_actor,
                    selector,
                    vtable,
                    read_u32(rdram, candidate_actor + 0x8Cu),
                    read_u32(rdram, candidate_actor + 0x98u),
                    static_cast<double>(float_from_word(
                        read_u32(rdram, candidate_actor + 0x40u))),
                    static_cast<double>(float_from_word(
                        read_u32(rdram, candidate_actor + 0x44u))),
                    static_cast<double>(float_from_word(
                        read_u32(rdram, candidate_actor + 0x48u))),
                    kMissionOneFactoryTriggerCollisionCallback
                );
                std::fflush(stderr);
            }
        } else if (selector == kMissionOneWeaponPickupSelector &&
            (subtype == kPlasmaWeapon || subtype == kGrenadeWeapon)) {
            std::scoped_lock lock(g_mission1_mutex);
            register_mission1_pickup_locked(
                rdram,
                candidate_actor,
                record,
                subtype
            );
        } else if (selector == kMissionOneWeaponPickupSelector &&
                   (subtype == kSmallHealthPickup ||
                    subtype == kFullHealthPickup)) {
            std::scoped_lock lock(g_mission1_mutex);
            register_mission1_health_pickup_locked(
                rdram,
                candidate_actor,
                record,
                subtype
            );
        }
    }
    if (record == kMissionOneInstallerRecords.front() &&
        !g_mission1_special_installer_candidate_logged.exchange(
            true,
            std::memory_order_acq_rel)) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=game_mission1_special_installer_candidate"
            " pc=0x800922C8 record=0x%08" PRIX32
            " callback_base=0x%08" PRIX32
            " relocation_word=0x%08" PRIX32
            " effective_callback=0x%08" PRIX32
            " actor_owner=0x%08" PRIX32 " actor=0x%08" PRIX32
            " observer_only=1\n",
            record,
            callback_base,
            callback_relocation,
            effective_callback,
            actor_owner,
            candidate_actor
        );
        std::fflush(stderr);
    }
    bool exact_record = false;
    for (uint32_t expected : kMissionOneInstallerRecords) {
        exact_record = exact_record || record == expected;
    }
    if (!exact_record || !valid_guest_pointer(record, 0x34u) ||
        !valid_guest_pointer(actor_owner, 0xA4u) ||
        read_u32(rdram, record + 0x18u) != callback_base ||
        effective_callback != kMissionOneTerminalCallback) {
        return;
    }
    const uint32_t actor = read_u32(rdram, actor_owner + 0xA0u);
    const bool actor_valid = valid_guest_pointer(actor, 0x74u);

    bool inserted = false;
    uint32_t count = 0;
    const uint32_t selector = read_u32(rdram, record + 0x28u);
    {
        std::scoped_lock lock(g_mission1_mutex);
        bool present = false;
        for (uint32_t existing : g_mission1_installer_records) {
            present = present || existing == record;
        }
        if (!present) {
            for (uint32_t& destination : g_mission1_installer_records) {
                if (destination == 0u) {
                    destination = record;
                    count = g_mission1_installer_count.fetch_add(
                        1,
                        std::memory_order_acq_rel
                    ) + 1u;
                    inserted = true;
                    break;
                }
            }
        }
        const bool direct_terminal_owner = selector == 0x400u ||
            selector == 0x2000u ||
            (selector == kMissionOneFlyingEnemySelector &&
             record != kMissionOneFactoryTriggerRecord);
        if (actor_valid && direct_terminal_owner) {
            register_mission1_target_locked(actor, 0x800922C8u, true);
        }
    }
    if (inserted) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=game_mission1_callback_installer_observed"
            " count=%" PRIu32 " expected=11 pc=0x800922C8"
            " record=0x%08" PRIX32 " actor=0x%08" PRIX32
            " selector=0x%08" PRIX32
            " callback_base=0x%08" PRIX32
            " relocation_word=0x%08" PRIX32
            " effective_callback=0x8034B0D8 guest_mutation=0\n",
            count,
            record,
            actor,
            selector,
            callback_base,
            callback_relocation
        );
        std::fflush(stderr);
    }
}

extern "C" void buck_mission1_child_target_probe(
    uint8_t* rdram,
    recomp_context*,
    uint32_t source_pc,
    uint32_t actor
) {
    if (!g_mission1_completion_replay_enabled.load(
            std::memory_order_acquire)) {
        return;
    }
    const uint32_t callback = valid_guest_pointer(actor, 0x74u)
        ? read_u32(rdram, actor + 0x70u)
        : 0u;
    const uint32_t candidate_count =
        g_mission1_child_probe_candidate_count.fetch_add(
            1,
            std::memory_order_acq_rel
        ) + 1u;
    if (candidate_count <= 32u) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=game_mission1_child_copy_candidate"
            " count=%" PRIu32 " source_pc=0x%08" PRIX32
            " actor=0x%08" PRIX32 " callback=0x%08" PRIX32
            " expected_callback=0x8034B0D8 observer_only=1\n",
            candidate_count,
            source_pc,
            actor,
            callback
        );
        std::fflush(stderr);
    }
    if (!valid_guest_pointer(actor, 0x74u) ||
        callback != kMissionOneTerminalCallback) {
        return;
    }
    std::scoped_lock lock(g_mission1_mutex);
    register_mission1_target_locked(actor, source_pc, true);
}

extern "C" void buck_mission1_terminal_caller_probe(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t caller_pc
) {
    g_mission1_pending_terminal_caller_pc = 0u;
    if (!g_mission1_completion_replay_enabled.load(
            std::memory_order_acquire) ||
        context == nullptr || !mission1_live(rdram) ||
        !mission1_terminal_caller(caller_pc)) {
        return;
    }

    const uint32_t actor = guest_u32(context->r4);
    const uint32_t callback = guest_u32(context->r5);
    if (!valid_guest_pointer(actor, 0x74u) ||
        callback != kMissionOneTerminalCallback ||
        read_u32(rdram, actor + 0x70u) != callback) {
        return;
    }
    g_mission1_pending_terminal_caller_pc = caller_pc;
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_mission1_terminal_dispatch_requested"
        " caller_pc=0x%08" PRIX32 " actor=0x%08" PRIX32
        " callback=0x%08" PRIX32 " counter=%" PRIu32
        " invocation_depth=%llu guest_mutation=0\n",
        caller_pc,
        actor,
        callback,
        read_u32(rdram, kMissionOneCounterAddress),
        static_cast<unsigned long long>(g_mission1_terminal_invocation_depth)
    );
    std::fflush(stderr);
}

extern "C" void buck_mission1_damage_event_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!g_mission1_completion_replay_enabled.load(
            std::memory_order_acquire) ||
        context == nullptr || !mission1_live(rdram)) {
        return;
    }
    const uint32_t actor = guest_u32(context->r4);
    const uint32_t event = guest_u32(context->r5);
    bool registered = false;
    {
        std::scoped_lock lock(g_mission1_mutex);
        for (const MissionOneTarget& target : g_mission1_targets) {
            registered = registered || target.actor == actor;
        }
    }
    if (!registered || !valid_guest_pointer(actor, 0xA8u) ||
        !valid_guest_pointer(event, 0x10u)) {
        return;
    }

    const uint32_t count = g_mission1_damage_event_count.fetch_add(
        1u,
        std::memory_order_acq_rel
    ) + 1u;
    if (count <= 128u || count % 120u == 0u) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=game_mission1_damage_event"
            " count=%" PRIu32 " pc=0x800990E0 actor=0x%08" PRIX32
            " event=0x%08" PRIX32 " event_type=0x%08" PRIX32
            " event_word0=0x%08" PRIX32 " event_word8=0x%08" PRIX32
            " state=%" PRIu32 " health=%d variant=%" PRIu32
            " observer_only=1\n",
            count,
            actor,
            event,
            read_u32(rdram, event + 4u),
            read_u32(rdram, event),
            read_u32(rdram, event + 8u),
            read_u32(rdram, actor + 0x8Cu),
            static_cast<int>(MEM_H(0, guest_address(actor + 0x7Cu))),
            read_u32(rdram, actor + 0xA4u)
        );
        std::fflush(stderr);
    }
}

extern "C" void buck_mission1_terminal_entry_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    if (g_mission1_terminal_invocation_depth >=
        g_mission1_terminal_invocation_stack.size()) {
        ++g_mission1_terminal_invocation_overflow;
        g_mission1_pending_terminal_caller_pc = 0u;
        return;
    }
    MissionOneTerminalInvocation& invocation =
        g_mission1_terminal_invocation_stack[
            g_mission1_terminal_invocation_depth++
        ];
    invocation = {};
    if (context == nullptr) {
        g_mission1_pending_terminal_caller_pc = 0u;
        return;
    }
    const uint32_t actor = guest_u32(context->r4);
    const uint32_t callback = guest_u32(context->r5);
    const uint32_t caller_pc = g_mission1_pending_terminal_caller_pc;
    g_mission1_pending_terminal_caller_pc = 0u;
    if (mission1_live(rdram) && callback == kMissionOneTerminalCallback) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=game_mission1_terminal_dispatch_entered"
            " caller_pc=0x%08" PRIX32 " actor=0x%08" PRIX32
            " callback=0x%08" PRIX32 " counter=%" PRIu32
            " invocation_depth=%llu caller_valid=%d guest_mutation=0\n",
            caller_pc,
            actor,
            callback,
            read_u32(rdram, kMissionOneCounterAddress),
            static_cast<unsigned long long>(g_mission1_terminal_invocation_depth),
            mission1_terminal_caller(caller_pc) ? 1 : 0
        );
        std::fflush(stderr);
    }
    if (!mission1_live(rdram) || callback != kMissionOneTerminalCallback ||
        !mission1_terminal_caller(caller_pc) ||
        read_u32(rdram, callback) != 0x80096064u ||
        read_u32(rdram, callback + 4u) != kMissionOneTerminalRawOperand) {
        return;
    }
    invocation = {
        true,
        actor,
        caller_pc,
        read_u32(rdram, kMissionOneCounterAddress),
    };
}

extern "C" void buck_mission1_terminal_exit_probe(
    uint8_t*,
    recomp_context*
) {
    if (g_mission1_terminal_invocation_overflow > 0u) {
        --g_mission1_terminal_invocation_overflow;
    } else if (g_mission1_terminal_invocation_depth > 0u) {
        --g_mission1_terminal_invocation_depth;
        g_mission1_terminal_invocation_stack[
            g_mission1_terminal_invocation_depth
        ] = {};
    }
    g_mission1_pending_terminal_caller_pc = 0u;
}

extern "C" void buck_mission1_decrement_store_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t target = guest_u32(context->r2);
    const uint32_t new_value = guest_u32(context->r3);
    if (target == kMissionOneDelayAddress && mission1_live(rdram) &&
        new_value <= 19u) {
        const uint32_t previous = new_value + 1u;
        g_mission1_delay_value.store(new_value, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=game_mission1_success_delay_decrement"
            " pc=0x80096090 effective_address=0x8034B008"
            " previous=%" PRIu32 " value=%" PRIu32
            " expected_initial=20 guest_mutation=game_owned\n",
            previous,
            new_value
        );
        std::fflush(stderr);
        return;
    }
    if (target != kMissionOneCounterAddress ||
        !mission1_live(rdram) ||
        read_u32(rdram, target) != new_value ||
        new_value >= kMissionOneTerminalOwnerCount) {
        return;
    }
    MissionOneTerminalInvocation* invocation =
        active_mission1_terminal_invocation();
    bool target_registered = false;
    bool target_already_terminal = false;
    uint32_t target_generation = 0u;
    if (invocation != nullptr) {
        std::scoped_lock lock(g_mission1_mutex);
        for (const MissionOneTarget& candidate : g_mission1_targets) {
            if (candidate.actor == invocation->actor) {
                target_registered = true;
                target_already_terminal = candidate.terminal_observed;
                target_generation = candidate.generation;
                break;
            }
        }
    }
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_mission1_counter_store_observed"
        " previous=%" PRIu32 " value=%" PRIu32
        " invocation_active=%d invocation_depth=%llu invocation_overflow=%llu"
        " caller_pc=0x%08" PRIX32 " actor=0x%08" PRIX32
        " invocation_counter_before=%" PRIu32 " counter_matches=%d"
        " target_registered=%d target_generation=%" PRIu32
        " target_already_terminal=%d guest_mutation=game_owned\n",
        new_value + 1u,
        new_value,
        invocation != nullptr ? 1 : 0,
        static_cast<unsigned long long>(g_mission1_terminal_invocation_depth),
        static_cast<unsigned long long>(g_mission1_terminal_invocation_overflow),
        invocation != nullptr ? invocation->caller_pc : 0u,
        invocation != nullptr ? invocation->actor : 0u,
        invocation != nullptr ? invocation->counter_before : 0u,
        invocation != nullptr && invocation->counter_before == new_value + 1u
            ? 1
            : 0,
        target_registered ? 1 : 0,
        target_generation,
        target_already_terminal ? 1 : 0
    );
    std::fflush(stderr);
    if (invocation == nullptr ||
        invocation->counter_before != new_value + 1u) {
        return;
    }

    bool unique_terminal = false;
    {
        std::scoped_lock lock(g_mission1_mutex);
        unique_terminal = mark_mission1_terminal_locked(
            invocation->actor
        );
        if (unique_terminal) {
            g_mission1_last_terminal_frame =
                g_mission1_driver_frame.load(std::memory_order_acquire);
        }
    }
    if (!unique_terminal) {
        return;
    }
    const uint32_t count = g_mission1_terminal_count.fetch_add(
        1,
        std::memory_order_acq_rel
    ) + 1u;
    g_mission1_counter_value.store(new_value, std::memory_order_release);
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_mission1_terminal_decrement_observed"
        " count=%" PRIu32 " expected=13 caller_pc=0x%08" PRIX32
        " actor=0x%08" PRIX32 " callback=0x8034B0D8"
        " raw_operand=0x00005DD0 effective_address=0x8034B010"
        " previous=%" PRIu32 " value=%" PRIu32
        " ordered=%d guest_mutation=game_owned\n",
        count,
        invocation->caller_pc,
        invocation->actor,
        invocation->counter_before,
        new_value,
        invocation->counter_before ==
                kMissionOneTerminalOwnerCount - count + 1u &&
            new_value == kMissionOneTerminalOwnerCount - count
            ? 1
            : 0
    );
    std::fflush(stderr);
}

extern "C" void buck_mission1_zero_gate_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!mission1_live(rdram) || guest_u32(context->r2) != 0u) {
        return;
    }
    const uint32_t interpreter = guest_u32(context->r4);
    if (!valid_guest_pointer(interpreter, 4u)) {
        return;
    }
    const uint32_t next = read_u32(rdram, interpreter);
    std::atomic_bool* destination = nullptr;
    const char* gate = nullptr;
    if (next == kMissionOneCounterGateNext &&
        g_mission1_counter_value.load(std::memory_order_acquire) == 0u) {
        destination = &g_mission1_counter_zero_gate_observed;
        gate = "terminal_counter";
    } else if (next == kMissionOneDelayGateNext &&
        g_mission1_delay_value.load(std::memory_order_acquire) == 0u) {
        destination = &g_mission1_delay_zero_gate_observed;
        gate = "success_delay";
    }
    if (destination == nullptr || destination->exchange(
            true,
            std::memory_order_acq_rel)) {
        return;
    }
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_mission1_zero_gate_observed"
        " pc=0x800965D8 gate=%s interpreter=0x%08" PRIX32
        " next_command=0x%08" PRIX32 " value=0 guest_mutation=0\n",
        gate,
        interpreter,
        next
    );
    std::fflush(stderr);
}

extern "C" void buck_mission1_success_callback_probe(
    uint8_t* rdram,
    recomp_context*
) {
    if (!mission1_live(rdram) ||
        !g_mission1_counter_zero_gate_observed.load(std::memory_order_acquire) ||
        !g_mission1_delay_zero_gate_observed.load(std::memory_order_acquire)) {
        return;
    }
    const uint32_t interpreter = read_u32(rdram, 0x800D735Cu);
    if (!valid_guest_pointer(interpreter, 4u)) {
        return;
    }
    const uint32_t next = read_u32(rdram, interpreter);
    if (next != 0x8034B038u || read_u32(rdram, next) != 0u ||
        g_mission1_success_callback_observed.exchange(
            true,
            std::memory_order_acq_rel)) {
        return;
    }
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_mission1_success_callback_observed"
        " pc=0x80095788 result=0 interpreter=0x%08" PRIX32
        " operand_address=0x8034B038 frontend_target=0x1A"
        " guest_mutation=game_owned\n",
        interpreter
    );
    std::fflush(stderr);
}

extern "C" void buck_mission1_success_frontend_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t object = guest_u32(context->r7);
    const uint32_t state = guest_u32(context->r2);
    if (!g_mission1_success_callback_observed.load(std::memory_order_acquire) ||
        object != kFrontendLiveObject || state != kMissionCompleteFrontendState ||
        read_u32(rdram, object + 0x84u) != state ||
        g_mission1_success_frontend_observed.exchange(
            true,
            std::memory_order_acq_rel)) {
        return;
    }
    g_mission1_buttons_requested.store(0u, std::memory_order_release);
    bumble::modern_controls::set_movement_input(0.0f, 0.0f);
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_mission1_complete_frontend_observed"
        " pc=0x800B1E60 live_object=0x800FFF80 state=0x0000001A"
        " descriptor=0x800FD648 localized_string=116"
        " guest_mutation=game_owned\n"
    );
    std::fflush(stderr);
}

extern "C" void buck_mission1_level_increment_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t object = guest_u32(context->r18);
    if (!g_mission1_success_frontend_observed.load(std::memory_order_acquire) ||
        object != kFrontendLiveObject ||
        read_u32(rdram, object + kFrontendCurrentLevelOffset) != 2u ||
        g_mission1_level_increment_observed.exchange(
            true,
            std::memory_order_acq_rel)) {
        return;
    }
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_mission1_level_increment_observed"
        " pc=0x800B2C6C live_object=0x800FFF80"
        " current_level_address=0x800FFF90 previous=1 value=2"
        " guest_mutation=game_owned\n"
    );
    std::fflush(stderr);
}

extern "C" void bumble_autosave_campaign_progress(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr) {
        return;
    }
    const uint32_t object = guest_u32(context->r18);
    if (object != kFrontendLiveObject ||
        read_u32(rdram, object + 0x84u) != kMissionCompleteFrontendState) {
        return;
    }
    bumble::graphics_options::record_campaign_progress(
        read_u32(rdram, object + kFrontendCurrentLevelOffset)
    );
}

extern "C" void buck_mission1_profile_save_entry_probe(
    uint8_t*,
    recomp_context* context
) {
    g_mission1_profile_save_active =
        g_mission1_level_increment_observed.load(std::memory_order_acquire);
    g_mission1_profile_save_slot = guest_u32(context->r4);
}

extern "C" void buck_mission1_profile_built_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!g_mission1_profile_save_active) {
        return;
    }
    const uint32_t buffer = guest_u32(context->r29) + 0x10u;
    const bool valid = mission1_profile_valid(rdram, buffer);
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_mission1_profile_built"
        " pc=0x800BB28C slot=%" PRIu32 " buffer=0x%08" PRIX32
        " level_byte=%u checksum=0x%02X checksum_valid=%d"
        " guest_mutation=game_owned\n",
        g_mission1_profile_save_slot,
        buffer,
        static_cast<unsigned>(MEM_BU(0x20, guest_address(buffer))),
        static_cast<unsigned>(MEM_BU(0x3F, guest_address(buffer))),
        valid ? 1 : 0
    );
    std::fflush(stderr);
}

extern "C" void buck_mission1_profile_save_result_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!g_mission1_profile_save_active) {
        return;
    }
    const uint32_t written = guest_u32(context->r29) + 0x10u;
    const uint32_t reread = guest_u32(context->r29) + 0x50u;
    const uint32_t result = guest_u32(context->r3);
    const bool valid = result == 0u &&
        mission1_profile_valid(rdram, written) &&
        mission1_profile_valid(rdram, reread) &&
        mission1_profile_buffers_equal(rdram, written, reread);
    if (valid) {
        g_mission1_profile_save_observed.store(true, std::memory_order_release);
    }
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_mission1_profile_save_result"
        " pc=0x800BB2F4 slot=%" PRIu32 " result=%" PRIu32
        " payload_level=2 write_read_equal=%d checksum_valid=%d"
        " pfs_offset=0x%08" PRIX32 " pfs_size=0x40"
        " guest_mutation=game_owned\n",
        g_mission1_profile_save_slot,
        result,
        mission1_profile_buffers_equal(rdram, written, reread) ? 1 : 0,
        mission1_profile_valid(rdram, written) &&
            mission1_profile_valid(rdram, reread) ? 1 : 0,
        g_mission1_profile_save_slot * 0x40u + 0xC0u
    );
    std::fflush(stderr);
    g_mission1_profile_save_active = false;
}

extern "C" void buck_mission1_profile_reopen_entry_probe(
    uint8_t*,
    recomp_context* context
) {
    g_mission1_profile_reopen_active = true;
    g_mission1_profile_reopen_slot = guest_u32(context->r4);
}

extern "C" void buck_mission1_profile_reopen_result_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!g_mission1_profile_reopen_active) {
        return;
    }
    const uint32_t buffer = guest_u32(context->r29) + 0x10u;
    const bool restored = mission1_profile_valid(rdram, buffer) &&
        read_u32(
            rdram,
            kFrontendLiveObject + kFrontendCurrentLevelOffset
        ) == 2u;
    if (restored) {
        g_mission1_profile_reopen_observed.store(true, std::memory_order_release);
        g_campaign_selector_index.store(2u, std::memory_order_release);
    }
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_mission1_profile_reopen_result"
        " pc=0x800BB370 slot=%" PRIu32 " payload_level=%u"
        " checksum_valid=%d restored_level=%" PRIu32
        " no_guest_write_in_observer=1 restored=%d\n",
        g_mission1_profile_reopen_slot,
        static_cast<unsigned>(MEM_BU(0x20, guest_address(buffer))),
        mission1_profile_valid(rdram, buffer) ? 1 : 0,
        read_u32(rdram, kFrontendLiveObject + kFrontendCurrentLevelOffset),
        restored ? 1 : 0
    );
    std::fflush(stderr);
    g_mission1_profile_reopen_active = false;
}

extern "C" void bumble_mission1_completion_player_update(
    uint8_t* rdram,
    recomp_context* context
) {
    apply_player_cheats(rdram, context);

    const bool completion_replay =
        g_mission1_completion_replay_enabled.load(std::memory_order_acquire);
    const bool widescreen_hud_validation =
        g_widescreen_hud_validation_enabled.load(std::memory_order_acquire);
    if ((!completion_replay && !widescreen_hud_validation) ||
        rdram == nullptr || context == nullptr) {
        return;
    }
    const uint32_t player = guest_u32(context->r4);
    if (player == 0u || player != read_u32(rdram, kPlayerOneOwnerSlot) ||
        !valid_guest_pointer(player, 0xF5u) ||
        read_u32(rdram, player + 0x88u) != kPlayerVtable ||
        MEM_BU(0xF4, guest_address(player)) != 0u) {
        return;
    }

    const uint8_t player_index = MEM_BU(0xF4, guest_address(player));
    const uint32_t weapon_state =
        kWeaponStateBase + player_index * kWeaponStateStride;
    const bool widescreen_hud_gameplay_ready =
        widescreen_hud_validation &&
        g_last_frontend_phase.load(std::memory_order_acquire) ==
            kGameplayFrontendPhase;
    if (widescreen_hud_gameplay_ready) {
        const uint64_t hud_frame = g_widescreen_hud_player_frame.fetch_add(
            1u,
            std::memory_order_acq_rel
        ) + 1u;
        if (!g_widescreen_hud_inventory_granted.load(
                std::memory_order_acquire)) {
            for (uint32_t weapon = 0u;
                 weapon < kWidescreenHudWeaponCount;
                 ++weapon) {
                write_u32(
                    rdram,
                    weapon_state + kWeaponAmmoOffset + weapon * 4u,
                    kWidescreenHudValidationAmmo
                );
            }
            uint32_t selected = read_u32(
                rdram,
                weapon_state + kSelectedWeaponOffset
            );
            if (selected >= kWidescreenHudWeaponCount) {
                selected = kStarterWeapon;
                write_u32(
                    rdram,
                    weapon_state + kSelectedWeaponOffset,
                    selected
                );
            }
            g_widescreen_hud_inventory_granted.store(
                true,
                std::memory_order_release
            );
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=widescreen_hud_inventory_granted"
                " player=0x%08" PRIX32 " player_index=%u"
                " weapon_slots=%" PRIu32 " ammo_each=%" PRIu32
                " inventory_mask=0x000007FF selected=%" PRIu32
                " gameplay_phase=0x%02" PRIX32
                " post_level_load=1 replay_only=1"
                " ordinary_save_mutated=0\n",
                player,
                static_cast<unsigned>(player_index),
                kWidescreenHudWeaponCount,
                kWidescreenHudValidationAmmo,
                selected,
                kGameplayFrontendPhase
            );
            std::fflush(stderr);
        }

        const uint32_t current_selected = read_u32(
            rdram,
            weapon_state + kSelectedWeaponOffset
        );
        const uint32_t completed_mask =
            g_widescreen_hud_capture_complete_mask.load(
                std::memory_order_acquire
            );
        if (hud_frame >= kWidescreenHudCycleStartFrame &&
            current_selected + 1u < kWidescreenHudWeaponCount &&
            (completed_mask & (1u << current_selected)) != 0u) {
            const uint32_t next_selected = current_selected + 1u;
            write_u32(
                rdram,
                weapon_state + kSelectedWeaponOffset,
                next_selected
            );
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=widescreen_hud_cycle_requested"
                " frame=%" PRIu64 " previous=%" PRIu32
                " selected=%" PRIu32
                " selection_path=replay_full_inventory_fixture"
                " carousel_owner=func_800A4B54"
                " ordinary_save_mutated=0 replay_only=1\n",
                hud_frame,
                current_selected,
                next_selected
            );
            std::fflush(stderr);
        }

        const uint32_t selected = read_u32(
            rdram,
            weapon_state + kSelectedWeaponOffset
        );
        const uint32_t previous_selected =
            g_widescreen_hud_last_selected.load(std::memory_order_acquire);
        if (hud_frame >= kWidescreenHudCycleStartFrame &&
            selected != previous_selected) {
            g_widescreen_hud_last_selected.store(
                selected,
                std::memory_order_release
            );
            if (selected < kWidescreenHudWeaponCount) {
                g_widescreen_hud_capture_request.store(
                    selected + 1u,
                    std::memory_order_release
                );
            }
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=widescreen_hud_weapon_changed"
                " frame=%" PRIu64 " previous=%" PRIu32
                " selected=%" PRIu32 " weapon_slots=%" PRIu32
                " ammo=%" PRIu32 " carousel_source=func_800A4B54\n",
                hud_frame,
                previous_selected,
                selected,
                kWidescreenHudWeaponCount,
                selected < kWidescreenHudWeaponCount
                    ? read_u32(
                        rdram,
                        weapon_state + kWeaponAmmoOffset + selected * 4u
                    )
                    : 0u
            );
            std::fflush(stderr);
        }
    }

    if (!completion_replay || !mission1_live(rdram) ||
        !bumble::modern_controls::gameplay_input_active()) {
        return;
    }

    const uint64_t frame = g_mission1_driver_frame.fetch_add(
        1,
        std::memory_order_acq_rel
    ) + 1u;
    const uint32_t player_state = read_u32(rdram, player + 0x8Cu);
    const uint32_t player_camera = read_u32(rdram, player + 0xF8u);
    const bool grounded = player_state >= 5u && player_state <= 8u;
    const bool takeoff_pulse = grounded && frame % 20u < 3u;
    bumble::modern_controls::set_takeoff_land_pressed(takeoff_pulse);
    const float player_health = float_from_word(
        read_u32(rdram, weapon_state + kPlayerHealthOffset)
    );
    const uint32_t plasma_ammo = read_u32(
        rdram,
        weapon_state + kWeaponAmmoOffset + kPlasmaWeapon * 4u
    );
    const uint32_t grenade_ammo = read_u32(
        rdram,
        weapon_state + kWeaponAmmoOffset + kGrenadeWeapon * 4u
    );
    const uint32_t selected_weapon = read_u32(
        rdram,
        weapon_state + kSelectedWeaponOffset
    );
    {
        std::scoped_lock lock(g_mission1_mutex);
        for (MissionOnePickup& pickup : g_mission1_pickups) {
            const uint32_t ammo = pickup.weapon == kPlasmaWeapon
                ? plasma_ammo
                : grenade_ammo;
            if (!pickup.consumed && pickup.actor != 0u &&
                pickup.actor == g_mission1_last_driver_target && ammo > 0u) {
                pickup.consumed = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=game_mission1_weapon_pickup_acquired"
                    " frame=%" PRIu64 " record=0x%08" PRIX32
                    " actor=0x%08" PRIX32 " weapon=%u ammo=%" PRIu32
                    " collision_path=func_80058500 guest_mutation=0\n",
                    frame,
                    pickup.record,
                    pickup.actor,
                    static_cast<unsigned>(pickup.weapon),
                    ammo
                );
                std::fflush(stderr);
            }
        }
        for (MissionOneHealthPickup& pickup : g_mission1_health_pickups) {
            if (!pickup.consumed && pickup.actor != 0u &&
                pickup.actor == g_mission1_last_driver_target &&
                std::isfinite(g_mission1_last_player_health) &&
                std::isfinite(player_health) &&
                player_health > g_mission1_last_player_health + 0.01f) {
                pickup.consumed = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=game_mission1_health_pickup_acquired"
                    " frame=%" PRIu64 " record=0x%08" PRIX32
                    " actor=0x%08" PRIX32 " subtype=%u"
                    " health_before=%.3f health_after=%.3f"
                    " collision_path=func_80058500 guest_mutation=0\n",
                    frame,
                    pickup.record,
                    pickup.actor,
                    static_cast<unsigned>(pickup.subtype),
                    static_cast<double>(g_mission1_last_player_health),
                    static_cast<double>(player_health)
                );
                std::fflush(stderr);
            }
        }
        g_mission1_last_player_health = player_health;
    }
    MissionOneDriverTarget target =
        choose_mission1_driver_target(
            rdram,
            player,
            frame,
            plasma_ammo,
            grenade_ammo,
            player_health
        );
    const bool trigger_target =
        target.wave_trigger || target.factory_trigger;
    if (target.actor != 0u && !target.waypoint) {
        const float horizontal_distance = std::sqrt(
            target.dx * target.dx + target.dz * target.dz
        );
        if (std::isfinite(horizontal_distance) &&
            horizontal_distance > 850.0f) {
            constexpr float kMissionOneCruiseAltitude = 900.0f;
            constexpr float kMissionOneCruiseEntryAltitude = 760.0f;
            target.cruise = true;
            target.altitude_staging =
                target.player_y < kMissionOneCruiseEntryAltitude;
            if (target.altitude_staging) {
                target.target_x = target.player_x;
                target.target_z = target.player_z;
                target.target_y = kMissionOneCruiseAltitude;
            } else {
                target.target_y = std::max(
                    target.target_y,
                    kMissionOneCruiseAltitude
                );
            }
            target.dx = target.target_x - target.player_x;
            target.dy = target.target_y - target.player_y;
            target.dz = target.target_z - target.player_z;
            target.distance = std::sqrt(
                target.dx * target.dx + target.dy * target.dy +
                target.dz * target.dz
            );
        }
    }
    if (target.actor == 0u) {
        g_mission1_buttons_requested.store(0u, std::memory_order_release);
        bumble::modern_controls::set_movement_input(0.0f, 0.0f);
        if (frame == 1u || frame % 300u == 0u) {
            uint32_t wave_actor = 0u;
            uint32_t wave_vtable = 0u;
            uint32_t wave_state = 0u;
            uint32_t factory_actor = 0u;
            uint32_t factory_vtable = 0u;
            uint32_t factory_state = 0u;
            {
                std::scoped_lock lock(g_mission1_mutex);
                wave_actor = g_mission1_wave_trigger.actor;
                if (valid_guest_pointer(wave_actor, 0x90u)) {
                    wave_vtable = read_u32(rdram, wave_actor + 0x88u);
                    wave_state = read_u32(rdram, wave_actor + 0x8Cu);
                }
                factory_actor = g_mission1_factory_trigger.actor;
                if (valid_guest_pointer(factory_actor, 0x90u)) {
                    factory_vtable = read_u32(rdram, factory_actor + 0x88u);
                    factory_state = read_u32(rdram, factory_actor + 0x8Cu);
                }
            }
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=mission1_completion_driver_wait"
                " frame=%" PRIu64 " installers=%" PRIu32
                " registered=%" PRIu32 " terminal=%" PRIu32
                " counter=%" PRIu32 " withheld_actor=0x%08" PRIX32
                " selected_weapon=%" PRIu32 " plasma_ammo=%" PRIu32
                " grenade_ammo=%" PRIu32 " player_health=%.3f"
                " player_state=%" PRIu32 " player_camera=0x%08" PRIX32
                " takeoff_pulse=%d wave_actor=0x%08" PRIX32
                " wave_vtable=0x%08" PRIX32
                " wave_state=%" PRIu32
                " factory_actor=0x%08" PRIX32
                " factory_vtable=0x%08" PRIX32
                " factory_state=%" PRIu32 "\n",
                frame,
                g_mission1_installer_count.load(std::memory_order_acquire),
                g_mission1_registered_owner_count.load(
                    std::memory_order_acquire
                ),
                g_mission1_terminal_count.load(std::memory_order_acquire),
                g_mission1_counter_value.load(std::memory_order_acquire),
                g_mission1_withheld_actor,
                selected_weapon,
                plasma_ammo,
                grenade_ammo,
                static_cast<double>(player_health),
                player_state,
                player_camera,
                takeoff_pulse ? 1 : 0,
                wave_actor,
                wave_vtable,
                wave_state,
                factory_actor,
                factory_vtable,
                factory_state
            );
            std::fflush(stderr);
        }
        return;
    }

    const bool target_changed =
        target.actor != g_mission1_last_driver_target;
    const int32_t target_health = target.health;
    const bool weapon_ready = target.pickup || target.waypoint || target.cruise ||
        (selected_weapon == target.desired_weapon &&
         (target.desired_weapon == kStarterWeapon ||
          (target.desired_weapon == kPlasmaWeapon && plasma_ammo > 0u) ||
          (target.desired_weapon == kGrenadeWeapon && grenade_ammo > 0u)));
    const float navigation_dx =
        target.player_x - g_mission1_navigation_anchor_x;
    const float navigation_dy =
        target.player_y - g_mission1_navigation_anchor_y;
    const float navigation_dz =
        target.player_z - g_mission1_navigation_anchor_z;
    const float navigation_displacement = std::sqrt(
        navigation_dx * navigation_dx +
        navigation_dy * navigation_dy +
        navigation_dz * navigation_dz
    );
    if (target_changed || !std::isfinite(navigation_displacement)) {
        g_mission1_navigation_progress_frame = frame;
        g_mission1_navigation_best_distance = target.distance;
        g_mission1_navigation_anchor_x = target.player_x;
        g_mission1_navigation_anchor_y = target.player_y;
        g_mission1_navigation_anchor_z = target.player_z;
    } else if (navigation_displacement >= 25.0f) {
        g_mission1_navigation_progress_frame = frame;
        g_mission1_navigation_best_distance = target.distance;
        g_mission1_navigation_anchor_x = target.player_x;
        g_mission1_navigation_anchor_y = target.player_y;
        g_mission1_navigation_anchor_z = target.player_z;
    }
    const uint64_t navigation_stall_limit = target.waypoint
        ? 90u
        : (target.pickup ? 900u
             : (target.distance > 1200.0f ? 45u : UINT64_MAX));
    if (navigation_stall_limit != UINT64_MAX &&
        frame - g_mission1_navigation_progress_frame >=
            navigation_stall_limit) {
        const bool southern_corridor_detour = !target.pickup &&
            !target.waypoint && !target.cruise && target.player_x >= 1000.0f &&
            target.player_x <= 1400.0f && target.player_z > -3700.0f &&
            target.target_x > 2000.0f;
        if (southern_corridor_detour) {
            {
                std::scoped_lock lock(g_mission1_mutex);
                g_mission1_detour = {
                    true,
                    target.actor,
                    2000.0f,
                    std::max(target.player_y + 160.0f, 520.0f),
                    -3900.0f,
                };
            }
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=mission1_completion_detour_armed"
                " frame=%" PRIu64 " actor=0x%08" PRIX32
                " blocked_player_xyz=%.3f,%.3f,%.3f"
                " target_xyz=%.3f,%.3f,%.3f"
                " waypoint_xyz=2000.000,%.3f,-3900.000"
                " reason=observed_southern_corridor input_only=1\n",
                frame,
                target.actor,
                static_cast<double>(target.player_x),
                static_cast<double>(target.player_y),
                static_cast<double>(target.player_z),
                static_cast<double>(target.target_x),
                static_cast<double>(target.target_y),
                static_cast<double>(target.target_z),
                static_cast<double>(std::max(
                    target.player_y + 160.0f,
                    520.0f
                ))
            );
            std::fflush(stderr);
            g_mission1_last_driver_target = 0u;
            g_mission1_navigation_progress_frame = frame;
            g_mission1_navigation_best_distance = INFINITY;
            g_mission1_navigation_anchor_x = NAN;
            g_mission1_navigation_anchor_y = NAN;
            g_mission1_navigation_anchor_z = NAN;
            g_mission1_buttons_requested.store(0u, std::memory_order_release);
            bumble::modern_controls::set_movement_input(0.0f, 0.0f);
            return;
        }
        constexpr uint64_t kRouteRetryDelayFrames = 1200u;
        {
            std::scoped_lock lock(g_mission1_mutex);
            if (target.pickup && target.health_pickup) {
                for (MissionOneHealthPickup& pickup :
                     g_mission1_health_pickups) {
                    if (pickup.actor == target.actor) {
                        pickup.retry_after_frame =
                            frame + kRouteRetryDelayFrames;
                        break;
                    }
                }
            } else if (target.pickup) {
                for (MissionOnePickup& pickup : g_mission1_pickups) {
                    if (pickup.actor == target.actor) {
                        pickup.retry_after_frame =
                            frame + kRouteRetryDelayFrames;
                        break;
                    }
                }
            } else {
                for (MissionOneWavePrerequisite& prerequisite :
                     g_mission1_wave_prerequisites) {
                    if (prerequisite.actor == target.actor) {
                        prerequisite.retry_after_frame =
                            frame + kRouteRetryDelayFrames;
                        break;
                    }
                }
                for (MissionOneTarget& candidate : g_mission1_targets) {
                    if (candidate.actor == target.actor) {
                        candidate.retry_after_frame =
                            frame + kRouteRetryDelayFrames;
                        break;
                    }
                }
            }
        }
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=mission1_completion_route_deferred"
            " frame=%" PRIu64 " actor=0x%08" PRIX32
            " target_kind=%s subtype=%u distance=%.3f"
            " no_progress_frames=%" PRIu64
            " retry_after_frame=%" PRIu64
            " reason=bounded_collision_route_retry input_only=1\n",
            frame,
            target.actor,
            target.waypoint
                ? "detour_waypoint"
                : (target.pickup
                ? (target.health_pickup ? "health_pickup" : "weapon_pickup")
                : (target.wave_prerequisite
                    ? "wave_prerequisite"
                    : (target.wave_trigger
                    ? "wave_activation_trigger"
                    : (target.factory_trigger
                        ? "factory_collision_trigger"
                        : "enemy")))),
            static_cast<unsigned>(target.pickup_subtype),
            static_cast<double>(target.distance),
            navigation_stall_limit,
            frame + kRouteRetryDelayFrames
        );
        std::fflush(stderr);
        g_mission1_last_driver_target = 0u;
        g_mission1_navigation_progress_frame = frame;
        g_mission1_navigation_best_distance = INFINITY;
        g_mission1_navigation_anchor_x = NAN;
        g_mission1_navigation_anchor_y = NAN;
        g_mission1_navigation_anchor_z = NAN;
        g_mission1_buttons_requested.store(0u, std::memory_order_release);
        bumble::modern_controls::set_movement_input(0.0f, 0.0f);
        return;
    }

    const bool durable_target = target_health >= 5;
    const float attack_engagement_distance = durable_target ? 240.0f : 600.0f;
    if (target.pickup || target.waypoint || target.cruise ||
        target_changed ||
        target.distance > attack_engagement_distance || !weapon_ready) {
        g_mission1_attack_start_frame = 0u;
        g_mission1_attack_last_health = target_health;
    } else if (g_mission1_attack_start_frame == 0u ||
               target_health < g_mission1_attack_last_health) {
        g_mission1_attack_start_frame = frame;
        g_mission1_attack_last_health = target_health;
    } else if (!trigger_target && !target.wave_prerequisite &&
               frame - g_mission1_attack_start_frame >= 360u) {
        constexpr uint64_t kRetryDelayFrames = 6000u;
        {
            std::scoped_lock lock(g_mission1_mutex);
            for (MissionOneWavePrerequisite& prerequisite :
                 g_mission1_wave_prerequisites) {
                if (prerequisite.actor == target.actor) {
                    prerequisite.retry_after_frame = frame + kRetryDelayFrames;
                    break;
                }
            }
            for (MissionOneTarget& candidate : g_mission1_targets) {
                if (candidate.actor == target.actor) {
                    candidate.retry_after_frame = frame + kRetryDelayFrames;
                    break;
                }
            }
        }
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=mission1_completion_target_deferred"
            " frame=%" PRIu64 " actor=0x%08" PRIX32
            " distance=%.3f health=%d no_damage_frames=360"
            " retry_after_frame=%" PRIu64
            " reason=bounded_no_damage_line_of_sight input_only=1\n",
            frame,
            target.actor,
            static_cast<double>(target.distance),
            target_health,
            frame + kRetryDelayFrames
        );
        std::fflush(stderr);
        g_mission1_last_driver_target = 0u;
        g_mission1_attack_start_frame = 0u;
        g_mission1_attack_last_health = INT32_MAX;
        g_mission1_buttons_requested.store(0u, std::memory_order_release);
        bumble::modern_controls::set_movement_input(0.0f, 0.0f);
        return;
    }

    constexpr float kRadiansToDegrees = 57.29577951308232f;
    const float horizontal = std::sqrt(
        target.dx * target.dx + target.dz * target.dz
    );
    const float base_yaw =
        std::atan2(target.dx, -target.dz) * kRadiansToDegrees;
    const float base_pitch =
        std::atan2(-target.dy, horizontal) * kRadiansToDegrees;
    constexpr std::array<float, 5> kYawSweep{-8.0f, -4.0f, 0.0f, 4.0f, 8.0f};
    constexpr std::array<float, 5> kPitchSweep{
        -10.0f, -5.0f, 0.0f, 5.0f, 10.0f,
    };
    const bool sweep_active = !target.pickup && !target.waypoint &&
        !trigger_target && !target.cruise && !durable_target &&
        weapon_ready &&
        target.distance <= 850.0f &&
        g_mission1_attack_start_frame != 0u &&
        frame - g_mission1_attack_start_frame >= 120u;
    const uint64_t sweep_step = frame / 6u;
    const float yaw_offset = sweep_active
        ? kYawSweep[sweep_step % kYawSweep.size()]
        : 0.0f;
    const float pitch_offset = sweep_active
        ? kPitchSweep[(sweep_step / kYawSweep.size()) % kPitchSweep.size()]
        : 0.0f;
    const float yaw = base_yaw + yaw_offset;
    const float pitch = base_pitch + pitch_offset;
    const bool aim_ready =
        bumble::modern_controls::set_automation_aim(pitch, yaw);
    const float forward = target.pickup || target.waypoint || target.cruise
        ? (target.distance > (target.waypoint
                ? 75.0f
                : 25.0f)
            ? 1.0f
            : 0.0f)
        : (durable_target
            ? (target.distance > 360.0f
                ? 1.0f
                : (target.distance < 260.0f ? -0.75f : 0.0f))
            : (target.distance > (trigger_target ? 75.0f : 500.0f)
                ? 1.0f
                : 0.0f));
    const float evasion_magnitude = target.waypoint
        ? 0.0f
        : (target.cruise
        ? (target.altitude_staging ? 0.0f : 0.70f)
        : (target.pickup
        ? (player_health < 65.0f ? 0.70f : 0.30f)
        : (trigger_target
            ? 0.0f
            : (durable_target
                ? (target.distance <= 600.0f ? 0.85f : 0.20f)
                : (target.distance <= 1200.0f ? 0.85f : 0.65f)))));
    const float strafe = (frame / 24u) % 2u == 0u
        ? evasion_magnitude
        : -evasion_magnitude;
    bumble::modern_controls::set_movement_input(forward, strafe);
    const bool cycle_weapon = !target.pickup && !target.waypoint &&
        !target.cruise &&
        !weapon_ready &&
        ((frame / 4u) % 2u == 0u);
    const bool grenade_fire_window = frame % 30u < 3u;
    const bool rapid_fire_window = frame % 12u < 3u;
    const float fire_distance = durable_target
        ? 520.0f
        : (target.desired_weapon == kGrenadeWeapon ? 700.0f : 950.0f);
    const bool fire = !target.pickup && !target.waypoint && !target.cruise &&
        weapon_ready &&
        aim_ready &&
        target.distance <= fire_distance &&
        (target.desired_weapon == kGrenadeWeapon
             ? grenade_fire_window
             : rapid_fire_window);
    uint16_t requested_buttons = 0u;
    if (cycle_weapon) {
        requested_buttons |= kRawCDownButton;
    } else if (fire) {
        requested_buttons |= kZButton;
    }
    g_mission1_buttons_requested.store(
        requested_buttons,
        std::memory_order_release
    );

    if (target_changed || frame % 120u == 0u) {
        g_mission1_last_driver_target = target.actor;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=mission1_completion_driver_control"
            " frame=%" PRIu64 " target_changed=%d actor=0x%08" PRIX32
            " target_kind=%s desired_weapon=%u selected_weapon=%" PRIu32
            " plasma_ammo=%" PRIu32 " grenade_ammo=%" PRIu32
            " distance=%.3f dy=%.3f yaw=%.3f pitch=%.3f"
            " yaw_offset=%.3f pitch_offset=%.3f sweep=%d"
            " state=%" PRIu32 " health=%d variant=%" PRIu32
            " player_health=%.3f player_state=%" PRIu32
            " player_camera=0x%08" PRIX32 " takeoff_pulse=%d"
            " cruise=%d altitude_staging=%d collision_center_aim=%d"
            " player_xyz=%.3f,%.3f,%.3f target_xyz=%.3f,%.3f,%.3f"
            " forward=%.3f strafe=%.3f buttons=0x%04X"
            " cycle_weapon=%d weapon_ready=%d fire=%d aim_ready=%d"
            " terminal=%" PRIu32 " counter=%" PRIu32
            " input_only=1 guest_counter_writes=0\n",
            frame,
            target_changed ? 1 : 0,
            target.actor,
            target.waypoint
                ? "detour_waypoint"
                : (target.pickup
                ? (target.health_pickup ? "health_pickup" : "weapon_pickup")
                : (target.wave_prerequisite
                    ? "wave_prerequisite"
                    : (target.wave_trigger
                    ? "wave_activation_trigger"
                    : (target.factory_trigger
                        ? "factory_collision_trigger"
                        : "enemy")))),
            static_cast<unsigned>(target.desired_weapon),
            selected_weapon,
            plasma_ammo,
            grenade_ammo,
            static_cast<double>(target.distance),
            static_cast<double>(target.dy),
            static_cast<double>(yaw),
            static_cast<double>(pitch),
            static_cast<double>(yaw_offset),
            static_cast<double>(pitch_offset),
            sweep_active ? 1 : 0,
            read_u32(rdram, target.actor + 0x8Cu),
            target_health,
            read_u32(rdram, target.actor + 0xA4u),
            static_cast<double>(player_health),
            player_state,
            player_camera,
            takeoff_pulse ? 1 : 0,
            target.cruise ? 1 : 0,
            target.altitude_staging ? 1 : 0,
            target.collision_center_aim ? 1 : 0,
            static_cast<double>(target.player_x),
            static_cast<double>(target.player_y),
            static_cast<double>(target.player_z),
            static_cast<double>(target.target_x),
            static_cast<double>(target.target_y),
            static_cast<double>(target.target_z),
            static_cast<double>(forward),
            static_cast<double>(strafe),
            static_cast<unsigned>(requested_buttons),
            cycle_weapon ? 1 : 0,
            weapon_ready ? 1 : 0,
            fire ? 1 : 0,
            aim_ready ? 1 : 0,
            g_mission1_terminal_count.load(std::memory_order_acquire),
            g_mission1_counter_value.load(std::memory_order_acquire)
        );
        std::fflush(stderr);
    }
}

extern "C" void buck_level_select_cheat_probe(uint8_t* rdram, recomp_context*) {
    const uint32_t guest_progress = read_u32(rdram, kLevelSelectCheatProgressAddress);
    const uint32_t sequence_flag = read_u32(rdram, kLevelSelectCheatFlagAddress);
    const uint32_t descriptor = read_u32(rdram, kFrontendDescriptorAddress);
    const uint8_t enabled = MEM_BU(0, guest_address(kLevelSelectEnabledAddress));
    const uint16_t held = MEM_HU(0, guest_address(kCurrentPadBase));
    const uint16_t pressed = MEM_HU(2, guest_address(kCurrentPadBase));
    const uint16_t input_word = static_cast<uint16_t>(
        (pressed & kDpadMask) | (held & kZButton)
    );
    const uint32_t host_progress =
        g_level_select_cheat_progress.load(std::memory_order_acquire);

    const uint32_t final_sequence_word = read_u32(
        rdram,
        kLevelSelectCheatSequenceAddress +
            static_cast<uint32_t>(kLevelSelectCheatSequence.size() - 1u) * 4u
    );
    const bool completion_identity = guest_progress == 0u && sequence_flag == 0u &&
        descriptor == kLevelSelectFrontendDescriptor && enabled == 1u;
    if (completion_identity) {
        if (host_progress == kLevelSelectCheatSequence.size() - 1u &&
            input_word == kLevelSelectCheatSequence.back() &&
            final_sequence_word == kLevelSelectCheatSequence.back()) {
            g_level_select_cheat_progress.store(
                static_cast<uint32_t>(kLevelSelectCheatSequence.size()),
                std::memory_order_relaxed
            );
            bool expected_complete = false;
            if (!g_level_select_cheat_complete.compare_exchange_strong(
                expected_complete,
                true,
                std::memory_order_acq_rel
            )) {
                return;
            }
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=game_level_select_cheat_complete"
                " pc=0x800B026C word=0x8FBF001C step=12"
                " progress_address=0x80107A58 progress=0"
                " sequence_flag_address=0x80107A64 sequence_flag=0"
                " sequence_table_address=0x800FE79C sequence_index=11"
                " sequence_word=0x%04" PRIX32
                " held=0x%04X pressed=0x%04X input_word=0x%04X"
                " descriptor_address=0x800FE5A8 descriptor=0x%08" PRIX32
                " enabled_address=0x800F598D enabled=%u\n",
                final_sequence_word,
                static_cast<unsigned>(held),
                static_cast<unsigned>(pressed),
                static_cast<unsigned>(input_word),
                descriptor,
                static_cast<unsigned>(enabled)
            );
            std::fflush(stderr);
        }
        return;
    }

    if (sequence_flag == 1u && guest_progress >= 1u &&
        guest_progress < kLevelSelectCheatSequence.size()) {
        const uint32_t sequence_index = guest_progress - 1u;
        const uint32_t expected_input_word = kLevelSelectCheatSequence[sequence_index];
        const uint32_t sequence_word = read_u32(
            rdram,
            kLevelSelectCheatSequenceAddress + sequence_index * 4u
        );
        uint32_t expected_host_progress = guest_progress - 1u;
        if (descriptor == kDefaultFrontendDescriptor && enabled == 0u &&
            input_word == expected_input_word && sequence_word == expected_input_word &&
            g_level_select_cheat_progress.compare_exchange_strong(
                expected_host_progress,
                guest_progress,
                std::memory_order_acq_rel
            )) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=game_level_select_cheat_progress"
                " pc=0x800B026C word=0x8FBF001C step=%" PRIu32
                " progress_address=0x80107A58 progress=%" PRIu32
                " sequence_flag_address=0x80107A64 sequence_flag=%" PRIu32
                " sequence_table_address=0x800FE79C sequence_index=%" PRIu32
                " sequence_word=0x%04" PRIX32
                " held=0x%04X pressed=0x%04X input_word=0x%04X"
                " descriptor_address=0x800FE5A8 descriptor=0x%08" PRIX32
                " enabled_address=0x800F598D enabled=%u\n",
                guest_progress,
                guest_progress,
                sequence_flag,
                sequence_index,
                sequence_word,
                static_cast<unsigned>(held),
                static_cast<unsigned>(pressed),
                static_cast<unsigned>(input_word),
                descriptor,
                static_cast<unsigned>(enabled)
            );
            std::fflush(stderr);
        }
        return;
    }

    if (host_progress > 0u &&
        host_progress < kLevelSelectCheatSequence.size() &&
        guest_progress == 0u && sequence_flag == 0u &&
        descriptor == kDefaultFrontendDescriptor && (pressed & kDpadMask) != 0u) {
        const uint32_t reset_from =
            g_level_select_cheat_progress.exchange(0u, std::memory_order_acq_rel);
        if (reset_from > 0u && reset_from < kLevelSelectCheatSequence.size()) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=game_level_select_cheat_reset"
                " pc=0x800B026C word=0x8FBF001C reset_from=%" PRIu32
                " progress_address=0x80107A58 progress=0"
                " sequence_flag_address=0x80107A64 sequence_flag=0"
                " held=0x%04X pressed=0x%04X input_word=0x%04X"
                " descriptor_address=0x800FE5A8 descriptor=0x%08" PRIX32
                " enabled_address=0x800F598D enabled=%u\n",
                reset_from,
                static_cast<unsigned>(held),
                static_cast<unsigned>(pressed),
                static_cast<unsigned>(input_word),
                descriptor,
                static_cast<unsigned>(enabled)
            );
            std::fflush(stderr);
        }
    }
}

extern "C" void buck_mission2_selector_init_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!g_level_select_cheat_complete.load(std::memory_order_acquire) ||
        g_mission2_selector_initialized.load(std::memory_order_acquire)) {
        return;
    }

    const uint32_t selection_index = read_u32(rdram, kMissionSelectorIndexAddress);
    const uint32_t previous_index = read_u32(
        rdram,
        kMissionSelectorPreviousIndexAddress
    );
    const uint32_t mission_number = read_u32(rdram, kMissionSelectorNumberAddress);
    const uint32_t descriptor = read_u32(rdram, kFrontendDescriptorAddress);
    const uint8_t enabled = MEM_BU(0, guest_address(kLevelSelectEnabledAddress));
    const uint32_t live_object = guest_u32(context->r18);
    const LevelRecordIdentity identity = read_level_record_identity(
        rdram,
        selection_index
    );
    const bumble::campaign_levels::Record* campaign_record =
        bumble::campaign_levels::find(selection_index);
    const bool generic_identity = campaign_record != nullptr &&
        campaign_record_matches(rdram, identity, *campaign_record);
    if (valid_guest_pointer(live_object, 0x88u) && generic_identity &&
        mission_number ==
            bumble::campaign_levels::displayed_mission_number(*campaign_record) &&
        previous_index == 0u &&
        descriptor == kLevelSelectFrontendDescriptor && enabled == 1u) {
        g_campaign_selector_index.store(
            selection_index,
            std::memory_order_release
        );
    }
    if (!valid_guest_pointer(live_object, 0x88u) || selection_index != 1u ||
        previous_index != 0u || mission_number != 1u ||
        descriptor != kLevelSelectFrontendDescriptor || enabled != 1u ||
        !level_record_matches(
            identity,
            kMissionOneLevelRecord,
            kMissionOneIdWord,
            kMissionOneLoadOffset
        )) {
        return;
    }

    bool expected = false;
    if (!g_mission2_selector_initialized.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel
        )) {
        return;
    }

    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_mission2_selector_initialized"
        " pc=0x800B0384 word=0x8E037A68 live_object=0x%08" PRIX32
        " selection_index_address=0x80107A68 selection_index=%" PRIu32
        " previous_index_address=0x80107A6C previous_index=%" PRIu32
        " mission_number_address=0x80107A70 mission_number=%" PRIu32
        " level_record=0x%08" PRIX32 " level_id_word=0x%08" PRIX32
        " level_load_offset=0x%08" PRIX32
        " descriptor_address=0x800FE5A8 descriptor=0x%08" PRIX32
        " enabled_address=0x800F598D enabled=%u\n",
        live_object,
        selection_index,
        previous_index,
        mission_number,
        identity.address,
        identity.id_word,
        identity.load_offset,
        descriptor,
        static_cast<unsigned>(enabled)
    );
    std::fflush(stderr);
}

extern "C" void buck_mission2_selector_select_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!g_mission2_selector_initialized.load(std::memory_order_acquire)) {
        return;
    }

    const uint32_t selection_index = read_u32(rdram, kMissionSelectorIndexAddress);
    const uint32_t previous_index = read_u32(
        rdram,
        kMissionSelectorPreviousIndexAddress
    );
    const uint32_t mission_number = read_u32(rdram, kMissionSelectorNumberAddress);
    const uint32_t descriptor = read_u32(rdram, kFrontendDescriptorAddress);
    const uint8_t enabled = MEM_BU(0, guest_address(kLevelSelectEnabledAddress));
    const uint32_t live_object = guest_u32(context->r20);
    const uint32_t object_10 = valid_guest_pointer(live_object, 0x88u)
        ? read_u32(rdram, live_object + 0x10u)
        : 0xFFFFFFFFu;
    const uint32_t object_40 = valid_guest_pointer(live_object, 0x88u)
        ? read_u32(rdram, live_object + 0x40u)
        : 0xFFFFFFFFu;
    const int16_t stick_y = MEM_H(6, guest_address(kCurrentPadBase));
    const LevelRecordIdentity identity = read_level_record_identity(
        rdram,
        selection_index
    );
    const bumble::campaign_levels::Record* campaign_record =
        bumble::campaign_levels::find(selection_index);
    const bool generic_identity = campaign_record != nullptr &&
        campaign_record_matches(rdram, identity, *campaign_record);
    if (valid_guest_pointer(live_object, 0x88u) && generic_identity &&
        mission_number ==
            bumble::campaign_levels::displayed_mission_number(*campaign_record) &&
        stick_y >= 0x29 &&
        descriptor == kLevelSelectFrontendDescriptor && enabled == 1u) {
        g_campaign_selector_index.store(
            selection_index,
            std::memory_order_release
        );
    }
    if (g_mission2_selected.load(std::memory_order_acquire)) {
        return;
    }
    if (!valid_guest_pointer(live_object, 0x88u) || selection_index != 2u ||
        previous_index != 1u || mission_number != 2u || object_10 != 1u ||
        object_40 != 0u || stick_y < 0x29 ||
        descriptor != kLevelSelectFrontendDescriptor || enabled != 1u ||
        !level_record_matches(
            identity,
            kMissionTwoLevelRecord,
            kMissionTwoIdWord,
            kMissionTwoLoadOffset
        )) {
        return;
    }

    bool expected = false;
    if (!g_mission2_selected.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel
        )) {
        return;
    }

    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_mission2_selected"
        " pc=0x800B04D8 word=0x3C028010 live_object=0x%08" PRIX32
        " object_10=%" PRIu32 " object_40=%" PRIu32 " stick_y=%d"
        " selection_index_address=0x80107A68 selection_index=%" PRIu32
        " previous_index_address=0x80107A6C previous_index=%" PRIu32
        " mission_number_address=0x80107A70 mission_number=%" PRIu32
        " level_record=0x%08" PRIX32 " level_id_word=0x%08" PRIX32
        " level_load_offset=0x%08" PRIX32
        " descriptor_address=0x800FE5A8 descriptor=0x%08" PRIX32
        " enabled_address=0x800F598D enabled=%u\n",
        live_object,
        object_10,
        object_40,
        static_cast<int>(stick_y),
        selection_index,
        previous_index,
        mission_number,
        identity.address,
        identity.id_word,
        identity.load_offset,
        descriptor,
        static_cast<unsigned>(enabled)
    );
    std::fflush(stderr);
}

extern "C" void buck_mission2_selection_commit_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!g_mission2_selector_initialized.load(std::memory_order_acquire)) {
        return;
    }

    const uint32_t selection_index = read_u32(rdram, kMissionSelectorIndexAddress);
    const uint32_t previous_index = read_u32(
        rdram,
        kMissionSelectorPreviousIndexAddress
    );
    const uint32_t mission_number = read_u32(rdram, kMissionSelectorNumberAddress);
    const uint32_t descriptor = read_u32(rdram, kFrontendDescriptorAddress);
    const uint8_t enabled = MEM_BU(0, guest_address(kLevelSelectEnabledAddress));
    const uint16_t held = MEM_HU(0, guest_address(kCurrentPadBase));
    const uint16_t pressed = MEM_HU(2, guest_address(kCurrentPadBase));
    const uint32_t live_object = guest_u32(context->r20);
    const uint32_t object_10 = valid_guest_pointer(live_object, 0x88u)
        ? read_u32(rdram, live_object + 0x10u)
        : 0xFFFFFFFFu;
    const uint32_t object_40 = valid_guest_pointer(live_object, 0x88u)
        ? read_u32(rdram, live_object + 0x40u)
        : 0xFFFFFFFFu;
    const uint32_t object_84 = valid_guest_pointer(live_object, 0x88u)
        ? read_u32(rdram, live_object + 0x84u)
        : 0xFFFFFFFFu;
    const LevelRecordIdentity identity = read_level_record_identity(
        rdram,
        selection_index
    );
    const bumble::campaign_levels::Record* campaign_record =
        bumble::campaign_levels::find(selection_index);
    const bool generic_identity = campaign_record != nullptr &&
        campaign_record_matches(rdram, identity, *campaign_record);
    if (valid_guest_pointer(live_object, 0x88u) && generic_identity &&
        mission_number ==
            bumble::campaign_levels::displayed_mission_number(*campaign_record) &&
        guest_u32(context->r4) == selection_index &&
        (pressed & 0x8000u) != 0u &&
        descriptor == kLevelSelectFrontendDescriptor && enabled == 1u) {
        g_campaign_selection_committed_index.store(
            selection_index,
            std::memory_order_release
        );
    }
    if (!g_mission2_selected.load(std::memory_order_acquire) ||
        g_mission2_selection_committed.load(std::memory_order_acquire)) {
        return;
    }
    if (!valid_guest_pointer(live_object, 0x88u) || selection_index != 2u ||
        previous_index != 1u || mission_number != 2u ||
        guest_u32(context->r4) != 2u ||
        object_10 != 2u || object_40 != 1u || object_84 != 0x2Bu ||
        (pressed & 0x8000u) == 0u ||
        descriptor != kLevelSelectFrontendDescriptor || enabled != 1u ||
        !level_record_matches(
            identity,
            kMissionTwoLevelRecord,
            kMissionTwoIdWord,
            kMissionTwoLoadOffset
        )) {
        return;
    }

    bool expected = false;
    if (!g_mission2_selection_committed.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel
        )) {
        return;
    }

    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_mission2_selection_committed"
        " pc=0x800B06B4 word=0x8FBF0024 live_object=0x%08" PRIX32
        " object_10=%" PRIu32 " object_40=%" PRIu32
        " object_84=0x%08" PRIX32 " a0=0x%08" PRIX32
        " held=0x%04X pressed=0x%04X"
        " selection_index_address=0x80107A68 selection_index=%" PRIu32
        " previous_index_address=0x80107A6C previous_index=%" PRIu32
        " mission_number_address=0x80107A70 mission_number=%" PRIu32
        " level_record=0x%08" PRIX32 " level_id_word=0x%08" PRIX32
        " level_load_offset=0x%08" PRIX32
        " descriptor_address=0x800FE5A8 descriptor=0x%08" PRIX32
        " enabled_address=0x800F598D enabled=%u\n",
        live_object,
        object_10,
        object_40,
        object_84,
        guest_u32(context->r4),
        static_cast<unsigned>(held),
        static_cast<unsigned>(pressed),
        selection_index,
        previous_index,
        mission_number,
        identity.address,
        identity.id_word,
        identity.load_offset,
        descriptor,
        static_cast<unsigned>(enabled)
    );
    std::fflush(stderr);
}

bool commit_mission_checkpoint(
    uint8_t* rdram,
    uint32_t actor,
    uint32_t level,
    MissionCheckpointKind kind,
    uint32_t source_teleport,
    uint32_t destination_teleport,
    uint32_t pair_id,
    const char* owner
) {
    if (rdram == nullptr) {
        return false;
    }
    uint8_t player_index = 0xFFu;
    const LevelRecordIdentity identity = read_level_record_identity(rdram, level);
    const bumble::campaign_levels::Record* campaign_record =
        bumble::campaign_levels::find(level);
    if (!settled_gameplay_is_live(rdram) ||
        read_u32(rdram, kPauseMenuVisibleAddress) != 0u ||
        !exact_player_actor(rdram, actor, player_index) ||
        player_index != 0u ||
        campaign_record == nullptr ||
        !campaign_record_matches(rdram, identity, *campaign_record)) {
        std::fprintf(
            stderr,
            "BUMBLE_MISSION_CHECKPOINT stage=commit_rejected"
            " kind=%s reason=gameplay_identity_changed"
            " level=%" PRIu32 " player=0x%08" PRIX32 "\n",
            mission_checkpoint_kind_name(kind),
            level,
            actor
        );
        std::fflush(stderr);
        return false;
    }

    MissionCheckpoint checkpoint{};
    checkpoint.rdram = rdram;
    checkpoint.actor = actor;
    checkpoint.level = level;
    checkpoint.kind = kind;
    checkpoint.source_teleport = source_teleport;
    checkpoint.destination_teleport = destination_teleport;
    checkpoint.pair_id = pair_id;
    for (size_t index = 0u;
         index < kMissionCheckpointPoseOffsets.size();
         ++index) {
        const uint32_t word = read_u32(
            rdram,
            actor + kMissionCheckpointPoseOffsets[index]
        );
        if (!std::isfinite(float_from_word(word))) {
            std::fprintf(
                stderr,
                "BUMBLE_MISSION_CHECKPOINT stage=commit_rejected"
                " kind=%s reason=non_finite_pose level=%" PRIu32
                " player=0x%08" PRIX32 "\n",
                mission_checkpoint_kind_name(kind),
                level,
                actor
            );
            std::fflush(stderr);
            return false;
        }
        checkpoint.pose_words[index] = word;
    }
    checkpoint.exit_speed_word = read_u32(rdram, actor + 0x5Cu);
    checkpoint.collision_category = read_u32(rdram, actor + 0x64u);
    if (!std::isfinite(float_from_word(checkpoint.exit_speed_word)) ||
        float_from_word(checkpoint.exit_speed_word) < 0.0f) {
        std::fprintf(
            stderr,
            "BUMBLE_MISSION_CHECKPOINT stage=commit_rejected"
            " kind=%s reason=invalid_speed level=%" PRIu32
            " player=0x%08" PRIX32 "\n",
            mission_checkpoint_kind_name(kind),
            level,
            actor
        );
        std::fflush(stderr);
        return false;
    }

    const uint32_t weapon_state = kWeaponStateBase;
    checkpoint.health_word = read_u32(
        rdram,
        weapon_state + kPlayerHealthOffset
    );
    const float health = float_from_word(checkpoint.health_word);
    if (!std::isfinite(health) || health <= 0.0f) {
        std::fprintf(
            stderr,
            "BUMBLE_MISSION_CHECKPOINT stage=commit_rejected"
            " kind=%s reason=invalid_health level=%" PRIu32
            " player=0x%08" PRIX32 "\n",
            mission_checkpoint_kind_name(kind),
            level,
            actor
        );
        std::fflush(stderr);
        return false;
    }
    for (uint32_t weapon = 0u;
         weapon < kWidescreenHudWeaponCount;
         ++weapon) {
        checkpoint.ammo_words[weapon] = read_u32(
            rdram,
            weapon_state + kWeaponAmmoOffset + weapon * sizeof(uint32_t)
        );
    }
    checkpoint.selected_weapon = read_u32(
        rdram,
        weapon_state + kSelectedWeaponOffset
    );
    if (checkpoint.selected_weapon >= kWidescreenHudWeaponCount) {
        std::fprintf(
            stderr,
            "BUMBLE_MISSION_CHECKPOINT stage=commit_rejected"
            " kind=%s reason=invalid_selected_weapon level=%" PRIu32
            " player=0x%08" PRIX32 " selected_weapon=%" PRIu32 "\n",
            mission_checkpoint_kind_name(kind),
            level,
            actor,
            checkpoint.selected_weapon
        );
        std::fflush(stderr);
        return false;
    }
    checkpoint.generation = g_mission_checkpoint_generation.fetch_add(
        1u,
        std::memory_order_acq_rel
    ) + 1u;
    checkpoint.valid = true;

    {
        std::scoped_lock lock(g_mission_checkpoint_mutex);
        g_pending_portal_checkpoint = PendingPortalMissionCheckpoint{};
        g_mission_checkpoint = checkpoint;
        g_mission_checkpoint_available.store(true, std::memory_order_release);
    }

    uint32_t weapon_mask = 0u;
    for (uint32_t weapon = 0u;
         weapon < kWidescreenHudWeaponCount;
         ++weapon) {
        if (checkpoint.ammo_words[weapon] != 0u) {
            weapon_mask |= 1u << weapon;
        }
    }
    std::fprintf(
        stderr,
        "BUMBLE_MISSION_CHECKPOINT stage=committed kind=%s"
        " generation=%" PRIu64 " level=%" PRIu32
        " player=0x%08" PRIX32 " source_teleport=0x%08" PRIX32
        " destination=0x%08" PRIX32 " pair_id=%" PRIu32
        " position=%.3f,%.3f,%.3f health=%.3f"
        " weapon_mask=0x%08" PRIX32 " selected_weapon=%" PRIu32
        " owner=%s\n",
        mission_checkpoint_kind_name(checkpoint.kind),
        checkpoint.generation,
        checkpoint.level,
        checkpoint.actor,
        checkpoint.source_teleport,
        checkpoint.destination_teleport,
        checkpoint.pair_id,
        static_cast<double>(float_from_word(checkpoint.pose_words[9u])),
        static_cast<double>(float_from_word(checkpoint.pose_words[10u])),
        static_cast<double>(float_from_word(checkpoint.pose_words[11u])),
        static_cast<double>(health),
        weapon_mask,
        checkpoint.selected_weapon,
        owner
    );
    std::fflush(stderr);
    return true;
}

void commit_pending_portal_mission_checkpoint(
    uint8_t* rdram,
    uint32_t actor,
    uint32_t level
) {
    PendingPortalMissionCheckpoint pending{};
    {
        std::scoped_lock lock(g_mission_checkpoint_mutex);
        if (!g_pending_portal_checkpoint.valid ||
            g_pending_portal_checkpoint.rdram != rdram ||
            g_pending_portal_checkpoint.actor != actor ||
            g_pending_portal_checkpoint.level != level) {
            return;
        }
        pending = g_pending_portal_checkpoint;
        g_pending_portal_checkpoint = PendingPortalMissionCheckpoint{};
    }

    if (!settled_gameplay_is_live(rdram) ||
        !exact_teleport_actor(rdram, pending.source_teleport) ||
        !exact_teleport_actor(rdram, pending.destination_teleport) ||
        pending.source_teleport == pending.destination_teleport ||
        read_u32(rdram, pending.source_teleport + 0x90u) != pending.pair_id ||
        read_u32(rdram, pending.destination_teleport + 0x90u) !=
            pending.pair_id) {
        std::fprintf(
            stderr,
            "BUMBLE_MISSION_CHECKPOINT stage=commit_rejected"
            " source=portal reason=settled_portal_identity_changed"
            " level=%" PRIu32 " player=0x%08" PRIX32
            " source_teleport=0x%08" PRIX32
            " destination=0x%08" PRIX32 "\n",
            level,
            actor,
            pending.source_teleport,
            pending.destination_teleport
        );
        std::fflush(stderr);
        return;
    }

    bumble::modern_controls::rebase_after_authored_portal_transfer(
        rdram,
        actor,
        pending.source_teleport,
        pending.destination_teleport,
        pending.pair_id
    );
    commit_mission_checkpoint(
        rdram,
        actor,
        level,
        MissionCheckpointKind::Portal,
        pending.source_teleport,
        pending.destination_teleport,
        pending.pair_id,
        "player_actor_update_after_authored_portal_tail"
    );
}

bool restore_mission_checkpoint(
    uint8_t* rdram,
    recomp_context* context,
    MissionCheckpointRestoreTrigger trigger
) {
    if (rdram == nullptr || context == nullptr) {
        return false;
    }

    const bool pause_visible =
        read_u32(rdram, kPauseMenuVisibleAddress) != 0u;
    const bool expected_pause =
        trigger == MissionCheckpointRestoreTrigger::PauseMenu;
    if (!settled_gameplay_is_live(rdram) ||
        pause_visible != expected_pause) {
        std::fprintf(
            stderr,
            "BUMBLE_MISSION_RESTART stage=checkpoint_restore_rejected"
            " reason=invalid_gameplay_owner trigger=%s pause_visible=%d\n",
            mission_checkpoint_restore_trigger_name(trigger),
            pause_visible ? 1 : 0
        );
        std::fflush(stderr);
        return false;
    }

    MissionCheckpoint checkpoint{};
    {
        std::scoped_lock lock(g_mission_checkpoint_mutex);
        checkpoint = g_mission_checkpoint;
    }
    uint8_t player_index = 0xFFu;
    const uint32_t current_actor = read_u32(rdram, kPlayerOneOwnerSlot);
    const uint32_t current_level = read_u32(rdram, kCurrentLevelIndexAddress);
    const LevelRecordIdentity identity = read_level_record_identity(
        rdram,
        current_level
    );
    const bumble::campaign_levels::Record* campaign_record =
        bumble::campaign_levels::find(current_level);
    if (!checkpoint.valid || checkpoint.rdram != rdram ||
        checkpoint.actor != current_actor ||
        checkpoint.level != current_level ||
        !exact_player_actor(rdram, current_actor, player_index) ||
        player_index != 0u || campaign_record == nullptr ||
        !campaign_record_matches(rdram, identity, *campaign_record)) {
        {
            std::scoped_lock lock(g_mission_checkpoint_mutex);
            invalidate_mission_checkpoint_locked();
        }
        std::fprintf(
            stderr,
            "BUMBLE_MISSION_RESTART stage=checkpoint_restore_rejected"
            " reason=stale_or_invalid_checkpoint trigger=%s"
            " level=%" PRIu32 " checkpoint_level=%" PRIu32 "\n",
            mission_checkpoint_restore_trigger_name(trigger),
            current_level,
            checkpoint.level
        );
        std::fflush(stderr);
        return false;
    }

    for (size_t index = 0u;
         index < kMissionCheckpointPoseOffsets.size();
         ++index) {
        write_u32(
            rdram,
            current_actor + kMissionCheckpointPoseOffsets[index],
            checkpoint.pose_words[index]
        );
    }
    write_u32(rdram, current_actor + 0x5Cu, checkpoint.exit_speed_word);
    // Death clears collision category at 0x8005B6C0; restore it with the state.
    write_u32(rdram, current_actor + 0x64u, checkpoint.collision_category);
    write_u32(rdram, current_actor + 0x8Cu, kPlayerStateTwo);
    write_u32(rdram, current_actor + 0x94u, 0u);
    write_u32(rdram, current_actor + 0x98u, 0u);
    MEM_B(0, guest_address(current_actor + 0xB0u)) = 0;
    write_u32(rdram, current_actor + 0xB4u, 0u);
    MEM_H(0, guest_address(current_actor + 0xB8u)) = 0;

    const uint32_t weapon_state = kWeaponStateBase;
    write_u32(
        rdram,
        weapon_state + kPlayerHealthOffset,
        checkpoint.health_word
    );
    for (uint32_t weapon = 0u;
         weapon < kWidescreenHudWeaponCount;
         ++weapon) {
        write_u32(
            rdram,
            weapon_state + kWeaponAmmoOffset + weapon * sizeof(uint32_t),
            checkpoint.ammo_words[weapon]
        );
    }
    write_u32(
        rdram,
        weapon_state + kSelectedWeaponOffset,
        checkpoint.selected_weapon
    );

    const bool camera_relocated =
        read_u32(rdram, kWorldCameraActiveAddress) != 0u;
    if (camera_relocated) {
        write_u32(rdram, kGuidedCameraOwnerAddress, 0u);
        recomp_context camera_context = *context;
        camera_context.r4 = guest_address(kObjectListBase);
        camera_context.r5 = player_index;
        func_8009C9E4(rdram, &camera_context);
    }

    if (trigger == MissionCheckpointRestoreTrigger::PauseMenu) {
        const gpr current_pad = guest_address(kCurrentPadBase);
        MEM_H(0, current_pad) = 0;
        MEM_H(2, current_pad) = static_cast<int16_t>(kStartButton);
        MEM_H(4, current_pad) = 0;
        MEM_H(6, current_pad) = 0;
        const gpr normalized_pad = guest_address(kNormalizedPadBase);
        MEM_H(0, normalized_pad) = 0;
        MEM_H(2, normalized_pad) = 0;
        MEM_H(4, normalized_pad) = 0;
        MEM_H(6, normalized_pad) = 0;
    }
    bumble::modern_controls::rebase_after_mission_checkpoint_restore(
        rdram,
        current_actor,
        checkpoint.destination_teleport,
        checkpoint.pair_id
    );

    std::fprintf(
        stderr,
        "BUMBLE_MISSION_RESTART stage=checkpoint_restored kind=%s"
        " trigger=%s generation=%" PRIu64 " level=%" PRIu32
        " player=0x%08" PRIX32
        " position=%.3f,%.3f,%.3f health=%.3f"
        " selected_weapon=%" PRIu32
        " resume_transport=%s world_progress=preserved"
        " collision_category=0x%08" PRIX32 " camera_relocated=%d\n",
        mission_checkpoint_kind_name(checkpoint.kind),
        mission_checkpoint_restore_trigger_name(trigger),
        checkpoint.generation,
        checkpoint.level,
        current_actor,
        static_cast<double>(float_from_word(checkpoint.pose_words[9u])),
        static_cast<double>(float_from_word(checkpoint.pose_words[10u])),
        static_cast<double>(float_from_word(checkpoint.pose_words[11u])),
        static_cast<double>(float_from_word(checkpoint.health_word)),
        checkpoint.selected_weapon,
        trigger == MissionCheckpointRestoreTrigger::PauseMenu
            ? "guest_start"
            : "failure_suppressed",
        checkpoint.collision_category,
        camera_relocated ? 1 : 0
    );
    std::fflush(stderr);
    return true;
}

extern "C" void buck_player_update_probe(uint8_t* rdram, recomp_context* context) {
    bumble::native_campaign_level::observe(
        rdram,
        bumble::native_campaign_level::ObservationSite::PlayerUpdate
    );

    const bool mission1_already_observed =
        g_mission1_player_observed.load(std::memory_order_acquire);
    const bool mission2_already_observed =
        g_mission2_player_observed.load(std::memory_order_acquire);
    const uint32_t actor = guest_u32(context->r4);
    const uint32_t owner = static_cast<uint32_t>(MEM_W(0, guest_address(kPlayerOneOwnerSlot)));
    if (actor == 0 || actor != owner || !valid_guest_pointer(actor, 0xF5u)) {
        return;
    }
    if (performance_reporting_enabled()) {
        g_performance_player_x.store(
            read_u32(rdram, actor + 0x40u),
            std::memory_order_release
        );
        g_performance_player_y.store(
            read_u32(rdram, actor + 0x44u),
            std::memory_order_release
        );
        g_performance_player_z.store(
            read_u32(rdram, actor + 0x48u),
            std::memory_order_release
        );
        g_performance_player_yaw.store(
            read_u32(rdram, actor + 0x54u),
            std::memory_order_release
        );
    }

    const uint32_t vtable = static_cast<uint32_t>(MEM_W(0x88, guest_address(actor)));
    const uint8_t player_index = MEM_BU(0xF4, guest_address(actor));
    const uint32_t level_index = static_cast<uint32_t>(
        MEM_W(0, guest_address(kCurrentLevelIndexAddress))
    );
    // Capture on the next player update, after teleport applies its final exit offset.
    commit_pending_portal_mission_checkpoint(rdram, actor, level_index);
    if (g_mission_checkpoint_available.load(std::memory_order_acquire)) {
        bool retired = false;
        uint32_t checkpoint_level = UINT32_MAX;
        uint32_t checkpoint_actor = 0u;
        {
            std::scoped_lock lock(g_mission_checkpoint_mutex);
            checkpoint_level = g_mission_checkpoint.level;
            checkpoint_actor = g_mission_checkpoint.actor;
            if (!g_mission_checkpoint.valid ||
                g_mission_checkpoint.rdram != rdram ||
                checkpoint_level != level_index ||
                checkpoint_actor != actor) {
                invalidate_mission_checkpoint_locked();
                retired = true;
            }
        }
        if (retired) {
            std::fprintf(
                stderr,
                "BUMBLE_MISSION_CHECKPOINT stage=retired"
                " reason=player_or_level_changed"
                " checkpoint_level=%" PRIu32 " level=%" PRIu32
                " checkpoint_player=0x%08" PRIX32
                " player=0x%08" PRIX32 "\n",
                checkpoint_level,
                level_index,
                checkpoint_actor,
                actor
            );
            std::fflush(stderr);
        }
    }
    const LevelRecordIdentity identity = read_level_record_identity(rdram, level_index);
    if (!identity.valid) {
        return;
    }
    const bumble::campaign_levels::Record* campaign_record =
        bumble::campaign_levels::find(level_index);
    const bool exact_campaign_identity = campaign_record != nullptr &&
        campaign_record_matches(rdram, identity, *campaign_record);
    if (exact_campaign_identity && vtable == kPlayerVtable &&
        player_index == 0u) {
        g_campaign_player_level_observed.store(
            level_index,
            std::memory_order_release
        );
    }
    if (!g_player_checkpoint_candidate_logged.exchange(true, std::memory_order_acq_rel)) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=game_player_checkpoint_candidate"
            " pc=0x80059E98 owner_slot=0x800E91D4 actor=0x%08" PRIX32
            " vtable=0x%08" PRIX32 " player_index=%u"
            " level_index_address=0x800E9640 level_index=%" PRIu32
            " level_record=0x%08" PRIX32 " level_id_word=0x%08" PRIX32
            " level_load_offset=0x%08" PRIX32 "\n",
            actor,
            vtable,
            static_cast<unsigned>(player_index),
            level_index,
            identity.address,
            identity.id_word,
            identity.load_offset
        );
        std::fflush(stderr);
    }
    if (vtable != kPlayerVtable || player_index != 0u) {
        return;
    }

    if ((!g_water_mission_start.valid ||
         g_water_mission_start.rdram != rdram ||
         g_water_mission_start.actor != actor ||
         g_water_mission_start.level != level_index) &&
        read_u32(rdram, actor + 0x8Cu) != 10u) {
        const std::array<uint32_t, 3> position_words{
            read_u32(rdram, actor + 0x40u),
            read_u32(rdram, actor + 0x44u),
            read_u32(rdram, actor + 0x48u),
        };
        if (std::all_of(
                position_words.begin(),
                position_words.end(),
                [](uint32_t word) {
                    return std::isfinite(float_from_word(word));
                })) {
            g_water_mission_start = {
                rdram,
                actor,
                level_index,
                position_words,
                true,
            };
        }
    }

    const bool mission1_identity = exact_campaign_identity &&
        level_index == kMissionOneLevelIndex;
    if (!mission1_already_observed && mission1_identity) {
        bool expected = false;
        if (g_mission1_player_observed.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel
            )) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=game_mission1_player_observed"
                " pc=0x80059E98 owner_slot=0x800E91D4 actor=0x%08" PRIX32
                " vtable=0x%08" PRIX32 " player_index=%u"
                " level_index_address=0x800E9640 level_index=%" PRIu32
                " level_record=0x%08" PRIX32 " level_id_word=0x%08" PRIX32
                " level_load_offset=0x%08" PRIX32 "\n",
                actor,
                vtable,
                static_cast<unsigned>(player_index),
                level_index,
                identity.address,
                identity.id_word,
                identity.load_offset
            );
            std::fflush(stderr);
        }
    }

    const bool mission2_identity = exact_campaign_identity &&
        level_index == kMissionTwoLevelIndex;
    const bool title_selector_route =
        g_mission2_selection_committed.load(std::memory_order_acquire);
    const bool completed_profile_route =
        g_mission1_level_increment_observed.load(std::memory_order_acquire) &&
        g_mission1_profile_save_observed.load(std::memory_order_acquire);
    const bool reopened_profile_route =
        g_mission1_profile_reopen_observed.load(std::memory_order_acquire);
    const bool reviewed_mission2_route = title_selector_route ||
        completed_profile_route || reopened_profile_route;
    if (!mission2_already_observed && mission2_identity &&
        reviewed_mission2_route) {
        bool expected = false;
        if (!g_mission2_player_observed.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel
            )) {
            return;
        }

        const uint32_t selection_index = read_u32(
            rdram,
            kMissionSelectorIndexAddress
        );
        const uint32_t mission_number = read_u32(
            rdram,
            kMissionSelectorNumberAddress
        );
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=game_mission2_player_observed"
            " pc=0x80059E98 word=0x3C06800D"
            " owner_slot=0x800E91D4 actor=0x%08" PRIX32
            " vtable=0x%08" PRIX32 " player_index=%u"
            " level_index_address=0x800E9640 level_index=%" PRIu32
            " selection_index_address=0x80107A68 selection_index=%" PRIu32
            " mission_number_address=0x80107A70 mission_number=%" PRIu32
            " level_record=0x%08" PRIX32 " level_id_word=0x%08" PRIX32
            " level_load_offset=0x%08" PRIX32
            " selection_committed=%d normal_profile_route=%d"
            " profile_save=%d profile_reopen=%d\n",
            actor,
            vtable,
            static_cast<unsigned>(player_index),
            level_index,
            selection_index,
            mission_number,
            identity.address,
            identity.id_word,
            identity.load_offset,
            title_selector_route ? 1 : 0,
            (completed_profile_route || reopened_profile_route) ? 1 : 0,
            g_mission1_profile_save_observed.load(std::memory_order_acquire)
                ? 1 : 0,
            reopened_profile_route ? 1 : 0
        );
        std::fflush(stderr);
    }
}

namespace {

void observe_player_owner_commit(uint8_t* rdram, uint32_t pc, uint32_t committed_slot) {
    const PlayerOwners owners = read_player_owners(rdram);
    const uint32_t player_one_state = valid_guest_pointer(owners.player_one, 0x90u)
        ? read_u32(rdram, owners.player_one + 0x8Cu)
        : 0xFFFFFFFFu;
    const uint32_t player_two_state = valid_guest_pointer(owners.player_two, 0x90u)
        ? read_u32(rdram, owners.player_two + 0x8Cu)
        : 0xFFFFFFFFu;
    if (!owners.distinct_nonzero || !owners.metadata_matches) {
        g_two_player_state2_ready.store(false, std::memory_order_release);
    }
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_two_player_owner_commit"
        " pc=0x%08" PRIX32 " committed_slot=0x%08" PRIX32
        " player_one_owner=0x%08" PRIX32 " player_two_owner=0x%08" PRIX32
        " distinct_nonzero=%d player_one_vtable=0x%08" PRIX32
        " player_two_vtable=0x%08" PRIX32
        " player_one_index=%u player_two_index=%u metadata_matches=%d"
        " player_one_state=%" PRIu32 " player_two_state=%" PRIu32 "\n",
        pc,
        committed_slot,
        owners.player_one,
        owners.player_two,
        owners.distinct_nonzero,
        owners.player_one_vtable,
        owners.player_two_vtable,
        static_cast<unsigned>(owners.player_one_index),
        static_cast<unsigned>(owners.player_two_index),
        owners.metadata_matches,
        player_one_state,
        player_two_state
    );
    std::fflush(stderr);
}

bool begin_two_player_stage_locked(
    uint8_t* rdram,
    uint32_t actor,
    TwoPlayerStage expected_stage,
    uint32_t pc,
    PlayerOwners* owners_out,
    NormalizedPad* player_one_pad_out
) {
    if (!g_two_player_chain.armed ||
        g_two_player_chain.stage == TwoPlayerStage::WaitingCallback) {
        return false;
    }

    const PlayerOwners owners = read_player_owners(rdram);
    if (actor == owners.player_one) {
        return false;
    }
    if (actor != owners.player_two || actor != g_two_player_chain.actor) {
        reject_two_player_chain_locked("actor_or_owner_mismatch", pc, actor);
        return false;
    }
    if (g_two_player_chain.stage != expected_stage) {
        reject_two_player_chain_locked("out_of_order_stage", pc, actor);
        return false;
    }
    if (!chain_identity_matches(rdram, actor, owners_out, player_one_pad_out)) {
        reject_two_player_chain_locked("identity_state_or_player_one_pad", pc, actor);
        return false;
    }
    return true;
}

} // namespace

extern "C" void bumble_handle_mission_outcome_checkpoint(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr ||
        guest_u32(context->r4) != kFrontendLiveObject) {
        return;
    }

    const uint32_t outcome = guest_u32(context->r5);
    if (outcome == 0u) {
        const uint32_t actor = read_u32(rdram, kPlayerOneOwnerSlot);
        const uint32_t level = read_u32(rdram, kCurrentLevelIndexAddress);
        commit_mission_checkpoint(
            rdram,
            actor,
            level,
            MissionCheckpointKind::Objective,
            0u,
            0u,
            0u,
            "func_80095788_0x800957D4_decoded_objective"
        );
    }
}

extern "C" int bumble_restore_imminent_mission_failure_checkpoint(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr ||
        !g_mission_checkpoint_available.load(std::memory_order_acquire)) {
        return 0;
    }

    const uint32_t actor = guest_u32(context->r17);
    if (actor == 0u || actor != read_u32(rdram, kPlayerOneOwnerSlot)) {
        return 0;
    }

    uint8_t player_index = 0xFFu;
    if (!exact_player_actor(rdram, actor, player_index) || player_index != 0u ||
        read_u32(rdram, actor + 0x8Cu) != 9u) {
        return 0;
    }

    if (!restore_mission_checkpoint(
            rdram,
            context,
            MissionCheckpointRestoreTrigger::MissionFailure)) {
        return 0;
    }

    std::fprintf(
        stderr,
        "BUMBLE_MISSION_RESTART stage=failure_transition_preempted"
        " owner=player_actor_update_0x8005C278"
        " statistics=unchanged terminal_callback=skipped\n"
    );
    std::fflush(stderr);
    return 1;
}

bool bumble::native_checkpoint::mission_checkpoint_available() {
    return g_mission_checkpoint_available.load(std::memory_order_acquire);
}

void bumble::native_checkpoint::capture_portal_mission_checkpoint(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr ||
        !settled_gameplay_is_live(rdram) ||
        read_u32(rdram, kPauseMenuVisibleAddress) != 0u) {
        return;
    }

    const uint32_t actor = guest_u32(context->r18);
    const uint32_t source_teleport = guest_u32(context->r19);
    const uint32_t destination_teleport = guest_u32(context->r16);
    const uint32_t collision_record = guest_u32(context->r22);
    uint8_t player_index = 0xFFu;
    if (!exact_player_actor(rdram, actor, player_index) || player_index != 0u ||
        !exact_teleport_actor(rdram, source_teleport) ||
        !exact_teleport_actor(rdram, destination_teleport) ||
        source_teleport == destination_teleport ||
        teleport_actor_from_collision_record(rdram, collision_record) !=
            source_teleport) {
        return;
    }

    const uint32_t pair_id = read_u32(rdram, destination_teleport + 0x90u);
    if (pair_id != read_u32(rdram, source_teleport + 0x90u) ||
        read_u32(rdram, destination_teleport + 0x54u) !=
            read_u32(rdram, actor + 0x54u)) {
        return;
    }

    const uint32_t level = read_u32(rdram, kCurrentLevelIndexAddress);
    const LevelRecordIdentity identity = read_level_record_identity(rdram, level);
    const bumble::campaign_levels::Record* campaign_record =
        bumble::campaign_levels::find(level);
    if (campaign_record == nullptr ||
        !campaign_record_matches(rdram, identity, *campaign_record)) {
        return;
    }

    PendingPortalMissionCheckpoint pending{};
    pending.rdram = rdram;
    pending.actor = actor;
    pending.level = level;
    pending.source_teleport = source_teleport;
    pending.destination_teleport = destination_teleport;
    pending.pair_id = pair_id;
    pending.valid = true;
    {
        std::scoped_lock lock(g_mission_checkpoint_mutex);
        g_pending_portal_checkpoint = pending;
    }
    std::fprintf(
        stderr,
        "BUMBLE_MISSION_CHECKPOINT stage=portal_transfer_staged"
        " level=%" PRIu32
        " player=0x%08" PRIX32 " source=0x%08" PRIX32
        " destination=0x%08" PRIX32 " pair_id=%" PRIu32
        " owner=func_80058500_0x8005988C\n",
        pending.level,
        pending.actor,
        pending.source_teleport,
        pending.destination_teleport,
        pending.pair_id
    );
    std::fflush(stderr);
}

void bumble::native_checkpoint::request_mission_restart(
    MissionRestart restart
) {
    g_pending_mission_restart.store(
        mission_restart_value(restart),
        std::memory_order_release
    );
}

bool bumble::native_checkpoint::apply_pending_mission_restart(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t request = g_pending_mission_restart.exchange(
        0u,
        std::memory_order_acq_rel
    );
    if (request == 0u) {
        return false;
    }
    if (rdram == nullptr || context == nullptr ||
        !settled_gameplay_is_live(rdram) ||
        read_u32(rdram, kPauseMenuVisibleAddress) == 0u) {
        std::fprintf(
            stderr,
            "BUMBLE_MISSION_RESTART stage=request_rejected"
            " reason=invalid_gameplay_owner request=%" PRIu32 "\n",
            request
        );
        std::fflush(stderr);
        return false;
    }
    if (request == 2u) {
        return restore_mission_checkpoint(
            rdram,
            context,
            MissionCheckpointRestoreTrigger::PauseMenu
        );
    }
    if (request != 1u) {
        std::fprintf(
            stderr,
            "BUMBLE_MISSION_RESTART stage=request_rejected"
            " reason=unknown_request request=%" PRIu32 "\n",
            request
        );
        std::fflush(stderr);
        return false;
    }

    {
        std::scoped_lock lock(g_mission_checkpoint_mutex);
        invalidate_mission_checkpoint_locked();
    }

    write_u32(rdram, kFrontendLiveObject + 0x84u, kBriefingFrontendPhase);
    write_u32(rdram, kFrontendLiveObject + 0x40u, 1u);
    osGetTime_recomp(rdram, context);
    write_u32(rdram, kFrontendLiveObject + 0x68u, guest_u32(context->r2));
    write_u32(rdram, kFrontendLiveObject + 0x6Cu, guest_u32(context->r3));
    std::fprintf(
        stderr,
        "BUMBLE_MISSION_RESTART stage=beginning_committed"
        " phase=0x%08" PRIX32 " level=%" PRIu32
        " route=authored_phase18\n",
        kBriefingFrontendPhase,
        read_u32(rdram, kCurrentLevelIndexAddress)
    );
    std::fflush(stderr);
    return true;
}

extern "C" void buck_player_one_owner_commit_probe(uint8_t* rdram, recomp_context*) {
    observe_player_owner_commit(rdram, 0x800912DCu, kPlayerOneOwnerSlot);
}

extern "C" void buck_player_two_owner_commit_probe(uint8_t* rdram, recomp_context*) {
    observe_player_owner_commit(rdram, 0x80091348u, kPlayerTwoOwnerSlot);
}

extern "C" void buck_generic_player_callback_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    const PlayerOwners owners = read_player_owners(rdram);
    const uint32_t actor = guest_u32(context->r4);
    const uint32_t callback_target = guest_u32(context->r2);
    const NormalizedPad player_one_pad = read_normalized_pad(rdram, 0u);
    const uint32_t state = valid_guest_pointer(actor, 0x90u)
        ? read_u32(rdram, actor + 0x8Cu)
        : 0xFFFFFFFFu;
    const bool ready = owners.distinct_nonzero && owners.metadata_matches &&
        actor == owners.player_two && callback_target == kPlayerActorUpdate &&
        state == kPlayerStateTwo && normalized_pad_is_neutral(player_one_pad);

    uint32_t ready_callback_count = 0;
    bool stable_ready = false;
    if (actor == owners.player_two) {
        if (ready) {
            ready_callback_count = g_two_player_ready_candidate_count.fetch_add(
                1,
                std::memory_order_acq_rel
            ) + 1;
            stable_ready = ready_callback_count >= 2u;
        } else {
            g_two_player_ready_candidate_count.store(0, std::memory_order_release);
        }
        g_two_player_state2_ready.store(stable_ready, std::memory_order_release);
    }
    if (stable_ready &&
        !g_two_player_ready_logged.exchange(true, std::memory_order_acq_rel)) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=game_two_player_state2_ready"
            " pc=0x8009D124 word=0x0040F809"
            " player_one_owner=0x%08" PRIX32 " player_two_owner=0x%08" PRIX32
            " actor=0x%08" PRIX32 " callback_target=0x%08" PRIX32
            " vtable=0x%08" PRIX32 " player_index=%u state=%" PRIu32
            " player_one_held=0x%04X player_one_pressed=0x%04X"
            " player_one_stick_x=%d player_one_stick_y=%d"
            " ready_callback_count=%" PRIu32
            " player_one_pad_neutral=1\n",
            owners.player_one,
            owners.player_two,
            actor,
            callback_target,
            owners.player_two_vtable,
            static_cast<unsigned>(owners.player_two_index),
            state,
            static_cast<unsigned>(player_one_pad.held),
            static_cast<unsigned>(player_one_pad.pressed),
            static_cast<int>(player_one_pad.stick_x),
            static_cast<int>(player_one_pad.stick_y),
            ready_callback_count
        );
        std::fflush(stderr);
    }

    std::scoped_lock lock(g_two_player_mutex);
    if (!g_two_player_chain.armed || actor != owners.player_two) {
        return;
    }
    if (g_two_player_chain.stage != TwoPlayerStage::WaitingCallback) {
        reject_two_player_chain_locked("callback_before_previous_chain_completed", 0x8009D124u, actor);
        return;
    }
    if (!ready) {
        reject_two_player_chain_locked("callback_preconditions", 0x8009D124u, actor);
        return;
    }

    g_two_player_chain.actor = actor;
    g_two_player_chain.stage = TwoPlayerStage::WaitingDispatch;
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_two_player_callback_accepted"
        " pc=0x8009D124 word=0x0040F809 actor=0x%08" PRIX32
        " callback_target=0x%08" PRIX32 " expected_stick_y=%d"
        " positive_control=%d\n",
        actor,
        callback_target,
        static_cast<int>(g_two_player_chain.expected_stick_y),
        g_two_player_chain.positive_control
    );
    std::fflush(stderr);
}

extern "C" void buck_player_state_dispatch_probe(uint8_t* rdram, recomp_context* context) {
    const uint32_t actor = guest_u32(context->r17);
    static const bool trace_lifecycle = std::getenv("BUMBLE_PLAYER_LIFECYCLE_TRACE") != nullptr;
    if (trace_lifecycle) {
        uint8_t index = 0xFFu;
        if (exact_player_actor(rdram, actor, index)) {
            const uint32_t state = read_u32(rdram, actor + 0x8Cu);
            const float health = float_from_word(read_u32(rdram,
                kWeaponStateBase + index * kWeaponStateStride + kPlayerHealthOffset));
            if (state >= 8u || health <= 0.0f) {
                std::fprintf(stderr, "BUMBLE_PLAYER_LIFECYCLE actor=0x%08X state=%u timer=%u health=%.3f support=%u\n",
                    actor, state, static_cast<unsigned>(MEM_HU(0xA8, static_cast<int32_t>(actor))),
                    static_cast<double>(health), read_u32(rdram, actor + 0x94u));
            }
        }
    }
    std::scoped_lock lock(g_two_player_mutex);
    PlayerOwners owners{};
    NormalizedPad player_one_pad{};
    if (!begin_two_player_stage_locked(
            rdram,
            actor,
            TwoPlayerStage::WaitingDispatch,
            0x8005A520u,
            &owners,
            &player_one_pad)) {
        return;
    }

    g_two_player_chain.stage = TwoPlayerStage::WaitingControlSample;
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_two_player_state2_dispatch"
        " pc=0x8005A520 word=0x8E22008C actor=0x%08" PRIX32
        " owner=0x%08" PRIX32 " vtable=0x%08" PRIX32
        " player_index=%u state=2 player_one_held=0x%04X"
        " player_one_pressed=0x%04X player_one_stick_x=%d"
        " player_one_stick_y=%d player_one_pad_neutral=1\n",
        actor,
        owners.player_two,
        owners.player_two_vtable,
        static_cast<unsigned>(owners.player_two_index),
        static_cast<unsigned>(player_one_pad.held),
        static_cast<unsigned>(player_one_pad.pressed),
        static_cast<int>(player_one_pad.stick_x),
        static_cast<int>(player_one_pad.stick_y)
    );
    std::fflush(stderr);
}

extern "C" void buck_player_state2_control_sample_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t actor = guest_u32(context->r17);
    std::scoped_lock lock(g_two_player_mutex);
    if (!begin_two_player_stage_locked(
            rdram,
            actor,
            TwoPlayerStage::WaitingControlSample,
            0x8005A61Cu,
            nullptr,
            nullptr)) {
        return;
    }

    const uint32_t pad_pointer = guest_u32(context->r2);
    const NormalizedPad player_two_pad = read_normalized_pad(rdram, 1u);
    const uint32_t vertical_bias_word = read_u32(rdram, actor + 0x90u);
    const ControlExpectation expectation = expected_scaled_controls(
        rdram,
        player_two_pad.stick_x,
        player_two_pad.stick_y,
        vertical_bias_word
    );
    const uint32_t observed_scaled_x_word = read_u32(rdram, actor + 0xA0u);
    const bool pad_pointer_matches = pad_pointer == kNormalizedPadBase + kNormalizedPadStride;
    const bool stick_y_matches =
        player_two_pad.stick_y == g_two_player_chain.expected_stick_y;
    const bool scaled_x_matches =
        observed_scaled_x_word == expectation.scaled_x_word;
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_two_player_state2_control_sample"
        " pc=0x8005A61C word=0x84420006 actor=0x%08" PRIX32
        " pad_pointer=0x%08" PRIX32 " pad_pointer_matches=%d"
        " player_two_held=0x%04X player_two_pressed=0x%04X"
        " stick_x=%d stick_y=%d expected_stick_y=%d stick_y_matches=%d"
        " vertical_bias_word=0x%08" PRIX32
        " scaled_x_constant_word=0x%08" PRIX32
        " stick_y_constant_word=0x%08" PRIX32
        " vertical_bias_constant_word=0x%08" PRIX32
        " minimum_word=0x%08" PRIX32 " maximum_word=0x%08" PRIX32
        " scaled_y_input_word=0x%08" PRIX32
        " vertical_bias_component_word=0x%08" PRIX32
        " unclamped_y_word=0x%08" PRIX32
        " expected_scaled_x_word=0x%08" PRIX32
        " observed_scaled_x_word=0x%08" PRIX32 " scaled_x_matches=%d"
        " expected_scaled_y_word=0x%08" PRIX32 " constants_match=%d\n",
        actor,
        pad_pointer,
        pad_pointer_matches,
        static_cast<unsigned>(player_two_pad.held),
        static_cast<unsigned>(player_two_pad.pressed),
        static_cast<int>(player_two_pad.stick_x),
        static_cast<int>(player_two_pad.stick_y),
        static_cast<int>(g_two_player_chain.expected_stick_y),
        stick_y_matches,
        vertical_bias_word,
        expectation.scaled_x_constant_word,
        expectation.stick_y_constant_word,
        expectation.vertical_bias_constant_word,
        expectation.minimum_word,
        expectation.maximum_word,
        expectation.scaled_y_input_word,
        expectation.vertical_bias_component_word,
        expectation.unclamped_y_word,
        expectation.scaled_x_word,
        observed_scaled_x_word,
        scaled_x_matches,
        expectation.scaled_y_word,
        expectation.constants_match
    );
    std::fflush(stderr);
    if (!pad_pointer_matches || !stick_y_matches ||
        !expectation.constants_match || !scaled_x_matches) {
        reject_two_player_chain_locked("control_sample_or_scaled_x", 0x8005A61Cu, actor);
        return;
    }

    g_two_player_chain.stick_x = player_two_pad.stick_x;
    g_two_player_chain.vertical_bias_word = vertical_bias_word;
    g_two_player_chain.expected_scaled_x_word = expectation.scaled_x_word;
    g_two_player_chain.expected_scaled_y_word = expectation.scaled_y_word;
    g_two_player_chain.stage = TwoPlayerStage::WaitingScaledControls;
}

extern "C" void buck_player_state2_scaled_controls_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t actor = guest_u32(context->r17);
    std::scoped_lock lock(g_two_player_mutex);
    if (!begin_two_player_stage_locked(
            rdram,
            actor,
            TwoPlayerStage::WaitingScaledControls,
            0x8005A680u,
            nullptr,
            nullptr)) {
        return;
    }

    const uint32_t scaled_x_word = read_u32(rdram, actor + 0xA0u);
    const uint32_t scaled_y_word = read_u32(rdram, actor + 0x9Cu);
    const bool scaled_x_matches =
        scaled_x_word == g_two_player_chain.expected_scaled_x_word;
    const bool scaled_y_matches =
        scaled_y_word == g_two_player_chain.expected_scaled_y_word;
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_two_player_state2_scaled_controls"
        " pc=0x8005A680 word=0x922200F4 actor=0x%08" PRIX32
        " scaled_x_word=0x%08" PRIX32 " expected_scaled_x_word=0x%08" PRIX32
        " scaled_x_matches=%d scaled_y_word=0x%08" PRIX32
        " expected_scaled_y_word=0x%08" PRIX32 " scaled_y_matches=%d\n",
        actor,
        scaled_x_word,
        g_two_player_chain.expected_scaled_x_word,
        scaled_x_matches,
        scaled_y_word,
        g_two_player_chain.expected_scaled_y_word,
        scaled_y_matches
    );
    std::fflush(stderr);
    if (!scaled_x_matches || !scaled_y_matches) {
        reject_two_player_chain_locked("scaled_control_words", 0x8005A680u, actor);
        return;
    }
    g_two_player_chain.stage = TwoPlayerStage::WaitingOrientationSample;
}

extern "C" void buck_player_state2_orientation_sample_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t actor = guest_u32(context->r17);
    std::scoped_lock lock(g_two_player_mutex);
    if (!begin_two_player_stage_locked(
            rdram,
            actor,
            TwoPlayerStage::WaitingOrientationSample,
            0x8005A860u,
            nullptr,
            nullptr)) {
        return;
    }

    const uint32_t pad_pointer = guest_u32(context->r2);
    const NormalizedPad player_two_pad = read_normalized_pad(rdram, 1u);
    const uint32_t orientation_before_word = read_u32(rdram, actor + 0x50u);
    const uint32_t orientation_constant_word = read_u32(rdram, kOrientationConstantAddress);
    const uint32_t expected_word = expected_orientation(
        player_two_pad.stick_y,
        orientation_before_word,
        orientation_constant_word
    );
    const uint16_t mode_timer = MEM_HU(0xB8, guest_address(actor));
    const uint32_t orientation_override = read_u32(rdram, actor + 0xB4u);
    const bool conditional_override_may_replace =
        mode_timer >= 0x3Cu && mode_timer < 0x53u &&
        (player_two_pad.pressed & 0x0004u) != 0 && orientation_override == 0;
    const bool pad_pointer_matches = pad_pointer == kNormalizedPadBase + kNormalizedPadStride;
    const bool stick_y_matches =
        player_two_pad.stick_y == g_two_player_chain.expected_stick_y;
    const bool constant_matches = orientation_constant_word == kOrientationConstantWord;
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_two_player_state2_orientation_sample"
        " pc=0x8005A860 word=0x84420006 actor=0x%08" PRIX32
        " pad_pointer=0x%08" PRIX32 " pad_pointer_matches=%d"
        " stick_y=%d expected_stick_y=%d stick_y_matches=%d"
        " orientation_before_word=0x%08" PRIX32
        " orientation_constant_word=0x%08" PRIX32 " constant_matches=%d"
        " expected_orientation_word=0x%08" PRIX32
        " mode_timer=0x%04X orientation_override=0x%08" PRIX32
        " conditional_override_may_replace=%d\n",
        actor,
        pad_pointer,
        pad_pointer_matches,
        static_cast<int>(player_two_pad.stick_y),
        static_cast<int>(g_two_player_chain.expected_stick_y),
        stick_y_matches,
        orientation_before_word,
        orientation_constant_word,
        constant_matches,
        expected_word,
        static_cast<unsigned>(mode_timer),
        orientation_override,
        conditional_override_may_replace
    );
    std::fflush(stderr);
    if (!pad_pointer_matches || !stick_y_matches || !constant_matches ||
        conditional_override_may_replace) {
        reject_two_player_chain_locked("orientation_sample", 0x8005A860u, actor);
        return;
    }

    g_two_player_chain.orientation_before_word = orientation_before_word;
    g_two_player_chain.expected_orientation_word = expected_word;
    g_two_player_chain.stage = TwoPlayerStage::WaitingOrientationPost;
}

extern "C" void buck_player_state2_orientation_post_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t actor = guest_u32(context->r17);
    std::scoped_lock lock(g_two_player_mutex);
    if (!begin_two_player_stage_locked(
            rdram,
            actor,
            TwoPlayerStage::WaitingOrientationPost,
            0x8005A9D8u,
            nullptr,
            nullptr)) {
        return;
    }

    const uint32_t orientation_word = read_u32(rdram, actor + 0x50u);
    const bool orientation_matches =
        orientation_word == g_two_player_chain.expected_orientation_word;
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_two_player_state2_orientation_post"
        " pc=0x8005A9D8 word=0x962200B8 actor=0x%08" PRIX32
        " orientation_before_word=0x%08" PRIX32
        " orientation_word=0x%08" PRIX32
        " expected_orientation_word=0x%08" PRIX32
        " orientation_matches=%d\n",
        actor,
        g_two_player_chain.orientation_before_word,
        orientation_word,
        g_two_player_chain.expected_orientation_word,
        orientation_matches
    );
    std::fflush(stderr);
    if (!orientation_matches) {
        reject_two_player_chain_locked("orientation_word", 0x8005A9D8u, actor);
        return;
    }
    g_two_player_chain.stage = TwoPlayerStage::WaitingIntegratorPre;
}

extern "C" void buck_player_state2_integrator_pre_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t actor = guest_u32(context->r17);
    std::scoped_lock lock(g_two_player_mutex);
    if (!begin_two_player_stage_locked(
            rdram,
            actor,
            TwoPlayerStage::WaitingIntegratorPre,
            0x8005AB14u,
            nullptr,
            nullptr)) {
        return;
    }

    const uint32_t scale_high = read_u32(rdram, kIntegratorScaleAddress);
    const uint32_t scale_low = read_u32(rdram, kIntegratorScaleAddress + 4u);
    const uint64_t scale_word =
        (static_cast<uint64_t>(scale_high) << 32) | static_cast<uint64_t>(scale_low);
    const bool scale_matches = scale_word == kIntegratorScaleWord;
    const double scale = std::bit_cast<double>(scale_word);
    g_two_player_chain.integrator_scale_word = scale_word;
    g_two_player_chain.speed_word = read_u32(rdram, actor + 0x5Cu);
    for (uint32_t axis = 0; axis < 3u; ++axis) {
        g_two_player_chain.basis_words[axis] =
            read_u32(rdram, actor + 0x30u + axis * 4u);
        g_two_player_chain.position_before_words[axis] =
            read_u32(rdram, actor + 0x40u + axis * 4u);
        g_two_player_chain.expected_position_words[axis] = expected_integrator_position(
            g_two_player_chain.basis_words[axis],
            g_two_player_chain.speed_word,
            g_two_player_chain.position_before_words[axis],
            scale
        );
    }
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_two_player_state2_integrator_pre"
        " pc=0x8005AB14 word=0x0C01503E delay_pc=0x8005AB18"
        " delay_word=0x02202021 actor=0x%08" PRIX32
        " basis_x_word=0x%08" PRIX32 " basis_y_word=0x%08" PRIX32
        " basis_z_word=0x%08" PRIX32 " speed_word=0x%08" PRIX32
        " position_x_before_word=0x%08" PRIX32
        " position_y_before_word=0x%08" PRIX32
        " position_z_before_word=0x%08" PRIX32
        " integrator_scale_word=0x%016" PRIX64 " scale_matches=%d"
        " expected_position_x_word=0x%08" PRIX32
        " expected_position_y_word=0x%08" PRIX32
        " expected_position_z_word=0x%08" PRIX32 "\n",
        actor,
        g_two_player_chain.basis_words[0],
        g_two_player_chain.basis_words[1],
        g_two_player_chain.basis_words[2],
        g_two_player_chain.speed_word,
        g_two_player_chain.position_before_words[0],
        g_two_player_chain.position_before_words[1],
        g_two_player_chain.position_before_words[2],
        scale_word,
        scale_matches,
        g_two_player_chain.expected_position_words[0],
        g_two_player_chain.expected_position_words[1],
        g_two_player_chain.expected_position_words[2]
    );
    std::fflush(stderr);
    if (!scale_matches) {
        reject_two_player_chain_locked("integrator_scale_constant", 0x8005AB14u, actor);
        return;
    }
    g_two_player_chain.stage = TwoPlayerStage::WaitingIntegratorPost;
}

extern "C" void buck_player_state2_integrator_post_probe(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t actor = guest_u32(context->r17);
    std::scoped_lock lock(g_two_player_mutex);
    if (!begin_two_player_stage_locked(
            rdram,
            actor,
            TwoPlayerStage::WaitingIntegratorPost,
            0x8005AB1Cu,
            nullptr,
            nullptr)) {
        return;
    }

    std::array<uint32_t, 3> observed{};
    std::array<bool, 3> matches{};
    bool all_match = true;
    for (uint32_t axis = 0; axis < 3u; ++axis) {
        observed[axis] = read_u32(rdram, actor + 0x40u + axis * 4u);
        matches[axis] = observed[axis] == g_two_player_chain.expected_position_words[axis];
        all_match = all_match && matches[axis];
    }
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_two_player_state2_integrator_post"
        " pc=0x8005AB1C word=0xC626005C actor=0x%08" PRIX32
        " position_x_word=0x%08" PRIX32 " expected_position_x_word=0x%08" PRIX32
        " position_x_matches=%d position_y_word=0x%08" PRIX32
        " expected_position_y_word=0x%08" PRIX32 " position_y_matches=%d"
        " position_z_word=0x%08" PRIX32 " expected_position_z_word=0x%08" PRIX32
        " position_z_matches=%d all_positions_match=%d\n",
        actor,
        observed[0],
        g_two_player_chain.expected_position_words[0],
        matches[0],
        observed[1],
        g_two_player_chain.expected_position_words[1],
        matches[1],
        observed[2],
        g_two_player_chain.expected_position_words[2],
        matches[2],
        all_match
    );
    std::fflush(stderr);
    if (!all_match) {
        reject_two_player_chain_locked("integrator_position_words", 0x8005AB1Cu, actor);
        return;
    }

    g_two_player_chain.stage = TwoPlayerStage::Complete;
    g_two_player_chain.armed = false;
    g_two_player_checkpoint_observed.store(true, std::memory_order_release);
    g_two_player_positive_control_observed.store(
        g_two_player_chain.positive_control,
        std::memory_order_release
    );
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_two_player_state2_observed"
        " actor=0x%08" PRIX32 " player_one_owner_slot=0x800E91D4"
        " player_two_owner_slot=0x800E91D8 expected_stick_y=%d"
        " positive_control=%d scaled_x_matches=1 scaled_y_matches=1"
        " orientation_matches=1 position_x_matches=1 position_y_matches=1"
        " position_z_matches=1 ordered_same_actor_stages=1\n",
        actor,
        static_cast<int>(g_two_player_chain.expected_stick_y),
        g_two_player_chain.positive_control
    );
    std::fflush(stderr);
}

uint32_t bumble::native_checkpoint::frontend_confirm_count() {
    return g_frontend_confirm_count.load(std::memory_order_acquire);
}

uint32_t bumble::native_checkpoint::last_frontend_phase() {
    return g_last_frontend_phase.load(std::memory_order_acquire);
}

uint32_t bumble::native_checkpoint::last_accepted_frontend_phase() {
    return g_last_accepted_frontend_phase.load(std::memory_order_acquire);
}

uint32_t bumble::native_checkpoint::last_frontend_descriptor() {
    return g_last_frontend_descriptor.load(std::memory_order_acquire);
}

uint32_t bumble::native_checkpoint::last_frontend_descriptor_item() {
    return g_last_frontend_descriptor_item.load(std::memory_order_acquire);
}

uint32_t bumble::native_checkpoint::level_select_cheat_progress() {
    return g_level_select_cheat_progress.load(std::memory_order_acquire);
}

bool bumble::native_checkpoint::level_select_cheat_complete() {
    return g_level_select_cheat_complete.load(std::memory_order_acquire);
}

bool bumble::native_checkpoint::mission2_selector_initialized() {
    return g_mission2_selector_initialized.load(std::memory_order_acquire);
}

bool bumble::native_checkpoint::mission2_selected() {
    return g_mission2_selected.load(std::memory_order_acquire);
}

bool bumble::native_checkpoint::mission2_selection_committed() {
    return g_mission2_selection_committed.load(std::memory_order_acquire);
}

uint32_t bumble::native_checkpoint::campaign_selector_index() {
    return g_campaign_selector_index.load(std::memory_order_acquire);
}

uint32_t bumble::native_checkpoint::campaign_selection_committed_index() {
    return g_campaign_selection_committed_index.load(
        std::memory_order_acquire
    );
}

bool bumble::native_checkpoint::campaign_grid_active() {
    return g_campaign_grid_active.load(std::memory_order_acquire);
}

uint32_t bumble::native_checkpoint::campaign_grid_selected_slot() {
    return g_campaign_grid_selected_slot.load(std::memory_order_acquire);
}

bool bumble::native_checkpoint::campaign_grid_back_hovered() {
    return g_campaign_grid_back_hovered.load(std::memory_order_acquire);
}

void bumble::native_checkpoint::retire_campaign_grid() {
    const bool was_active =
        g_campaign_grid_active.exchange(false, std::memory_order_acq_rel);
    g_campaign_grid_back_hovered.store(false, std::memory_order_release);
    if (was_active) {
        bumble::graphics_options::set_campaign_grid_pointer_active(false);
    }
}

uint32_t bumble::native_checkpoint::campaign_grid_progress_level() {
    return g_campaign_grid_progress_level.load(std::memory_order_acquire);
}

uint32_t bumble::native_checkpoint::campaign_player_level_observed() {
    return g_campaign_player_level_observed.load(std::memory_order_acquire);
}

bool bumble::native_checkpoint::mission1_player_observed() {
    return g_mission1_player_observed.load(std::memory_order_acquire);
}

bool bumble::native_checkpoint::mission2_player_observed() {
    return g_mission2_player_observed.load(std::memory_order_acquire);
}

void bumble::native_checkpoint::configure_mission1_completion_replay(
    bool enabled,
    bool withhold_one_owner
) {
    {
        std::scoped_lock lock(g_mission1_mutex);
        g_mission1_installer_records.fill(0u);
        g_mission1_targets.fill({});
        g_mission1_pickups.fill({});
        g_mission1_health_pickups.fill({});
        g_mission1_detour = {};
        g_mission1_wave_trigger = {};
        g_mission1_factory_trigger = {};
        g_mission1_wave_prerequisites.fill({});
        g_mission1_wave_prerequisites_cleared_logged = false;
        g_mission1_withheld_actor = 0u;
        g_mission1_last_driver_target = 0u;
        g_mission1_last_terminal_frame = 0u;
        g_mission1_attack_start_frame = 0u;
        g_mission1_attack_last_health = INT32_MAX;
        g_mission1_navigation_progress_frame = 0u;
        g_mission1_navigation_best_distance = INFINITY;
        g_mission1_navigation_anchor_x = NAN;
        g_mission1_navigation_anchor_y = NAN;
        g_mission1_navigation_anchor_z = NAN;
        g_mission1_last_player_health = NAN;
    }
    g_mission1_terminal_invocation_stack.fill({});
    g_mission1_terminal_invocation_depth = 0u;
    g_mission1_terminal_invocation_overflow = 0u;
    g_mission1_pending_terminal_caller_pc = 0u;
    g_mission1_profile_save_active = false;
    g_mission1_profile_save_slot = UINT32_MAX;
    g_mission1_profile_reopen_active = false;
    g_mission1_profile_reopen_slot = UINT32_MAX;
    g_mission1_buttons_requested.store(0u, std::memory_order_release);
    g_mission1_installer_count.store(0u, std::memory_order_release);
    g_mission1_installer_candidate_count.store(0u, std::memory_order_release);
    g_mission1_registered_owner_count.store(0u, std::memory_order_release);
    g_mission1_terminal_count.store(0u, std::memory_order_release);
    g_mission1_damage_event_count.store(0u, std::memory_order_release);
    g_mission1_child_probe_candidate_count.store(0u, std::memory_order_release);
    g_mission1_special_installer_candidate_logged.store(
        false,
        std::memory_order_release
    );
    g_mission1_counter_value.store(
        kMissionOneTerminalOwnerCount,
        std::memory_order_release
    );
    g_mission1_delay_value.store(20u, std::memory_order_release);
    g_mission1_counter_zero_gate_observed.store(
        false,
        std::memory_order_release
    );
    g_mission1_delay_zero_gate_observed.store(false, std::memory_order_release);
    g_mission1_success_callback_observed.store(false, std::memory_order_release);
    g_mission1_success_frontend_observed.store(false, std::memory_order_release);
    g_mission1_level_increment_observed.store(false, std::memory_order_release);
    g_mission1_profile_save_observed.store(false, std::memory_order_release);
    g_mission1_profile_reopen_observed.store(false, std::memory_order_release);
    g_mission1_driver_frame.store(0u, std::memory_order_release);
    g_mission1_withhold_one_owner.store(
        enabled && withhold_one_owner,
        std::memory_order_release
    );
    g_mission1_completion_replay_enabled.store(
        enabled,
        std::memory_order_release
    );
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=mission1_completion_replay_configured"
        " enabled=%d withhold_one_owner=%d"
        " control_path=modern_aim_movement_plus_guest_buttons"
        " pickup_path=guest_collision weapon_path=guest_c_button"
        " counter_result_level_writes=0\n",
        enabled ? 1 : 0,
        enabled && withhold_one_owner ? 1 : 0
    );
    std::fflush(stderr);
}

bool bumble::native_checkpoint::mission1_completion_replay_enabled() {
    return g_mission1_completion_replay_enabled.load(
        std::memory_order_acquire
    );
}

uint16_t bumble::native_checkpoint::mission1_completion_buttons_requested() {
    return mission1_completion_replay_enabled()
        ? g_mission1_buttons_requested.load(std::memory_order_acquire)
        : 0u;
}

uint32_t bumble::native_checkpoint::mission1_installer_count() {
    return g_mission1_installer_count.load(std::memory_order_acquire);
}

uint32_t bumble::native_checkpoint::mission1_registered_owner_count() {
    return g_mission1_registered_owner_count.load(std::memory_order_acquire);
}

uint32_t bumble::native_checkpoint::mission1_terminal_count() {
    return g_mission1_terminal_count.load(std::memory_order_acquire);
}

uint32_t bumble::native_checkpoint::mission1_counter_value() {
    return g_mission1_counter_value.load(std::memory_order_acquire);
}

uint32_t bumble::native_checkpoint::mission1_delay_value() {
    return g_mission1_delay_value.load(std::memory_order_acquire);
}

bool bumble::native_checkpoint::mission1_success_callback_observed() {
    return g_mission1_success_callback_observed.load(std::memory_order_acquire);
}

bool bumble::native_checkpoint::mission1_success_frontend_observed() {
    return g_mission1_success_frontend_observed.load(std::memory_order_acquire);
}

bool bumble::native_checkpoint::mission1_level_increment_observed() {
    return g_mission1_level_increment_observed.load(std::memory_order_acquire);
}

bool bumble::native_checkpoint::mission1_profile_save_observed() {
    return g_mission1_profile_save_observed.load(std::memory_order_acquire);
}

bool bumble::native_checkpoint::mission1_profile_reopen_observed() {
    return g_mission1_profile_reopen_observed.load(std::memory_order_acquire);
}

void bumble::native_checkpoint::configure_widescreen_hud_validation(
    bool enabled
) {
    g_widescreen_hud_player_frame.store(0u, std::memory_order_release);
    g_widescreen_hud_last_selected.store(UINT32_MAX, std::memory_order_release);
    g_widescreen_hud_capture_request.store(0u, std::memory_order_release);
    g_widescreen_hud_capture_complete_mask.store(
        0u,
        std::memory_order_release
    );
    g_widescreen_hud_inventory_granted.store(false, std::memory_order_release);
    g_widescreen_hud_validation_enabled.store(
        enabled,
        std::memory_order_release
    );
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=widescreen_hud_validation_configured"
        " enabled=%d aspect=widescreen weapon_slots=%" PRIu32
        " ammo_each=%" PRIu32
        " cycle_driver=replay_full_inventory_fixture"
        " carousel_owner=func_800A4B54 replay_only=1\n",
        enabled ? 1 : 0,
        kWidescreenHudWeaponCount,
        kWidescreenHudValidationAmmo
    );
    std::fflush(stderr);
}

uint32_t bumble::native_checkpoint::take_widescreen_hud_capture_request() {
    return g_widescreen_hud_validation_enabled.load(
            std::memory_order_acquire)
        ? g_widescreen_hud_capture_request.exchange(
            0u,
            std::memory_order_acq_rel
        )
        : 0u;
}

void bumble::native_checkpoint::mark_widescreen_hud_capture_complete(
    uint32_t selected_weapon
) {
    if (!g_widescreen_hud_validation_enabled.load(
            std::memory_order_acquire) ||
        selected_weapon >= kWidescreenHudWeaponCount) {
        return;
    }
    const uint32_t completed_mask =
        g_widescreen_hud_capture_complete_mask.fetch_or(
        1u << selected_weapon,
        std::memory_order_acq_rel
    ) | (1u << selected_weapon);
    const uint32_t full_mask = (1u << kWidescreenHudWeaponCount) - 1u;
    if (completed_mask == full_mask) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=widescreen_hud_validation_complete"
            " captured_mask=0x%08" PRIX32
            " weapon_slots=%" PRIu32 " early_and_settled=1\n",
            completed_mask,
            kWidescreenHudWeaponCount
        );
        std::fflush(stderr);
    }
}

bool bumble::native_checkpoint::widescreen_hud_validation_complete() {
    if (!g_widescreen_hud_validation_enabled.load(
            std::memory_order_acquire)) {
        return true;
    }
    const uint32_t full_mask = (1u << kWidescreenHudWeaponCount) - 1u;
    return g_widescreen_hud_capture_complete_mask.load(
        std::memory_order_acquire
    ) == full_mask;
}

bool bumble::native_checkpoint::two_player_state2_ready() {
    return g_two_player_state2_ready.load(std::memory_order_acquire);
}

void bumble::native_checkpoint::arm_two_player_control_observation(
    bool positive_control
) {
    std::scoped_lock lock(g_two_player_mutex);
    g_two_player_chain = {};
    g_two_player_chain.armed = true;
    g_two_player_chain.positive_control = positive_control;
    g_two_player_chain.expected_stick_y = positive_control ? 80 : 0;
    g_two_player_chain.stage = TwoPlayerStage::WaitingCallback;
    g_two_player_checkpoint_observed.store(false, std::memory_order_release);
    g_two_player_positive_control_observed.store(false, std::memory_order_release);
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=game_two_player_control_observation_armed"
        " positive_control=%d expected_stick_y=%d\n",
        positive_control,
        static_cast<int>(g_two_player_chain.expected_stick_y)
    );
    std::fflush(stderr);
}

bool bumble::native_checkpoint::two_player_checkpoint_observed() {
    return g_two_player_checkpoint_observed.load(std::memory_order_acquire);
}

bool bumble::native_checkpoint::two_player_positive_control_observed() {
    return g_two_player_positive_control_observed.load(std::memory_order_acquire);
}

bool bumble::native_checkpoint::half_player_health_active() {
    return g_half_player_health_active.load(std::memory_order_acquire);
}

bumble::native_checkpoint::RuntimePerformanceCounts
bumble::native_checkpoint::runtime_performance_counts() {
    RuntimePerformanceCounts counts{
        g_performance_active_actors.load(std::memory_order_acquire),
        g_performance_rendered_actors.load(std::memory_order_acquire),
        g_performance_particle_actors.load(std::memory_order_acquire),
        {},
        float_from_word(g_performance_player_x.load(std::memory_order_acquire)),
        float_from_word(g_performance_player_y.load(std::memory_order_acquire)),
        float_from_word(g_performance_player_z.load(std::memory_order_acquire)),
        float_from_word(g_performance_player_yaw.load(std::memory_order_acquire)),
    };
    for (size_t family = 0u; family < counts.weapon_actors.size(); ++family) {
        counts.weapon_actors[family] = g_performance_weapon_actors[family].load(
            std::memory_order_acquire
        );
    }
    return counts;
}
