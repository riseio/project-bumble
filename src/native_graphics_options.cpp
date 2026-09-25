#include "native_graphics_options.hpp"
#include "native_first_run_assets.hpp"

#if defined(_WIN32)
#include <windowsx.h>
#else
#include <SDL2/SDL.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "librecomp/addresses.hpp"
#include "native_campaign_levels.hpp"
#include "native_checkpoint_bridge.hpp"
#include "native_controller_pak.hpp"
#include "native_death_screen.hpp"
#include "native_game_completion_screen.hpp"
#include "native_input_bindings.hpp"
#include "native_menu_actions.hpp"
#include "native_rt64_renderer.hpp"
#include "native_sdl_io.hpp"
#include "native_text_overlay_state.hpp"
#include "bumble_version.hpp"
#include "render/rt64_bumble_collision_debug.h"

namespace {

#if defined(_WIN32)
constexpr UINT kApplyWindowedResolutionMessage = WM_APP + 0x45u;
using NativeWindow = HWND;
#else
using NativeWindow = SDL_Window*;
#endif
constexpr uint32_t kFrontendObject = 0x800FFF80u;
constexpr uint32_t kFrontendPhaseOffset = 0x00u;
constexpr uint32_t kLevelSelectPhase = 0x0000000Eu;
constexpr uint32_t kOptionsPhase = 0x0000000Fu;
constexpr uint32_t kEndingCreditsPhase = 0x00000021u;
constexpr uint32_t kEndingCreditsLevelIndex = 25u;
constexpr uint32_t kGameCompletionPhase = 0x00000022u;
constexpr uint32_t kGameCompletionDescriptor = 0x800FDC80u;
constexpr uint32_t kOptionsDescriptor = 0x800FCC90u;
constexpr uint32_t kOriginalOptionsFirstItem = 0x800FCBF8u;
constexpr uint32_t kMainMenuPhase = 0x0000000Cu;
constexpr uint32_t kMainMenuDescriptor = 0x800FC940u;
constexpr uint32_t kMainMenuOriginalFirstItem = 0x800FC820u;
constexpr uint32_t kMainMenuOriginalSecondItem = 0x800FC848u;
constexpr uint32_t kMainMenuOriginalThirdItem = 0x800FC870u;
constexpr uint32_t kMainMenuOriginalFourthItem = 0x800FC898u;
constexpr uint32_t kMainMenuOriginalTail = 0x800FC8C0u;
constexpr uint32_t kPauseMenuDescriptor = 0x800FD458u;
constexpr uint32_t kPauseMenuOriginalHeader = 0x800FE7CCu;
constexpr uint32_t kPauseMenuOriginalFirstItem = 0x800FE7E8u;
constexpr uint32_t kPauseMenuOriginalTail = 0x800FE8B0u;
constexpr uint32_t kPauseMenuVisible = 0x80100124u;
constexpr uint32_t kCurrentPad = 0x80035F00u;
constexpr uint32_t kCurrentLevelIndex = 0x800E9640u;
constexpr uint32_t kLevelBackgroundRgb = 0x800E952Cu;
constexpr uint16_t kButtonA = 0x8000u;
constexpr uint16_t kButtonB = 0x4000u;
constexpr uint16_t kButtonStart = 0x1000u;

constexpr size_t kMaximumRows = 24u;
constexpr uint32_t kMenuNodeBytes = 0x28u;
constexpr uint32_t kStringSlotBytes = 160u;
constexpr size_t kMenuAllocationBytes =
    kMaximumRows * (kMenuNodeBytes + kStringSlotBytes);
constexpr uint32_t kDirectTextItemFlags = 0x00000019u;
constexpr uint32_t kOriginalItemColour = 0x7FFF6666u;
constexpr uint32_t kOriginalItemStyle = 0x00000012u;
constexpr uint32_t kOriginalLinkedItemFlags = 0x00000419u;
constexpr uint32_t kMainMenuNewGameSuccessor = 0x0000000Eu;
constexpr uint32_t kMainMenuMultiplayerSuccessor = 0x00000023u;
constexpr uint32_t kMainMenuLoadGameSuccessor = 0x00000016u;
constexpr uint32_t kMainMenuTrainingSuccessor = 0x00000027u;
constexpr uint32_t kMainMenuOptionsSuccessor = 0x0000000Fu;
constexpr uint32_t kLevelSelectDescriptorTableEntry = 0x800FE5A8u;
constexpr uint32_t kLevelSelectDescriptor = 0x800FD320u;
constexpr uint32_t kNewGameDescriptor = 0x800FD388u;
constexpr uint32_t kLevelSelectEnabled = 0x800F598Du;
constexpr uint32_t kMissionCompletePhase = 0x0000001Au;
constexpr uint32_t kMissionCompleteDescriptor = 0x800FD648u;
constexpr uint32_t kFrontendCurrentLevelOffset = 0x10u;
constexpr uint32_t kMissionCompleteSaveItem = 0x800FD5F8u;
constexpr uint32_t kMissionCompleteContinueItem = 0x800FD620u;
constexpr uint32_t kMissionCompleteLegacySaveSuccessor = 0x0000001Fu;
constexpr uint32_t kMissionCompleteMainMenuSuccessor = kMainMenuPhase;
constexpr uint32_t kMissionGameplayPhase = 0x00000018u;
constexpr uint32_t kMissionCompleteContinueSuccessor = kMissionGameplayPhase;
constexpr uint32_t kMissionCompleteMissionText = 0x8004A4ACu;
constexpr uint32_t kMissionCompleteScoreText = 0x8004A4B8u;
constexpr uint32_t kMissionCompleteBonusText = 0x8004A4C0u;
constexpr uint32_t kMissionCompleteTotalText = 0x8004A4C8u;
constexpr uint32_t kMissionCompleteMultiplierText = 0x8004A4D0u;
constexpr uint32_t kPauseMenuConfirmReturnToTitleSuccessor = 0x00000008u;
constexpr uint32_t kPauseMenuItemStyle = 0x0000000Au;
constexpr uint32_t kNativeMenuSentinel = 0x7FFFFFFEu;
constexpr size_t kMainMenuMaximumRows = 4u;
constexpr size_t kMainMenuAllocationBytes =
    kMainMenuMaximumRows * (kMenuNodeBytes + kStringSlotBytes);
constexpr size_t kPauseMenuMaximumRows = 24u;
constexpr size_t kPauseMenuAllocationBytes =
    kPauseMenuMaximumRows * (kMenuNodeBytes + kStringSlotBytes);
constexpr float kMenuGuestWidth = 320.0f;
constexpr float kMenuGuestHeight = 240.0f;
constexpr uint32_t kMainMenuRowX = 105u;
constexpr uint32_t kMainMenuRowY = 140u;
constexpr uint32_t kMainMenuRowSpacing = 22u;
constexpr float kMainMenuRowScale = 0.90f;
constexpr uint32_t kStandardMenuRowX = 90u;
constexpr uint32_t kStandardMenuRowY = 70u;
constexpr uint32_t kStandardMenuRowSpacing = 22u;
constexpr float kStandardMenuRowScale = 0.78f;
constexpr uint32_t kLongMenuRowY = 62u;
constexpr uint32_t kLongMenuRowSpacing = 18u;
constexpr float kLongMenuRowScale = 0.70f;
constexpr uint32_t kCompletionMenuRowY = 190u;
constexpr uint32_t kDenseMenuLeftX = 30u;
constexpr uint32_t kDenseMenuRightX = 220u;
constexpr uint32_t kDenseMenuRowY = 25u;
constexpr uint32_t kDenseMenuRowSpacing = 17u;
constexpr float kDenseMenuRowScale = 0.66f;

enum class RowKind : uint32_t {
    ControlsMenu,
    InterfaceHudSettings,
    GameplaySettings,
    InputDevice,
    KeyboardBindingsMenu,
    ControllerBindingsMenu,
    JoystickSettingsMenu,
    MouseSettingsMenu,
    MouseSensitivity,
    MouseSensitivityX,
    MouseSensitivityY,
    MouseAcceleration,
    ResetControls,
    StickLayout,
    JoystickSensitivity,
    JoystickSensitivityX,
    JoystickSensitivityY,
    JoystickDeadzone,
    InvertJoystickX,
    InvertJoystickY,
    ResetJoystick,
    BindMoveForward,
    BindMoveBackward,
    BindStrafeLeft,
    BindStrafeRight,
    BindPrimaryFire,
    BindTakeOffLand,
    BindPreviousWeapon,
    BindNextWeapon,
    BindLoopDeLoop,
    BindQuickFlip,
    BindForwardDash,
    BindBarrelRoll,
    BindPause,
    BindMenuConfirm,
    BindMenuBack,
    BindToggleModernVisuals,
    BindFlyUp,
    BindFlyDown,
    ResetKeyboardBindings,
    ResetControllerBindings,
    DisplaySettings,
    RenderingSettings,
    DisplayMode,
    Resolution,
    Aspect,
    FramePacing,
    Fog,
    HdTerrain,
    TexturePreparation,
    ModernLighting,
    Grass,
    CollisionOverlay,
    CheatsMenu,
    CheatSettings,
    AllWeapons,
    UnlimitedAmmo,
    UnlimitedHealth,
    WaterHazard,
    AmmoPickupAmount,
    MissionTimeLimits,
    CutsceneTextSpeed,
    EnemyHealth,
    EnemyAwareness,
    AdStrafing,
    UnlockAllLevels,
    HealthDisplay,
    PlayerMaximumHealth,
    ResetDefaultsMenu,
    ConfirmResetDefaults,
    EndingCredits,
    HighScores,
    ClearSaveDataMenu,
    ConfirmClearSaveData,
    Back,
};

enum class MenuPage : uint32_t {
    Root,
    Controls,
    InterfaceHud,
    Gameplay,
    KeyboardBindings,
    ControllerBindings,
    JoystickSettings,
    MouseSettings,
    Display,
    Rendering,
    CheatsAndTests,
    Cheats,
    ClearSaveData,
    ResetDefaults,
};

enum class PauseRowKind : uint32_t {
    Resume,
    RestartMenu,
    RestartBeginning,
    RestartCheckpoint,
    CancelRestart,
    OptionsMenu,
    InterfaceHudMenu,
    GameplayMenu,
    GraphicsMenu,
    ControlsMenu,
    InputDevice,
    KeyboardBindingsMenu,
    ControllerBindingsMenu,
    JoystickSettingsMenu,
    MouseSettingsMenu,
    MouseSensitivity,
    MouseSensitivityX,
    MouseSensitivityY,
    MouseAcceleration,
    StickLayout,
    JoystickSensitivity,
    JoystickSensitivityX,
    JoystickSensitivityY,
    JoystickDeadzone,
    InvertJoystickX,
    InvertJoystickY,
    ResetJoystick,
    BindMoveForward,
    BindMoveBackward,
    BindStrafeLeft,
    BindStrafeRight,
    BindPrimaryFire,
    BindTakeOffLand,
    BindPreviousWeapon,
    BindNextWeapon,
    BindLoopDeLoop,
    BindQuickFlip,
    BindForwardDash,
    BindBarrelRoll,
    BindPause,
    BindMenuConfirm,
    BindMenuBack,
    BindToggleModernVisuals,
    BindFlyUp,
    BindFlyDown,
    ResetKeyboardBindings,
    ResetControllerBindings,
    CheatsMenu,
    ExitMenu,
    DisplaySettings,
    RenderingSettings,
    ReturnToTitle,
    QuitGame,
    ConfirmReturnToTitle,
    CancelReturnToTitle,
    DisplayMode,
    Resolution,
    Aspect,
    FramePacing,
    Fog,
    HdTerrain,
    ModernLighting,
    CollisionOverlay,
    AllWeapons,
    UnlimitedAmmo,
    UnlimitedHealth,
    UnlockAllLevels,
    HealthDisplay,
    WaterHazard,
    PlayerMaximumHealth,
    AmmoPickupAmount,
    MissionTimeLimits,
    CutsceneTextSpeed,
    EnemyHealth,
    EnemyAwareness,
    AdStrafing,
    Back,
};

enum class PauseMenuPage : uint32_t {
    Root,
    Restart,
    Options,
    Controls,
    InterfaceHud,
    Gameplay,
    KeyboardBindings,
    ControllerBindings,
    JoystickSettings,
    MouseSettings,
    Graphics,
    Display,
    Rendering,
    Cheats,
    Exit,
    ConfirmTitle,
};

enum class MainMenuRowKind : uint32_t {
    Play,
    Multiplayer,
    Options,
    QuitGame,
    NewGame,
    LevelSelect,
    Training,
    Back,
};

enum class MainMenuPage : uint32_t {
    Root,
    Play,
};

enum class MenuOverlayLayout : uint8_t {
    Main,
    Standard,
    Completion,
    DenseBindings,
    CampaignGrid,
};

enum class MouseMenuContext : uint8_t {
    None,
    Main,
    Options,
    Pause,
    LevelSelect,
    Completion,
};

struct NativeMenuState {
    uint8_t* rdram = nullptr;
    uint8_t* allocation = nullptr;
    uint32_t guest_base = 0;
    size_t row_count = 0;
    std::array<RowKind, kMaximumRows> rows{};
    MenuPage page = MenuPage::Root;
    bool horizontal_latched = false;
    bool vertical_latched = false;
    uint32_t last_selected_node = 0u;
    int pending_vertical_direction = 0;
    uint32_t pending_vertical_node = 0u;
    std::chrono::steady_clock::time_point confirm_fallback_not_before{};
    bool confirm_neutral_observed = false;
    bool entry_confirm_suppressed = false;
    bool confirm_armed = false;
    bool installed = false;
    bool clear_save_failed = false;
    uint64_t last_pointer_motion_revision = 0u;
};

struct NativeMainMenuState {
    uint8_t* rdram = nullptr;
    uint8_t* allocation = nullptr;
    uint32_t guest_base = 0u;
    size_t row_count = 0u;
    std::array<MainMenuRowKind, kMainMenuMaximumRows> rows{};
    MainMenuPage page = MainMenuPage::Root;
    uint32_t last_selected_node = 0u;
    bool vertical_latched = false;
    int pending_vertical_direction = 0;
    uint32_t pending_vertical_node = 0u;
    bool installed = false;
    bool install_failure_logged = false;
    uint64_t last_pointer_motion_revision = 0u;
};

struct NativePauseMenuState {
    uint8_t* rdram = nullptr;
    uint8_t* allocation = nullptr;
    uint32_t guest_base = 0u;
    size_t row_count = 0u;
    std::array<PauseRowKind, kPauseMenuMaximumRows> rows{};
    PauseMenuPage page = PauseMenuPage::Root;
    bool installed = false;
    bool install_failure_logged = false;
    bool vertical_latched = false;
    bool horizontal_latched = false;
    bool return_to_title_logged = false;
    uint64_t last_pointer_motion_revision = 0u;
};

struct MenuPointerState {
    int32_t client_x = -1;
    int32_t client_y = -1;
    uint32_t client_width = 0u;
    uint32_t client_height = 0u;
    uint64_t motion_revision = 0u;
    uint64_t click_revision = 0u;
    bool inside_client = false;
    bool click_down = false;
};

struct MenuPointerInteraction {
    bool pointer_moved = false;
    bool click_pressed = false;
    bool input_suppressed = false;
    std::optional<size_t> row_index;
};

std::filesystem::path g_config_path;
std::atomic_uint32_t g_display_mode{
    static_cast<uint32_t>(bumble::graphics_options::DisplayMode::BorderedWindow)
};
std::atomic_uint32_t g_resolution_width{1280u};
std::atomic_uint32_t g_resolution_height{720u};
std::atomic_uint32_t g_aspect_mode{
    static_cast<uint32_t>(bumble::graphics_options::AspectMode::Widescreen)
};
std::atomic_uint32_t g_frame_pacing{
    static_cast<uint32_t>(
        bumble::graphics_options::FramePacing::Interpolated120Hz
    )
};
std::atomic_uint32_t g_fog_mode{
    static_cast<uint32_t>(bumble::graphics_options::FogMode::Full)
};
std::atomic_bool g_high_resolution_textures{true};
std::atomic_bool g_hd_terrain{true};
std::atomic_bool g_enhanced_textures{false};
std::atomic_bool g_modern_lighting{true};
std::atomic_uint32_t g_lighting_environment{0xC0000000u};
std::atomic_uint32_t g_last_gameplay_level_index{0u};
std::atomic_uint32_t g_grass_mode{
    static_cast<uint32_t>(bumble::graphics_options::GrassMode::Off)
};
std::atomic_bool g_collision_overlay{false};
std::atomic_bool g_all_weapons{false};
std::atomic_bool g_unlimited_ammo{false};
std::atomic_bool g_unlimited_health{false};
std::atomic_bool g_honeycomb_water_rescue{false};
std::atomic_bool g_double_ammo_pickups{false};
std::atomic_bool g_double_mission_time_limits{false};
std::atomic_uint32_t g_cutscene_text_speed{4u};
std::atomic_bool g_double_enemy_health{false};
std::atomic_bool g_double_enemy_awareness{false};
std::atomic_bool g_a_d_strafing{true};
std::atomic_bool g_unlock_all_levels{false};
std::atomic_bool g_honeycomb_health{false};
std::atomic_bool g_half_player_health{false};
std::atomic_bool g_play_menu_return_requested{false};
std::atomic_bool g_main_menu_handoff_pending{false};
std::atomic_bool g_pause_restart_handoff_pending{false};
std::atomic_bool g_main_menu_handoff_ready{false};
std::atomic_uint64_t g_main_menu_handoff_completion_count{0u};
std::atomic_int64_t g_pause_restart_handoff_started_ms{0};
std::atomic_bool g_runtime_shutdown_ready{false};
std::atomic_uint32_t g_campaign_unlocked_level{1u};
std::atomic_bool g_menu_enabled{false};
std::atomic<NativeWindow> g_game_window{nullptr};
#if !defined(_WIN32)
std::atomic_uint64_t g_pending_window_size{0u};
#endif
std::atomic_bool g_menu_exit_requested{false};
std::atomic<MouseMenuContext> g_mouse_menu_context{MouseMenuContext::None};
std::atomic_int64_t g_menu_transition_started_ms{0};
uint64_t g_last_controls_revision = 0u;
size_t g_last_connected_controller_count = 0u;
uint64_t g_consumed_pointer_click_revision = 0u;
uint64_t g_campaign_grid_pointer_motion_revision = 0u;
uint64_t g_mission_complete_pointer_motion_revision = 0u;

std::mutex g_state_mutex;
std::mutex g_pointer_mutex;
std::mutex g_campaign_save_mutex;
MenuPointerState g_menu_pointer{};
std::vector<std::pair<uint32_t, uint32_t>> g_resolution_choices;
NativeMenuState g_native_menu{};
NativeMainMenuState g_main_menu{};
NativePauseMenuState g_pause_menu{};
bool g_pause_overlay_staged = false;
bool g_pause_overlay_precommit_observed = false;

void deactivate_menu_overlay() {
    const MouseMenuContext previous_context = g_mouse_menu_context.exchange(
        MouseMenuContext::None,
        std::memory_order_acq_rel
    );
    const bool had_staged_pause =
        g_pause_overlay_staged || g_pause_overlay_precommit_observed;
    g_pause_overlay_staged = false;
    g_pause_overlay_precommit_observed = false;
    if (previous_context == MouseMenuContext::None && !had_staged_pause) {
        return;
    }
    bumble::text_overlay::clear_kind(
        bumble::text_overlay::TextKind::Menu
    );
}

gpr guest_address(uint32_t address) {
    return static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(address)));
}

uint32_t low_guest_address(gpr address) {
    return static_cast<uint32_t>(address);
}

uint32_t read_guest_u32(uint8_t* rdram, uint32_t address) {
    return static_cast<uint32_t>(MEM_W(0, guest_address(address)));
}

bumble::graphics_options::LightingEnvironmentProfile
lighting_profile_for_level(uint32_t level_index) {
    using Profile =
        bumble::graphics_options::LightingEnvironmentProfile;
    switch (level_index) {
    case 8u:  // Sewer
    case 14u: // Mucus Storage
    case 17u: // Sterilization
    case 19u: // Core Nuke
    case 20u: // Gatekeepers
    case 21u: // Queen 2
    case 22u: // Queen
        return Profile::Underground;
    case 7u:  // Outpost
    case 9u:  // Clean Up
    case 10u: // Scramble Pylon
    case 11u: // Herdling Research
    case 12u: // The Extractor
    case 13u: // Nuke Tower
    case 15u: // Depot Attack
    case 16u: // Zeppelin 2
    case 18u: // Scorpion Killer
        return Profile::TrenchIndustrial;
    default:
        return (level_index >= 1u && level_index <= 6u)
            ? Profile::OutdoorGarden
            : Profile::Neutral;
    }
}

void write_guest_u32(uint8_t* rdram, uint32_t address, uint32_t value) {
    MEM_W(0, guest_address(address)) = static_cast<int32_t>(value);
}

uint32_t normalize_campaign_unlocked_level(uint32_t level) {
    for (const uint32_t selectable :
         bumble::campaign_levels::kMissionSelectLevelIndices) {
        if (selectable >= std::max(level, 1u)) {
            return selectable;
        }
    }
    return bumble::campaign_levels::kMissionSelectLevelIndices.back();
}

void write_guest_string(uint8_t* rdram, uint32_t address, const char* text) {
    const size_t length = std::min(
        std::strlen(text),
        static_cast<size_t>(kStringSlotBytes - 1u)
    );
    for (size_t index = 0; index < length; ++index) {
        MEM_BU(index, guest_address(address)) =
            static_cast<uint8_t>(text[index]);
    }
    MEM_BU(length, guest_address(address)) = 0u;
    for (size_t index = length + 1u; index < kStringSlotBytes; ++index) {
        MEM_BU(index, guest_address(address)) = 0u;
    }
}

std::string read_guest_string(
    uint8_t* rdram,
    uint32_t address,
    size_t capacity
) {
    std::string text;
    text.reserve(capacity);
    for (size_t index = 0u; index < capacity; ++index) {
        const uint8_t character = MEM_BU(index, guest_address(address));
        if (character == 0u) {
            break;
        }
        if (character < 0x20u || character > 0x7Eu) {
            return {};
        }
        text.push_back(static_cast<char>(character));
    }
    return text;
}

std::string completed_mission_label(uint8_t* rdram) {
    const uint32_t completed_level =
        g_last_gameplay_level_index.load(std::memory_order_acquire);
    const bumble::campaign_levels::Record* record =
        bumble::campaign_levels::find(completed_level);
    if (record != nullptr) {
        const uint32_t mission_number =
            bumble::campaign_levels::displayed_mission_number(*record);
        if (mission_number >= 1u &&
            mission_number <= bumble::campaign_levels::kMissionSelectLevelIndices.size() &&
            record->identifier.size() > 2u) {
            char label[96]{};
            const std::string_view mission_name = record->identifier.substr(2u);
            std::snprintf(
                label,
                sizeof(label),
                "MISSION %02" PRIu32 " - %.*s",
                mission_number,
                static_cast<int>(mission_name.size()),
                mission_name.data()
            );
            return label;
        }
    }

    return read_guest_string(rdram, kMissionCompleteMissionText, 12u);
}

MenuPointerState menu_pointer_snapshot() {
    std::lock_guard lock(g_pointer_mutex);
    return g_menu_pointer;
}

void synchronize_pointer_revisions_for_page_locked(
    uint64_t& last_motion_revision
) {
    const MenuPointerState pointer = menu_pointer_snapshot();
    last_motion_revision = pointer.motion_revision;
    g_consumed_pointer_click_revision = pointer.click_revision;
}

std::optional<size_t> menu_pointer_row_hit(
    const MenuPointerState& pointer,
    MenuOverlayLayout layout,
    size_t row_count
) {
    if (!pointer.inside_client || pointer.client_width == 0u ||
        pointer.client_height == 0u || row_count == 0u) {
        return std::nullopt;
    }

    const float guest_scale =
        static_cast<float>(pointer.client_height) / kMenuGuestHeight;
    if (!(guest_scale > 0.0f)) {
        return std::nullopt;
    }
    const float guest_x = static_cast<float>(pointer.client_x) / guest_scale;
    const float guest_y = static_cast<float>(pointer.client_y) / guest_scale;
    const float wide_guest_width =
        static_cast<float>(pointer.client_width) / guest_scale;
    const float center_x = wide_guest_width * 0.5f;

    const auto row_from_y = [guest_y](
        uint32_t first_y,
        uint32_t spacing,
        size_t rows
    ) -> std::optional<size_t> {
        const float top = static_cast<float>(first_y) - 4.0f;
        const float relative_y = guest_y - top;
        if (relative_y < 0.0f) {
            return std::nullopt;
        }
        const size_t row = static_cast<size_t>(
            std::floor(relative_y / static_cast<float>(spacing))
        );
        return row < rows ? std::optional<size_t>(row) : std::nullopt;
    };

    if (layout == MenuOverlayLayout::CampaignGrid) {
        constexpr size_t kMissionCount =
            bumble::campaign_levels::kMissionSelectLevelIndices.size();
        if (row_count > kMissionCount &&
            guest_x >= center_x - 55.0f && guest_x <= center_x + 55.0f &&
            guest_y >= 207.0f && guest_y <= 222.0f) {
            return kMissionCount;
        }
        const size_t mission_count = std::min(row_count, kMissionCount);
        const size_t left_count = std::min<size_t>(
            bumble::campaign_levels::kMissionGridRows, mission_count);
        const size_t right_count = mission_count - left_count;
        const float gutter = 6.0f;
        if (guest_x >= 10.0f && guest_x <= center_x - gutter) {
            return row_from_y(bumble::campaign_levels::kMissionGridFirstY,
                bumble::campaign_levels::kMissionGridRowSpacing, left_count);
        }
        if (guest_x >= center_x + gutter &&
            guest_x <= wide_guest_width - 10.0f) {
            const std::optional<size_t> column_row =
                row_from_y(bumble::campaign_levels::kMissionGridFirstY,
                    bumble::campaign_levels::kMissionGridRowSpacing, right_count);
            if (column_row.has_value()) {
                return left_count + *column_row;
            }
        }
        return std::nullopt;
    }
    if (layout == MenuOverlayLayout::DenseBindings) {
        const float gutter = 6.0f;
        const size_t left_count = (row_count + 1u) / 2u;
        const size_t right_count = row_count - left_count;
        if (guest_x >= 10.0f && guest_x <= center_x - gutter) {
            return row_from_y(
                kDenseMenuRowY,
                kDenseMenuRowSpacing,
                left_count
            );
        }
        if (guest_x >= center_x + gutter &&
            guest_x <= wide_guest_width - 10.0f) {
            const std::optional<size_t> column_row = row_from_y(
                kDenseMenuRowY,
                kDenseMenuRowSpacing,
                right_count
            );
            if (column_row.has_value()) {
                return left_count + *column_row;
            }
        }
        return std::nullopt;
    }

    const float half_width =
        layout == MenuOverlayLayout::Main ? 120.0f : 140.0f;
    if (guest_x < center_x - half_width ||
        guest_x > center_x + half_width) {
        return std::nullopt;
    }
    const bool long_standard_layout = layout == MenuOverlayLayout::Standard &&
        row_count >= 8u;
    return row_from_y(
        layout == MenuOverlayLayout::Main
            ? kMainMenuRowY
            : layout == MenuOverlayLayout::Completion
                ? kCompletionMenuRowY
                : long_standard_layout
                    ? kLongMenuRowY
                    : kStandardMenuRowY,
        layout == MenuOverlayLayout::Main
            ? kMainMenuRowSpacing
            : long_standard_layout
                ? kLongMenuRowSpacing
                : kStandardMenuRowSpacing,
        row_count
    );
}

MenuPointerInteraction consume_menu_pointer_interaction_locked(
    MenuOverlayLayout layout,
    size_t row_count,
    uint64_t& last_motion_revision
) {
    const MenuPointerState pointer = menu_pointer_snapshot();
    MenuPointerInteraction interaction{};
    interaction.pointer_moved =
        pointer.motion_revision != last_motion_revision;
    if (interaction.pointer_moved) {
        last_motion_revision = pointer.motion_revision;
    }
    interaction.click_pressed =
        pointer.click_revision != g_consumed_pointer_click_revision;
    if (interaction.click_pressed) {
        g_consumed_pointer_click_revision = pointer.click_revision;
    }
    if (!interaction.pointer_moved && !interaction.click_pressed) {
        return interaction;
    }

    interaction.input_suppressed =
        bumble::input_bindings::input_suppressed();
    if (!interaction.input_suppressed) {
        interaction.row_index =
            menu_pointer_row_hit(pointer, layout, row_count);
    }
    return interaction;
}

int64_t steady_clock_milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

void begin_menu_transition() {
    g_menu_transition_started_ms.store(
        steady_clock_milliseconds(),
        std::memory_order_release
    );
}

void apply_pointer_confirm_to_guest(
    uint8_t* rdram,
    const MenuPointerInteraction& interaction,
    gpr current_pad,
    recomp_context* context
) {
    if (!interaction.click_pressed) {
        return;
    }

    uint16_t held = static_cast<uint16_t>(
        MEM_HU(0, current_pad) & ~kButtonA
    );
    uint16_t pressed = static_cast<uint16_t>(
        MEM_HU(2, current_pad) & ~kButtonA
    );
    uint16_t accepted = context != nullptr
        ? static_cast<uint16_t>(context->r2) & ~kButtonA
        : 0u;
    if (interaction.row_index.has_value() &&
        !interaction.input_suppressed) {
        pressed |= kButtonA;
        if (context != nullptr) {
            accepted |= kButtonA;
        }
    }
    MEM_H(0, current_pad) = static_cast<int16_t>(held);
    MEM_H(2, current_pad) = static_cast<int16_t>(pressed);
    if (context != nullptr) {
        context->r2 = static_cast<gpr>(accepted);
    }
}

uint32_t main_menu_node_address(size_t index) {
    return g_main_menu.guest_base +
        static_cast<uint32_t>(index * kMenuNodeBytes);
}

uint32_t main_menu_string_address(size_t index) {
    return g_main_menu.guest_base +
        static_cast<uint32_t>(kMainMenuMaximumRows * kMenuNodeBytes) +
        static_cast<uint32_t>(index * kStringSlotBytes);
}

const char* main_menu_page_name(MainMenuPage page) {
    switch (page) {
    case MainMenuPage::Root: return "root";
    case MainMenuPage::Play: return "play";
    }
    return "unknown";
}

const char* main_menu_row_name(MainMenuRowKind row) {
    switch (row) {
    case MainMenuRowKind::Play: return "play";
    case MainMenuRowKind::Multiplayer: return "multiplayer";
    case MainMenuRowKind::Options: return "options";
    case MainMenuRowKind::QuitGame: return "quit_game";
    case MainMenuRowKind::NewGame:
        return bumble::graphics_options::campaign_unlocked_level() > 1u
            ? "continue"
            : "new_game";
    case MainMenuRowKind::LevelSelect: return "level_select";
    case MainMenuRowKind::Training: return "training";
    case MainMenuRowKind::Back: return "back";
    }
    return "unknown";
}

const char* main_menu_row_text(MainMenuRowKind row) {
    switch (row) {
    case MainMenuRowKind::Play: return "PLAY";
    case MainMenuRowKind::Multiplayer: return "MULTIPLAYER";
    case MainMenuRowKind::Options: return "OPTIONS";
    case MainMenuRowKind::QuitGame: return "QUIT GAME";
    case MainMenuRowKind::NewGame:
        return bumble::graphics_options::campaign_unlocked_level() > 1u
            ? "CONTINUE"
            : "NEW GAME";
    case MainMenuRowKind::LevelSelect: return "LEVEL SELECT";
    case MainMenuRowKind::Training: return "TRAINING";
    case MainMenuRowKind::Back: return "BACK";
    }
    return "";
}

uint32_t main_menu_row_successor(MainMenuRowKind row) {
    switch (row) {
    case MainMenuRowKind::Multiplayer:
        return kMainMenuMultiplayerSuccessor;
    case MainMenuRowKind::Options:
        return kMainMenuOptionsSuccessor;
    case MainMenuRowKind::NewGame:
        return kMainMenuNewGameSuccessor;
    case MainMenuRowKind::LevelSelect:
        return kMainMenuNewGameSuccessor;
    case MainMenuRowKind::Training:
        return kMainMenuTrainingSuccessor;
    case MainMenuRowKind::Play:
    case MainMenuRowKind::QuitGame:
    case MainMenuRowKind::Back:
        return kNativeMenuSentinel;
    }
    return kNativeMenuSentinel;
}

bool main_menu_original_contract_matches(uint8_t* rdram) {
    const uint32_t descriptor_first = read_guest_u32(
        rdram,
        kMainMenuDescriptor + 0x14u
    );
    const bool native_first =
        g_main_menu.rdram == rdram && g_main_menu.allocation != nullptr &&
        descriptor_first == main_menu_node_address(0u);
    const uint32_t load_flags = read_guest_u32(
        rdram,
        kMainMenuOriginalThirdItem + 0x10u
    );
    return (descriptor_first == kMainMenuOriginalFirstItem || native_first) &&
        read_guest_u32(rdram, kMainMenuOriginalFirstItem + 0x00u) == 0u &&
        read_guest_u32(rdram, kMainMenuOriginalFirstItem + 0x04u) ==
            kMainMenuOriginalSecondItem &&
        read_guest_u32(rdram, kMainMenuOriginalFirstItem + 0x18u) ==
            kMainMenuNewGameSuccessor &&
        read_guest_u32(rdram, kMainMenuOriginalSecondItem + 0x00u) ==
            kMainMenuOriginalFirstItem &&
        read_guest_u32(rdram, kMainMenuOriginalSecondItem + 0x04u) ==
            kMainMenuOriginalThirdItem &&
        read_guest_u32(rdram, kMainMenuOriginalSecondItem + 0x18u) ==
            kMainMenuMultiplayerSuccessor &&
        read_guest_u32(rdram, kMainMenuOriginalThirdItem + 0x00u) ==
            kMainMenuOriginalSecondItem &&
        read_guest_u32(rdram, kMainMenuOriginalThirdItem + 0x04u) ==
            kMainMenuOriginalFourthItem &&
        (load_flags == 0x00000411u ||
            load_flags == kOriginalLinkedItemFlags) &&
        read_guest_u32(rdram, kMainMenuOriginalThirdItem + 0x18u) ==
            kMainMenuLoadGameSuccessor &&
        read_guest_u32(rdram, kMainMenuOriginalFourthItem + 0x00u) ==
            kMainMenuOriginalThirdItem &&
        read_guest_u32(rdram, kMainMenuOriginalFourthItem + 0x04u) ==
            kMainMenuOriginalTail &&
        read_guest_u32(rdram, kMainMenuOriginalFourthItem + 0x18u) ==
            kMainMenuTrainingSuccessor &&
        read_guest_u32(rdram, kMainMenuOriginalTail + 0x00u) ==
            kMainMenuOriginalFourthItem &&
        read_guest_u32(rdram, kMainMenuOriginalTail + 0x04u) == 0u &&
        read_guest_u32(rdram, kMainMenuOriginalTail + 0x10u) ==
            kOriginalLinkedItemFlags &&
        read_guest_u32(rdram, kMainMenuOriginalTail + 0x14u) ==
            kOriginalItemStyle &&
        read_guest_u32(rdram, kMainMenuOriginalTail + 0x18u) ==
            kMainMenuOptionsSuccessor;
}

bool install_mission_complete_menu(uint8_t* rdram) {
    const uint32_t first = read_guest_u32(
        rdram,
        kMissionCompleteDescriptor + 0x14u
    );
    const uint32_t previous = read_guest_u32(
        rdram,
        kMissionCompleteContinueItem + 0x00u
    );
    const bool contract_matches =
        (first == kMissionCompleteSaveItem ||
            first == kMissionCompleteContinueItem) &&
        (previous == kMissionCompleteSaveItem || previous == 0u) &&
        read_guest_u32(rdram, kMissionCompleteSaveItem + 0x04u) ==
            kMissionCompleteContinueItem &&
        (read_guest_u32(rdram, kMissionCompleteSaveItem + 0x18u) ==
                kMissionCompleteLegacySaveSuccessor ||
            read_guest_u32(rdram, kMissionCompleteSaveItem + 0x18u) ==
                kMissionCompleteMainMenuSuccessor) &&
        read_guest_u32(rdram, kMissionCompleteContinueItem + 0x04u) == 0u &&
        read_guest_u32(rdram, kMissionCompleteContinueItem + 0x18u) ==
            kMissionCompleteContinueSuccessor;
    if (!contract_matches) {
        return false;
    }

    const uint32_t selected = read_guest_u32(
        rdram,
        kMissionCompleteDescriptor + 0x38u
    );
    const bool already_installed =
        first == kMissionCompleteSaveItem &&
        previous == kMissionCompleteSaveItem &&
        read_guest_u32(rdram, kMissionCompleteSaveItem + 0x18u) ==
            kMissionCompleteMainMenuSuccessor;

    write_guest_u32(rdram, kMissionCompleteSaveItem + 0x00u, 0u);
    write_guest_u32(
        rdram,
        kMissionCompleteSaveItem + 0x18u,
        kMissionCompleteMainMenuSuccessor
    );
    write_guest_u32(
        rdram,
        kMissionCompleteContinueItem + 0x00u,
        kMissionCompleteSaveItem
    );
    write_guest_u32(
        rdram,
        kMissionCompleteDescriptor + 0x14u,
        kMissionCompleteSaveItem
    );
    write_guest_u32(
        rdram,
        kMissionCompleteDescriptor + 0x38u,
        already_installed &&
                (selected == kMissionCompleteSaveItem ||
                    selected == kMissionCompleteContinueItem)
            ? selected
            : kMissionCompleteContinueItem
    );
    return true;
}

bool activate_mission_complete_menu_locked(uint8_t* rdram) {
    if (g_mouse_menu_context.load(std::memory_order_acquire) !=
            MouseMenuContext::Completion) {
        bumble::text_overlay::clear_kind(
            bumble::text_overlay::TextKind::Menu
        );
        synchronize_pointer_revisions_for_page_locked(
            g_mission_complete_pointer_motion_revision
        );
        begin_menu_transition();
    }
    g_mouse_menu_context.store(
        MouseMenuContext::Completion,
        std::memory_order_release
    );
    return install_mission_complete_menu(rdram);
}

bool allocate_main_menu_locked(uint8_t* rdram) {
    if (g_main_menu.rdram == rdram && g_main_menu.allocation != nullptr) {
        return true;
    }
    if (g_main_menu.allocation != nullptr && g_main_menu.rdram != nullptr) {
        recomp::free(g_main_menu.rdram, g_main_menu.allocation);
    }
    g_main_menu = NativeMainMenuState{};

    auto* allocation = static_cast<uint8_t*>(
        recomp::alloc(rdram, kMainMenuAllocationBytes)
    );
    if (allocation == nullptr) {
        return false;
    }
    const ptrdiff_t offset = allocation - rdram;
    if (offset < 0 ||
        static_cast<size_t>(offset) + kMainMenuAllocationBytes >
            recomp::mem_size ||
        static_cast<uint64_t>(offset) > UINT32_C(0x7FFFFFFF)) {
        recomp::free(rdram, allocation);
        return false;
    }

    g_main_menu.rdram = rdram;
    g_main_menu.allocation = allocation;
    g_main_menu.guest_base = UINT32_C(0x80000000) +
        static_cast<uint32_t>(offset);
    return true;
}

bool install_main_menu_locked(uint8_t* rdram, MainMenuPage page) {
    if (!main_menu_original_contract_matches(rdram) ||
        !allocate_main_menu_locked(rdram)) {
        return false;
    }

    const bool page_changed =
        !g_main_menu.installed || g_main_menu.page != page ||
        g_mouse_menu_context.load(std::memory_order_acquire) !=
            MouseMenuContext::Main;
    g_main_menu.row_count = 0u;
    g_main_menu.page = page;
    const auto add_row = [](MainMenuRowKind row) {
        if (g_main_menu.row_count < kMainMenuMaximumRows) {
            g_main_menu.rows[g_main_menu.row_count++] = row;
        }
    };
    switch (page) {
    case MainMenuPage::Root:
        add_row(MainMenuRowKind::Play);
        add_row(MainMenuRowKind::Multiplayer);
        add_row(MainMenuRowKind::Options);
        add_row(MainMenuRowKind::QuitGame);
        break;
    case MainMenuPage::Play:
        add_row(MainMenuRowKind::NewGame);
        add_row(MainMenuRowKind::LevelSelect);
        add_row(MainMenuRowKind::Training);
        add_row(MainMenuRowKind::Back);
        break;
    }
    if (g_main_menu.row_count == 0u ||
        g_main_menu.row_count > kMainMenuMaximumRows) {
        return false;
    }

    for (size_t index = 0; index < g_main_menu.row_count; ++index) {
        const uint32_t node = main_menu_node_address(index);
        const MainMenuRowKind row = g_main_menu.rows[index];
        write_guest_string(
            rdram,
            main_menu_string_address(index),
            main_menu_row_text(row)
        );
        write_guest_u32(
            rdram,
            node + 0x00u,
            index == 0u ? 0u : main_menu_node_address(index - 1u)
        );
        write_guest_u32(
            rdram,
            node + 0x04u,
            index + 1u == g_main_menu.row_count
                ? 0u
                : main_menu_node_address(index + 1u)
        );
        write_guest_u32(rdram, node + 0x08u, kOriginalItemColour);
        write_guest_u32(rdram, node + 0x0Cu, main_menu_string_address(index));
        write_guest_u32(
            rdram,
            node + 0x10u,
            row == MainMenuRowKind::Multiplayer
                ? (kDirectTextItemFlags & ~0x8u)
                : kDirectTextItemFlags
        );
        write_guest_u32(rdram, node + 0x14u, kPauseMenuItemStyle);
        write_guest_u32(rdram, node + 0x18u, main_menu_row_successor(row));
        write_guest_u32(rdram, node + 0x1Cu, 0u);
        write_guest_u32(rdram, node + 0x20u, 0u);
        write_guest_u32(rdram, node + 0x24u, 0u);
    }

    const uint32_t first = main_menu_node_address(0u);
    const uint32_t selected = page_changed
        ? first
        : g_main_menu.last_selected_node;
    g_main_menu.installed = true;
    g_runtime_shutdown_ready.store(true, std::memory_order_release);
    if (page_changed) {
        g_main_menu.last_selected_node = first;
        g_main_menu.vertical_latched = false;
        g_main_menu.pending_vertical_direction = 0;
        g_main_menu.pending_vertical_node = 0u;
        synchronize_pointer_revisions_for_page_locked(
            g_main_menu.last_pointer_motion_revision
        );
    }
    g_mouse_menu_context.store(
        MouseMenuContext::Main,
        std::memory_order_release
    );
    if (page_changed) {
        begin_menu_transition();
    }
    write_guest_u32(rdram, kMainMenuDescriptor + 0x14u, first);
    write_guest_u32(rdram, kMainMenuDescriptor + 0x18u, 0u);
    write_guest_u32(rdram, kMainMenuDescriptor + 0x38u, selected);
    std::fprintf(
        stderr,
        "BUMBLE_MAIN_MENU stage=native_page_installed"
        " phase=0x%08" PRIX32 " descriptor=0x%08" PRIX32
        " original_first=0x%08" PRIX32 " native_first=0x%08" PRIX32
        " page=%s rows=%zu direct_text=1 compact_style=0x%08" PRIX32
        " level_select_available=1\n",
        kMainMenuPhase,
        kMainMenuDescriptor,
        kMainMenuOriginalFirstItem,
        first,
        main_menu_page_name(page),
        g_main_menu.row_count,
        kPauseMenuItemStyle
    );
    std::fflush(stderr);
    return true;
}

uint32_t pause_node_address(size_t index) {
    return g_pause_menu.guest_base +
        static_cast<uint32_t>(index * kMenuNodeBytes);
}

uint32_t pause_string_address(size_t index) {
    return g_pause_menu.guest_base +
        static_cast<uint32_t>(kPauseMenuMaximumRows * kMenuNodeBytes) +
        static_cast<uint32_t>(index * kStringSlotBytes);
}

bool allocate_pause_menu_locked(uint8_t* rdram) {
    if (g_pause_menu.rdram == rdram && g_pause_menu.allocation != nullptr) {
        return true;
    }
    if (g_pause_menu.allocation != nullptr && g_pause_menu.rdram != nullptr) {
        recomp::free(g_pause_menu.rdram, g_pause_menu.allocation);
    }
    g_pause_menu = NativePauseMenuState{};

    auto* allocation = static_cast<uint8_t*>(
        recomp::alloc(rdram, kPauseMenuAllocationBytes)
    );
    if (allocation == nullptr) {
        return false;
    }
    const ptrdiff_t offset = allocation - rdram;
    if (offset < 0 ||
        static_cast<size_t>(offset) + kPauseMenuAllocationBytes >
            recomp::mem_size ||
        static_cast<uint64_t>(offset) > UINT32_C(0x7FFFFFFF)) {
        recomp::free(rdram, allocation);
        return false;
    }

    g_pause_menu.rdram = rdram;
    g_pause_menu.allocation = allocation;
    g_pause_menu.guest_base = UINT32_C(0x80000000) +
        static_cast<uint32_t>(offset);
    return true;
}

bool request_menu_exit_locked(const char* source) {
    const NativeWindow window = g_game_window.load(std::memory_order_acquire);
#if defined(_WIN32)
    if (window == nullptr || !IsWindow(window)) {
#else
    if (window == nullptr) {
#endif
        std::fprintf(
            stderr,
            "BUMBLE_MENU_ACTION stage=quit_request_rejected"
            " source=%s reason=no_live_window\n",
            source
        );
        std::fflush(stderr);
        return false;
    }
    if (g_menu_exit_requested.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }
#if defined(_WIN32)
    if (!PostMessageW(window, WM_CLOSE, 0, 0)) {
        const DWORD error = GetLastError();
        g_menu_exit_requested.store(false, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_MENU_ACTION stage=quit_request_failed"
            " source=%s win32_error=%lu\n",
            source,
            static_cast<unsigned long>(error)
        );
        std::fflush(stderr);
        return false;
    }
#else
    SDL_Event event{};
    event.type = SDL_QUIT;
    if (SDL_PushEvent(&event) < 0) {
        g_menu_exit_requested.store(false, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_MENU_ACTION stage=quit_request_failed"
            " source=%s sdl_error=%s\n",
            source,
            SDL_GetError()
        );
        std::fflush(stderr);
        return false;
    }
#endif
    std::fprintf(
        stderr,
        "BUMBLE_MENU_ACTION stage=quit_requested source=%s"
        " transport=native_window_event explicit_menu_selection=1\n",
        source
    );
    std::fflush(stderr);
    return true;
}

bool selected_main_menu_row_locked(
    uint8_t* rdram,
    size_t& index,
    MainMenuRowKind& row,
    uint32_t& node
) {
    if (!g_main_menu.installed || g_main_menu.rdram != rdram ||
        g_main_menu.row_count == 0u) {
        return false;
    }
    const uint32_t selected = read_guest_u32(
        rdram,
        kMainMenuDescriptor + 0x38u
    );
    for (size_t candidate = 0; candidate < g_main_menu.row_count;
         ++candidate) {
        if (selected == main_menu_node_address(candidate)) {
            index = candidate;
            row = g_main_menu.rows[candidate];
            node = selected;
            return true;
        }
    }

    index = 0u;
    node = main_menu_node_address(0u);
    for (size_t candidate = 0; candidate < g_main_menu.row_count;
         ++candidate) {
        if (g_main_menu.last_selected_node ==
            main_menu_node_address(candidate)) {
            index = candidate;
            node = g_main_menu.last_selected_node;
            break;
        }
    }
    row = g_main_menu.rows[index];
    write_guest_u32(rdram, kMainMenuDescriptor + 0x38u, node);
    return true;
}

size_t main_menu_vertical_target_locked(
    uint8_t* rdram,
    size_t selected_index,
    int direction
) {
    if (direction > 0) {
        for (size_t candidate = selected_index + 1u;
             candidate < g_main_menu.row_count; ++candidate) {
            if ((read_guest_u32(
                    rdram,
                    main_menu_node_address(candidate) + 0x10u
                ) & 0x8u) != 0u) {
                return candidate;
            }
        }
    } else if (direction < 0) {
        for (size_t candidate = selected_index; candidate > 0u;) {
            --candidate;
            if ((read_guest_u32(
                    rdram,
                    main_menu_node_address(candidate) + 0x10u
                ) & 0x8u) != 0u) {
                return candidate;
            }
        }
    }
    return selected_index;
}

bool handle_main_menu_input_locked(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t live_object,
    uint32_t descriptor,
    uint32_t phase
) {
    if (live_object != kFrontendObject || descriptor != kMainMenuDescriptor ||
        phase != kMainMenuPhase) {
        return false;
    }
    size_t selected_index = 0u;
    MainMenuRowKind selected_row{};
    uint32_t selected_node = 0u;
    if (!selected_main_menu_row_locked(
            rdram,
            selected_index,
            selected_row,
            selected_node
        )) {
        return true;
    }
    const uint32_t previous_selected_node = g_main_menu.last_selected_node;
    const char* navigation_source = "guest_original";
    const gpr current_pad = guest_address(kCurrentPad);
    MenuPointerInteraction pointer =
        consume_menu_pointer_interaction_locked(
            MenuOverlayLayout::Main,
            g_main_menu.row_count,
            g_main_menu.last_pointer_motion_revision
        );
    if (pointer.row_index.has_value()) {
        const size_t target_index = *pointer.row_index;
        const uint32_t target_node = main_menu_node_address(target_index);
        const bool target_enabled =
            (read_guest_u32(rdram, target_node + 0x10u) & 0x8u) != 0u;
        if (!target_enabled) {
            pointer.row_index.reset();
        } else {
            selected_index = target_index;
            selected_row = g_main_menu.rows[selected_index];
            selected_node = target_node;
            write_guest_u32(
                rdram,
                kMainMenuDescriptor + 0x38u,
                selected_node
            );
            navigation_source = pointer.click_pressed
                ? "mouse_click"
                : "mouse_hover";
            g_main_menu.pending_vertical_direction = 0;
            g_main_menu.pending_vertical_node = 0u;
            MEM_H(6, current_pad) = 0;
        }
    }
    apply_pointer_confirm_to_guest(rdram, pointer, current_pad, context);
    if (pointer.click_pressed) {
        std::fprintf(
            stderr,
            "BUMBLE_MENU_POINTER stage=click menu=main page=%s"
            " row_hit=%d row=%s index=%zu"
            " input_suppressed=%d exact_target=1\n",
            main_menu_page_name(g_main_menu.page),
            pointer.row_index.has_value() ? 1 : 0,
            pointer.row_index.has_value()
                ? main_menu_row_name(selected_row)
                : "none",
            pointer.row_index.has_value() ? selected_index : 0u,
            pointer.input_suppressed ? 1 : 0
        );
        std::fflush(stderr);
    }
    const uint16_t pressed = MEM_HU(2, current_pad);
    const int16_t stick_y = MEM_H(6, current_pad);
    const bool vertical_neutral = stick_y > -15 && stick_y < 15;

    if (stick_y <= -41 && !g_main_menu.vertical_latched) {
        g_main_menu.vertical_latched = true;
        if (selected_node == previous_selected_node) {
            g_main_menu.pending_vertical_direction = 1;
            g_main_menu.pending_vertical_node = selected_node;
        } else {
            g_main_menu.pending_vertical_direction = 0;
            g_main_menu.pending_vertical_node = 0u;
        }
    } else if (stick_y >= 41 && !g_main_menu.vertical_latched) {
        g_main_menu.vertical_latched = true;
        if (selected_node == previous_selected_node) {
            g_main_menu.pending_vertical_direction = -1;
            g_main_menu.pending_vertical_node = selected_node;
        } else {
            g_main_menu.pending_vertical_direction = 0;
            g_main_menu.pending_vertical_node = 0u;
        }
    } else if (vertical_neutral) {
        g_main_menu.vertical_latched = false;
    }

    const bool vertical_pending =
        g_main_menu.pending_vertical_direction != 0;
    if (vertical_pending &&
        selected_node != g_main_menu.pending_vertical_node) {
        g_main_menu.pending_vertical_direction = 0;
        g_main_menu.pending_vertical_node = 0u;
    } else if (vertical_pending && vertical_neutral) {
        const size_t target_index = main_menu_vertical_target_locked(
            rdram,
            selected_index,
            g_main_menu.pending_vertical_direction
        );
        if (target_index != selected_index) {
            selected_index = target_index;
            selected_row = g_main_menu.rows[selected_index];
            selected_node = main_menu_node_address(selected_index);
            write_guest_u32(
                rdram,
                kMainMenuDescriptor + 0x38u,
                selected_node
            );
            navigation_source = "native_fallback";
        }
        g_main_menu.pending_vertical_direction = 0;
        g_main_menu.pending_vertical_node = 0u;
    }

    if (selected_node != previous_selected_node) {
        std::fprintf(
            stderr,
            "BUMBLE_MAIN_MENU stage=navigation page=%s"
            " from=0x%08" PRIX32 " to=0x%08" PRIX32
            " row=%s index=%zu source=%s single_visible_step=1\n",
            main_menu_page_name(g_main_menu.page),
            previous_selected_node,
            selected_node,
            main_menu_row_name(selected_row),
            selected_index,
            navigation_source
        );
        std::fflush(stderr);
    }
    g_main_menu.last_selected_node = selected_node;

    if (g_main_menu.page != MainMenuPage::Root &&
        (pressed & kButtonB) != 0u) {
        const MainMenuPage previous_page = g_main_menu.page;
        MEM_H(2, current_pad) = static_cast<int16_t>(
            pressed & ~(kButtonA | kButtonB)
        );
        context->r2 = static_cast<gpr>(
            static_cast<uint16_t>(context->r2) & ~kButtonA
        );
        install_main_menu_locked(rdram, MainMenuPage::Root);
        std::fprintf(
            stderr,
            "BUMBLE_MAIN_MENU stage=back_requested source=back_button"
            " from_page=%s to_page=root\n",
            main_menu_page_name(previous_page)
        );
        std::fflush(stderr);
        return true;
    }

    const uint16_t accepted = static_cast<uint16_t>(context->r2);
    if ((accepted & kButtonA) == 0u) {
        return true;
    }

    const auto consume_a = [&]() {
        context->r2 = static_cast<gpr>(accepted & ~kButtonA);
        MEM_H(2, current_pad) = static_cast<int16_t>(pressed & ~kButtonA);
    };
    switch (selected_row) {
    case MainMenuRowKind::Play:
        consume_a();
        install_main_menu_locked(rdram, MainMenuPage::Play);
        break;
    case MainMenuRowKind::Back:
        consume_a();
        install_main_menu_locked(rdram, MainMenuPage::Root);
        break;
    case MainMenuRowKind::QuitGame:
        consume_a();
        deactivate_menu_overlay();
        request_menu_exit_locked("main_menu");
        break;
    case MainMenuRowKind::Multiplayer:
        consume_a();
        std::fprintf(
            stderr,
            "BUMBLE_MAIN_MENU stage=disabled_row_rejected"
            " row=multiplayer\n"
        );
        std::fflush(stderr);
        break;
    case MainMenuRowKind::LevelSelect:
        write_guest_u32(
            rdram,
            kLevelSelectDescriptorTableEntry,
            kLevelSelectDescriptor
        );
        MEM_B(0, guest_address(kLevelSelectEnabled)) = 1;
        write_guest_u32(
            rdram,
            kFrontendObject + 0x10u,
            bumble::graphics_options::campaign_unlocked_level()
        );
        [[fallthrough]];
    case MainMenuRowKind::Options:
    case MainMenuRowKind::Training:
        if (selected_row == MainMenuRowKind::Training) {
            deactivate_menu_overlay();
        }
        std::fprintf(
            stderr,
            "BUMBLE_MAIN_MENU stage=route_selected page=%s row=%s"
            " index=%zu successor=0x%08" PRIX32
            " source=guest_menu_successor\n",
            main_menu_page_name(g_main_menu.page),
            main_menu_row_name(selected_row),
            selected_index,
            main_menu_row_successor(selected_row)
        );
        std::fflush(stderr);
        break;
    case MainMenuRowKind::NewGame:
        write_guest_u32(
            rdram,
            kLevelSelectDescriptorTableEntry,
            kNewGameDescriptor
        );
        MEM_B(0, guest_address(kLevelSelectEnabled)) = 0;
        write_guest_u32(
            rdram,
            kFrontendObject + 0x10u,
            bumble::graphics_options::campaign_unlocked_level()
        );
        g_main_menu_handoff_ready.store(false, std::memory_order_release);
        g_main_menu_handoff_pending.store(true, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_MAIN_MENU stage=route_selected page=%s row=%s"
            " index=%zu successor=0x%08" PRIX32
            " source=guest_menu_successor\n",
            main_menu_page_name(g_main_menu.page),
            main_menu_row_name(selected_row),
            selected_index,
            main_menu_row_successor(selected_row)
        );
        std::fflush(stderr);
        break;
    }
    return true;
}

uint32_t clamp_enum(uint32_t value, uint32_t maximum, uint32_t fallback) {
    return value <= maximum ? value : fallback;
}

#if defined(_WIN32)
uint32_t read_ini_uint(const wchar_t* key, uint32_t fallback) {
    return static_cast<uint32_t>(GetPrivateProfileIntW(
        L"Graphics",
        key,
        static_cast<INT>(fallback),
        g_config_path.c_str()
    ));
}

uint32_t read_ini_section_uint(
    const wchar_t* section,
    const wchar_t* key,
    uint32_t fallback
) {
    return static_cast<uint32_t>(GetPrivateProfileIntW(
        section,
        key,
        static_cast<INT>(fallback),
        g_config_path.c_str()
    ));
}

bool read_ini_bool(const wchar_t* key, bool fallback) {
    return read_ini_uint(key, fallback ? 1u : 0u) != 0u;
}

bool read_ini_cheat_bool(const wchar_t* key, bool fallback) {
    return read_ini_section_uint(
        L"Cheats",
        key,
        fallback ? 1u : 0u
    ) != 0u;
}

void write_ini_uint(const wchar_t* key, uint32_t value) {
    const std::wstring text = std::to_wstring(value);
    WritePrivateProfileStringW(
        L"Graphics",
        key,
        text.c_str(),
        g_config_path.c_str()
    );
}

bool write_ini_section_uint(
    const wchar_t* section,
    const wchar_t* key,
    uint32_t value
) {
    const std::wstring text = std::to_wstring(value);
    return WritePrivateProfileStringW(
        section,
        key,
        text.c_str(),
        g_config_path.c_str()
    ) != 0;
}

bool clear_ini_section(const wchar_t* section) {
    return WritePrivateProfileStringW(
        section,
        nullptr,
        nullptr,
        g_config_path.c_str()
    ) != 0;
}

void delete_ini_value(const wchar_t* section, const wchar_t* key) {
    WritePrivateProfileStringW(
        section,
        key,
        nullptr,
        g_config_path.c_str()
    );
}

void load_ini_file() {
}

bool flush_ini_file() {
    return WritePrivateProfileStringW(
        nullptr,
        nullptr,
        nullptr,
        g_config_path.c_str()
    ) != 0;
}
#else
using IniSection = std::map<std::wstring, uint32_t>;
std::map<std::wstring, IniSection> g_ini_values;

std::string trim_ascii(std::string value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1u);
}

std::wstring widen_ascii(const std::string& value) {
    std::wstring result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        result.push_back(static_cast<wchar_t>(character));
    }
    return result;
}

std::string narrow_ascii(const std::wstring& value) {
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        result.push_back(
            character >= 0 && character <= 0x7F
                ? static_cast<char>(character)
                : '?'
        );
    }
    return result;
}

void load_ini_file() {
    g_ini_values.clear();
    std::ifstream input(g_config_path);
    std::string line;
    std::wstring section = L"Graphics";
    while (std::getline(input, line)) {
        line = trim_ascii(std::move(line));
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = widen_ascii(trim_ascii(
                line.substr(1u, line.size() - 2u)
            ));
            continue;
        }
        const size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const std::string key = trim_ascii(line.substr(0u, separator));
        const std::string value = trim_ascii(line.substr(separator + 1u));
        try {
            size_t consumed = 0u;
            const unsigned long parsed = std::stoul(value, &consumed, 10);
            if (consumed == value.size() && parsed <= UINT32_MAX) {
                g_ini_values[section][widen_ascii(key)] =
                    static_cast<uint32_t>(parsed);
            }
        } catch (...) {
        }
    }
}

uint32_t read_ini_section_uint(
    const wchar_t* section,
    const wchar_t* key,
    uint32_t fallback
) {
    const auto section_it = g_ini_values.find(section);
    if (section_it == g_ini_values.end()) {
        return fallback;
    }
    const auto value_it = section_it->second.find(key);
    return value_it != section_it->second.end()
        ? value_it->second
        : fallback;
}

uint32_t read_ini_uint(const wchar_t* key, uint32_t fallback) {
    return read_ini_section_uint(L"Graphics", key, fallback);
}

bool read_ini_bool(const wchar_t* key, bool fallback) {
    return read_ini_uint(key, fallback ? 1u : 0u) != 0u;
}

bool read_ini_cheat_bool(const wchar_t* key, bool fallback) {
    return read_ini_section_uint(
        L"Cheats",
        key,
        fallback ? 1u : 0u
    ) != 0u;
}

bool write_ini_section_uint(
    const wchar_t* section,
    const wchar_t* key,
    uint32_t value
) {
    g_ini_values[section][key] = value;
    return true;
}

void write_ini_uint(const wchar_t* key, uint32_t value) {
    write_ini_section_uint(L"Graphics", key, value);
}

bool clear_ini_section(const wchar_t* section) {
    g_ini_values.erase(section);
    return true;
}

void delete_ini_value(const wchar_t* section, const wchar_t* key) {
    const auto section_it = g_ini_values.find(section);
    if (section_it != g_ini_values.end()) {
        section_it->second.erase(key);
    }
}

bool flush_ini_file() {
    std::ofstream output(g_config_path, std::ios::trunc);
    if (!output) {
        return false;
    }
    for (const auto& [section, values] : g_ini_values) {
        output << '[' << narrow_ascii(section) << "]\n";
        for (const auto& [key, value] : values) {
            output << narrow_ascii(key) << '=' << value << '\n';
        }
    }
    output.flush();
    return static_cast<bool>(output);
}
#endif

std::wstring control_binding_key(
    bumble::input_bindings::InputAction action,
    size_t slot
) {
    const char* name = bumble::input_bindings::action_config_name(action);
    std::wstring result;
    while (*name != '\0') {
        result.push_back(static_cast<wchar_t>(
            static_cast<unsigned char>(*name++)
        ));
    }
    result += std::to_wstring(slot);
    return result;
}

bumble::input_bindings::Settings load_control_settings() {
    using namespace bumble::input_bindings;
    const uint32_t version = read_ini_section_uint(
        L"Controls",
        L"Version",
        0u
    );
    Settings controls = default_settings(version == 0u ? 8u : version);
    if (version == 0u) {
        return controls;
    }
    controls.device_mode = static_cast<InputDeviceMode>(std::min(
        read_ini_section_uint(
            L"Controls",
            L"DeviceMode",
            static_cast<uint32_t>(controls.device_mode)
        ),
        static_cast<uint32_t>(InputDeviceMode::Controller)
    ));
    controls.stick_layout = static_cast<StickLayout>(std::min(
        read_ini_section_uint(
            L"Controls",
            L"StickLayout",
            static_cast<uint32_t>(controls.stick_layout)
        ),
        static_cast<uint32_t>(StickLayout::RightMoveLeftLook)
    ));
    controls.mouse_sensitivity = read_ini_section_uint(L"Controls", L"MouseSensitivity", 100u);
    controls.mouse_sensitivity_x = read_ini_section_uint(L"Controls", L"MouseSensitivityX", 100u);
    controls.mouse_sensitivity_y = read_ini_section_uint(L"Controls", L"MouseSensitivityY", 100u);
    controls.mouse_acceleration = read_ini_section_uint(L"Controls", L"MouseAcceleration", 0u) != 0u;
    controls.joystick_look_sensitivity = read_ini_section_uint(
        L"Controls", L"JoystickLookSensitivity", 100u
    );
    controls.joystick_look_sensitivity_x = read_ini_section_uint(
        L"Controls", L"JoystickLookSensitivityX", 100u
    );
    controls.joystick_look_sensitivity_y = read_ini_section_uint(
        L"Controls", L"JoystickLookSensitivityY", 100u
    );
    controls.joystick_look_deadzone = read_ini_section_uint(
        L"Controls", L"JoystickLookDeadzone", 7000u
    );
    controls.invert_joystick_look_x = read_ini_section_uint(
        L"Controls", L"InvertJoystickLookX", 0u
    ) != 0u;
    controls.invert_joystick_look_y = read_ini_section_uint(
        L"Controls", L"InvertJoystickLookY", 0u
    ) != 0u;
    for (size_t action_index = 0u;
         action_index < kInputActionCount;
         ++action_index) {
        const auto action = static_cast<InputAction>(action_index);
        for (size_t slot = 0u; slot < kBindingSlots; ++slot) {
            const std::wstring key = control_binding_key(action, slot);
            controls.keyboard_mouse[action_index][slot] = static_cast<uint16_t>(
                read_ini_section_uint(
                    L"KeyboardMouseBindings",
                    key.c_str(),
                    controls.keyboard_mouse[action_index][slot]
                )
            );
            controls.controller[action_index][slot] = static_cast<uint16_t>(
                read_ini_section_uint(
                    L"ControllerBindings",
                    key.c_str(),
                    controls.controller[action_index][slot]
                )
            );
        }
    }
    if (version < 4u) {
        const size_t back_index = static_cast<size_t>(InputAction::MenuBack);
        const ActionBindings legacy_back{
            static_cast<uint16_t>(ControllerInput::X),
            0u,
            0u,
        };
        if (controls.controller[back_index] == legacy_back) {
            controls.controller[back_index] =
                default_settings().controller[back_index];
        }
    }
    if (version < 5u) {
        const Settings defaults = default_settings();
        const size_t sprint_index = static_cast<size_t>(InputAction::ForwardDash);
        const size_t roll_index = static_cast<size_t>(InputAction::BarrelRoll);
        const ActionBindings legacy_sprint{
            static_cast<uint16_t>(ControllerInput::LeftStick), 0u, 0u,
        };
        const ActionBindings legacy_roll{
            static_cast<uint16_t>(ControllerInput::RightStick), 0u, 0u,
        };
        if (controls.controller[sprint_index] == legacy_sprint) {
            controls.controller[sprint_index] = defaults.controller[sprint_index];
        }
        if (controls.controller[roll_index] == legacy_roll) {
            controls.controller[roll_index] = defaults.controller[roll_index];
        }
    }
    upgrade_keyboard_defaults(controls, version);
    if (version < 8u) {
        const auto legacy = default_settings(7u);
        const auto defaults = default_settings();
        for (const auto action : {InputAction::ForwardDash, InputAction::BarrelRoll}) {
            const auto index = size_t(action);
            if (controls.controller[index] == legacy.controller[index])
                controls.controller[index] = defaults.controller[index];
        }
        for (const auto action : {InputAction::FlyUp, InputAction::FlyDown}) {
            const auto index = size_t(action);
            for (auto* table : {&controls.keyboard_mouse, &controls.controller}) {
                const auto& initial = table == &controls.controller ? defaults.controller : defaults.keyboard_mouse;
                const uint16_t binding = initial[index][0];
                const bool used = std::any_of(table->begin(), table->end(), [binding](const auto& slots) {
                    return std::find(slots.begin(), slots.end(), binding) != slots.end();
                });
                if (!used) (*table)[index] = initial[index];
            }
        }
    }
    return controls;
}

void save_control_settings() {
    using namespace bumble::input_bindings;
    const Settings controls = current();
    clear_ini_section(L"Controls");
    clear_ini_section(L"KeyboardMouseBindings");
    clear_ini_section(L"ControllerBindings");
    write_ini_section_uint(L"Controls", L"Version", 8u);
    write_ini_section_uint(L"Controls", L"MouseSensitivity", controls.mouse_sensitivity);
    write_ini_section_uint(L"Controls", L"MouseSensitivityX", controls.mouse_sensitivity_x);
    write_ini_section_uint(L"Controls", L"MouseSensitivityY", controls.mouse_sensitivity_y);
    write_ini_section_uint(L"Controls", L"MouseAcceleration", controls.mouse_acceleration ? 1u : 0u);
    write_ini_section_uint(
        L"Controls",
        L"DeviceMode",
        static_cast<uint32_t>(controls.device_mode)
    );
    write_ini_section_uint(
        L"Controls",
        L"StickLayout",
        static_cast<uint32_t>(controls.stick_layout)
    );
    write_ini_section_uint(
        L"Controls", L"JoystickLookSensitivity",
        controls.joystick_look_sensitivity
    );
    write_ini_section_uint(
        L"Controls", L"JoystickLookSensitivityX",
        controls.joystick_look_sensitivity_x
    );
    write_ini_section_uint(
        L"Controls", L"JoystickLookSensitivityY",
        controls.joystick_look_sensitivity_y
    );
    write_ini_section_uint(
        L"Controls", L"JoystickLookDeadzone",
        controls.joystick_look_deadzone
    );
    write_ini_section_uint(
        L"Controls", L"InvertJoystickLookX",
        controls.invert_joystick_look_x ? 1u : 0u
    );
    write_ini_section_uint(
        L"Controls", L"InvertJoystickLookY",
        controls.invert_joystick_look_y ? 1u : 0u
    );
    for (size_t action_index = 0u;
         action_index < kInputActionCount;
         ++action_index) {
        const auto action = static_cast<InputAction>(action_index);
        for (size_t slot = 0u; slot < kBindingSlots; ++slot) {
            const std::wstring key = control_binding_key(action, slot);
            write_ini_section_uint(
                L"KeyboardMouseBindings",
                key.c_str(),
                controls.keyboard_mouse[action_index][slot]
            );
            write_ini_section_uint(
                L"ControllerBindings",
                key.c_str(),
                controls.controller[action_index][slot]
            );
        }
    }
    flush_ini_file();
    g_last_controls_revision = revision();
}

void save_settings(const bumble::graphics_options::Settings& settings) {
    write_ini_uint(L"Version", 15u);
    write_ini_uint(L"DisplayMode", static_cast<uint32_t>(settings.display_mode));
    write_ini_uint(L"ResolutionWidth", settings.resolution_width);
    write_ini_uint(L"ResolutionHeight", settings.resolution_height);
    write_ini_uint(L"AspectMode", static_cast<uint32_t>(settings.aspect_mode));
    write_ini_uint(L"FramePacing", static_cast<uint32_t>(settings.frame_pacing));
    write_ini_uint(L"FogMode", static_cast<uint32_t>(settings.fog_mode));
    write_ini_uint(L"HighResolutionTextures", 1u);
    write_ini_uint(L"HdTerrain", settings.hd_terrain ? 1u : 0u);
    write_ini_uint(L"ModernLighting", settings.modern_lighting ? 1u : 0u);
    write_ini_uint(L"EnhancedTextures", settings.enhanced_textures ? 1u : 0u);
    write_ini_uint(L"GrassMode", static_cast<uint32_t>(settings.grass_mode));
    write_ini_section_uint(
        L"Debug",
        L"CollisionOverlay",
        settings.collision_overlay ? 1u : 0u
    );
    write_ini_section_uint(L"Cheats", L"AllWeapons", settings.all_weapons ? 1u : 0u);
    write_ini_section_uint(L"Cheats", L"UnlimitedAmmo", settings.unlimited_ammo ? 1u : 0u);
    write_ini_section_uint(L"Cheats", L"UnlimitedHealth", settings.unlimited_health ? 1u : 0u);
    write_ini_section_uint(
        L"Gameplay",
        L"HoneycombWaterRescue",
        settings.honeycomb_water_rescue ? 1u : 0u
    );
    write_ini_section_uint(
        L"Gameplay",
        L"DoubleAmmoPickups",
        settings.double_ammo_pickups ? 1u : 0u
    );
    write_ini_section_uint(
        L"Gameplay",
        L"DoubleMissionTimeLimits",
        settings.double_mission_time_limits ? 1u : 0u
    );
    write_ini_section_uint(L"Gameplay", L"CutsceneTextSpeed", settings.cutscene_text_speed);
    write_ini_section_uint(
        L"Gameplay",
        L"DoubleEnemyHealth",
        settings.double_enemy_health ? 1u : 0u
    );
    write_ini_section_uint(
        L"Gameplay",
        L"DoubleEnemyAwareness",
        settings.double_enemy_awareness ? 1u : 0u
    );
    write_ini_section_uint(
        L"Gameplay",
        L"ADStrafing",
        settings.a_d_strafing ? 1u : 0u
    );
    delete_ini_value(L"Cheats", L"SafeWater");
    write_ini_section_uint(
        L"Interface",
        L"HoneycombHealth",
        settings.honeycomb_health ? 1u : 0u
    );
    write_ini_section_uint(
        L"Gameplay",
        L"HalfPlayerHealth",
        settings.half_player_health ? 1u : 0u
    );
    write_ini_section_uint(
        L"Debug",
        L"UnlockAllLevels",
        settings.unlock_all_levels ? 1u : 0u
    );
    delete_ini_value(
        L"Graphics",
        L"RayTracedLighting"
    );
    save_control_settings();
    flush_ini_file();
}

bool clear_save_data_locked() {
    if (!bumble::controller_pak::clear_all_storage()) {
        return false;
    }
    const bool section_operation = clear_ini_section(L"Campaign");
    const bool flush_operation = flush_ini_file();
    const bool section_absent = read_ini_section_uint(
        L"Campaign",
        L"UnlockedLevel",
        UINT32_MAX
    ) == UINT32_MAX;
#if defined(_WIN32)
    // Windows may report a failed flush after writing; verify by reading back.
    const bool persisted = section_absent;
#else
    const bool persisted = section_operation && flush_operation &&
        section_absent;
#endif
    if (!persisted) {
        std::fprintf(
            stderr,
            "BUMBLE_CAMPAIGN_SAVE stage=all_save_data_clear_failed"
            " reason=campaign_persistence_failed"
            " section_operation=%d flush_operation=%d section_absent=%d\n",
            section_operation ? 1 : 0,
            flush_operation ? 1 : 0,
            section_absent ? 1 : 0
        );
        std::fflush(stderr);
        return false;
    }
    g_campaign_unlocked_level.store(1u, std::memory_order_release);
    std::fprintf(
        stderr,
        "BUMBLE_CAMPAIGN_SAVE stage=all_save_data_cleared"
        " unlocked_level=1 controller_paks=cleared\n"
    );
    std::fflush(stderr);
    return true;
}

void publish_settings(const bumble::graphics_options::Settings& settings) {
    g_display_mode.store(
        static_cast<uint32_t>(settings.display_mode),
        std::memory_order_release
    );
    g_resolution_width.store(settings.resolution_width, std::memory_order_release);
    g_resolution_height.store(settings.resolution_height, std::memory_order_release);
    g_aspect_mode.store(
        static_cast<uint32_t>(settings.aspect_mode),
        std::memory_order_release
    );
    g_frame_pacing.store(
        static_cast<uint32_t>(settings.frame_pacing),
        std::memory_order_release
    );
    g_fog_mode.store(
        static_cast<uint32_t>(settings.fog_mode),
        std::memory_order_release
    );
    g_high_resolution_textures.store(
        settings.high_resolution_textures,
        std::memory_order_release
    );
    g_hd_terrain.store(settings.hd_terrain, std::memory_order_release);
    g_enhanced_textures.store(settings.enhanced_textures && settings.hd_terrain, std::memory_order_release);
    g_modern_lighting.store(
        settings.modern_lighting,
        std::memory_order_release
    );
    g_grass_mode.store(
        static_cast<uint32_t>(settings.grass_mode),
        std::memory_order_release
    );
    g_collision_overlay.store(
        settings.collision_overlay,
        std::memory_order_release
    );
    g_all_weapons.store(settings.all_weapons, std::memory_order_release);
    g_unlimited_ammo.store(settings.unlimited_ammo, std::memory_order_release);
    g_unlimited_health.store(settings.unlimited_health, std::memory_order_release);
    g_honeycomb_water_rescue.store(
        settings.honeycomb_water_rescue,
        std::memory_order_release
    );
    g_double_ammo_pickups.store(
        settings.double_ammo_pickups,
        std::memory_order_release
    );
    g_double_mission_time_limits.store(
        settings.double_mission_time_limits,
        std::memory_order_release
    );
    g_cutscene_text_speed.store(settings.cutscene_text_speed == 1u || settings.cutscene_text_speed == 2u
        ? settings.cutscene_text_speed : 4u, std::memory_order_release);
    g_double_enemy_health.store(
        settings.double_enemy_health,
        std::memory_order_release
    );
    g_double_enemy_awareness.store(
        settings.double_enemy_awareness,
        std::memory_order_release
    );
    g_a_d_strafing.store(settings.a_d_strafing, std::memory_order_release);
    g_unlock_all_levels.store(
        settings.unlock_all_levels,
        std::memory_order_release
    );
    g_honeycomb_health.store(
        settings.honeycomb_health,
        std::memory_order_release
    );
    g_half_player_health.store(
        settings.half_player_health,
        std::memory_order_release
    );
}

bool suitable_aspect(uint32_t width, uint32_t height) {
    if (height == 0u) {
        return false;
    }
    const double aspect = static_cast<double>(width) / static_cast<double>(height);
    constexpr std::array<double, 6> common_aspects{
        4.0 / 3.0,
        5.0 / 4.0,
        16.0 / 10.0,
        16.0 / 9.0,
        21.0 / 9.0,
        32.0 / 9.0,
    };
    return std::any_of(
        common_aspects.begin(),
        common_aspects.end(),
        [aspect](double candidate) {
            return std::abs(aspect - candidate) < 0.035;
        }
    );
}

std::vector<std::pair<uint32_t, uint32_t>> enumerate_resolutions(
    NativeWindow owner
) {
    std::set<std::pair<uint32_t, uint32_t>> unique;
    unique.emplace(1280u, 800u);
#if defined(_WIN32)
    const HMONITOR monitor = owner != nullptr
        ? MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST)
        : MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    if (GetMonitorInfoW(monitor, &monitor_info)) {
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        for (DWORD index = 0;
             EnumDisplaySettingsW(monitor_info.szDevice, index, &mode);
             ++index) {
            const uint32_t width = mode.dmPelsWidth;
            const uint32_t height = mode.dmPelsHeight;
            if (width >= 640u && height >= 480u &&
                mode.dmBitsPerPel >= 24u && suitable_aspect(width, height)) {
                unique.emplace(width, height);
            }
            mode = DEVMODEW{};
            mode.dmSize = sizeof(mode);
        }
        unique.emplace(
            static_cast<uint32_t>(
                monitor_info.rcMonitor.right - monitor_info.rcMonitor.left
            ),
            static_cast<uint32_t>(
                monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top
            )
        );
    }
#else
    const int display_index = owner != nullptr
        ? std::max(0, SDL_GetWindowDisplayIndex(owner))
        : 0;
    const int mode_count = SDL_GetNumDisplayModes(display_index);
    for (int index = 0; index < mode_count; ++index) {
        SDL_DisplayMode mode{};
        if (SDL_GetDisplayMode(display_index, index, &mode) == 0 &&
            mode.w >= 640 && mode.h >= 480 &&
            suitable_aspect(
                static_cast<uint32_t>(mode.w),
                static_cast<uint32_t>(mode.h)
            )) {
            unique.emplace(
                static_cast<uint32_t>(mode.w),
                static_cast<uint32_t>(mode.h)
            );
        }
    }
    SDL_DisplayMode desktop{};
    if (SDL_GetDesktopDisplayMode(display_index, &desktop) == 0 &&
        desktop.w > 0 && desktop.h > 0) {
        unique.emplace(
            static_cast<uint32_t>(desktop.w),
            static_cast<uint32_t>(desktop.h)
        );
    }
#endif
    if (unique.empty()) {
        unique.emplace(1280u, 720u);
    }

    std::vector<std::pair<uint32_t, uint32_t>> result(unique.begin(), unique.end());
    std::sort(
        result.begin(),
        result.end(),
        [](const auto& left, const auto& right) {
            const uint64_t left_pixels =
                static_cast<uint64_t>(left.first) * left.second;
            const uint64_t right_pixels =
                static_cast<uint64_t>(right.first) * right.second;
            return left_pixels == right_pixels
                ? left.first < right.first
                : left_pixels < right_pixels;
        }
    );
    return result;
}

size_t closest_resolution_index(
    const std::vector<std::pair<uint32_t, uint32_t>>& choices,
    uint32_t width,
    uint32_t height
) {
    size_t best = 0u;
    uint64_t best_distance = std::numeric_limits<uint64_t>::max();
    for (size_t index = 0; index < choices.size(); ++index) {
        if (choices[index].first == width && choices[index].second == height) {
            return index;
        }
        const int64_t dx = static_cast<int64_t>(choices[index].first) - width;
        const int64_t dy = static_cast<int64_t>(choices[index].second) - height;
        const uint64_t distance = static_cast<uint64_t>(dx * dx + dy * dy);
        if (distance < best_distance) {
            best_distance = distance;
            best = index;
        }
    }
    return best;
}

void clamp_resolution_to_monitor(
    bumble::graphics_options::Settings& settings,
    const std::vector<std::pair<uint32_t, uint32_t>>& choices
) {
    const size_t index = closest_resolution_index(
        choices,
        settings.resolution_width,
        settings.resolution_height
    );
    settings.resolution_width = choices[index].first;
    settings.resolution_height = choices[index].second;
}

#if defined(_WIN32)
bool validation_monitor_coordinate(const char* name, LONG& coordinate) {
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' ||
        parsed < std::numeric_limits<LONG>::min() ||
        parsed > std::numeric_limits<LONG>::max()) {
        return false;
    }
    coordinate = static_cast<LONG>(parsed);
    return true;
}

HMONITOR validation_monitor_from_environment() {
    POINT point{};
    if (!validation_monitor_coordinate("BUMBLE_VALIDATION_MONITOR_X", point.x) ||
        !validation_monitor_coordinate("BUMBLE_VALIDATION_MONITOR_Y", point.y)) {
        return nullptr;
    }
    const HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONULL);
    if (monitor != nullptr) {
        std::fprintf(
            stderr,
            "BUMBLE_GRAPHICS_OPTIONS stage=validation_monitor_selected"
            " x=%ld y=%ld\n",
            point.x,
            point.y
        );
        std::fflush(stderr);
    }
    return monitor;
}

void resize_bordered_window(
    HWND window,
    uint32_t client_width,
    uint32_t client_height,
    HMONITOR preferred_monitor = nullptr
) {
    if (window == nullptr || !IsWindow(window) || IsZoomed(window)) {
        return;
    }
    RECT outer{0, 0, static_cast<LONG>(client_width), static_cast<LONG>(client_height)};
    AdjustWindowRectEx(&outer, WS_OVERLAPPEDWINDOW, FALSE, 0);
    const int width = outer.right - outer.left;
    const int height = outer.bottom - outer.top;

    HMONITOR monitor = preferred_monitor;
    if (monitor == nullptr) {
        monitor = validation_monitor_from_environment();
    }
    if (monitor == nullptr) {
        monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    }
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    if (GetMonitorInfoW(monitor, &info)) {
        x = info.rcWork.left + std::max<LONG>(
            0L,
            (info.rcWork.right - info.rcWork.left - width) / 2
        );
        y = info.rcWork.top + std::max<LONG>(
            0L,
            (info.rcWork.bottom - info.rcWork.top - height) / 2
        );
    }
    SetWindowPos(
        window,
        nullptr,
        x,
        y,
        width,
        height,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED
    );
}
#else
void resize_bordered_window(
    NativeWindow window,
    uint32_t client_width,
    uint32_t client_height
) {
    if (window == nullptr) {
        return;
    }
    SDL_SetWindowSize(
        window,
        static_cast<int>(client_width),
        static_cast<int>(client_height)
    );
    SDL_SetWindowPosition(
        window,
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED
    );
}
#endif

const char* row_name(RowKind row) {
    switch (row) {
    case RowKind::ControlsMenu: return "controls_menu";
    case RowKind::InterfaceHudSettings: return "interface_hud_settings";
    case RowKind::GameplaySettings: return "gameplay_settings";
    case RowKind::InputDevice: return "input_device";
    case RowKind::KeyboardBindingsMenu: return "keyboard_bindings_menu";
    case RowKind::ControllerBindingsMenu: return "controller_bindings_menu";
    case RowKind::JoystickSettingsMenu: return "joystick_settings_menu";
    case RowKind::MouseSettingsMenu: return "mouse_settings_menu";
    case RowKind::MouseSensitivity: return "MouseSensitivity";
    case RowKind::MouseSensitivityX: return "MouseSensitivityX";
    case RowKind::MouseSensitivityY: return "MouseSensitivityY";
    case RowKind::MouseAcceleration: return "MouseAcceleration";
    case RowKind::BindFlyUp: return "BindFlyUp";
    case RowKind::BindFlyDown: return "BindFlyDown";
    case RowKind::ResetControls: return "reset_controls";
    case RowKind::StickLayout: return "stick_layout";
    case RowKind::JoystickSensitivity: return "joystick_sensitivity";
    case RowKind::JoystickSensitivityX: return "joystick_sensitivity_x";
    case RowKind::JoystickSensitivityY: return "joystick_sensitivity_y";
    case RowKind::JoystickDeadzone: return "joystick_deadzone";
    case RowKind::InvertJoystickX: return "invert_joystick_x";
    case RowKind::InvertJoystickY: return "invert_joystick_y";
    case RowKind::ResetJoystick: return "reset_joystick";
    case RowKind::BindMoveForward: return "bind_move_forward";
    case RowKind::BindMoveBackward: return "bind_move_backward";
    case RowKind::BindStrafeLeft: return "bind_strafe_left";
    case RowKind::BindStrafeRight: return "bind_strafe_right";
    case RowKind::BindPrimaryFire: return "bind_primary_fire";
    case RowKind::BindTakeOffLand: return "bind_take_off_land";
    case RowKind::BindPreviousWeapon: return "bind_previous_weapon";
    case RowKind::BindNextWeapon: return "bind_next_weapon";
    case RowKind::BindLoopDeLoop: return "bind_loop_de_loop";
    case RowKind::BindQuickFlip: return "bind_quick_flip";
    case RowKind::BindForwardDash: return "bind_forward_dash";
    case RowKind::BindBarrelRoll: return "bind_barrel_roll";
    case RowKind::BindPause: return "bind_pause";
    case RowKind::BindMenuConfirm: return "bind_menu_confirm";
    case RowKind::BindMenuBack: return "bind_menu_back";
    case RowKind::BindToggleModernVisuals: return "bind_toggle_modern_visuals";
    case RowKind::ResetKeyboardBindings: return "reset_keyboard_bindings";
    case RowKind::ResetControllerBindings: return "reset_controller_bindings";
    case RowKind::DisplaySettings: return "display_settings";
    case RowKind::RenderingSettings: return "rendering_settings";
    case RowKind::DisplayMode: return "display_mode";
    case RowKind::Resolution: return "resolution";
    case RowKind::Aspect: return "aspect";
    case RowKind::FramePacing: return "frame_pacing";
    case RowKind::Fog: return "fog";
    case RowKind::HdTerrain: return "hd_terrain";
    case RowKind::TexturePreparation: return "texture_preparation";
    case RowKind::ModernLighting: return "modern_lighting";
    case RowKind::Grass: return "grass";
    case RowKind::CollisionOverlay: return "collision_overlay";
    case RowKind::CheatsMenu: return "cheats_menu";
    case RowKind::CheatSettings: return "cheat_settings";
    case RowKind::AllWeapons: return "all_weapons";
    case RowKind::UnlimitedAmmo: return "unlimited_ammo";
    case RowKind::UnlimitedHealth: return "unlimited_health";
    case RowKind::WaterHazard: return "water_hazard";
    case RowKind::AmmoPickupAmount: return "ammo_pickup_amount";
    case RowKind::CutsceneTextSpeed: return "cutscene_text_speed";
    case RowKind::MissionTimeLimits: return "mission_time_limits";
    case RowKind::EnemyHealth: return "enemy_health";
    case RowKind::EnemyAwareness: return "enemy_awareness";
    case RowKind::AdStrafing: return "a_d_strafing";
    case RowKind::UnlockAllLevels: return "unlock_all_levels";
    case RowKind::HealthDisplay: return "health_display";
    case RowKind::PlayerMaximumHealth: return "player_maximum_health";
    case RowKind::ResetDefaultsMenu: return "reset_defaults_menu";
    case RowKind::ConfirmResetDefaults: return "confirm_reset_defaults";
    case RowKind::EndingCredits: return "ending_credits";
    case RowKind::HighScores: return "high_scores";
    case RowKind::ClearSaveDataMenu: return "clear_save_data_menu";
    case RowKind::ConfirmClearSaveData: return "confirm_clear_save_data";
    case RowKind::Back: return "back";
    default: return "unknown";
    }
}

const char* menu_page_name(MenuPage page) {
    switch (page) {
    case MenuPage::Root: return "root";
    case MenuPage::Controls: return "controls";
    case MenuPage::InterfaceHud: return "interface_hud";
    case MenuPage::Gameplay: return "gameplay";
    case MenuPage::KeyboardBindings: return "keyboard_bindings";
    case MenuPage::ControllerBindings: return "controller_bindings";
    case MenuPage::MouseSettings: return "mouse_settings";
    case MenuPage::JoystickSettings: return "joystick_settings";
    case MenuPage::Display: return "display";
    case MenuPage::Rendering: return "rendering";
    case MenuPage::CheatsAndTests: return "cheats_and_tests";
    case MenuPage::Cheats: return "cheats";
    case MenuPage::ClearSaveData: return "clear_save_data";
    case MenuPage::ResetDefaults: return "reset_defaults";
    }
    return "unknown";
}

MenuPage menu_parent_page(MenuPage page) {
    switch (page) {
    case MenuPage::Controls:
    case MenuPage::InterfaceHud:
    case MenuPage::Gameplay:
    case MenuPage::Display:
    case MenuPage::Rendering:
    case MenuPage::CheatsAndTests:
    case MenuPage::ClearSaveData:
        return MenuPage::Root;
    case MenuPage::ResetDefaults:
        return MenuPage::Gameplay;
    case MenuPage::KeyboardBindings:
    case MenuPage::ControllerBindings:
    case MenuPage::MouseSettings:
    case MenuPage::JoystickSettings:
        return MenuPage::Controls;
    case MenuPage::Cheats:
        return MenuPage::CheatsAndTests;
    case MenuPage::Root:
        return MenuPage::Root;
    }
    return MenuPage::Root;
}

bool row_binding_action(
    RowKind row,
    bumble::input_bindings::InputAction& action
) {
    using bumble::input_bindings::InputAction;
    switch (row) {
    case RowKind::BindFlyUp: action = InputAction::FlyUp; return true;
    case RowKind::BindFlyDown: action = InputAction::FlyDown; return true;
    case RowKind::BindMoveForward: action = InputAction::MoveForward; return true;
    case RowKind::BindMoveBackward: action = InputAction::MoveBackward; return true;
    case RowKind::BindStrafeLeft: action = InputAction::StrafeLeft; return true;
    case RowKind::BindStrafeRight: action = InputAction::StrafeRight; return true;
    case RowKind::BindPrimaryFire: action = InputAction::PrimaryFire; return true;
    case RowKind::BindTakeOffLand: action = InputAction::TakeOffLand; return true;
    case RowKind::BindPreviousWeapon: action = InputAction::PreviousWeapon; return true;
    case RowKind::BindNextWeapon: action = InputAction::NextWeapon; return true;
    case RowKind::BindLoopDeLoop: action = InputAction::LoopDeLoop; return true;
    case RowKind::BindQuickFlip: action = InputAction::QuickFlip; return true;
    case RowKind::BindForwardDash: action = InputAction::ForwardDash; return true;
    case RowKind::BindBarrelRoll: action = InputAction::BarrelRoll; return true;
    case RowKind::BindPause: action = InputAction::Pause; return true;
    case RowKind::BindMenuConfirm: action = InputAction::MenuConfirm; return true;
    case RowKind::BindMenuBack: action = InputAction::MenuBack; return true;
    case RowKind::BindToggleModernVisuals: action = InputAction::ToggleModernVisuals; return true;
    default: return false;
    }
}

bool keyboard_bindings_page(MenuPage page) {
    return page == MenuPage::KeyboardBindings;
}

bool controller_bindings_page(MenuPage page) {
    return page == MenuPage::ControllerBindings;
}

bumble::input_bindings::BindingProfile binding_profile_for_page(
    MenuPage page
) {
    return controller_bindings_page(page)
        ? bumble::input_bindings::BindingProfile::Controller
        : bumble::input_bindings::BindingProfile::KeyboardMouse;
}

std::string row_text(
    RowKind row,
    const bumble::graphics_options::Settings& settings
) {
    char buffer[kStringSlotBytes]{};
    switch (row) {
    case RowKind::ControlsMenu:
        std::snprintf(buffer, sizeof(buffer), "CONTROLS");
        break;
    case RowKind::InterfaceHudSettings:
        std::snprintf(buffer, sizeof(buffer), "INTERFACE / HUD");
        break;
    case RowKind::GameplaySettings:
        std::snprintf(buffer, sizeof(buffer), "GAMEPLAY");
        break;
    case RowKind::InputDevice: {
        const auto controls = bumble::input_bindings::current();
        const bool controller_connected =
            bumble::native_io::connected_controller_count() != 0u;
        const char* effective = controller_connected &&
                controls.device_mode !=
                    bumble::input_bindings::InputDeviceMode::KeyboardMouse
            ? "CONTROLLER"
            : "KEYBOARD & MOUSE";
        if (controls.device_mode ==
            bumble::input_bindings::InputDeviceMode::Auto) {
            std::snprintf(
                buffer,
                sizeof(buffer),
                "INPUT DEVICE  <AUTO: %s>",
                effective
            );
        } else if (controls.device_mode ==
                bumble::input_bindings::InputDeviceMode::Controller &&
            !controller_connected) {
            std::snprintf(
                buffer,
                sizeof(buffer),
                "INPUT DEVICE  <CONTROLLER: KBM FALLBACK>"
            );
        } else {
            std::snprintf(
                buffer,
                sizeof(buffer),
                "INPUT DEVICE  <%s>",
                bumble::input_bindings::device_mode_name(
                    controls.device_mode
                )
            );
        }
        break;
    }
    case RowKind::KeyboardBindingsMenu:
        std::snprintf(buffer, sizeof(buffer), "KEYBOARD & MOUSE BINDINGS");
        break;
    case RowKind::ControllerBindingsMenu:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "CONTROLLER BINDINGS  <%s>",
            bumble::native_io::connected_controller_count() != 0u
                ? "CONNECTED"
                : "NOT CONNECTED"
        );
        break;
    case RowKind::MouseSettingsMenu:
        std::snprintf(buffer, sizeof(buffer), "MOUSE SETTINGS");
        break;
    case RowKind::MouseSensitivity:
    case RowKind::MouseSensitivityX:
    case RowKind::MouseSensitivityY:
    case RowKind::MouseAcceleration: {
        const auto controls = bumble::input_bindings::current();
        if (row == RowKind::MouseAcceleration)
            std::snprintf(buffer, sizeof(buffer), "ACCELERATION <%s>", controls.mouse_acceleration ? "ON" : "OFF");
        else {
            const char* label = row == RowKind::MouseSensitivity ? "SENSITIVITY" :
                row == RowKind::MouseSensitivityX ? "HORIZONTAL SENSITIVITY" : "VERTICAL SENSITIVITY";
            const uint32_t value = row == RowKind::MouseSensitivity ? controls.mouse_sensitivity :
                row == RowKind::MouseSensitivityX ? controls.mouse_sensitivity_x : controls.mouse_sensitivity_y;
            std::snprintf(buffer, sizeof(buffer), "%s <%u%%>", label, value);
        }
        break;
    }
    case RowKind::JoystickSettingsMenu:
        std::snprintf(buffer, sizeof(buffer), "JOYSTICK SETTINGS");
        break;
    case RowKind::ResetControls:
        std::snprintf(buffer, sizeof(buffer), "RESET ALL CONTROLS");
        break;
    case RowKind::StickLayout:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "STICK LAYOUT  <%s>",
            bumble::input_bindings::stick_layout_name(
                bumble::input_bindings::current().stick_layout
            )
        );
        break;
    case RowKind::JoystickSensitivity:
    case RowKind::JoystickSensitivityX:
    case RowKind::JoystickSensitivityY:
    case RowKind::JoystickDeadzone:
    case RowKind::InvertJoystickX:
    case RowKind::InvertJoystickY: {
        const auto controls = bumble::input_bindings::current();
        if (row == RowKind::JoystickSensitivity) {
            std::snprintf(buffer, sizeof(buffer), "LOOK SENSITIVITY  <%" PRIu32 "%%>", controls.joystick_look_sensitivity);
        } else if (row == RowKind::JoystickSensitivityX) {
            std::snprintf(buffer, sizeof(buffer), "HORIZONTAL FINE TUNE  <%" PRIu32 "%%>", controls.joystick_look_sensitivity_x);
        } else if (row == RowKind::JoystickSensitivityY) {
            std::snprintf(buffer, sizeof(buffer), "VERTICAL FINE TUNE  <%" PRIu32 "%%>", controls.joystick_look_sensitivity_y);
        } else if (row == RowKind::JoystickDeadzone) {
            const uint32_t percent =
                (controls.joystick_look_deadzone * 100u + 16383u) / 32767u;
            std::snprintf(
                buffer,
                sizeof(buffer),
                "LOOK DEADZONE  <%" PRIu32 "%%>",
                percent
            );
        } else if (row == RowKind::InvertJoystickX) {
            std::snprintf(buffer, sizeof(buffer), "INVERT HORIZONTAL LOOK  <%s>", controls.invert_joystick_look_x ? "ON" : "OFF");
        } else {
            std::snprintf(buffer, sizeof(buffer), "INVERT VERTICAL LOOK  <%s>", controls.invert_joystick_look_y ? "ON" : "OFF");
        }
        break;
    }
    case RowKind::ResetJoystick:
        std::snprintf(buffer, sizeof(buffer), "RESET JOYSTICK SETTINGS");
        break;
    case RowKind::BindFlyUp:
    case RowKind::BindFlyDown:
    case RowKind::BindMoveForward:
    case RowKind::BindMoveBackward:
    case RowKind::BindStrafeLeft:
    case RowKind::BindStrafeRight:
    case RowKind::BindPrimaryFire:
    case RowKind::BindTakeOffLand:
    case RowKind::BindPreviousWeapon:
    case RowKind::BindNextWeapon:
    case RowKind::BindLoopDeLoop:
    case RowKind::BindQuickFlip:
    case RowKind::BindForwardDash:
    case RowKind::BindBarrelRoll:
    case RowKind::BindPause:
    case RowKind::BindMenuConfirm:
    case RowKind::BindToggleModernVisuals:
    case RowKind::BindMenuBack: {
        bumble::input_bindings::InputAction action{};
        if (!row_binding_action(row, action)) {
            break;
        }
        const auto profile = binding_profile_for_page(g_native_menu.page);
        const auto capture = bumble::input_bindings::capture_state();
        const bool capturing = capture.active &&
            capture.profile == profile && capture.action == action;
        const std::string value = capturing
            ? (capture.waiting_for_neutral
                ? "RELEASE CONTROLS"
                : (profile ==
                        bumble::input_bindings::BindingProfile::Controller
                    ? "PRESS BUTTON - BACK CANCELS"
                    : "PRESS KEY - BACKSPACE CANCELS"))
            : bumble::input_bindings::binding_text(profile, action);
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%s  <%s>",
            bumble::input_bindings::action_label(action),
            value.c_str()
        );
        break;
    }
    case RowKind::ResetKeyboardBindings:
        std::snprintf(buffer, sizeof(buffer), "RESET KEYBOARD & MOUSE");
        break;
    case RowKind::ResetControllerBindings:
        std::snprintf(buffer, sizeof(buffer), "RESET CONTROLLER");
        break;
    case RowKind::DisplaySettings:
        std::snprintf(buffer, sizeof(buffer), "DISPLAY SETTINGS");
        break;
    case RowKind::RenderingSettings:
        std::snprintf(buffer, sizeof(buffer), "GRAPHICS / EFFECTS");
        break;
    case RowKind::DisplayMode:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "DISPLAY MODE  <%s>",
            settings.display_mode ==
                    bumble::graphics_options::DisplayMode::Fullscreen
                ? "FULLSCREEN"
                : "BORDERED WINDOW"
        );
        break;
    case RowKind::Resolution:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "RESOLUTION  <%" PRIu32 " X %" PRIu32 ">",
            settings.resolution_width,
            settings.resolution_height
        );
        break;
    case RowKind::Aspect:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "ASPECT  <%s>",
            settings.aspect_mode ==
                    bumble::graphics_options::AspectMode::Widescreen
                ? "WIDESCREEN"
                : "4:3"
        );
        break;
    case RowKind::FramePacing:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "FRAME PACING  <%s>",
            settings.frame_pacing ==
                    bumble::graphics_options::FramePacing::Interpolated120Hz
                ? "INTERPOLATED 120 HZ"
                : settings.frame_pacing ==
                        bumble::graphics_options::FramePacing::Interpolated60Hz
                    ? "INTERPOLATED 60 HZ"
                    : "ORIGINAL 30 HZ"
        );
        break;
    case RowKind::Fog:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "FOG  <%s>",
            settings.fog_mode == bumble::graphics_options::FogMode::None
                ? "NONE"
                : settings.fog_mode == bumble::graphics_options::FogMode::Half
                    ? "HALF"
                    : "FULL"
        );
        break;
    case RowKind::HdTerrain:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "TEXTURES <%s>",
            !settings.hd_terrain ? "ORIGINAL" : settings.enhanced_textures ? "ENHANCED" : "MODERN"
        );
        break;
    case RowKind::TexturePreparation:
        std::snprintf(buffer, sizeof(buffer), "%s", bumble::first_run::texture_prompt_requested()
            ? "TEXTURE PROMPT ENABLED" : "ENHANCED TEXTURES: ASK NEXT LAUNCH");
        break;
    case RowKind::ModernLighting:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "MODERN LIGHTING  <%s>",
            settings.modern_lighting ? "ON" : "OFF"
        );
        break;
    case RowKind::Grass:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "GRASS  <%s>",
            settings.grass_mode == bumble::graphics_options::GrassMode::Off
                ? "OFF"
                : settings.grass_mode == bumble::graphics_options::GrassMode::Low
                    ? "LOW"
                    : "HIGH"
        );
        break;
    case RowKind::CollisionOverlay:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "COLLISION OVERLAY  <%s>",
            settings.collision_overlay ? "ON" : "OFF"
        );
        break;
    case RowKind::CheatsMenu:
        std::snprintf(buffer, sizeof(buffer), "CHEATS & TESTS");
        break;
    case RowKind::CheatSettings:
        std::snprintf(buffer, sizeof(buffer), "CHEAT SETTINGS");
        break;
    case RowKind::AllWeapons:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "ALL WEAPONS  <%s>",
            settings.all_weapons ? "ON" : "OFF"
        );
        break;
    case RowKind::UnlimitedAmmo:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "UNLIMITED AMMO  <%s>",
            settings.unlimited_ammo ? "ON" : "OFF"
        );
        break;
    case RowKind::UnlimitedHealth:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "UNLIMITED HEALTH  <%s>",
            settings.unlimited_health ? "ON" : "OFF"
        );
        break;
    case RowKind::WaterHazard:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "WATER HAZARD  <%s>",
            settings.honeycomb_water_rescue
                ? "HONEYCOMB RESCUE"
                : "ORIGINAL"
        );
        break;
    case RowKind::AmmoPickupAmount:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "AMMO PICKUP AMOUNT  <%s>",
            settings.double_ammo_pickups ? "2X" : "ORIGINAL"
        );
        break;
    case RowKind::CutsceneTextSpeed:
        std::snprintf(buffer, sizeof(buffer), "CUTSCENE TEXT SPEED <%uX>", settings.cutscene_text_speed);
        break;
    case RowKind::MissionTimeLimits:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "MISSION TIME LIMITS  <%s>",
            settings.double_mission_time_limits ? "2X" : "ORIGINAL"
        );
        break;
    case RowKind::EnemyHealth:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "ENEMY HEALTH  <%s>",
            settings.double_enemy_health ? "2X" : "ORIGINAL"
        );
        break;
    case RowKind::EnemyAwareness:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "ENEMY AWARENESS RANGE  <%s>",
            settings.double_enemy_awareness ? "2X" : "ORIGINAL"
        );
        break;
    case RowKind::AdStrafing:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "A/D STRAFING  <%s>",
            settings.a_d_strafing ? "ENABLED" : "DISABLED"
        );
        break;
    case RowKind::UnlockAllLevels:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "UNLOCK ALL LEVELS  <%s>",
            settings.unlock_all_levels ? "ON" : "OFF"
        );
        break;
    case RowKind::HealthDisplay:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "HEALTH DISPLAY  <%s>",
            settings.honeycomb_health ? "HONEYCOMB" : "CLASSIC BAR"
        );
        break;
    case RowKind::PlayerMaximumHealth:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "PLAYER MAXIMUM HEALTH  <%s>",
            settings.half_player_health ? "HALF" : "NORMAL"
        );
        break;
    case RowKind::ResetDefaultsMenu:
        std::snprintf(buffer, sizeof(buffer), "RESET TO DEFAULTS");
        break;
    case RowKind::ConfirmResetDefaults:
        std::snprintf(buffer, sizeof(buffer), "CONFIRM RESET TO DEFAULTS");
        break;
    case RowKind::EndingCredits:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "TEST ENDING & CREDITS"
        );
        break;
    case RowKind::HighScores:
        std::snprintf(buffer, sizeof(buffer), "HIGH SCORES");
        break;
    case RowKind::ClearSaveDataMenu:
        std::snprintf(buffer, sizeof(buffer), "CLEAR SAVE DATA");
        break;
    case RowKind::ConfirmClearSaveData:
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%s",
            g_native_menu.clear_save_failed
                ? "CLEAR FAILED - RETRY"
                : "CONFIRM CLEAR ALL SAVE DATA"
        );
        break;
    case RowKind::Back:
        std::snprintf(buffer, sizeof(buffer), "BACK");
        break;
    }
    return buffer;
}

const char* row_description(RowKind row) {
    switch (row) {
    case RowKind::TexturePreparation:
        return "Show the enhanced texture choice next time you launch the game.";
    case RowKind::HdTerrain:
        return bumble::first_run::enhanced_textures_available()
            ? "Choose Original, Modern, or Enhanced textures."
            : "Enhanced textures are unavailable because they have not been generated.";
    case RowKind::FramePacing:
        return "Smoother rendering; gameplay stays at 30 Hz. Limited by display refresh.";
    case RowKind::JoystickSettingsMenu:
        return "Tune controller look speed, each axis, deadzone, and inversion.";
    case RowKind::JoystickSensitivity:
        return "Set controller look speed before per-axis tuning.";
    case RowKind::JoystickSensitivityX:
        return "Fine-tune horizontal controller look speed without changing vertical look.";
    case RowKind::JoystickSensitivityY:
        return "Fine-tune vertical controller look speed without changing horizontal look.";
    case RowKind::JoystickDeadzone:
        return "Prevent look-stick drift without changing movement.";
    case RowKind::InvertJoystickX:
        return "Reverse horizontal look direction for the selected look stick.";
    case RowKind::InvertJoystickY:
        return "Reverse vertical look direction for the selected look stick.";
    case RowKind::ResetJoystick:
        return "Restore joystick look speed, axis tuning, deadzone, and inversion defaults.";
    case RowKind::HealthDisplay:
        return "Choose Buck's original health bar or a precise honeycomb display.";
    case RowKind::PlayerMaximumHealth:
        return "Half uses five cells and takes effect on a health reset or mission restart.";
    case RowKind::WaterHazard:
        return "Rescue costs one health cell and returns Buck to his latest safe position.";
    case RowKind::AmmoPickupAmount:
        return "Double finite ammo from pickups while keeping each weapon's original capacity.";
    case RowKind::MissionTimeLimits:
        return "Double mission timers on the next start or restart.";
    case RowKind::EnemyHealth:
        return "Hostile combatants and bosses require twice the damaging hits.";
    case RowKind::EnemyAwareness:
        return "Double hostile response distance; sight, walls, and scripts stay original.";
    case RowKind::AdStrafing:
        return "Disable keyboard strafing for more original-style A/D controls.";
    case RowKind::ResetDefaultsMenu:
        return "Restore all display, interface, graphics, gameplay, and cheat defaults.";
    case RowKind::ConfirmResetDefaults:
        return "Apply the shipped defaults now.";
    case RowKind::Back:
        return "Return to the previous menu.";
    default:
        return "";
    }
}

uint32_t node_address(const NativeMenuState& menu, size_t index) {
    return menu.guest_base + static_cast<uint32_t>(index * kMenuNodeBytes);
}

uint32_t string_address(const NativeMenuState& menu, size_t index) {
    return menu.guest_base + static_cast<uint32_t>(kMaximumRows * kMenuNodeBytes) +
        static_cast<uint32_t>(index * kStringSlotBytes);
}

void refresh_menu_strings_locked() {
    if (!g_native_menu.installed || g_native_menu.rdram == nullptr) {
        return;
    }
    const auto settings = bumble::graphics_options::current();
    for (size_t index = 0; index < g_native_menu.row_count; ++index) {
        const std::string text = row_text(g_native_menu.rows[index], settings);
        write_guest_string(
            g_native_menu.rdram,
            string_address(g_native_menu, index),
            text.c_str()
        );
    }
}

bool allocate_native_menu_locked(uint8_t* rdram) {
    if (g_native_menu.rdram == rdram && g_native_menu.allocation != nullptr) {
        return true;
    }
    if (g_native_menu.allocation != nullptr && g_native_menu.rdram != nullptr) {
        recomp::free(g_native_menu.rdram, g_native_menu.allocation);
    }
    g_native_menu = NativeMenuState{};

    auto* allocation = static_cast<uint8_t*>(
        recomp::alloc(rdram, kMenuAllocationBytes)
    );
    if (allocation == nullptr) {
        return false;
    }
    const ptrdiff_t offset = allocation - rdram;
    if (offset < 0 ||
        static_cast<size_t>(offset) + kMenuAllocationBytes > recomp::mem_size ||
        static_cast<uint64_t>(offset) > UINT32_C(0x7FFFFFFF)) {
        recomp::free(rdram, allocation);
        return false;
    }

    g_native_menu.rdram = rdram;
    g_native_menu.allocation = allocation;
    g_native_menu.guest_base = UINT32_C(0x80000000) +
        static_cast<uint32_t>(offset);
    return true;
}

bool install_native_menu_locked(
    uint8_t* rdram,
    MenuPage page = MenuPage::Root,
    bool native_page_transition = false
) {
    if (!allocate_native_menu_locked(rdram)) {
        return false;
    }

    const bool page_changed =
        !g_native_menu.installed || g_native_menu.page != page ||
        g_mouse_menu_context.load(std::memory_order_acquire) !=
            MouseMenuContext::Options;
    if (page_changed && page == MenuPage::ClearSaveData) {
        g_native_menu.clear_save_failed = false;
    }
    g_native_menu.row_count = 0u;
    g_native_menu.page = page;
    const auto add_row = [](RowKind row) {
        if (g_native_menu.row_count < kMaximumRows) {
            g_native_menu.rows[g_native_menu.row_count++] = row;
        }
    };
    switch (page) {
    case MenuPage::Root:
        add_row(RowKind::ControlsMenu);
        add_row(RowKind::InterfaceHudSettings);
        add_row(RowKind::RenderingSettings);
        add_row(RowKind::GameplaySettings);
        add_row(RowKind::CheatsMenu);
        add_row(RowKind::HighScores);
        add_row(RowKind::ClearSaveDataMenu);
        add_row(RowKind::Back);
        break;
    case MenuPage::Controls:
        add_row(RowKind::InputDevice);
        add_row(RowKind::JoystickSettingsMenu);
        add_row(RowKind::MouseSettingsMenu);
        add_row(RowKind::KeyboardBindingsMenu);
        add_row(RowKind::ControllerBindingsMenu);
        add_row(RowKind::ResetControls);
        add_row(RowKind::Back);
        break;
    case MenuPage::MouseSettings:
        add_row(RowKind::MouseSensitivity);
        add_row(RowKind::MouseSensitivityX);
        add_row(RowKind::MouseSensitivityY);
        add_row(RowKind::MouseAcceleration);
        add_row(RowKind::Back);
        break;
    case MenuPage::JoystickSettings:
        add_row(RowKind::JoystickSensitivity);
        add_row(RowKind::JoystickSensitivityX);
        add_row(RowKind::JoystickSensitivityY);
        add_row(RowKind::JoystickDeadzone);
        add_row(RowKind::InvertJoystickX);
        add_row(RowKind::InvertJoystickY);
        add_row(RowKind::ResetJoystick);
        add_row(RowKind::Back);
        break;
    case MenuPage::InterfaceHud:
        add_row(RowKind::HealthDisplay);
        add_row(RowKind::Back);
        break;
    case MenuPage::Gameplay:
        add_row(RowKind::WaterHazard);
        add_row(RowKind::PlayerMaximumHealth);
        add_row(RowKind::AmmoPickupAmount);
        add_row(RowKind::MissionTimeLimits);
        add_row(RowKind::CutsceneTextSpeed);
        add_row(RowKind::EnemyHealth);
        add_row(RowKind::EnemyAwareness);
        add_row(RowKind::AdStrafing);
        add_row(RowKind::ResetDefaultsMenu);
        add_row(RowKind::Back);
        break;
    case MenuPage::KeyboardBindings:
        add_row(RowKind::BindMoveForward);
        add_row(RowKind::BindMoveBackward);
        add_row(RowKind::BindStrafeLeft);
        add_row(RowKind::BindStrafeRight);
        add_row(RowKind::BindPrimaryFire);
        add_row(RowKind::BindTakeOffLand);
        add_row(RowKind::BindFlyUp);
        add_row(RowKind::BindFlyDown);
        add_row(RowKind::BindPreviousWeapon);
        add_row(RowKind::BindNextWeapon);
        add_row(RowKind::BindLoopDeLoop);
        add_row(RowKind::BindQuickFlip);
        add_row(RowKind::BindForwardDash);
        add_row(RowKind::BindBarrelRoll);
        add_row(RowKind::BindPause);
        add_row(RowKind::BindMenuConfirm);
        add_row(RowKind::BindMenuBack);
        add_row(RowKind::BindToggleModernVisuals);
        add_row(RowKind::ResetKeyboardBindings);
        add_row(RowKind::Back);
        break;
    case MenuPage::ControllerBindings:
        add_row(RowKind::StickLayout);
        add_row(RowKind::BindPrimaryFire);
        add_row(RowKind::BindTakeOffLand);
        add_row(RowKind::BindFlyUp);
        add_row(RowKind::BindFlyDown);
        add_row(RowKind::BindPause);
        add_row(RowKind::BindPreviousWeapon);
        add_row(RowKind::BindNextWeapon);
        add_row(RowKind::BindLoopDeLoop);
        add_row(RowKind::BindQuickFlip);
        add_row(RowKind::BindForwardDash);
        add_row(RowKind::BindBarrelRoll);
        add_row(RowKind::BindMenuConfirm);
        add_row(RowKind::BindMenuBack);
        add_row(RowKind::ResetControllerBindings);
        add_row(RowKind::Back);
        break;
    case MenuPage::Display:
        add_row(RowKind::DisplayMode);
        add_row(RowKind::Resolution);
        add_row(RowKind::Aspect);
        add_row(RowKind::FramePacing);
        add_row(RowKind::Back);
        break;
    case MenuPage::Rendering:
        add_row(RowKind::DisplaySettings);
        add_row(RowKind::Fog);
        add_row(RowKind::HdTerrain);
        if (!bumble::first_run::enhanced_textures_available()) add_row(RowKind::TexturePreparation);
        add_row(RowKind::ModernLighting);
        add_row(RowKind::Back);
        break;
    case MenuPage::CheatsAndTests:
        add_row(RowKind::CheatSettings);
        add_row(RowKind::EndingCredits);
        add_row(RowKind::CollisionOverlay);
        add_row(RowKind::UnlockAllLevels);
        add_row(RowKind::Back);
        break;
    case MenuPage::Cheats:
        add_row(RowKind::AllWeapons);
        add_row(RowKind::UnlimitedAmmo);
        add_row(RowKind::UnlimitedHealth);
        add_row(RowKind::Back);
        break;
    case MenuPage::ClearSaveData:
        add_row(RowKind::ConfirmClearSaveData);
        add_row(RowKind::Back);
        break;
    case MenuPage::ResetDefaults:
        add_row(RowKind::ConfirmResetDefaults);
        add_row(RowKind::Back);
        break;
    }
    if (g_native_menu.row_count == 0u ||
        g_native_menu.row_count > kMaximumRows) {
        return false;
    }

    for (size_t index = 0; index < g_native_menu.row_count; ++index) {
        const uint32_t node = node_address(g_native_menu, index);
        const uint32_t previous = index == 0u
            ? 0u
            : node_address(g_native_menu, index - 1u);
        const uint32_t next = index + 1u == g_native_menu.row_count
            ? 0u
            : node_address(g_native_menu, index + 1u);
        write_guest_u32(rdram, node + 0x00u, previous);
        write_guest_u32(rdram, node + 0x04u, next);
        write_guest_u32(rdram, node + 0x08u, kOriginalItemColour);
        write_guest_u32(rdram, node + 0x0Cu, string_address(g_native_menu, index));
        write_guest_u32(rdram, node + 0x10u, kDirectTextItemFlags);
        write_guest_u32(rdram, node + 0x14u, kPauseMenuItemStyle);
        const RowKind row = g_native_menu.rows[index];
        const uint32_t successor = row == RowKind::EndingCredits
            ? kEndingCreditsPhase
            : row == RowKind::HighScores ? 0x17u
            : (page == MenuPage::Root && row == RowKind::Back
                ? kMainMenuPhase
                : kOptionsPhase);
        write_guest_u32(rdram, node + 0x18u, successor);
        write_guest_u32(rdram, node + 0x1Cu, 0u);
        write_guest_u32(rdram, node + 0x20u, 0u);
        write_guest_u32(rdram, node + 0x24u, 0u);
    }

    g_native_menu.installed = true;
    if (page_changed) {
        g_native_menu.horizontal_latched = false;
        g_native_menu.vertical_latched = false;
        g_native_menu.pending_vertical_direction = 0;
        g_native_menu.pending_vertical_node = 0u;
        g_native_menu.confirm_fallback_not_before =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        g_native_menu.confirm_neutral_observed = false;
        g_native_menu.entry_confirm_suppressed = native_page_transition;
        g_native_menu.confirm_armed = false;
        synchronize_pointer_revisions_for_page_locked(
            g_native_menu.last_pointer_motion_revision
        );
    }
    g_mouse_menu_context.store(
        MouseMenuContext::Options,
        std::memory_order_release
    );
    if (page_changed) {
        begin_menu_transition();
    }
    refresh_menu_strings_locked();

    const uint32_t first = node_address(g_native_menu, 0u);
    const uint32_t selected = page_changed
        ? first
        : g_native_menu.last_selected_node;
    if (page_changed) {
        g_native_menu.last_selected_node = first;
    }
    write_guest_u32(rdram, kOptionsDescriptor + 0x14u, first);
    write_guest_u32(rdram, kOptionsDescriptor + 0x18u, 0u);
    write_guest_u32(rdram, kOptionsDescriptor + 0x38u, selected);

    std::fprintf(
        stderr,
        "BUMBLE_GRAPHICS_OPTIONS stage=guest_menu_installed"
        " phase=0x%08" PRIX32 " descriptor=0x%08" PRIX32
        " original_first=0x%08" PRIX32 " native_first=0x%08" PRIX32
        " rows=%zu capacity=%zu page=%s"
        " direct_text=1 compact_style=0x%08" PRIX32
        " ray_tracing_row=0"
        " ending_credits_phase=0x%08" PRIX32
        " ending_level=%" PRIu32
        " node_bytes=%" PRIu32 " string_slot_bytes=%" PRIu32 "\n",
        kOptionsPhase,
        kOptionsDescriptor,
        kOriginalOptionsFirstItem,
        first,
        g_native_menu.row_count,
        kMaximumRows,
        menu_page_name(page),
        kPauseMenuItemStyle,
        kEndingCreditsPhase,
        kEndingCreditsLevelIndex,
        kMenuNodeBytes,
        kStringSlotBytes
    );
    std::fflush(stderr);
    return true;
}

void post_windowed_resolution_if_needed(
    const bumble::graphics_options::Settings& settings
) {
    if (settings.display_mode !=
            bumble::graphics_options::DisplayMode::BorderedWindow) {
        return;
    }
#if defined(_WIN32)
    const NativeWindow game_window =
        g_game_window.load(std::memory_order_acquire);
    if (game_window != nullptr && IsWindow(game_window)) {
        PostMessageW(game_window, kApplyWindowedResolutionMessage, 0, 0);
    }
#else
    g_pending_window_size.store(
        (static_cast<uint64_t>(settings.resolution_width) << 32u) |
            settings.resolution_height,
        std::memory_order_release
    );
#endif
}

void apply_settings(const bumble::graphics_options::Settings& requested) {
    const auto previous = bumble::graphics_options::current();
    auto settings = requested;
    publish_settings(settings);
    RT64::setBumbleCollisionDebugMode(
        settings.collision_overlay ? 2u : 0u
    );
    save_settings(settings);

    auto graphics_config = ultramodern::renderer::get_graphics_config();
    bumble::graphics_options::apply_to_graphics_config(graphics_config);
    ultramodern::renderer::set_graphics_config(graphics_config);
    bumble::rt64_renderer::set_fog_scale(bumble::graphics_options::fog_scale());
    if (settings.resolution_width != previous.resolution_width ||
        settings.resolution_height != previous.resolution_height) {
        post_windowed_resolution_if_needed(settings);
    }

    std::fprintf(
        stderr,
        "BUMBLE_GRAPHICS_OPTIONS stage=applied display=%s"
        " resolution=%" PRIu32 "x%" PRIu32 " aspect=%s fog=%s"
        " frame_pacing=%s hd_terrain=%d modern_lighting=%d grass=%s"
        " collision_overlay=%d"
        " all_weapons=%d unlimited_ammo=%d unlimited_health=%d"
        " honeycomb_water_rescue=%d ray_traced_lighting=%d\n",
        settings.display_mode ==
                bumble::graphics_options::DisplayMode::Fullscreen
            ? "fullscreen"
            : "bordered_window",
        settings.resolution_width,
        settings.resolution_height,
        settings.aspect_mode ==
                bumble::graphics_options::AspectMode::Widescreen
            ? "widescreen"
            : "4x3",
        settings.fog_mode == bumble::graphics_options::FogMode::None
            ? "none"
            : settings.fog_mode == bumble::graphics_options::FogMode::Half
                ? "half"
                : "full",
        settings.frame_pacing ==
                bumble::graphics_options::FramePacing::Interpolated120Hz
            ? "interpolated_120hz"
            : settings.frame_pacing ==
                    bumble::graphics_options::FramePacing::Interpolated60Hz
                ? "interpolated_60hz"
                : "original_30hz",
        settings.hd_terrain ? 1 : 0,
        settings.modern_lighting ? 1 : 0,
        settings.grass_mode == bumble::graphics_options::GrassMode::Off
            ? "off"
            : settings.grass_mode == bumble::graphics_options::GrassMode::Low
                ? "low"
                : "high",
        settings.collision_overlay ? 1 : 0,
        settings.all_weapons ? 1 : 0,
        settings.unlimited_ammo ? 1 : 0,
        settings.unlimited_health ? 1 : 0,
        settings.honeycomb_water_rescue ? 1 : 0,
        0
    );
    std::fflush(stderr);
}

void reset_joystick_settings() {
    auto controls = bumble::input_bindings::current();
    const auto defaults = bumble::input_bindings::default_settings();
    controls.joystick_look_sensitivity = defaults.joystick_look_sensitivity;
    controls.joystick_look_sensitivity_x =
        defaults.joystick_look_sensitivity_x;
    controls.joystick_look_sensitivity_y =
        defaults.joystick_look_sensitivity_y;
    controls.joystick_look_deadzone = defaults.joystick_look_deadzone;
    controls.invert_joystick_look_x = defaults.invert_joystick_look_x;
    controls.invert_joystick_look_y = defaults.invert_joystick_look_y;
    bumble::input_bindings::configure(controls);
}

void cycle_row_locked(RowKind row, int direction) {
    auto settings = bumble::graphics_options::current();
    const int step = direction < 0 ? -1 : 1;
    switch (row) {
    case RowKind::TexturePreparation:
        bumble::first_run::request_texture_prompt();
        break;
    case RowKind::InputDevice:
        bumble::input_bindings::cycle_device_mode(step);
        break;
    case RowKind::MouseSensitivity:
    case RowKind::MouseSensitivityX:
    case RowKind::MouseSensitivityY:
    case RowKind::MouseAcceleration: {
        auto controls = bumble::input_bindings::current();
        if (row == RowKind::MouseAcceleration) controls.mouse_acceleration = !controls.mouse_acceleration;
        else {
            auto& value = row == RowKind::MouseSensitivity ? controls.mouse_sensitivity :
                row == RowKind::MouseSensitivityX ? controls.mouse_sensitivity_x : controls.mouse_sensitivity_y;
            value = uint32_t(std::clamp(int(value) + step * 5, 10, 400));
        }
        bumble::input_bindings::configure(controls);
        break;
    }
    case RowKind::StickLayout:
        bumble::input_bindings::cycle_stick_layout(step);
        break;
    case RowKind::JoystickSensitivity:
    case RowKind::JoystickSensitivityX:
    case RowKind::JoystickSensitivityY:
    case RowKind::JoystickDeadzone:
    case RowKind::InvertJoystickX:
    case RowKind::InvertJoystickY: {
        auto controls = bumble::input_bindings::current();
        const auto adjust = [step](uint32_t value, uint32_t minimum, uint32_t maximum, uint32_t increment) {
            const int next = static_cast<int>(value) + step * static_cast<int>(increment);
            return static_cast<uint32_t>(std::clamp(next, static_cast<int>(minimum), static_cast<int>(maximum)));
        };
        if (row == RowKind::JoystickSensitivity) {
            controls.joystick_look_sensitivity = adjust(controls.joystick_look_sensitivity, 25u, 200u, 5u);
        } else if (row == RowKind::JoystickSensitivityX) {
            controls.joystick_look_sensitivity_x = adjust(controls.joystick_look_sensitivity_x, 50u, 150u, 5u);
        } else if (row == RowKind::JoystickSensitivityY) {
            controls.joystick_look_sensitivity_y = adjust(controls.joystick_look_sensitivity_y, 50u, 150u, 5u);
        } else if (row == RowKind::JoystickDeadzone) {
            controls.joystick_look_deadzone = adjust(
                controls.joystick_look_deadzone, 0u, 16384u, 328u
            );
        } else if (row == RowKind::InvertJoystickX) {
            controls.invert_joystick_look_x = !controls.invert_joystick_look_x;
        } else {
            controls.invert_joystick_look_y = !controls.invert_joystick_look_y;
        }
        bumble::input_bindings::configure(controls);
        break;
    }
    case RowKind::DisplayMode:
        settings.display_mode = settings.display_mode ==
                bumble::graphics_options::DisplayMode::BorderedWindow
            ? bumble::graphics_options::DisplayMode::Fullscreen
            : bumble::graphics_options::DisplayMode::BorderedWindow;
        break;
    case RowKind::Resolution: {
        if (g_resolution_choices.empty()) {
            break;
        }
        const size_t current_index = closest_resolution_index(
            g_resolution_choices,
            settings.resolution_width,
            settings.resolution_height
        );
        const int count = static_cast<int>(g_resolution_choices.size());
        const int next_index =
            (static_cast<int>(current_index) + step + count) % count;
        settings.resolution_width =
            g_resolution_choices[static_cast<size_t>(next_index)].first;
        settings.resolution_height =
            g_resolution_choices[static_cast<size_t>(next_index)].second;
        break;
    }
    case RowKind::Aspect:
        settings.aspect_mode = settings.aspect_mode ==
                bumble::graphics_options::AspectMode::Standard4x3
            ? bumble::graphics_options::AspectMode::Widescreen
            : bumble::graphics_options::AspectMode::Standard4x3;
        break;
    case RowKind::FramePacing: {
        constexpr int count = 3;
        const int current = static_cast<int>(settings.frame_pacing);
        settings.frame_pacing =
            static_cast<bumble::graphics_options::FramePacing>(
                (current + step + count) % count
            );
        break;
    }
    case RowKind::Fog: {
        constexpr int count = 3;
        const int current = static_cast<int>(settings.fog_mode);
        settings.fog_mode = static_cast<bumble::graphics_options::FogMode>(
            (current + step + count) % count
        );
        break;
    }
    case RowKind::HdTerrain:
        {
            const int mode = !settings.hd_terrain ? 0 : settings.enhanced_textures ? 2 : 1;
            const int count = bumble::first_run::enhanced_textures_available() ? 3 : 2;
            const int next = (mode + step + count) % count;
            settings.hd_terrain = next != 0;
            settings.enhanced_textures = next == 2;
        }
        break;
    case RowKind::ModernLighting:
        settings.modern_lighting = !settings.modern_lighting;
        break;
    case RowKind::Grass: {
        constexpr int count = 3;
        const int current = static_cast<int>(settings.grass_mode);
        settings.grass_mode = static_cast<bumble::graphics_options::GrassMode>(
            (current + step + count) % count
        );
        break;
    }
    case RowKind::CollisionOverlay:
        settings.collision_overlay = !settings.collision_overlay;
        break;
    case RowKind::AllWeapons:
        settings.all_weapons = !settings.all_weapons;
        break;
    case RowKind::UnlimitedAmmo:
        settings.unlimited_ammo = !settings.unlimited_ammo;
        break;
    case RowKind::UnlimitedHealth:
        settings.unlimited_health = !settings.unlimited_health;
        break;
    case RowKind::WaterHazard:
        settings.honeycomb_water_rescue =
            !settings.honeycomb_water_rescue;
        break;
    case RowKind::AmmoPickupAmount:
        settings.double_ammo_pickups = !settings.double_ammo_pickups;
        break;
    case RowKind::CutsceneTextSpeed:
        settings.cutscene_text_speed = step > 0
            ? (settings.cutscene_text_speed == 4u ? 1u : settings.cutscene_text_speed * 2u)
            : (settings.cutscene_text_speed == 1u ? 4u : settings.cutscene_text_speed / 2u);
        break;
    case RowKind::MissionTimeLimits:
        settings.double_mission_time_limits =
            !settings.double_mission_time_limits;
        break;
    case RowKind::EnemyHealth:
        settings.double_enemy_health = !settings.double_enemy_health;
        break;
    case RowKind::EnemyAwareness:
        settings.double_enemy_awareness = !settings.double_enemy_awareness;
        break;
    case RowKind::AdStrafing:
        settings.a_d_strafing = !settings.a_d_strafing;
        break;
    case RowKind::UnlockAllLevels:
        settings.unlock_all_levels = !settings.unlock_all_levels;
        break;
    case RowKind::HealthDisplay:
        settings.honeycomb_health = !settings.honeycomb_health;
        break;
    case RowKind::PlayerMaximumHealth:
        settings.half_player_health = !settings.half_player_health;
        break;
    case RowKind::ControlsMenu:
    case RowKind::InterfaceHudSettings:
    case RowKind::GameplaySettings:
    case RowKind::KeyboardBindingsMenu:
    case RowKind::ControllerBindingsMenu:
    case RowKind::MouseSettingsMenu:
    case RowKind::JoystickSettingsMenu:
    case RowKind::ResetJoystick:
    case RowKind::ResetControls:
    case RowKind::BindFlyUp:
    case RowKind::BindFlyDown:
    case RowKind::BindMoveForward:
    case RowKind::BindMoveBackward:
    case RowKind::BindStrafeLeft:
    case RowKind::BindStrafeRight:
    case RowKind::BindPrimaryFire:
    case RowKind::BindTakeOffLand:
    case RowKind::BindPreviousWeapon:
    case RowKind::BindNextWeapon:
    case RowKind::BindLoopDeLoop:
    case RowKind::BindQuickFlip:
    case RowKind::BindForwardDash:
    case RowKind::BindBarrelRoll:
    case RowKind::BindPause:
    case RowKind::BindMenuConfirm:
    case RowKind::BindMenuBack:
    case RowKind::BindToggleModernVisuals:
    case RowKind::ResetKeyboardBindings:
    case RowKind::ResetControllerBindings:
    case RowKind::DisplaySettings:
    case RowKind::RenderingSettings:
    case RowKind::CheatsMenu:
    case RowKind::CheatSettings:
    case RowKind::ClearSaveDataMenu:
    case RowKind::ConfirmClearSaveData:
    case RowKind::ResetDefaultsMenu:
    case RowKind::ConfirmResetDefaults:
    case RowKind::Back:
    case RowKind::EndingCredits:
    case RowKind::HighScores:
        return;
    }

    apply_settings(settings);
    refresh_menu_strings_locked();
    std::fprintf(
        stderr,
        "BUMBLE_GRAPHICS_OPTIONS stage=guest_menu_action row=%s direction=%d\n",
        row_name(row),
        step
    );
    std::fflush(stderr);
}

const char* pause_page_name(PauseMenuPage page) {
    switch (page) {
    case PauseMenuPage::Root: return "root";
    case PauseMenuPage::Restart: return "restart";
    case PauseMenuPage::Options: return "options";
    case PauseMenuPage::Controls: return "controls";
    case PauseMenuPage::InterfaceHud: return "interface_hud";
    case PauseMenuPage::Gameplay: return "gameplay";
    case PauseMenuPage::KeyboardBindings: return "keyboard_bindings";
    case PauseMenuPage::ControllerBindings: return "controller_bindings";
    case PauseMenuPage::MouseSettings: return "mouse_settings";
    case PauseMenuPage::JoystickSettings: return "joystick_settings";
    case PauseMenuPage::Graphics: return "graphics";
    case PauseMenuPage::Display: return "display";
    case PauseMenuPage::Rendering: return "rendering";
    case PauseMenuPage::Cheats: return "cheats";
    case PauseMenuPage::Exit: return "exit";
    case PauseMenuPage::ConfirmTitle: return "confirm_title";
    default: return "unknown";
    }
}

const char* pause_row_name(PauseRowKind row) {
    switch (row) {
    case PauseRowKind::Resume: return "resume";
    case PauseRowKind::RestartMenu: return "restart_menu";
    case PauseRowKind::RestartBeginning: return "restart_beginning";
    case PauseRowKind::RestartCheckpoint: return "restart_checkpoint";
    case PauseRowKind::CancelRestart: return "cancel_restart";
    case PauseRowKind::OptionsMenu: return "options_menu";
    case PauseRowKind::InterfaceHudMenu: return "interface_hud_menu";
    case PauseRowKind::GameplayMenu: return "gameplay_menu";
    case PauseRowKind::GraphicsMenu: return "graphics_menu";
    case PauseRowKind::ControlsMenu: return "controls_menu";
    case PauseRowKind::InputDevice: return "input_device";
    case PauseRowKind::KeyboardBindingsMenu: return "keyboard_bindings_menu";
    case PauseRowKind::ControllerBindingsMenu: return "controller_bindings_menu";
    case PauseRowKind::JoystickSettingsMenu: return "joystick_settings_menu";
    case PauseRowKind::MouseSettingsMenu: return "mouse_settings_menu";
    case PauseRowKind::MouseSensitivity: return "MouseSensitivity";
    case PauseRowKind::MouseSensitivityX: return "MouseSensitivityX";
    case PauseRowKind::MouseSensitivityY: return "MouseSensitivityY";
    case PauseRowKind::MouseAcceleration: return "MouseAcceleration";
    case PauseRowKind::BindFlyUp: return "BindFlyUp";
    case PauseRowKind::BindFlyDown: return "BindFlyDown";
    case PauseRowKind::StickLayout: return "stick_layout";
    case PauseRowKind::JoystickSensitivity: return "joystick_sensitivity";
    case PauseRowKind::JoystickSensitivityX: return "joystick_sensitivity_x";
    case PauseRowKind::JoystickSensitivityY: return "joystick_sensitivity_y";
    case PauseRowKind::JoystickDeadzone: return "joystick_deadzone";
    case PauseRowKind::InvertJoystickX: return "invert_joystick_x";
    case PauseRowKind::InvertJoystickY: return "invert_joystick_y";
    case PauseRowKind::ResetJoystick: return "reset_joystick";
    case PauseRowKind::BindMoveForward: return "bind_move_forward";
    case PauseRowKind::BindMoveBackward: return "bind_move_backward";
    case PauseRowKind::BindStrafeLeft: return "bind_strafe_left";
    case PauseRowKind::BindStrafeRight: return "bind_strafe_right";
    case PauseRowKind::BindPrimaryFire: return "bind_primary_fire";
    case PauseRowKind::BindTakeOffLand: return "bind_take_off_land";
    case PauseRowKind::BindPreviousWeapon: return "bind_previous_weapon";
    case PauseRowKind::BindNextWeapon: return "bind_next_weapon";
    case PauseRowKind::BindLoopDeLoop: return "bind_loop_de_loop";
    case PauseRowKind::BindQuickFlip: return "bind_quick_flip";
    case PauseRowKind::BindForwardDash: return "bind_forward_dash";
    case PauseRowKind::BindBarrelRoll: return "bind_barrel_roll";
    case PauseRowKind::BindPause: return "bind_pause";
    case PauseRowKind::BindMenuConfirm: return "bind_menu_confirm";
    case PauseRowKind::BindMenuBack: return "bind_menu_back";
    case PauseRowKind::BindToggleModernVisuals: return "bind_toggle_modern_visuals";
    case PauseRowKind::ResetKeyboardBindings: return "reset_keyboard_bindings";
    case PauseRowKind::ResetControllerBindings: return "reset_controller_bindings";
    case PauseRowKind::CheatsMenu: return "cheats_menu";
    case PauseRowKind::ExitMenu: return "exit_menu";
    case PauseRowKind::DisplaySettings: return "display_settings";
    case PauseRowKind::RenderingSettings: return "rendering_settings";
    case PauseRowKind::ReturnToTitle: return "return_to_title";
    case PauseRowKind::QuitGame: return "quit_game";
    case PauseRowKind::ConfirmReturnToTitle: return "confirm_return_to_title";
    case PauseRowKind::CancelReturnToTitle: return "cancel_return_to_title";
    case PauseRowKind::DisplayMode: return "display_mode";
    case PauseRowKind::Resolution: return "resolution";
    case PauseRowKind::Aspect: return "aspect";
    case PauseRowKind::FramePacing: return "frame_pacing";
    case PauseRowKind::Fog: return "fog";
    case PauseRowKind::HdTerrain: return "hd_terrain";
    case PauseRowKind::ModernLighting: return "modern_lighting";
    case PauseRowKind::CollisionOverlay: return "collision_overlay";
    case PauseRowKind::AllWeapons: return "all_weapons";
    case PauseRowKind::UnlimitedAmmo: return "unlimited_ammo";
    case PauseRowKind::UnlimitedHealth: return "unlimited_health";
    case PauseRowKind::UnlockAllLevels: return "unlock_all_levels";
    case PauseRowKind::HealthDisplay: return "health_display";
    case PauseRowKind::WaterHazard: return "water_hazard";
    case PauseRowKind::PlayerMaximumHealth: return "player_maximum_health";
    case PauseRowKind::AmmoPickupAmount: return "ammo_pickup_amount";
    case PauseRowKind::CutsceneTextSpeed: return "cutscene_text_speed";
    case PauseRowKind::MissionTimeLimits: return "mission_time_limits";
    case PauseRowKind::EnemyHealth: return "enemy_health";
    case PauseRowKind::EnemyAwareness: return "enemy_awareness";
    case PauseRowKind::AdStrafing: return "a_d_strafing";
    case PauseRowKind::Back: return "back";
    default: return "unknown";
    }
}

bool pause_row_setting(PauseRowKind pause_row, RowKind& setting_row) {
    switch (pause_row) {
    case PauseRowKind::InputDevice:
        setting_row = RowKind::InputDevice;
        return true;
    case PauseRowKind::StickLayout:
        setting_row = RowKind::StickLayout;
        return true;
    case PauseRowKind::MouseSensitivity:
        setting_row = RowKind::MouseSensitivity;
        return true;
    case PauseRowKind::MouseSensitivityX:
        setting_row = RowKind::MouseSensitivityX;
        return true;
    case PauseRowKind::MouseSensitivityY:
        setting_row = RowKind::MouseSensitivityY;
        return true;
    case PauseRowKind::MouseAcceleration:
        setting_row = RowKind::MouseAcceleration;
        return true;
    case PauseRowKind::JoystickSensitivity:
        setting_row = RowKind::JoystickSensitivity;
        return true;
    case PauseRowKind::JoystickSensitivityX:
        setting_row = RowKind::JoystickSensitivityX;
        return true;
    case PauseRowKind::JoystickSensitivityY:
        setting_row = RowKind::JoystickSensitivityY;
        return true;
    case PauseRowKind::JoystickDeadzone:
        setting_row = RowKind::JoystickDeadzone;
        return true;
    case PauseRowKind::InvertJoystickX:
        setting_row = RowKind::InvertJoystickX;
        return true;
    case PauseRowKind::InvertJoystickY:
        setting_row = RowKind::InvertJoystickY;
        return true;
    case PauseRowKind::ResetJoystick:
        setting_row = RowKind::ResetJoystick;
        return true;
    case PauseRowKind::BindFlyUp:
        setting_row = RowKind::BindFlyUp;
        return true;
    case PauseRowKind::BindFlyDown:
        setting_row = RowKind::BindFlyDown;
        return true;
    case PauseRowKind::BindMoveForward:
        setting_row = RowKind::BindMoveForward;
        return true;
    case PauseRowKind::BindMoveBackward:
        setting_row = RowKind::BindMoveBackward;
        return true;
    case PauseRowKind::BindStrafeLeft:
        setting_row = RowKind::BindStrafeLeft;
        return true;
    case PauseRowKind::BindStrafeRight:
        setting_row = RowKind::BindStrafeRight;
        return true;
    case PauseRowKind::BindPrimaryFire:
        setting_row = RowKind::BindPrimaryFire;
        return true;
    case PauseRowKind::BindTakeOffLand:
        setting_row = RowKind::BindTakeOffLand;
        return true;
    case PauseRowKind::BindPreviousWeapon:
        setting_row = RowKind::BindPreviousWeapon;
        return true;
    case PauseRowKind::BindNextWeapon:
        setting_row = RowKind::BindNextWeapon;
        return true;
    case PauseRowKind::BindLoopDeLoop:
        setting_row = RowKind::BindLoopDeLoop;
        return true;
    case PauseRowKind::BindQuickFlip:
        setting_row = RowKind::BindQuickFlip;
        return true;
    case PauseRowKind::BindForwardDash:
        setting_row = RowKind::BindForwardDash;
        return true;
    case PauseRowKind::BindBarrelRoll:
        setting_row = RowKind::BindBarrelRoll;
        return true;
    case PauseRowKind::BindPause:
        setting_row = RowKind::BindPause;
        return true;
    case PauseRowKind::BindMenuConfirm:
        setting_row = RowKind::BindMenuConfirm;
        return true;
    case PauseRowKind::BindMenuBack:
        setting_row = RowKind::BindMenuBack;
        return true;
    case PauseRowKind::BindToggleModernVisuals:
        setting_row = RowKind::BindToggleModernVisuals;
        return true;
    case PauseRowKind::DisplayMode:
        setting_row = RowKind::DisplayMode;
        return true;
    case PauseRowKind::Resolution:
        setting_row = RowKind::Resolution;
        return true;
    case PauseRowKind::Aspect:
        setting_row = RowKind::Aspect;
        return true;
    case PauseRowKind::FramePacing:
        setting_row = RowKind::FramePacing;
        return true;
    case PauseRowKind::Fog:
        setting_row = RowKind::Fog;
        return true;
    case PauseRowKind::HdTerrain:
        setting_row = RowKind::HdTerrain;
        return true;
    case PauseRowKind::ModernLighting:
        setting_row = RowKind::ModernLighting;
        return true;
    case PauseRowKind::CollisionOverlay:
        setting_row = RowKind::CollisionOverlay;
        return true;
    case PauseRowKind::AllWeapons:
        setting_row = RowKind::AllWeapons;
        return true;
    case PauseRowKind::UnlimitedAmmo:
        setting_row = RowKind::UnlimitedAmmo;
        return true;
    case PauseRowKind::UnlimitedHealth:
        setting_row = RowKind::UnlimitedHealth;
        return true;
    case PauseRowKind::UnlockAllLevels:
        setting_row = RowKind::UnlockAllLevels;
        return true;
    case PauseRowKind::HealthDisplay:
        setting_row = RowKind::HealthDisplay;
        return true;
    case PauseRowKind::WaterHazard:
        setting_row = RowKind::WaterHazard;
        return true;
    case PauseRowKind::PlayerMaximumHealth:
        setting_row = RowKind::PlayerMaximumHealth;
        return true;
    case PauseRowKind::AmmoPickupAmount:
        setting_row = RowKind::AmmoPickupAmount;
        return true;
    case PauseRowKind::CutsceneTextSpeed:
        setting_row = RowKind::CutsceneTextSpeed;
        return true;
    case PauseRowKind::MissionTimeLimits:
        setting_row = RowKind::MissionTimeLimits;
        return true;
    case PauseRowKind::EnemyHealth:
        setting_row = RowKind::EnemyHealth;
        return true;
    case PauseRowKind::EnemyAwareness:
        setting_row = RowKind::EnemyAwareness;
        return true;
    case PauseRowKind::AdStrafing:
        setting_row = RowKind::AdStrafing;
        return true;
    default:
        return false;
    }
}

bool pause_keyboard_bindings_page(PauseMenuPage page) {
    return page == PauseMenuPage::KeyboardBindings;
}

bool pause_controller_bindings_page(PauseMenuPage page) {
    return page == PauseMenuPage::ControllerBindings;
}

bumble::input_bindings::BindingProfile pause_binding_profile(
    PauseMenuPage page
) {
    return pause_controller_bindings_page(page)
        ? bumble::input_bindings::BindingProfile::Controller
        : bumble::input_bindings::BindingProfile::KeyboardMouse;
}

std::string pause_row_text(
    PauseRowKind row,
    const bumble::graphics_options::Settings& settings
) {
    switch (row) {
    case PauseRowKind::Resume: return "RESUME";
    case PauseRowKind::RestartMenu: return "RESTART MISSION";
    case PauseRowKind::RestartBeginning: return "RESTART FROM BEGINNING";
    case PauseRowKind::RestartCheckpoint:
        return bumble::native_checkpoint::mission_checkpoint_available()
            ? "RESTART FROM LAST CHECKPOINT"
            : "RESTART FROM LAST CHECKPOINT  <UNAVAILABLE>";
    case PauseRowKind::CancelRestart: return "CANCEL";
    case PauseRowKind::OptionsMenu: return "OPTIONS";
    case PauseRowKind::InterfaceHudMenu: return "INTERFACE / HUD";
    case PauseRowKind::GameplayMenu: return "GAMEPLAY";
    case PauseRowKind::GraphicsMenu: return "GRAPHICS";
    case PauseRowKind::ControlsMenu: return "CONTROLS";
    case PauseRowKind::KeyboardBindingsMenu:
        return "KEYBOARD & MOUSE BINDINGS";
    case PauseRowKind::ControllerBindingsMenu:
        return bumble::native_io::connected_controller_count() != 0u
            ? "CONTROLLER BINDINGS  <CONNECTED>"
            : "CONTROLLER BINDINGS  <NOT CONNECTED>";
    case PauseRowKind::MouseSettingsMenu: return "MOUSE SETTINGS";
    case PauseRowKind::JoystickSettingsMenu:
        return "JOYSTICK SETTINGS";
    case PauseRowKind::InputDevice:
        return row_text(RowKind::InputDevice, settings);
    case PauseRowKind::StickLayout:
        return row_text(RowKind::StickLayout, settings);
    case PauseRowKind::BindFlyUp:
    case PauseRowKind::BindFlyDown:
    case PauseRowKind::BindMoveForward:
    case PauseRowKind::BindMoveBackward:
    case PauseRowKind::BindStrafeLeft:
    case PauseRowKind::BindStrafeRight:
    case PauseRowKind::BindPrimaryFire:
    case PauseRowKind::BindTakeOffLand:
    case PauseRowKind::BindPreviousWeapon:
    case PauseRowKind::BindNextWeapon:
    case PauseRowKind::BindLoopDeLoop:
    case PauseRowKind::BindQuickFlip:
    case PauseRowKind::BindForwardDash:
    case PauseRowKind::BindBarrelRoll:
    case PauseRowKind::BindPause:
    case PauseRowKind::BindMenuConfirm:
    case PauseRowKind::BindToggleModernVisuals:
    case PauseRowKind::BindMenuBack: {
        RowKind binding_row{};
        bumble::input_bindings::InputAction action{};
        if (!pause_row_setting(row, binding_row) ||
            !row_binding_action(binding_row, action)) {
            return {};
        }
        const auto profile = pause_binding_profile(g_pause_menu.page);
        const auto capture = bumble::input_bindings::capture_state();
        const bool capturing = capture.active &&
            capture.profile == profile && capture.action == action;
        const std::string value = capturing
            ? (capture.waiting_for_neutral
                ? "RELEASE CONTROLS"
                : (profile ==
                        bumble::input_bindings::BindingProfile::Controller
                    ? "PRESS BUTTON - BACK CANCELS"
                    : "PRESS KEY - BACKSPACE CANCELS"))
            : bumble::input_bindings::binding_text(profile, action);
        return std::string(
            bumble::input_bindings::action_label(action)
        ) + "  <" + value + ">";
    }
    case PauseRowKind::ResetKeyboardBindings:
        return "RESET KEYBOARD & MOUSE";
    case PauseRowKind::ResetControllerBindings:
        return "RESET CONTROLLER";
    case PauseRowKind::CheatsMenu: return "CHEATS";
    case PauseRowKind::ExitMenu: return "QUIT";
    case PauseRowKind::DisplaySettings: return "DISPLAY SETTINGS";
    case PauseRowKind::RenderingSettings: return "RENDERING SETTINGS";
    case PauseRowKind::ReturnToTitle: return "RETURN TO TITLE";
    case PauseRowKind::QuitGame: return "QUIT GAME";
    case PauseRowKind::ConfirmReturnToTitle: return "YES - RETURN TO TITLE";
    case PauseRowKind::CancelReturnToTitle: return "CANCEL";
    case PauseRowKind::Back: return "BACK";
    default:
        RowKind setting{};
        if (pause_row_setting(row, setting)) {
            return row_text(setting, settings);
        }
        return {};
    }
}

PauseMenuPage pause_parent_page(PauseMenuPage page) {
    switch (page) {
    case PauseMenuPage::Display:
    case PauseMenuPage::Rendering:
        return PauseMenuPage::Graphics;
    case PauseMenuPage::Graphics:
    case PauseMenuPage::Controls:
    case PauseMenuPage::InterfaceHud:
    case PauseMenuPage::Gameplay:
        return PauseMenuPage::Options;
    case PauseMenuPage::Options:
    case PauseMenuPage::Cheats:
    case PauseMenuPage::Exit:
    case PauseMenuPage::Restart:
        return PauseMenuPage::Root;
    case PauseMenuPage::KeyboardBindings:
    case PauseMenuPage::ControllerBindings:
    case PauseMenuPage::MouseSettings:
    case PauseMenuPage::JoystickSettings:
        return PauseMenuPage::Controls;
    case PauseMenuPage::ConfirmTitle:
        return PauseMenuPage::Exit;
    case PauseMenuPage::Root:
    default:
        return PauseMenuPage::Root;
    }
}

void refresh_pause_menu_strings_locked() {
    if (!g_pause_menu.installed || g_pause_menu.rdram == nullptr) {
        return;
    }
    const auto settings = bumble::graphics_options::current();
    for (size_t index = 0; index < g_pause_menu.row_count; ++index) {
        const std::string text = pause_row_text(
            g_pause_menu.rows[index],
            settings
        );
        write_guest_string(
            g_pause_menu.rdram,
            pause_string_address(index),
            text.c_str()
        );
    }
}

void synchronize_control_settings_locked() {
    const uint64_t revision = bumble::input_bindings::revision();
    const size_t controller_count =
        bumble::native_io::connected_controller_count();
    if (revision == g_last_controls_revision &&
        controller_count == g_last_connected_controller_count) {
        return;
    }
    if (revision != g_last_controls_revision) {
        save_control_settings();
    }
    g_last_controls_revision = revision;
    g_last_connected_controller_count = controller_count;
    refresh_menu_strings_locked();
    refresh_pause_menu_strings_locked();
}

bool install_pause_menu_locked(uint8_t* rdram, PauseMenuPage page) {
    const uint32_t descriptor_header = read_guest_u32(
        rdram,
        kPauseMenuDescriptor + 0x08u
    );
    const uint32_t descriptor_first = read_guest_u32(
        rdram,
        kPauseMenuDescriptor + 0x14u
    );
    const bool current_native_first =
        g_pause_menu.rdram == rdram && g_pause_menu.allocation != nullptr &&
        descriptor_first == pause_node_address(0u);
    const bool original_pause_contract =
        descriptor_header == kPauseMenuOriginalHeader &&
        (descriptor_first == kPauseMenuOriginalFirstItem ||
            current_native_first) &&
        read_guest_u32(rdram, kPauseMenuOriginalFirstItem + 0x04u) ==
            0x800FE810u &&
        read_guest_u32(rdram, kPauseMenuOriginalTail + 0x04u) == 0u &&
        read_guest_u32(rdram, kPauseMenuOriginalTail + 0x10u) ==
            kOriginalLinkedItemFlags &&
        read_guest_u32(rdram, kPauseMenuOriginalTail + 0x14u) ==
            kPauseMenuItemStyle &&
        read_guest_u32(rdram, kPauseMenuOriginalTail + 0x18u) == 6u;
    if (!original_pause_contract || !allocate_pause_menu_locked(rdram)) {
        return false;
    }

    const bool entering_pause = !g_pause_menu.installed;
    const bool staged_handoff = entering_pause && g_pause_overlay_staged;
    if (entering_pause && !staged_handoff) {
        bumble::text_overlay::clear_kind(
            bumble::text_overlay::TextKind::Menu
        );
    }
    g_pause_menu.row_count = 0u;
    g_pause_menu.page = page;
    const auto add_row = [](PauseRowKind row) {
        if (g_pause_menu.row_count < kPauseMenuMaximumRows) {
            g_pause_menu.rows[g_pause_menu.row_count++] = row;
        }
    };
    switch (page) {
    case PauseMenuPage::Root:
        add_row(PauseRowKind::Resume);
        add_row(PauseRowKind::RestartMenu);
        add_row(PauseRowKind::OptionsMenu);
        add_row(PauseRowKind::CheatsMenu);
        add_row(PauseRowKind::ExitMenu);
        break;
    case PauseMenuPage::Restart:
        add_row(PauseRowKind::RestartBeginning);
        add_row(PauseRowKind::RestartCheckpoint);
        add_row(PauseRowKind::CancelRestart);
        break;
    case PauseMenuPage::Options:
        add_row(PauseRowKind::ControlsMenu);
        add_row(PauseRowKind::InterfaceHudMenu);
        add_row(PauseRowKind::GraphicsMenu);
        add_row(PauseRowKind::GameplayMenu);
        add_row(PauseRowKind::Back);
        break;
    case PauseMenuPage::Controls:
        add_row(PauseRowKind::InputDevice);
        add_row(PauseRowKind::JoystickSettingsMenu);
        add_row(PauseRowKind::MouseSettingsMenu);
        add_row(PauseRowKind::KeyboardBindingsMenu);
        add_row(PauseRowKind::ControllerBindingsMenu);
        add_row(PauseRowKind::Back);
        break;
    case PauseMenuPage::MouseSettings:
        add_row(PauseRowKind::MouseSensitivity);
        add_row(PauseRowKind::MouseSensitivityX);
        add_row(PauseRowKind::MouseSensitivityY);
        add_row(PauseRowKind::MouseAcceleration);
        add_row(PauseRowKind::Back);
        break;
    case PauseMenuPage::JoystickSettings:
        add_row(PauseRowKind::JoystickSensitivity);
        add_row(PauseRowKind::JoystickSensitivityX);
        add_row(PauseRowKind::JoystickSensitivityY);
        add_row(PauseRowKind::JoystickDeadzone);
        add_row(PauseRowKind::InvertJoystickX);
        add_row(PauseRowKind::InvertJoystickY);
        add_row(PauseRowKind::ResetJoystick);
        add_row(PauseRowKind::Back);
        break;
    case PauseMenuPage::InterfaceHud:
        add_row(PauseRowKind::HealthDisplay);
        add_row(PauseRowKind::Back);
        break;
    case PauseMenuPage::Gameplay:
        add_row(PauseRowKind::WaterHazard);
        add_row(PauseRowKind::PlayerMaximumHealth);
        add_row(PauseRowKind::AmmoPickupAmount);
        add_row(PauseRowKind::MissionTimeLimits);
        add_row(PauseRowKind::CutsceneTextSpeed);
        add_row(PauseRowKind::EnemyHealth);
        add_row(PauseRowKind::EnemyAwareness);
        add_row(PauseRowKind::AdStrafing);
        add_row(PauseRowKind::Back);
        break;
    case PauseMenuPage::KeyboardBindings:
        add_row(PauseRowKind::BindMoveForward);
        add_row(PauseRowKind::BindMoveBackward);
        add_row(PauseRowKind::BindStrafeLeft);
        add_row(PauseRowKind::BindStrafeRight);
        add_row(PauseRowKind::BindPrimaryFire);
        add_row(PauseRowKind::BindTakeOffLand);
        add_row(PauseRowKind::BindFlyUp);
        add_row(PauseRowKind::BindFlyDown);
        add_row(PauseRowKind::BindPreviousWeapon);
        add_row(PauseRowKind::BindNextWeapon);
        add_row(PauseRowKind::BindLoopDeLoop);
        add_row(PauseRowKind::BindQuickFlip);
        add_row(PauseRowKind::BindForwardDash);
        add_row(PauseRowKind::BindBarrelRoll);
        add_row(PauseRowKind::BindPause);
        add_row(PauseRowKind::BindMenuConfirm);
        add_row(PauseRowKind::BindMenuBack);
        add_row(PauseRowKind::BindToggleModernVisuals);
        add_row(PauseRowKind::ResetKeyboardBindings);
        add_row(PauseRowKind::Back);
        break;
    case PauseMenuPage::ControllerBindings:
        add_row(PauseRowKind::StickLayout);
        add_row(PauseRowKind::BindPrimaryFire);
        add_row(PauseRowKind::BindTakeOffLand);
        add_row(PauseRowKind::BindFlyUp);
        add_row(PauseRowKind::BindFlyDown);
        add_row(PauseRowKind::BindPause);
        add_row(PauseRowKind::BindPreviousWeapon);
        add_row(PauseRowKind::BindNextWeapon);
        add_row(PauseRowKind::BindLoopDeLoop);
        add_row(PauseRowKind::BindQuickFlip);
        add_row(PauseRowKind::BindForwardDash);
        add_row(PauseRowKind::BindBarrelRoll);
        add_row(PauseRowKind::BindMenuConfirm);
        add_row(PauseRowKind::BindMenuBack);
        add_row(PauseRowKind::ResetControllerBindings);
        add_row(PauseRowKind::Back);
        break;
    case PauseMenuPage::Graphics:
        add_row(PauseRowKind::DisplaySettings);
        add_row(PauseRowKind::RenderingSettings);
        add_row(PauseRowKind::CollisionOverlay);
        add_row(PauseRowKind::Back);
        break;
    case PauseMenuPage::Display:
        add_row(PauseRowKind::DisplayMode);
        add_row(PauseRowKind::Resolution);
        add_row(PauseRowKind::Aspect);
        add_row(PauseRowKind::FramePacing);
        add_row(PauseRowKind::Back);
        break;
    case PauseMenuPage::Rendering:
        add_row(PauseRowKind::Fog);
        add_row(PauseRowKind::HdTerrain);
        add_row(PauseRowKind::ModernLighting);
        add_row(PauseRowKind::Back);
        break;
    case PauseMenuPage::Cheats:
        add_row(PauseRowKind::AllWeapons);
        add_row(PauseRowKind::UnlimitedAmmo);
        add_row(PauseRowKind::UnlimitedHealth);
        add_row(PauseRowKind::UnlockAllLevels);
        add_row(PauseRowKind::Back);
        break;
    case PauseMenuPage::Exit:
        add_row(PauseRowKind::ReturnToTitle);
        add_row(PauseRowKind::QuitGame);
        add_row(PauseRowKind::Back);
        break;
    case PauseMenuPage::ConfirmTitle:
        add_row(PauseRowKind::ConfirmReturnToTitle);
        add_row(PauseRowKind::CancelReturnToTitle);
        break;
    }
    if (g_pause_menu.row_count == 0u ||
        g_pause_menu.row_count > kPauseMenuMaximumRows) {
        return false;
    }

    for (size_t index = 0; index < g_pause_menu.row_count; ++index) {
        const uint32_t node = pause_node_address(index);
        write_guest_u32(
            rdram,
            node + 0x00u,
            index == 0u ? 0u : pause_node_address(index - 1u)
        );
        write_guest_u32(
            rdram,
            node + 0x04u,
            index + 1u == g_pause_menu.row_count
                ? 0u
                : pause_node_address(index + 1u)
        );
        write_guest_u32(rdram, node + 0x08u, kOriginalItemColour);
        write_guest_u32(rdram, node + 0x0Cu, pause_string_address(index));
        write_guest_u32(rdram, node + 0x10u, kDirectTextItemFlags);
        write_guest_u32(rdram, node + 0x14u, kPauseMenuItemStyle);
        uint32_t successor = 0u;
        if (g_pause_menu.rows[index] ==
                PauseRowKind::ConfirmReturnToTitle) {
            successor = kPauseMenuConfirmReturnToTitleSuccessor;
        }
        write_guest_u32(
            rdram,
            node + 0x18u,
            successor
        );
        write_guest_u32(rdram, node + 0x1Cu, 0u);
        write_guest_u32(rdram, node + 0x20u, 0u);
        write_guest_u32(rdram, node + 0x24u, 0u);
    }

    g_pause_menu.installed = true;
    g_pause_menu.vertical_latched = false;
    g_pause_menu.horizontal_latched = false;
    g_pause_menu.return_to_title_logged = false;
    synchronize_pointer_revisions_for_page_locked(
        g_pause_menu.last_pointer_motion_revision
    );
    g_mouse_menu_context.store(
        MouseMenuContext::Pause,
        std::memory_order_release
    );
    if (entering_pause && !staged_handoff) {
        begin_menu_transition();
    }
    refresh_pause_menu_strings_locked();
    const uint32_t first = pause_node_address(0u);
    const size_t selected_index = page == PauseMenuPage::ConfirmTitle
        ? 1u
        : (page == PauseMenuPage::Restart ? 2u : 0u);
    const uint32_t selected = pause_node_address(selected_index);
    if (page == PauseMenuPage::ConfirmTitle) {
        write_guest_u32(rdram, kPauseMenuDescriptor + 0x10u, 0u);
    }
    write_guest_u32(rdram, kPauseMenuDescriptor + 0x14u, first);
    write_guest_u32(rdram, kPauseMenuDescriptor + 0x18u, 0u);
    write_guest_u32(rdram, kPauseMenuDescriptor + 0x38u, selected);

    std::fprintf(
        stderr,
        "BUMBLE_PAUSE_MENU stage=native_page_installed"
        " descriptor=0x%08" PRIX32
        " original_first=0x%08" PRIX32 " native_first=0x%08" PRIX32
        " page=%s rows=%zu direct_text=1 legacy_controls=0"
        " selected_index=%zu confirm_title_successor=0x%08" PRIX32 "\n",
        kPauseMenuDescriptor,
        kPauseMenuOriginalFirstItem,
        first,
        pause_page_name(page),
        g_pause_menu.row_count,
        selected_index,
        kPauseMenuConfirmReturnToTitleSuccessor
    );
    std::fflush(stderr);
    return true;
}

bool selected_pause_row_locked(size_t& index, PauseRowKind& row) {
    if (!g_pause_menu.installed || g_pause_menu.rdram == nullptr) {
        return false;
    }
    const uint32_t selected = read_guest_u32(
        g_pause_menu.rdram,
        kPauseMenuDescriptor + 0x38u
    );
    for (size_t candidate = 0; candidate < g_pause_menu.row_count;
         ++candidate) {
        if (selected == pause_node_address(candidate)) {
            index = candidate;
            row = g_pause_menu.rows[candidate];
            return true;
        }
    }
    index = 0u;
    row = g_pause_menu.rows[0u];
    write_guest_u32(
        g_pause_menu.rdram,
        kPauseMenuDescriptor + 0x38u,
        pause_node_address(0u)
    );
    return true;
}

void handle_pause_menu_input_locked(uint8_t* rdram) {
    size_t selected_index = 0u;
    PauseRowKind selected_row{};
    if (!selected_pause_row_locked(selected_index, selected_row)) {
        return;
    }

    const gpr current_pad = guest_address(kCurrentPad);
    const MenuOverlayLayout pointer_layout =
        pause_keyboard_bindings_page(g_pause_menu.page) ||
            pause_controller_bindings_page(g_pause_menu.page)
        ? MenuOverlayLayout::DenseBindings
        : MenuOverlayLayout::Standard;
    MenuPointerInteraction pointer =
        consume_menu_pointer_interaction_locked(
            pointer_layout,
            g_pause_menu.row_count,
            g_pause_menu.last_pointer_motion_revision
        );
    if (pointer.row_index.has_value()) {
        selected_index = *pointer.row_index;
        selected_row = g_pause_menu.rows[selected_index];
        write_guest_u32(
            rdram,
            kPauseMenuDescriptor + 0x38u,
            pause_node_address(selected_index)
        );
        MEM_H(4, current_pad) = 0;
        MEM_H(6, current_pad) = 0;
    }
    apply_pointer_confirm_to_guest(rdram, pointer, current_pad, nullptr);
    if (pointer.pointer_moved && pointer.row_index.has_value()) {
        std::fprintf(
            stderr,
            "BUMBLE_PAUSE_MENU stage=mouse_navigation"
            " source=%s page=%s row=%s index=%zu exact_target=1\n",
            pointer.click_pressed ? "mouse_click" : "mouse_hover",
            pause_page_name(g_pause_menu.page),
            pause_row_name(selected_row),
            selected_index
        );
        std::fflush(stderr);
    }
    if (pointer.click_pressed) {
        std::fprintf(
            stderr,
            "BUMBLE_MENU_POINTER stage=click menu=pause page=%s"
            " row_hit=%d row=%s index=%zu"
            " input_suppressed=%d exact_target=1\n",
            pause_page_name(g_pause_menu.page),
            pointer.row_index.has_value() ? 1 : 0,
            pointer.row_index.has_value()
                ? pause_row_name(selected_row)
                : "none",
            pointer.row_index.has_value() ? selected_index : 0u,
            pointer.input_suppressed ? 1 : 0
        );
        std::fflush(stderr);
    }
    int16_t stick_y = MEM_H(6, current_pad);
    const bool vertical_neutral = stick_y > -15 && stick_y < 15;
    int vertical_direction = 0;
    if (stick_y <= -41 && !g_pause_menu.vertical_latched) {
        vertical_direction = 1;
        g_pause_menu.vertical_latched = true;
    } else if (stick_y >= 41 && !g_pause_menu.vertical_latched) {
        vertical_direction = -1;
        g_pause_menu.vertical_latched = true;
    } else if (vertical_neutral) {
        g_pause_menu.vertical_latched = false;
    }
    if (!vertical_neutral) {
        MEM_H(6, current_pad) = 0;
    }
    if (vertical_direction != 0) {
        const size_t target_index = vertical_direction > 0
            ? std::min(selected_index + 1u, g_pause_menu.row_count - 1u)
            : (selected_index == 0u ? 0u : selected_index - 1u);
        if (target_index != selected_index) {
            const uint32_t from = pause_node_address(selected_index);
            selected_index = target_index;
            selected_row = g_pause_menu.rows[selected_index];
            const uint32_t to = pause_node_address(selected_index);
            write_guest_u32(rdram, kPauseMenuDescriptor + 0x38u, to);
            std::fprintf(
                stderr,
                "BUMBLE_PAUSE_MENU stage=navigation page=%s"
                " direction=%d from=0x%08" PRIX32 " to=0x%08" PRIX32
                " row=%s index=%zu\n",
                pause_page_name(g_pause_menu.page),
                vertical_direction,
                from,
                to,
                pause_row_name(selected_row),
                selected_index
            );
            std::fflush(stderr);
        }
    }

    const int16_t stick_x = MEM_H(4, current_pad);
    const bool horizontal_neutral = stick_x > -15 && stick_x < 15;
    int horizontal_direction = 0;
    if (stick_x >= 41 && !g_pause_menu.horizontal_latched) {
        horizontal_direction = 1;
        g_pause_menu.horizontal_latched = true;
    } else if (stick_x <= -41 && !g_pause_menu.horizontal_latched) {
        horizontal_direction = -1;
        g_pause_menu.horizontal_latched = true;
    } else if (horizontal_neutral) {
        g_pause_menu.horizontal_latched = false;
    }
    if (!horizontal_neutral) {
        MEM_H(4, current_pad) = 0;
    }

    RowKind setting_row{};
    bool setting_changed = false;
    bumble::input_bindings::InputAction horizontal_binding_action{};
    const bool horizontal_row_is_binding =
        pause_row_setting(selected_row, setting_row) &&
        row_binding_action(setting_row, horizontal_binding_action);
    if (horizontal_direction != 0 &&
        pause_row_setting(selected_row, setting_row) &&
        !horizontal_row_is_binding) {
        cycle_row_locked(setting_row, horizontal_direction);
        refresh_pause_menu_strings_locked();
        setting_changed = true;
    }

    const uint16_t pressed = MEM_HU(2, current_pad);
    if ((pressed & kButtonB) != 0u) {
        const uint16_t consumed = static_cast<uint16_t>(pressed & ~kButtonB);
        if (g_pause_menu.page == PauseMenuPage::Root) {
            // Root cancel uses the game's START close path.
            MEM_H(2, current_pad) = static_cast<int16_t>(
                consumed | kButtonStart
            );
            deactivate_menu_overlay();
            std::fprintf(
                stderr,
                "BUMBLE_PAUSE_MENU stage=resume_requested"
                " source=back_button transport=guest_start\n"
            );
        } else if (g_pause_menu.page == PauseMenuPage::ConfirmTitle) {
            MEM_H(2, current_pad) = static_cast<int16_t>(consumed);
            install_pause_menu_locked(rdram, PauseMenuPage::Exit);
            std::fprintf(
                stderr,
                "BUMBLE_PAUSE_MENU stage=return_to_title_cancel_requested"
                " source=back_button page=exit\n"
            );
        } else if (g_pause_menu.page == PauseMenuPage::Restart) {
            MEM_H(2, current_pad) = static_cast<int16_t>(consumed);
            install_pause_menu_locked(rdram, PauseMenuPage::Root);
        } else {
            MEM_H(2, current_pad) = static_cast<int16_t>(consumed);
            install_pause_menu_locked(
                rdram,
                pause_parent_page(g_pause_menu.page)
            );
        }
        std::fflush(stderr);
        return;
    }
    if ((pressed & kButtonA) == 0u) {
        return;
    }

    const uint16_t consumed_a = static_cast<uint16_t>(pressed & ~kButtonA);
    switch (selected_row) {
    case PauseRowKind::Resume:
        MEM_H(2, current_pad) = static_cast<int16_t>(
            consumed_a | kButtonStart
        );
        deactivate_menu_overlay();
        std::fprintf(
            stderr,
            "BUMBLE_PAUSE_MENU stage=resume_requested"
            " source=confirm transport=guest_start\n"
        );
        break;
    case PauseRowKind::RestartMenu:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        install_pause_menu_locked(rdram, PauseMenuPage::Restart);
        break;
    case PauseRowKind::RestartBeginning:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        begin_menu_transition();
        g_main_menu_handoff_ready.store(false, std::memory_order_release);
        g_pause_restart_handoff_started_ms.store(
            steady_clock_milliseconds(),
            std::memory_order_release
        );
        g_pause_restart_handoff_pending.store(true, std::memory_order_release);
        bumble::native_checkpoint::request_mission_restart(
            bumble::native_checkpoint::MissionRestart::Beginning
        );
        std::fprintf(
            stderr,
            "BUMBLE_PAUSE_MENU stage=restart_beginning_requested"
            " source=native_confirmation\n"
        );
        break;
    case PauseRowKind::RestartCheckpoint:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        if (bumble::native_checkpoint::mission_checkpoint_available()) {
            begin_menu_transition();
            bumble::native_checkpoint::request_mission_restart(
                bumble::native_checkpoint::MissionRestart::LastCheckpoint
            );
        } else {
            std::fprintf(
                stderr,
                "BUMBLE_PAUSE_MENU stage=restart_checkpoint_rejected"
                " reason=unavailable\n"
            );
        }
        break;
    case PauseRowKind::CancelRestart:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        install_pause_menu_locked(rdram, PauseMenuPage::Root);
        break;
    case PauseRowKind::OptionsMenu:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        install_pause_menu_locked(rdram, PauseMenuPage::Options);
        break;
    case PauseRowKind::InterfaceHudMenu:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        install_pause_menu_locked(rdram, PauseMenuPage::InterfaceHud);
        break;
    case PauseRowKind::GameplayMenu:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        install_pause_menu_locked(rdram, PauseMenuPage::Gameplay);
        break;
    case PauseRowKind::GraphicsMenu:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        install_pause_menu_locked(rdram, PauseMenuPage::Graphics);
        break;
    case PauseRowKind::ControlsMenu:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        install_pause_menu_locked(rdram, PauseMenuPage::Controls);
        break;
    case PauseRowKind::KeyboardBindingsMenu:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        install_pause_menu_locked(rdram, PauseMenuPage::KeyboardBindings);
        break;
    case PauseRowKind::ControllerBindingsMenu:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        install_pause_menu_locked(rdram, PauseMenuPage::ControllerBindings);
        break;
    case PauseRowKind::MouseSettingsMenu:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        install_pause_menu_locked(rdram, PauseMenuPage::MouseSettings);
        break;
    case PauseRowKind::JoystickSettingsMenu:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        install_pause_menu_locked(rdram, PauseMenuPage::JoystickSettings);
        break;
    case PauseRowKind::ResetJoystick:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        reset_joystick_settings();
        save_control_settings();
        refresh_pause_menu_strings_locked();
        break;
    case PauseRowKind::ResetKeyboardBindings:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        bumble::input_bindings::reset_profile(
            bumble::input_bindings::BindingProfile::KeyboardMouse
        );
        save_control_settings();
        refresh_pause_menu_strings_locked();
        break;
    case PauseRowKind::ResetControllerBindings:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        bumble::input_bindings::reset_profile(
            bumble::input_bindings::BindingProfile::Controller
        );
        save_control_settings();
        refresh_pause_menu_strings_locked();
        break;
    case PauseRowKind::CheatsMenu:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        install_pause_menu_locked(rdram, PauseMenuPage::Cheats);
        break;
    case PauseRowKind::ExitMenu:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        install_pause_menu_locked(rdram, PauseMenuPage::Exit);
        break;
    case PauseRowKind::DisplaySettings:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        install_pause_menu_locked(rdram, PauseMenuPage::Display);
        break;
    case PauseRowKind::RenderingSettings:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        install_pause_menu_locked(rdram, PauseMenuPage::Rendering);
        break;
    case PauseRowKind::Back:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        install_pause_menu_locked(
            rdram,
            pause_parent_page(g_pause_menu.page)
        );
        break;
    case PauseRowKind::QuitGame:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        deactivate_menu_overlay();
        request_menu_exit_locked("pause_menu");
        break;
    case PauseRowKind::ReturnToTitle:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        install_pause_menu_locked(rdram, PauseMenuPage::ConfirmTitle);
        if (!g_pause_menu.return_to_title_logged) {
            g_pause_menu.return_to_title_logged = true;
            std::fprintf(
                stderr,
                "BUMBLE_PAUSE_MENU stage=return_to_title_requested"
                " source=native_confirmation next_guest_successor=0x%08" PRIX32
                " guest_state_writes=0\n",
                kPauseMenuConfirmReturnToTitleSuccessor
            );
        }
        break;
    case PauseRowKind::ConfirmReturnToTitle:
        deactivate_menu_overlay();
        std::fprintf(
            stderr,
            "BUMBLE_PAUSE_MENU stage=return_to_title_confirmed"
            " source=guest_menu_successor successor=0x%08" PRIX32
            " guest_state_writes=0\n",
            kPauseMenuConfirmReturnToTitleSuccessor
        );
        break;
    case PauseRowKind::CancelReturnToTitle:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        install_pause_menu_locked(rdram, PauseMenuPage::Exit);
        std::fprintf(
            stderr,
            "BUMBLE_PAUSE_MENU stage=return_to_title_cancel_requested"
            " source=confirm page=exit\n"
        );
        break;
    default:
        MEM_H(2, current_pad) = static_cast<int16_t>(consumed_a);
        if (!setting_changed && pause_row_setting(selected_row, setting_row)) {
            bumble::input_bindings::InputAction binding_action{};
            if (row_binding_action(setting_row, binding_action)) {
                bumble::input_bindings::begin_capture(
                    pause_binding_profile(g_pause_menu.page),
                    binding_action
                );
            } else {
                cycle_row_locked(setting_row, 1);
            }
            refresh_pause_menu_strings_locked();
        }
        break;
    }
    std::fflush(stderr);
}

bool selected_row_locked(
    uint8_t* rdram,
    RowKind& row,
    size_t& selected_index,
    uint32_t& selected_node
) {
    if (!g_native_menu.installed || g_native_menu.rdram != rdram) {
        return false;
    }
    const uint32_t selected = read_guest_u32(
        rdram,
        kOptionsDescriptor + 0x38u
    );
    for (size_t index = 0; index < g_native_menu.row_count; ++index) {
        if (selected == node_address(g_native_menu, index)) {
            row = g_native_menu.rows[index];
            selected_index = index;
            selected_node = selected;
            return true;
        }
    }
    return false;
}

bool publish_menu_overlay_rows(
    const char* title,
    const std::vector<std::string>& rows,
    size_t selected_index,
    MenuOverlayLayout layout,
    std::optional<size_t> disabled_index = std::nullopt,
    const char* description = ""
) {
    using bumble::text_overlay::HorizontalAnchor;
    using bumble::text_overlay::TextKind;
    using bumble::text_overlay::VerticalAnchor;
    constexpr uint32_t kWhite = 0xFFF4D8FFu;
    constexpr uint32_t kGrey = 0x777777FFu;
    constexpr uint32_t kYellow = 0xFFD64AFFu;
    constexpr uint32_t kBlack = 0x000000FFu;
    bool accepted = bumble::text_overlay::observe_menu_backdrop(
        0x162A3DDBu,
        TextKind::Menu,
        layout == MenuOverlayLayout::Main
    );
    if (title != nullptr && title[0] != '\0') {
        const bool completion = layout == MenuOverlayLayout::Completion;
        accepted &= bumble::text_overlay::observe(
            TextKind::Menu,
            completion ? 160u : 98u,
            layout == MenuOverlayLayout::DenseBindings
                ? 5u
                : (completion ? 30u : 38u),
            title,
            completion
                ? HorizontalAnchor::CenterText
                : HorizontalAnchor::Center,
            VerticalAnchor::Authored,
            layout == MenuOverlayLayout::DenseBindings ? 0.76f : 0.86f,
            kYellow,
            kBlack
        );
    }

    const size_t dense_left_count = (rows.size() + 1u) / 2u;
    const bool long_standard_layout = layout == MenuOverlayLayout::Standard &&
        rows.size() >= 8u;
    for (size_t index = 0u; index < rows.size(); ++index) {
        const bool selected = index == selected_index;
        const bool disabled =
            disabled_index.has_value() && *disabled_index == index;
        std::string text = selected && !disabled ? "> " : "  ";
        text += rows[index];

        uint32_t x = kStandardMenuRowX;
        uint32_t y = kStandardMenuRowY +
            static_cast<uint32_t>(index) * kStandardMenuRowSpacing;
        float scale = kStandardMenuRowScale;
        HorizontalAnchor anchor = HorizontalAnchor::Center;
        if (long_standard_layout) {
            y = kLongMenuRowY +
                static_cast<uint32_t>(index) * kLongMenuRowSpacing;
            scale = kLongMenuRowScale;
        } else if (layout == MenuOverlayLayout::Main) {
            x = kMainMenuRowX;
            y = kMainMenuRowY +
                static_cast<uint32_t>(index) * kMainMenuRowSpacing;
            scale = kMainMenuRowScale;
        } else if (layout == MenuOverlayLayout::Completion) {
            x = 160u;
            y = kCompletionMenuRowY +
                static_cast<uint32_t>(index) * kStandardMenuRowSpacing;
            scale = 0.80f;
            anchor = HorizontalAnchor::CenterText;
        } else if (layout == MenuOverlayLayout::DenseBindings) {
            const bool right_column = index >= dense_left_count;
            const size_t column_row = right_column
                ? index - dense_left_count
                : index;
            x = right_column ? kDenseMenuRightX : kDenseMenuLeftX;
            y = kDenseMenuRowY +
                static_cast<uint32_t>(column_row) * kDenseMenuRowSpacing;
            scale = kDenseMenuRowScale;
            anchor = right_column
                ? HorizontalAnchor::Center
                : HorizontalAnchor::Left;
        }

        accepted &= bumble::text_overlay::observe(
            TextKind::Menu,
            x,
            y,
            text.c_str(),
            anchor,
            VerticalAnchor::Authored,
            scale,
            disabled ? kGrey : (selected ? kYellow : kWhite),
            kBlack
        );
    }
    if (description != nullptr && description[0] != '\0') {
        accepted &= bumble::text_overlay::observe(
            TextKind::Menu,
            160u,
            long_standard_layout ? 216u : 205u,
            description,
            HorizontalAnchor::Center,
            VerticalAnchor::Authored,
            0.58f,
            kWhite,
            kBlack,
            UINT64_C(0x4D454E5544455343)
        );
    }
    if (layout == MenuOverlayLayout::Main) {
        accepted &= bumble::text_overlay::observe(
            TextKind::Menu,
            12u,
            218u,
            bumble::build::kDisplayVersion,
            HorizontalAnchor::Left,
            VerticalAnchor::Bottom,
            0.50f,
            0xFFF4D8CCu,
            kBlack,
            UINT64_C(0x4D454E5556455253)
        );
    }
    return accepted;
}

size_t selected_overlay_index(
    uint8_t* rdram,
    uint32_t selected_address,
    size_t row_count,
    const auto& address_for_index
) {
    const uint32_t selected = read_guest_u32(rdram, selected_address);
    for (size_t index = 0u; index < row_count; ++index) {
        if (selected == address_for_index(index)) {
            return index;
        }
    }
    return 0u;
}

} // namespace

bool bumble::graphics_options::native_menu_descriptor_owned(
    uint8_t* rdram,
    uint32_t descriptor
) {
    if (rdram == nullptr ||
        !g_menu_enabled.load(std::memory_order_acquire) ||
        !bumble::text_overlay::renderer_ready()) {
        return false;
    }

    std::lock_guard lock(g_state_mutex);
    if (descriptor == kMainMenuDescriptor) {
        return g_main_menu.installed && g_main_menu.rdram == rdram &&
            g_main_menu.row_count != 0u &&
            read_guest_u32(rdram, descriptor + 0x14u) ==
                main_menu_node_address(0u);
    }
    if (descriptor == kOptionsDescriptor) {
        return g_native_menu.installed && g_native_menu.rdram == rdram &&
            g_native_menu.row_count != 0u &&
            read_guest_u32(rdram, descriptor + 0x14u) ==
                node_address(g_native_menu, 0u);
    }
    if (descriptor == kPauseMenuDescriptor) {
        return g_pause_menu.installed && g_pause_menu.rdram == rdram &&
            g_pause_menu.row_count != 0u &&
            read_guest_u32(rdram, kPauseMenuVisible) != 0u &&
            read_guest_u32(rdram, descriptor + 0x14u) ==
                pause_node_address(0u);
    }
    if (descriptor == kMissionCompleteDescriptor) {
        if (read_guest_u32(
                rdram,
                kFrontendObject + kFrontendPhaseOffset) ==
                kMissionCompletePhase &&
            g_mouse_menu_context.load(std::memory_order_acquire) ==
                MouseMenuContext::Completion) {
            return true;
        }
        const uint32_t selected = read_guest_u32(rdram, descriptor + 0x38u);
        return read_guest_u32(rdram, descriptor + 0x14u) ==
                kMissionCompleteSaveItem &&
            (selected == kMissionCompleteSaveItem ||
                selected == kMissionCompleteContinueItem) &&
            read_guest_u32(rdram, kMissionCompleteSaveItem + 0x00u) == 0u &&
            read_guest_u32(rdram, kMissionCompleteSaveItem + 0x04u) ==
                kMissionCompleteContinueItem &&
            read_guest_u32(rdram, kMissionCompleteSaveItem + 0x18u) ==
                kMissionCompleteMainMenuSuccessor &&
            read_guest_u32(rdram, kMissionCompleteContinueItem + 0x00u) ==
                kMissionCompleteSaveItem &&
            read_guest_u32(rdram, kMissionCompleteContinueItem + 0x04u) ==
                0u &&
            read_guest_u32(rdram, kMissionCompleteContinueItem + 0x18u) ==
                kMissionCompleteContinueSuccessor;
    }
    return false;
}

bool bumble::graphics_options::native_menu_overlay_active(
    uint8_t* rdram,
    uint32_t descriptor
) {
    if (!native_menu_descriptor_owned(rdram, descriptor)) {
        return false;
    }
    const MouseMenuContext context =
        g_mouse_menu_context.load(std::memory_order_acquire);
    return
        (descriptor == kMainMenuDescriptor &&
            context == MouseMenuContext::Main) ||
        (descriptor == kOptionsDescriptor &&
            context == MouseMenuContext::Options) ||
        (descriptor == kPauseMenuDescriptor &&
            context == MouseMenuContext::Pause) ||
        (descriptor == kMissionCompleteDescriptor &&
            context == MouseMenuContext::Completion);
}

bool bumble::graphics_options::publish_native_menu_overlay(
    uint8_t* rdram,
    uint32_t descriptor
) {
    if (rdram == nullptr || !bumble::text_overlay::renderer_ready()) {
        return false;
    }

    std::lock_guard lock(g_state_mutex);
    std::vector<std::string> rows;
    size_t selected_index = 0u;
    const char* title = "";
    const char* description = "";
    MenuOverlayLayout layout = MenuOverlayLayout::Standard;
    std::optional<size_t> disabled_index;
    if (descriptor == kMainMenuDescriptor &&
        g_main_menu.installed && g_main_menu.rdram == rdram) {
        rows.reserve(g_main_menu.row_count);
        for (size_t index = 0u; index < g_main_menu.row_count; ++index) {
            rows.emplace_back(main_menu_row_text(g_main_menu.rows[index]));
            if (g_main_menu.rows[index] == MainMenuRowKind::Multiplayer) {
                disabled_index = index;
            }
        }
        selected_index = selected_overlay_index(
            rdram,
            kMainMenuDescriptor + 0x38u,
            g_main_menu.row_count,
            [](size_t index) {
                return main_menu_node_address(index);
            }
        );
        title = "";
        layout = MenuOverlayLayout::Main;
    } else if (descriptor == kOptionsDescriptor &&
        g_native_menu.installed && g_native_menu.rdram == rdram) {
        const Settings settings = current();
        rows.reserve(g_native_menu.row_count);
        for (size_t index = 0u; index < g_native_menu.row_count; ++index) {
            rows.emplace_back(row_text(g_native_menu.rows[index], settings));
        }
        selected_index = selected_overlay_index(
            rdram,
            kOptionsDescriptor + 0x38u,
            g_native_menu.row_count,
            [](size_t index) {
                return node_address(g_native_menu, index);
            }
        );
        switch (g_native_menu.page) {
        case MenuPage::Root:
            title = "OPTIONS";
            break;
        case MenuPage::Controls: title = "CONTROLS"; break;
        case MenuPage::InterfaceHud: title = "INTERFACE / HUD"; break;
        case MenuPage::Gameplay:
            title = "GAMEPLAY";
            layout = MenuOverlayLayout::DenseBindings;
            break;
        case MenuPage::KeyboardBindings:
            title = "KEYBOARD & MOUSE BINDINGS";
            layout = MenuOverlayLayout::DenseBindings;
            break;
        case MenuPage::ControllerBindings:
            title = "CONTROLLER BINDINGS";
            layout = MenuOverlayLayout::DenseBindings;
            break;
        case MenuPage::MouseSettings:
            title = "MOUSE SETTINGS";
            break;
        case MenuPage::JoystickSettings:
            title = "JOYSTICK SETTINGS";
            break;
        case MenuPage::Display: title = "DISPLAY SETTINGS"; break;
        case MenuPage::Rendering: title = "GRAPHICS / EFFECTS"; break;
        case MenuPage::CheatsAndTests: title = "CHEATS & TESTS"; break;
        case MenuPage::Cheats: title = "CHEAT SETTINGS"; break;
        case MenuPage::ClearSaveData: title = "CLEAR SAVE DATA"; break;
        case MenuPage::ResetDefaults: title = "RESET TO DEFAULTS?"; break;
        }
        if (g_native_menu.page != MenuPage::Root) {
            description = row_description(g_native_menu.rows[selected_index]);
        }
    } else if (descriptor == kPauseMenuDescriptor &&
        g_pause_menu.installed && g_pause_menu.rdram == rdram &&
        read_guest_u32(rdram, kPauseMenuVisible) != 0u) {
        const Settings settings = current();
        rows.reserve(g_pause_menu.row_count);
        for (size_t index = 0u; index < g_pause_menu.row_count; ++index) {
            rows.emplace_back(
                pause_row_text(g_pause_menu.rows[index], settings)
            );
            if (g_pause_menu.rows[index] ==
                    PauseRowKind::RestartCheckpoint &&
                !bumble::native_checkpoint::mission_checkpoint_available()) {
                disabled_index = index;
            }
        }
        selected_index = selected_overlay_index(
            rdram,
            kPauseMenuDescriptor + 0x38u,
            g_pause_menu.row_count,
            [](size_t index) {
                return pause_node_address(index);
            }
        );
        switch (g_pause_menu.page) {
        case PauseMenuPage::Root: title = "PAUSED"; break;
        case PauseMenuPage::Restart: title = "RESTART MISSION?"; break;
        case PauseMenuPage::Options: title = "OPTIONS"; break;
        case PauseMenuPage::Controls: title = "CONTROLS"; break;
        case PauseMenuPage::InterfaceHud: title = "INTERFACE / HUD"; break;
        case PauseMenuPage::Gameplay: title = "GAMEPLAY"; break;
        case PauseMenuPage::KeyboardBindings:
            title = "KEYBOARD & MOUSE BINDINGS";
            layout = MenuOverlayLayout::DenseBindings;
            break;
        case PauseMenuPage::ControllerBindings:
            title = "CONTROLLER BINDINGS";
            layout = MenuOverlayLayout::DenseBindings;
            break;
        case PauseMenuPage::MouseSettings:
            title = "MOUSE SETTINGS";
            break;
        case PauseMenuPage::JoystickSettings:
            title = "JOYSTICK SETTINGS";
            break;
        case PauseMenuPage::Graphics: title = "GRAPHICS"; break;
        case PauseMenuPage::Display: title = "DISPLAY SETTINGS"; break;
        case PauseMenuPage::Rendering: title = "RENDERING SETTINGS"; break;
        case PauseMenuPage::Cheats: title = "CHEATS"; break;
        case PauseMenuPage::Exit: title = "QUIT"; break;
        case PauseMenuPage::ConfirmTitle:
            title = "RETURN TO TITLE?";
            break;
        }
        if (g_pause_menu.page == PauseMenuPage::Restart) {
            switch (g_pause_menu.rows[selected_index]) {
            case PauseRowKind::RestartBeginning:
                description =
                    "Reload the current mission from its clean start.";
                break;
            case PauseRowKind::RestartCheckpoint:
                description = bumble::native_checkpoint::
                        mission_checkpoint_available()
                    ? "Return to the latest mission checkpoint with its saved health and weapons."
                    : "Use a portal to set a mission checkpoint first.";
                break;
            case PauseRowKind::CancelRestart:
                description =
                    "Return to the pause menu without restarting.";
                break;
            default:
                break;
            }
        } else {
            RowKind setting{};
            if (pause_row_setting(
                    g_pause_menu.rows[selected_index],
                    setting)) {
                description = row_description(setting);
            }
        }
    } else if (descriptor == kMissionCompleteDescriptor &&
        read_guest_u32(rdram, kFrontendObject + kFrontendPhaseOffset) ==
            kMissionCompletePhase &&
        g_mouse_menu_context.load(std::memory_order_acquire) ==
            MouseMenuContext::Completion) {
        rows.emplace_back("MAIN MENU");
        rows.emplace_back("CONTINUE");
        selected_index = read_guest_u32(rdram, descriptor + 0x38u) ==
            kMissionCompleteSaveItem ? 0u : 1u;
        title = "MISSION COMPLETE!";
        layout = MenuOverlayLayout::Completion;
    } else {
        return false;
    }

    bumble::text_overlay::begin_menu_observations();
    bool accepted = !rows.empty() && publish_menu_overlay_rows(
        title,
        rows,
        selected_index,
        layout,
        disabled_index,
        description
    );
    if (accepted && layout == MenuOverlayLayout::Completion) {
        using bumble::text_overlay::HorizontalAnchor;
        using bumble::text_overlay::TextKind;
        using bumble::text_overlay::VerticalAnchor;
        constexpr uint32_t kWhite = 0xFFF4D8FFu;
        constexpr uint32_t kYellow = 0xFFD64AFFu;
        constexpr uint32_t kBlack = 0x000000FFu;
        const std::string mission = completed_mission_label(rdram);
        const std::string score = read_guest_string(
            rdram,
            kMissionCompleteScoreText,
            8u
        );
        const std::string bonus = read_guest_string(
            rdram,
            kMissionCompleteBonusText,
            8u
        );
        const std::string total = read_guest_string(
            rdram,
            kMissionCompleteTotalText,
            8u
        );
        const std::string multiplier = read_guest_string(
            rdram,
            kMissionCompleteMultiplierText,
            8u
        );
        accepted &= bumble::text_overlay::observe(
            TextKind::Menu,
            160u,
            68u,
            mission.empty() ? "MISSION" : mission.c_str(),
            HorizontalAnchor::CenterText,
            VerticalAnchor::Authored,
            0.76f,
            kYellow,
            kBlack
        );
        const std::array<std::string, 4> summary{{
            "SCORE  " + (score.empty() ? std::string("0") : score),
            "BONUS  " + (bonus.empty() ? std::string("0") : bonus),
            "TOTAL  " + (total.empty() ? std::string("0") : total),
            "MULTIPLIER  " +
                (multiplier.empty() ? std::string("-") : multiplier),
        }};
        for (size_t index = 0u; index < summary.size(); ++index) {
            const uint32_t y = 96u + static_cast<uint32_t>(index) * 18u;
            accepted &= bumble::text_overlay::observe(
                TextKind::Menu,
                160u,
                y,
                summary[index].c_str(),
                HorizontalAnchor::CenterText,
                VerticalAnchor::Authored,
                0.62f,
                kWhite,
                kBlack
            );
        }
    }
    if (accepted) {
        accepted = bumble::text_overlay::commit_menu_observations();
    }
    else {
        bumble::text_overlay::cancel_menu_observations();
    }
    static bool logged = false;
    if (accepted && !logged) {
        logged = true;
        std::fprintf(
            stderr,
            "BUMBLE_UI stage=native_menu_overlay"
            " fixed_size_selection=1 pulse_scale=0"
            " full_viewport_layout=1 guest_list_rewound=1\n"
        );
        std::fflush(stderr);
    }
    static bool backdrop_logged = false;
    if (accepted && layout != MenuOverlayLayout::Main &&
        !backdrop_logged) {
        backdrop_logged = true;
        std::fprintf(
            stderr,
            "BUMBLE_UI stage=native_menu_backdrop"
            " full_height=1 output_resolution=1"
            " translucent=1\n"
        );
        std::fflush(stderr);
    }
    return accepted;
}

bool bumble::graphics_options::initialize(
    const std::filesystem::path& config_root
) {
    g_config_path = std::filesystem::absolute(config_root / "graphics.ini");
    load_ini_file();
    g_menu_enabled.store(true, std::memory_order_release);
    g_runtime_shutdown_ready.store(false, std::memory_order_release);
    g_menu_exit_requested.store(false, std::memory_order_release);
    g_menu_transition_started_ms.store(0, std::memory_order_release);
    g_pause_restart_handoff_pending.store(false, std::memory_order_release);
    g_pause_restart_handoff_started_ms.store(0, std::memory_order_release);
    g_mouse_menu_context.store(
        MouseMenuContext::None,
        std::memory_order_release
    );
    {
        std::lock_guard lock(g_state_mutex);
        g_native_menu = NativeMenuState{};
        g_main_menu = NativeMainMenuState{};
        g_pause_menu = NativePauseMenuState{};
        g_consumed_pointer_click_revision = 0u;
        g_campaign_grid_pointer_motion_revision = 0u;
        g_mission_complete_pointer_motion_revision = 0u;
    }
    {
        std::lock_guard lock(g_pointer_mutex);
        g_menu_pointer = MenuPointerState{};
    }
    g_resolution_choices = enumerate_resolutions(nullptr);
    bumble::input_bindings::configure(
        bumble::input_bindings::default_settings()
    );
    g_last_controls_revision = bumble::input_bindings::revision();
    g_last_connected_controller_count = 0u;

    Settings settings{};
    settings.display_mode = static_cast<DisplayMode>(clamp_enum(
        read_ini_uint(L"DisplayMode", static_cast<uint32_t>(settings.display_mode)),
        static_cast<uint32_t>(DisplayMode::Fullscreen),
        static_cast<uint32_t>(DisplayMode::BorderedWindow)
    ));
    settings.resolution_width = std::clamp(
        read_ini_uint(L"ResolutionWidth", settings.resolution_width),
        640u,
        16384u
    );
    settings.resolution_height = std::clamp(
        read_ini_uint(L"ResolutionHeight", settings.resolution_height),
        480u,
        8640u
    );
    settings.aspect_mode = static_cast<AspectMode>(clamp_enum(
        read_ini_uint(L"AspectMode", static_cast<uint32_t>(settings.aspect_mode)),
        static_cast<uint32_t>(AspectMode::Widescreen),
        static_cast<uint32_t>(AspectMode::Widescreen)
    ));
    const uint32_t persisted_version = read_ini_uint(L"Version", 0u);
    settings.fog_mode = persisted_version < 7u
        ? FogMode::Full
        : static_cast<FogMode>(clamp_enum(
            read_ini_uint(
                L"FogMode",
                static_cast<uint32_t>(settings.fog_mode)
            ),
            static_cast<uint32_t>(FogMode::Full),
            static_cast<uint32_t>(FogMode::None)
        ));
    // Migrate schema 13's "Maximum" and pre-15 defaults to 120 Hz, capped by RT64.
    const uint32_t persisted_frame_pacing = read_ini_uint(
        L"FramePacing",
        static_cast<uint32_t>(settings.frame_pacing)
    );
    settings.frame_pacing = persisted_version < 5u
        ? FramePacing::Interpolated120Hz
        : persisted_version < 15u &&
                persisted_frame_pacing ==
                    static_cast<uint32_t>(FramePacing::Original30Hz)
            ? FramePacing::Interpolated120Hz
        : persisted_version < 14u && persisted_frame_pacing == 1u
            ? FramePacing::Interpolated120Hz
            : static_cast<FramePacing>(clamp_enum(
                persisted_frame_pacing,
                static_cast<uint32_t>(FramePacing::Interpolated120Hz),
                static_cast<uint32_t>(FramePacing::Original30Hz)
            ));
    settings.high_resolution_textures = true;
    settings.enhanced_textures = read_ini_bool(L"EnhancedTextures", false);
    settings.hd_terrain = read_ini_bool(L"HdTerrain", settings.hd_terrain);
    settings.modern_lighting = persisted_version < 4u
        ? true
        : read_ini_bool(L"ModernLighting", settings.modern_lighting);
    settings.grass_mode = GrassMode::Off;
    settings.collision_overlay = read_ini_section_uint(
        L"Debug",
        L"CollisionOverlay",
        0u
    ) != 0u;
    settings.all_weapons = read_ini_cheat_bool(L"AllWeapons", false);
    settings.unlimited_ammo = read_ini_cheat_bool(L"UnlimitedAmmo", false);
    settings.unlimited_health = read_ini_cheat_bool(L"UnlimitedHealth", false);
    settings.honeycomb_water_rescue = read_ini_section_uint(
        L"Gameplay",
        L"HoneycombWaterRescue",
        0u
    ) != 0u;
    settings.double_ammo_pickups = read_ini_section_uint(
        L"Gameplay",
        L"DoubleAmmoPickups",
        0u
    ) != 0u;
    settings.cutscene_text_speed = read_ini_section_uint(L"Gameplay", L"CutsceneTextSpeed", 4u);
    settings.double_mission_time_limits = read_ini_section_uint(
        L"Gameplay",
        L"DoubleMissionTimeLimits",
        0u
    ) != 0u;
    settings.double_enemy_health = read_ini_section_uint(
        L"Gameplay",
        L"DoubleEnemyHealth",
        0u
    ) != 0u;
    settings.double_enemy_awareness = read_ini_section_uint(
        L"Gameplay",
        L"DoubleEnemyAwareness",
        0u
    ) != 0u;
    settings.a_d_strafing = read_ini_section_uint(
        L"Gameplay",
        L"ADStrafing",
        1u
    ) != 0u;
    settings.honeycomb_health = read_ini_section_uint(
        L"Interface",
        L"HoneycombHealth",
        0u
    ) != 0u;
    settings.half_player_health = read_ini_section_uint(
        L"Gameplay",
        L"HalfPlayerHealth",
        0u
    ) != 0u;
    settings.unlock_all_levels = read_ini_section_uint(
        L"Debug",
        L"UnlockAllLevels",
        0u
    ) != 0u;
    const uint32_t persisted_campaign_level = read_ini_section_uint(
        L"Campaign",
        L"UnlockedLevel",
        1u
    );
    g_campaign_unlocked_level.store(
        normalize_campaign_unlocked_level(
            persisted_campaign_level <=
                    bumble::campaign_levels::kMissionSelectLevelIndices.back()
                ? persisted_campaign_level
                : 1u
        ),
        std::memory_order_release
    );
    bumble::input_bindings::configure(load_control_settings());
    g_last_controls_revision = bumble::input_bindings::revision();
    clamp_resolution_to_monitor(settings, g_resolution_choices);
    publish_settings(settings);
    RT64::setBumbleCollisionDebugMode(
        settings.collision_overlay ? 2u : 0u
    );
    save_settings(settings);

    std::fprintf(
        stderr,
        "BUMBLE_GRAPHICS_OPTIONS stage=loaded path=%s menu_enabled=1"
        " display=%s resolution=%" PRIu32 "x%" PRIu32
        " aspect=%s fog_scale=%.1f frame_pacing=%s"
        " hd_terrain=%d modern_lighting=%d grass=%s"
        " collision_overlay=%d"
        " all_weapons=%d unlimited_ammo=%d unlimited_health=%d"
        " honeycomb_water_rescue=%d ray_traced_lighting=%d resolution_choices=%zu\n",
        g_config_path.string().c_str(),
        settings.display_mode == DisplayMode::Fullscreen
            ? "fullscreen"
            : "bordered_window",
        settings.resolution_width,
        settings.resolution_height,
        settings.aspect_mode == AspectMode::Widescreen ? "widescreen" : "4x3",
        static_cast<double>(fog_scale()),
        settings.frame_pacing == FramePacing::Interpolated120Hz
            ? "interpolated_120hz"
            : settings.frame_pacing == FramePacing::Interpolated60Hz
                ? "interpolated_60hz"
                : "original_30hz",
        settings.hd_terrain ? 1 : 0,
        settings.modern_lighting ? 1 : 0,
        settings.grass_mode == GrassMode::Off
            ? "off"
            : settings.grass_mode == GrassMode::Low ? "low" : "high",
        settings.collision_overlay ? 1 : 0,
        settings.all_weapons ? 1 : 0,
        settings.unlimited_ammo ? 1 : 0,
        settings.unlimited_health ? 1 : 0,
        settings.honeycomb_water_rescue ? 1 : 0,
        0,
        g_resolution_choices.size()
    );
    std::fflush(stderr);
    return true;
}

void bumble::graphics_options::shutdown() {
    std::lock_guard lock(g_state_mutex);
    bumble::text_overlay::clear_kind(
        bumble::text_overlay::TextKind::Menu
    );
    // The guest allocator is gone; menu blocks are freed with RDRAM.
    g_native_menu = NativeMenuState{};
    g_main_menu = NativeMainMenuState{};
    g_pause_menu = NativePauseMenuState{};
    g_mission_complete_pointer_motion_revision = 0u;
    g_main_menu_handoff_pending.store(false, std::memory_order_release);
    g_pause_restart_handoff_pending.store(false, std::memory_order_release);
    g_main_menu_handoff_ready.store(false, std::memory_order_release);
    g_pause_restart_handoff_started_ms.store(0, std::memory_order_release);
    g_runtime_shutdown_ready.store(false, std::memory_order_release);
    RT64::setBumbleCollisionDebugMode(0u);
    g_menu_exit_requested.store(false, std::memory_order_release);
    g_menu_transition_started_ms.store(0, std::memory_order_release);
    g_mouse_menu_context.store(
        MouseMenuContext::None,
        std::memory_order_release
    );
    g_game_window.store(nullptr, std::memory_order_release);
#if !defined(_WIN32)
    g_pending_window_size.store(0u, std::memory_order_release);
#endif
    {
        std::lock_guard pointer_lock(g_pointer_mutex);
        g_menu_pointer = MenuPointerState{};
    }
}

bool bumble::graphics_options::runtime_shutdown_ready() {
    return g_runtime_shutdown_ready.load(std::memory_order_acquire);
}

bumble::graphics_options::Settings bumble::graphics_options::current() {
    Settings settings{};
    settings.display_mode = static_cast<DisplayMode>(
        g_display_mode.load(std::memory_order_acquire)
    );
    settings.resolution_width = g_resolution_width.load(std::memory_order_acquire);
    settings.resolution_height = g_resolution_height.load(std::memory_order_acquire);
    settings.aspect_mode = static_cast<AspectMode>(
        g_aspect_mode.load(std::memory_order_acquire)
    );
    settings.frame_pacing = static_cast<FramePacing>(
        g_frame_pacing.load(std::memory_order_acquire)
    );
    settings.fog_mode = static_cast<FogMode>(
        g_fog_mode.load(std::memory_order_acquire)
    );
    settings.high_resolution_textures =
        g_high_resolution_textures.load(std::memory_order_acquire);
    settings.hd_terrain = g_hd_terrain.load(std::memory_order_acquire);
    settings.enhanced_textures = enhanced_textures_enabled();
    settings.modern_lighting =
        g_modern_lighting.load(std::memory_order_acquire);
    settings.grass_mode = static_cast<GrassMode>(
        g_grass_mode.load(std::memory_order_acquire)
    );
    settings.collision_overlay =
        g_collision_overlay.load(std::memory_order_acquire);
    settings.all_weapons = g_all_weapons.load(std::memory_order_acquire);
    settings.unlimited_ammo = g_unlimited_ammo.load(std::memory_order_acquire);
    settings.unlimited_health = g_unlimited_health.load(std::memory_order_acquire);
    settings.honeycomb_water_rescue =
        g_honeycomb_water_rescue.load(std::memory_order_acquire);
    settings.double_ammo_pickups =
        g_double_ammo_pickups.load(std::memory_order_acquire);
    settings.cutscene_text_speed = g_cutscene_text_speed.load(std::memory_order_acquire);
    settings.double_mission_time_limits =
        g_double_mission_time_limits.load(std::memory_order_acquire);
    settings.double_enemy_health =
        g_double_enemy_health.load(std::memory_order_acquire);
    settings.double_enemy_awareness =
        g_double_enemy_awareness.load(std::memory_order_acquire);
    settings.a_d_strafing = g_a_d_strafing.load(std::memory_order_acquire);
    settings.unlock_all_levels =
        g_unlock_all_levels.load(std::memory_order_acquire);
    settings.honeycomb_health =
        g_honeycomb_health.load(std::memory_order_acquire);
    settings.half_player_health =
        g_half_player_health.load(std::memory_order_acquire);
    return settings;
}

void bumble::graphics_options::apply(const Settings& settings) {
    std::lock_guard lock(g_state_mutex);
    apply_settings(settings);
}

std::vector<std::pair<uint32_t, uint32_t>>
bumble::graphics_options::resolution_choices() {
    std::lock_guard lock(g_state_mutex);
    return g_resolution_choices;
}

uint32_t bumble::graphics_options::initial_client_width() {
    return g_resolution_width.load(std::memory_order_acquire);
}

uint32_t bumble::graphics_options::initial_client_height() {
    return g_resolution_height.load(std::memory_order_acquire);
}

bool bumble::graphics_options::widescreen_enabled() {
    return g_aspect_mode.load(std::memory_order_acquire) ==
        static_cast<uint32_t>(AspectMode::Widescreen);
}

bumble::graphics_options::FramePacing
bumble::graphics_options::frame_pacing() {
    return static_cast<FramePacing>(
        g_frame_pacing.load(std::memory_order_acquire)
    );
}

bool bumble::graphics_options::high_resolution_textures_enabled() {
    return g_high_resolution_textures.load(std::memory_order_acquire);
}

bool bumble::graphics_options::hd_terrain_enabled() {
    return g_hd_terrain.load(std::memory_order_acquire);
}

bool bumble::graphics_options::modern_lighting_enabled() {
    return g_modern_lighting.load(std::memory_order_acquire);
}

void bumble::graphics_options::toggle_modern_visuals() {
    std::lock_guard lock(g_state_mutex);
    auto settings = current();
    const int mode = settings.enhanced_textures ? 2 :
        (settings.hd_terrain && settings.modern_lighting ? 1 : 0);
    const int next = (mode + 1) % (bumble::first_run::enhanced_textures_available() ? 3 : 2);
    settings.hd_terrain = next != 0;
    settings.modern_lighting = next != 0;
    settings.enhanced_textures = next == 2;
    apply_settings(settings);
    std::fprintf(
        stderr,
        "BUMBLE_GRAPHICS_OPTIONS stage=modern_visuals_toggled"
        " source=shared_input_toggle mode=%d\n",
        next
    );
    std::fflush(stderr);
}

bool bumble::graphics_options::enhanced_textures_enabled() {
    return bumble::first_run::enhanced_textures_available() && g_enhanced_textures.load(std::memory_order_acquire);
}

uint32_t bumble::graphics_options::cutscene_text_speed() {
    return g_cutscene_text_speed.load(std::memory_order_acquire);
}

bool bumble::graphics_options::all_weapons_enabled() {
    return g_all_weapons.load(std::memory_order_acquire);
}

bool bumble::graphics_options::unlimited_ammo_enabled() {
    return g_unlimited_ammo.load(std::memory_order_acquire);
}

bool bumble::graphics_options::unlimited_health_enabled() {
    return g_unlimited_health.load(std::memory_order_acquire);
}

bool bumble::graphics_options::honeycomb_water_rescue_enabled() {
    return g_honeycomb_water_rescue.load(std::memory_order_acquire);
}

bool bumble::graphics_options::double_ammo_pickups_enabled() {
    return g_double_ammo_pickups.load(std::memory_order_acquire);
}

bool bumble::graphics_options::double_mission_time_limits_enabled() {
    return g_double_mission_time_limits.load(std::memory_order_acquire);
}

bool bumble::graphics_options::double_enemy_health_enabled() {
    return g_double_enemy_health.load(std::memory_order_acquire);
}

bool bumble::graphics_options::double_enemy_awareness_enabled() {
    return g_double_enemy_awareness.load(std::memory_order_acquire);
}

bool bumble::graphics_options::a_d_strafing_enabled() {
    return g_a_d_strafing.load(std::memory_order_acquire);
}

bool bumble::graphics_options::unlock_all_levels_enabled() {
    return g_unlock_all_levels.load(std::memory_order_acquire);
}

bool bumble::graphics_options::honeycomb_health_enabled() {
    return g_honeycomb_health.load(std::memory_order_acquire);
}

bool bumble::graphics_options::half_player_health_enabled() {
    return g_half_player_health.load(std::memory_order_acquire);
}

uint32_t bumble::graphics_options::campaign_unlocked_level() {
    return g_campaign_unlocked_level.load(std::memory_order_acquire);
}

bool bumble::graphics_options::record_campaign_progress(uint32_t next_level) {
    std::lock_guard save_lock(g_campaign_save_mutex);
    const uint32_t unlocked = normalize_campaign_unlocked_level(next_level);
    const uint32_t previous = g_campaign_unlocked_level.load(
        std::memory_order_acquire
    );
    if (unlocked <= previous) {
        return true;
    }
    const bool write_succeeded = write_ini_section_uint(
        L"Campaign",
        L"UnlockedLevel",
        unlocked
    );
    const bool flush_succeeded = write_succeeded && flush_ini_file();
    const bool verified = flush_succeeded && read_ini_section_uint(
        L"Campaign",
        L"UnlockedLevel",
        0u
    ) == unlocked;
    if (!verified) {
        std::fprintf(
            stderr,
            "BUMBLE_CAMPAIGN_SAVE stage=autosave_failed previous=%" PRIu32
            " requested_level=%" PRIu32 " write=%d flush=%d"
            " in_memory_advanced=0 retry=score_menu\n",
            previous,
            unlocked,
            write_succeeded ? 1 : 0,
            flush_succeeded ? 1 : 0
        );
        std::fflush(stderr);
        return false;
    }
    g_campaign_unlocked_level.store(unlocked, std::memory_order_release);
    std::fprintf(
        stderr,
        "BUMBLE_CAMPAIGN_SAVE stage=autosaved previous=%" PRIu32
        " unlocked_level=%" PRIu32
        " durable_write=1 verified_readback=1\n",
        previous,
        unlocked
    );
    std::fflush(stderr);
    return true;
}

bool bumble::graphics_options::interactive_menu_enabled() {
    return g_menu_enabled.load(std::memory_order_acquire);
}

void bumble::graphics_options::request_play_menu_return() {
    g_play_menu_return_requested.store(true, std::memory_order_release);
}

void bumble::graphics_options::publish_lighting_environment(uint8_t* rdram) {
    if (rdram == nullptr) {
        return;
    }

    uint32_t level_index = read_guest_u32(rdram, kCurrentLevelIndex);
    if (level_index > 32u) {
        level_index = 0u;
    }
    const uint32_t frontend_phase = read_guest_u32(
        rdram,
        kFrontendObject + kFrontendPhaseOffset
    );
    const bumble::campaign_levels::Record* gameplay_record =
        bumble::campaign_levels::find(level_index);
    if (frontend_phase == kMissionGameplayPhase && gameplay_record != nullptr) {
        const uint32_t mission_number =
            bumble::campaign_levels::displayed_mission_number(*gameplay_record);
        if (mission_number >= 1u &&
            mission_number <= bumble::campaign_levels::kMissionSelectLevelIndices.size()) {
            const uint32_t previous_gameplay_level =
                g_last_gameplay_level_index.exchange(
                    level_index,
                    std::memory_order_acq_rel
                );
            if (previous_gameplay_level != level_index) {
                std::fprintf(
                    stderr,
                    "BUMBLE_MISSION_COMPLETE stage=gameplay_identity_frozen"
                    " level=%" PRIu32 " mission=%" PRIu32
                    " ownership=last_gameplay_phase guest_writes=0\n",
                    level_index,
                    mission_number
                );
                std::fflush(stderr);
            }
        }
    }
    const uint32_t background_rgb =
        (static_cast<uint32_t>(MEM_BU(
             0,
             guest_address(kLevelBackgroundRgb)
         )) << 16u) |
        (static_cast<uint32_t>(MEM_BU(
             1,
             guest_address(kLevelBackgroundRgb)
         )) << 8u) |
        static_cast<uint32_t>(MEM_BU(
            2,
            guest_address(kLevelBackgroundRgb)
        ));
    const LightingEnvironmentProfile profile =
        lighting_profile_for_level(level_index);
    const uint32_t packed =
        (background_rgb & 0x00FFFFFFu) |
        ((level_index & 0x3Fu) << 24u) |
        ((static_cast<uint32_t>(profile) & 0x3u) << 30u);
    const uint32_t previous = g_lighting_environment.exchange(
        packed,
        std::memory_order_acq_rel
    );
    if ((previous & 0xFF000000u) != (packed & 0xFF000000u)) {
        std::fprintf(
            stderr,
            "BUMBLE_LIGHTING stage=environment_published"
            " level=%" PRIu32 " profile=%" PRIu32
            " background_rgb=0x%06" PRIX32
            " ownership=guest_read_only packed_atomic=1\n",
            level_index,
            static_cast<uint32_t>(profile),
            background_rgb
        );
        std::fflush(stderr);
    }
}

bumble::graphics_options::LightingEnvironment
bumble::graphics_options::lighting_environment() {
    const uint32_t packed = g_lighting_environment.load(
        std::memory_order_acquire
    );
    LightingEnvironment result{};
    result.backgroundRgb = packed & 0x00FFFFFFu;
    result.levelIndex = (packed >> 24u) & 0x3Fu;
    result.profile = static_cast<LightingEnvironmentProfile>(
        (packed >> 30u) & 0x3u
    );
    return result;
}

bumble::graphics_options::GrassMode bumble::graphics_options::grass_mode() {
    return static_cast<GrassMode>(g_grass_mode.load(std::memory_order_acquire));
}

bool bumble::graphics_options::ray_traced_lighting_enabled() {
    return false;
}

float bumble::graphics_options::fog_scale() {
    switch (static_cast<FogMode>(g_fog_mode.load(std::memory_order_acquire))) {
    case FogMode::Half:
        return 0.5f;
    case FogMode::Full:
        return 1.0f;
    case FogMode::None:
    default:
        return 0.0f;
    }
}

void bumble::graphics_options::apply_to_graphics_config(
    ultramodern::renderer::GraphicsConfig& config
) {
    const Settings settings = current();
    config.wm_option = settings.display_mode == DisplayMode::Fullscreen
        ? ultramodern::renderer::WindowMode::Fullscreen
        : ultramodern::renderer::WindowMode::Windowed;
    config.ar_option = settings.aspect_mode == AspectMode::Widescreen
        ? ultramodern::renderer::AspectRatio::Expand
        : ultramodern::renderer::AspectRatio::Original;
    config.hr_option = settings.aspect_mode == AspectMode::Widescreen
        ? ultramodern::renderer::HUDRatioMode::Full
        : ultramodern::renderer::HUDRatioMode::Original;
}

void bumble::graphics_options::update_menu_pointer(
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    bool click,
    bool inside
) {
    std::lock_guard pointer_lock(g_pointer_mutex);
    const bool changed =
        g_menu_pointer.client_x != x ||
        g_menu_pointer.client_y != y ||
        g_menu_pointer.inside_client != inside;
    g_menu_pointer.client_x = x;
    g_menu_pointer.client_y = y;
    g_menu_pointer.client_width = width;
    g_menu_pointer.client_height = height;
    g_menu_pointer.inside_client = inside;
    if (changed) {
        ++g_menu_pointer.motion_revision;
    }
    if (click && !g_menu_pointer.click_down) {
        ++g_menu_pointer.click_revision;
    }
    if (click) {
        g_menu_pointer.click_down = true;
    }
}

void bumble::graphics_options::resize_menu_pointer(
    uint32_t width,
    uint32_t height
) {
    std::lock_guard pointer_lock(g_pointer_mutex);
    g_menu_pointer.client_width = width;
    g_menu_pointer.client_height = height;
}

void bumble::graphics_options::release_menu_pointer_click() {
    std::lock_guard pointer_lock(g_pointer_mutex);
    g_menu_pointer.click_down = false;
}

void bumble::graphics_options::clear_menu_pointer() {
    std::lock_guard pointer_lock(g_pointer_mutex);
    g_menu_pointer.click_down = false;
    if (g_menu_pointer.inside_client) {
        g_menu_pointer.inside_client = false;
        ++g_menu_pointer.motion_revision;
    }
}

void bumble::graphics_options::stage_pause_menu_transition() {
    if (!g_menu_enabled.load(std::memory_order_acquire) ||
        !bumble::text_overlay::renderer_ready()) {
        return;
    }

    std::lock_guard lock(g_state_mutex);
    if (g_pause_menu.installed || g_pause_overlay_staged ||
        g_mouse_menu_context.load(std::memory_order_acquire) !=
            MouseMenuContext::None) {
        return;
    }

    const Settings settings = current();
    const std::array root_rows{
        PauseRowKind::Resume,
        PauseRowKind::RestartMenu,
        PauseRowKind::OptionsMenu,
        PauseRowKind::CheatsMenu,
        PauseRowKind::ExitMenu,
    };
    std::vector<std::string> rows;
    rows.reserve(root_rows.size());
    for (const PauseRowKind row : root_rows) {
        rows.emplace_back(pause_row_text(row, settings));
    }

    begin_menu_transition();
    bumble::text_overlay::begin_menu_observations();
    if (!publish_menu_overlay_rows(
            "PAUSED",
            rows,
            0u,
            MenuOverlayLayout::Standard) ||
        !bumble::text_overlay::commit_menu_observations()) {
        bumble::text_overlay::cancel_menu_observations();
        return;
    }
    g_pause_overlay_staged = true;
    g_pause_overlay_precommit_observed = false;
    std::fprintf(
        stderr,
        "BUMBLE_PAUSE_MENU stage=input_edge_composition_staged"
        " rows=%zu guest_state_writes=0\n",
        rows.size()
    );
    std::fflush(stderr);
}

bumble::graphics_options::PointerSnapshot
bumble::graphics_options::pointer_snapshot() {
    const MenuPointerState pointer = menu_pointer_snapshot();
    return {
        pointer.client_x,
        pointer.client_y,
        pointer.client_width,
        pointer.client_height,
        pointer.motion_revision,
        pointer.click_revision,
        pointer.inside_client,
        pointer.click_down,
    };
}

void bumble::graphics_options::set_campaign_grid_pointer_active(bool active) {
    std::lock_guard lock(g_state_mutex);
    if (active) {
        if (g_mouse_menu_context.load(std::memory_order_acquire) !=
                MouseMenuContext::LevelSelect) {
            synchronize_pointer_revisions_for_page_locked(
                g_campaign_grid_pointer_motion_revision
            );
        }
        g_mouse_menu_context.store(
            MouseMenuContext::LevelSelect,
            std::memory_order_release
        );
    }
    else if (g_mouse_menu_context.load(std::memory_order_acquire) ==
            MouseMenuContext::LevelSelect) {
        g_mouse_menu_context.store(
            MouseMenuContext::None,
            std::memory_order_release
        );
        if (bumble::native_checkpoint::campaign_selection_committed_index() !=
                0u) {
            g_main_menu_handoff_ready.store(false, std::memory_order_release);
            g_main_menu_handoff_pending.store(true, std::memory_order_release);
        }
    }
}

bool bumble::graphics_options::consume_campaign_grid_pointer(
    uint32_t& slot,
    bool& click
) {
    click = false;
    std::lock_guard lock(g_state_mutex);
    if (g_mouse_menu_context.load(std::memory_order_acquire) !=
            MouseMenuContext::LevelSelect) {
        return false;
    }
    const MenuPointerInteraction interaction =
        consume_menu_pointer_interaction_locked(
            MenuOverlayLayout::CampaignGrid,
            bumble::campaign_levels::kMissionSelectLevelIndices.size() + 1u,
            g_campaign_grid_pointer_motion_revision
        );
    if (!interaction.row_index.has_value()) {
        return false;
    }
    slot = static_cast<uint32_t>(*interaction.row_index);
    click = interaction.click_pressed && !interaction.input_suppressed;
    return true;
}

uint8_t bumble::graphics_options::menu_transition_alpha() {
    constexpr int64_t kTransitionMilliseconds = 180;
    constexpr uint8_t kReadableStartAlpha = 192u;
    const int64_t started = g_menu_transition_started_ms.load(
        std::memory_order_acquire
    );
    if (started == 0) {
        return 255u;
    }
    const int64_t elapsed = steady_clock_milliseconds() - started;
    if (elapsed <= 0) {
        return kReadableStartAlpha;
    }
    if (elapsed >= kTransitionMilliseconds) {
        return 255u;
    }
    const double linear = static_cast<double>(elapsed) /
        static_cast<double>(kTransitionMilliseconds);
    const double smooth = linear * linear * (3.0 - 2.0 * linear);
    return static_cast<uint8_t>(
        kReadableStartAlpha +
        std::lround(smooth * (255.0 - kReadableStartAlpha))
    );
}

#if defined(_WIN32)
void bumble::graphics_options::attach_game_window(HWND window) {
    g_game_window.store(window, std::memory_order_release);
    const HMONITOR validation_monitor = validation_monitor_from_environment();
    if (validation_monitor != nullptr) {
        resize_bordered_window(
            window,
            initial_client_width(),
            initial_client_height(),
            validation_monitor
        );
    }
    if (!g_menu_enabled.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard lock(g_state_mutex);
    g_resolution_choices = enumerate_resolutions(window);
    auto settings = current();
    const auto previous = std::pair{
        settings.resolution_width,
        settings.resolution_height,
    };
    clamp_resolution_to_monitor(settings, g_resolution_choices);
    if (previous.first != settings.resolution_width ||
        previous.second != settings.resolution_height) {
        publish_settings(settings);
        save_settings(settings);
        post_windowed_resolution_if_needed(settings);
    }
}

void bumble::graphics_options::notify_window_mode_applied(bool fullscreen) {
    if (fullscreen) {
        return;
    }
    const HWND window = g_game_window.load(std::memory_order_acquire);
    if (window != nullptr && IsWindow(window)) {
        PostMessageW(window, kApplyWindowedResolutionMessage, 0, 0);
    }
}

bool bumble::graphics_options::handle_game_window_message(
    HWND window,
    UINT message,
    WPARAM,
    LPARAM lparam,
    LRESULT& result
) {
    const auto update_pointer = [window, lparam](bool click) {
        RECT client{};
        if (!GetClientRect(window, &client)) {
            return;
        }
        const int32_t x = static_cast<int32_t>(GET_X_LPARAM(lparam));
        const int32_t y = static_cast<int32_t>(GET_Y_LPARAM(lparam));
        const uint32_t width = static_cast<uint32_t>(
            std::max<LONG>(0, client.right - client.left)
        );
        const uint32_t height = static_cast<uint32_t>(
            std::max<LONG>(0, client.bottom - client.top)
        );
        const bool inside =
            x >= 0 && y >= 0 &&
            static_cast<uint32_t>(x) < width &&
            static_cast<uint32_t>(y) < height;
        update_menu_pointer(x, y, width, height, click, inside);
    };

    if (message == WM_MOUSEMOVE) {
        update_pointer(false);
        TRACKMOUSEEVENT tracking{
            .cbSize = sizeof(TRACKMOUSEEVENT),
            .dwFlags = TME_LEAVE,
            .hwndTrack = window,
            .dwHoverTime = 0u,
        };
        TrackMouseEvent(&tracking);
        return false;
    }
    if (message == WM_LBUTTONDOWN) {
        update_pointer(true);
        return false;
    }
    if (message == WM_LBUTTONUP) {
        update_pointer(false);
        release_menu_pointer_click();
        return false;
    }
    if (message == WM_MOUSELEAVE || message == WM_KILLFOCUS) {
        clear_menu_pointer();
        return false;
    }
    if (message == WM_SIZE) {
        RECT client{};
        if (GetClientRect(window, &client)) {
            const uint32_t width = static_cast<uint32_t>(
                std::max<LONG>(0, client.right - client.left)
            );
            const uint32_t height = static_cast<uint32_t>(
                std::max<LONG>(0, client.bottom - client.top)
            );
            resize_menu_pointer(width, height);
        }
        return false;
    }
    if (message != kApplyWindowedResolutionMessage) {
        return false;
    }
    resize_bordered_window(window, initial_client_width(), initial_client_height());
    std::fprintf(
        stderr,
        "BUMBLE_GRAPHICS_OPTIONS stage=windowed_resolution_applied"
        " width=%" PRIu32 " height=%" PRIu32 "\n",
        initial_client_width(),
        initial_client_height()
    );
    std::fflush(stderr);
    result = 0;
    return true;
}
#else
void bumble::graphics_options::attach_game_window(SDL_Window* window) {
    g_game_window.store(window, std::memory_order_release);
    std::lock_guard lock(g_state_mutex);
    g_resolution_choices = enumerate_resolutions(window);
    auto settings = current();
    const auto previous = std::pair{
        settings.resolution_width,
        settings.resolution_height,
    };
    clamp_resolution_to_monitor(settings, g_resolution_choices);
    if (previous.first != settings.resolution_width ||
        previous.second != settings.resolution_height) {
        publish_settings(settings);
        save_settings(settings);
    }
    post_windowed_resolution_if_needed(settings);
}

void bumble::graphics_options::apply_pending_windowed_resolution() {
    const uint64_t size = g_pending_window_size.exchange(
        0u,
        std::memory_order_acq_rel
    );
    if (size == 0u) {
        return;
    }
    resize_bordered_window(
        g_game_window.load(std::memory_order_acquire),
        static_cast<uint32_t>(size >> 32u),
        static_cast<uint32_t>(size)
    );
}

void bumble::graphics_options::notify_window_mode_applied(bool fullscreen) {
    if (!fullscreen) {
        post_windowed_resolution_if_needed(current());
    }
}
#endif

bool bumble::graphics_options::mouse_menu_navigation_active() {
    return g_menu_enabled.load(std::memory_order_acquire) &&
        g_mouse_menu_context.load(std::memory_order_acquire) !=
            MouseMenuContext::None;
}

void bumble::menu_actions::observe_pause_menu(uint8_t* rdram) {
    if (rdram == nullptr ||
        !g_menu_enabled.load(std::memory_order_acquire)) {
        return;
    }

    std::lock_guard lock(g_state_mutex);
    synchronize_control_settings_locked();
    if (read_guest_u32(rdram, kPauseMenuVisible) == 0u) {
        if (g_pause_overlay_staged) {
            if (!g_pause_overlay_precommit_observed) {
                g_pause_overlay_precommit_observed = true;
            } else {
                bumble::text_overlay::clear_kind(
                    bumble::text_overlay::TextKind::Menu
                );
                g_pause_overlay_staged = false;
                g_pause_overlay_precommit_observed = false;
            }
        }
        if (g_pause_menu.installed &&
            !g_pause_restart_handoff_pending.load(
                std::memory_order_acquire)) {
            bumble::text_overlay::clear_kind(
                bumble::text_overlay::TextKind::Menu
            );
        }
        g_pause_menu.installed = false;
        g_pause_menu.page = PauseMenuPage::Root;
        g_pause_menu.vertical_latched = false;
        g_pause_menu.horizontal_latched = false;
        g_pause_menu.return_to_title_logged = false;
        if (g_mouse_menu_context.load(std::memory_order_acquire) ==
            MouseMenuContext::Pause) {
            g_mouse_menu_context.store(
                MouseMenuContext::None,
                std::memory_order_release
            );
        }
        return;
    }

    if (g_pause_menu.page == PauseMenuPage::ConfirmTitle &&
        read_guest_u32(rdram, kFrontendObject + 0x84u) ==
            kMainMenuPhase &&
        read_guest_u32(rdram, kPauseMenuDescriptor + 0x14u) == 0u) {
        g_pause_menu.installed = false;
        return;
    }

    const bool native_descriptor_active =
        g_pause_menu.installed && g_pause_menu.rdram == rdram &&
        g_pause_menu.allocation != nullptr &&
        read_guest_u32(rdram, kPauseMenuDescriptor + 0x14u) ==
            pause_node_address(0u);
    if (!native_descriptor_active &&
        !install_pause_menu_locked(rdram, PauseMenuPage::Root)) {
        if (!g_pause_menu.install_failure_logged) {
            g_pause_menu.install_failure_logged = true;
            std::fprintf(
                stderr,
                "BUMBLE_PAUSE_MENU stage=native_page_install_failed"
                " menu=pause descriptor=0x%08" PRIX32 "\n",
                kPauseMenuDescriptor
            );
            std::fflush(stderr);
        }
        return;
    }
    g_pause_overlay_staged = false;
    g_pause_overlay_precommit_observed = false;
    g_pause_menu.install_failure_logged = false;
    handle_pause_menu_input_locked(rdram);
}

extern "C" void bumble_prepare_native_graphics_options_menu(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t object_from_a0 = context != nullptr
        ? low_guest_address(context->r4)
        : 0u;
    const uint32_t object_from_s0 = context != nullptr
        ? low_guest_address(context->r16)
        : 0u;
    // At 0x800AB080 the a0 delay slot has not run; the frontend object is still in s0.
    const uint32_t live_object = object_from_a0 == kFrontendObject
        ? object_from_a0
        : object_from_s0;
    const uint32_t phase = context != nullptr
        ? low_guest_address(context->r5)
        : 0u;
    const uint32_t descriptor = context != nullptr
        ? low_guest_address(context->r6)
        : 0u;
    bool completion_autosave_persisted = true;
    if (rdram != nullptr && context != nullptr &&
        live_object == kFrontendObject) {
        bumble::death_screen::prepare_frontend_phase(phase, descriptor);
        bumble::game_completion_screen::prepare_frontend_phase(
            rdram,
            phase,
            descriptor
        );
        if (phase == kMissionCompletePhase &&
            descriptor == kMissionCompleteDescriptor) {
            completion_autosave_persisted =
                bumble::graphics_options::record_campaign_progress(
                    read_guest_u32(
                        rdram,
                        kFrontendObject + kFrontendCurrentLevelOffset
                    )
                );
        }
    }
    if (context != nullptr && g_menu_enabled.load(std::memory_order_acquire)) {
        std::fprintf(
            stderr,
            "BUMBLE_GRAPHICS_OPTIONS stage=prepare_hook_observed"
            " object=0x%08" PRIX32 " phase=0x%08" PRIX32
            " descriptor=0x%08" PRIX32 "\n",
            live_object,
            phase,
            descriptor
        );
        std::fflush(stderr);
    }
    if (rdram == nullptr || context == nullptr ||
        !g_menu_enabled.load(std::memory_order_acquire) ||
        live_object != kFrontendObject) {
        return;
    }
    if (phase != kLevelSelectPhase) {
        bumble::native_checkpoint::retire_campaign_grid();
    }
    std::lock_guard lock(g_state_mutex);
    if (phase != kLevelSelectPhase &&
        g_mouse_menu_context.load(std::memory_order_acquire) ==
            MouseMenuContext::LevelSelect) {
        deactivate_menu_overlay();
    }
    if (phase == kMissionCompletePhase &&
        descriptor == kMissionCompleteDescriptor) {
        const bool installed = activate_mission_complete_menu_locked(rdram);
        std::fprintf(
            stderr,
            "BUMBLE_CAMPAIGN_SAVE stage=completion_menu"
            " autosave_persisted=%d rows=%d main_menu=%d continue=%d\n",
            completion_autosave_persisted ? 1 : 0,
            installed ? 2 : 0,
            installed ? 1 : 0,
            installed ? 1 : 0
        );
        std::fflush(stderr);
        return;
    }
    if (phase == kMainMenuPhase && descriptor == kMainMenuDescriptor) {
            g_main_menu_handoff_pending.store(false, std::memory_order_release);
        g_pause_restart_handoff_pending.store(false, std::memory_order_release);
        g_main_menu_handoff_ready.store(false, std::memory_order_release);
        g_pause_restart_handoff_started_ms.store(0, std::memory_order_release);
        const bool return_to_play = g_play_menu_return_requested.exchange(
            false,
            std::memory_order_acq_rel
        );
        const bool retain_page =
            g_main_menu.installed && g_main_menu.rdram == rdram &&
            g_mouse_menu_context.load(std::memory_order_acquire) ==
                MouseMenuContext::Main;
        const MainMenuPage page = return_to_play
            ? MainMenuPage::Play
            : (retain_page ? g_main_menu.page : MainMenuPage::Root);
        if (!install_main_menu_locked(rdram, page)) {
            if (!g_main_menu.install_failure_logged) {
                g_main_menu.install_failure_logged = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_MAIN_MENU stage=native_page_install_failed"
                    " page=%s phase=0x%08" PRIX32
                    " descriptor=0x%08" PRIX32 "\n",
                    main_menu_page_name(page),
                    kMainMenuPhase,
                    kMainMenuDescriptor
                );
                std::fflush(stderr);
            }
        } else {
            g_main_menu.install_failure_logged = false;
        }
        return;
    }
    if (phase != kOptionsPhase || descriptor != kOptionsDescriptor) {
        return;
    }
    const bool retain_page =
        g_native_menu.installed && g_native_menu.rdram == rdram &&
        g_mouse_menu_context.load(std::memory_order_acquire) ==
            MouseMenuContext::Options;
    const MenuPage page = retain_page
        ? g_native_menu.page
        : MenuPage::Root;
    if (!install_native_menu_locked(rdram, page)) {
        std::fprintf(
            stderr,
            "BUMBLE_GRAPHICS_OPTIONS stage=guest_menu_install_failed"
            " phase=0x%08" PRIX32 " descriptor=0x%08" PRIX32 "\n",
            kOptionsPhase,
            kOptionsDescriptor
        );
        std::fflush(stderr);
    }
}

void bumble::graphics_options::mark_main_menu_handoff_ready() {
    if (g_main_menu_handoff_pending.load(std::memory_order_acquire) ||
        g_pause_restart_handoff_pending.load(std::memory_order_acquire)) {
        g_main_menu_handoff_ready.store(true, std::memory_order_release);
    }
}

void bumble::graphics_options::complete_main_menu_handoff() {
    const bool main_menu_pending =
        g_main_menu_handoff_pending.load(std::memory_order_acquire);
    const bool pause_restart_pending =
        g_pause_restart_handoff_pending.load(std::memory_order_acquire);
    const bool any_handoff_pending =
        main_menu_pending || pause_restart_pending;
    bool incoming_composition_ready = g_main_menu_handoff_ready.exchange(
        false,
        std::memory_order_acq_rel
    );
    const char* readiness_source = "committed_incoming_frame";
    if (!incoming_composition_ready && main_menu_pending &&
        !pause_restart_pending) {
        const auto observations = bumble::text_overlay::active_observations();
        incoming_composition_ready = std::ranges::any_of(
            observations,
            [](const bumble::text_overlay::Observation& observation) {
                using bumble::text_overlay::TextKind;
                return observation.kind == TextKind::Briefing ||
                    observation.kind == TextKind::Script ||
                    observation.kind == TextKind::GameplayHud;
            }
        );
        readiness_source = "presented_incoming_composition";
    }
    if (!incoming_composition_ready) {
        return;
    }
    const bool main_menu_handoff = g_main_menu_handoff_pending.exchange(
        false,
        std::memory_order_acq_rel
    );
    const bool pause_restart_handoff =
        g_pause_restart_handoff_pending.exchange(
            false,
            std::memory_order_acq_rel
        );
    if (!main_menu_handoff && !pause_restart_handoff) {
        return;
    }
    const int64_t restart_started_ms =
        g_pause_restart_handoff_started_ms.exchange(
            0,
            std::memory_order_acq_rel
        );
    std::lock_guard lock(g_state_mutex);
    deactivate_menu_overlay();
    bumble::text_overlay::clear_kind(
        bumble::text_overlay::TextKind::Menu
    );
    const uint64_t completion_count =
        g_main_menu_handoff_completion_count.fetch_add(
            1u,
            std::memory_order_acq_rel
        ) + 1u;
    std::fprintf(
        stderr,
        "BUMBLE_MAIN_MENU stage=composition_handoff_completed"
        " count=%" PRIu64 " owner=%s readiness_source=%s"
        " elapsed_ms=%" PRId64 " outgoing_menu_cleared=1\n",
        completion_count,
        pause_restart_handoff ? "pause_restart" : "main_menu",
        readiness_source,
        pause_restart_handoff && restart_started_ms != 0
            ? std::max<int64_t>(
                0,
                steady_clock_milliseconds() - restart_started_ms
            )
            : 0
    );
    std::fflush(stderr);
}

uint64_t bumble::graphics_options::main_menu_handoff_completion_count() {
    return g_main_menu_handoff_completion_count.load(std::memory_order_acquire);
}

extern "C" void bumble_handle_native_graphics_options_input(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr ||
        !g_menu_enabled.load(std::memory_order_acquire)) {
        return;
    }

    const uint32_t live_object = low_guest_address(context->r19);
    const uint32_t descriptor = low_guest_address(context->r18);
    const uint32_t phase = read_guest_u32(
        rdram,
        kFrontendObject + kFrontendPhaseOffset
    );
    std::lock_guard lock(g_state_mutex);
    synchronize_control_settings_locked();
    if (live_object == kFrontendObject &&
        descriptor == kMissionCompleteDescriptor &&
        phase == kMissionCompletePhase) {
        activate_mission_complete_menu_locked(rdram);
    }
    if (live_object == kFrontendObject &&
        descriptor == kGameCompletionDescriptor &&
        phase == kGameCompletionPhase) {
        const MenuPointerState pointer = menu_pointer_snapshot();
        const MouseMenuContext previous_context =
            g_mouse_menu_context.exchange(
                MouseMenuContext::Completion,
                std::memory_order_acq_rel
            );
        if (previous_context != MouseMenuContext::Completion) {
            bumble::text_overlay::clear_kind(
                bumble::text_overlay::TextKind::Menu
            );
            g_consumed_pointer_click_revision = pointer.click_revision;
            std::fprintf(
                stderr,
                "BUMBLE_GAME_COMPLETION_SCREEN"
                " stage=pointer_context_armed"
                " click_revision=%" PRIu64
                " stale_entry_click_suppressed=1 anywhere_confirm=1\n",
                pointer.click_revision
            );
            std::fflush(stderr);
            return;
        }

        MenuPointerInteraction interaction{};
        interaction.pointer_moved = false;
        interaction.click_pressed =
            pointer.click_revision != g_consumed_pointer_click_revision;
        if (interaction.click_pressed) {
            g_consumed_pointer_click_revision = pointer.click_revision;
        }
        interaction.input_suppressed =
            bumble::input_bindings::input_suppressed();
        if (interaction.click_pressed && pointer.inside_client &&
            !interaction.input_suppressed) {
            interaction.row_index = 0u;
        }
        apply_pointer_confirm_to_guest(
            rdram,
            interaction,
            guest_address(kCurrentPad),
            context
        );
        if (interaction.click_pressed) {
            std::fprintf(
                stderr,
                "BUMBLE_GAME_COMPLETION_SCREEN"
                " stage=pointer_click"
                " inside_client=%d input_suppressed=%d"
                " accepted=%d anywhere_confirm=1\n",
                pointer.inside_client ? 1 : 0,
                interaction.input_suppressed ? 1 : 0,
                interaction.row_index.has_value() ? 1 : 0
            );
            std::fflush(stderr);
        }
        return;
    }
    if (live_object == kFrontendObject &&
        descriptor == kMissionCompleteDescriptor &&
        phase == kMissionCompletePhase &&
        read_guest_u32(rdram, descriptor + 0x14u) ==
            kMissionCompleteSaveItem &&
        (read_guest_u32(rdram, descriptor + 0x38u) ==
                kMissionCompleteSaveItem ||
            read_guest_u32(rdram, descriptor + 0x38u) ==
                kMissionCompleteContinueItem)) {
        MenuPointerInteraction interaction =
            consume_menu_pointer_interaction_locked(
                MenuOverlayLayout::Completion,
                2u,
                g_mission_complete_pointer_motion_revision
            );
        if (interaction.row_index.has_value()) {
            write_guest_u32(
                rdram,
                kMissionCompleteDescriptor + 0x38u,
                *interaction.row_index == 0u
                    ? kMissionCompleteSaveItem
                    : kMissionCompleteContinueItem
            );
            MEM_H(4, guest_address(kCurrentPad)) = 0;
            MEM_H(6, guest_address(kCurrentPad)) = 0;
        }
        apply_pointer_confirm_to_guest(
            rdram,
            interaction,
            guest_address(kCurrentPad),
            context
        );
        const uint32_t selected = read_guest_u32(
            rdram,
            kMissionCompleteDescriptor + 0x38u
        );
        const bool continue_confirm =
            selected == kMissionCompleteContinueItem &&
            ((static_cast<uint16_t>(context->r2) & kButtonA) != 0u ||
                (interaction.click_pressed &&
                    interaction.row_index == std::optional<size_t>{1u}));
        if (continue_confirm) {
            g_main_menu_handoff_ready.store(false, std::memory_order_release);
            g_main_menu_handoff_pending.store(
                true,
                std::memory_order_release
            );
        }
        if (interaction.click_pressed) {
            std::fprintf(
                stderr,
                "BUMBLE_MENU_POINTER stage=click menu=mission_complete"
                " row_hit=%d row=%s index=%zu"
                " input_suppressed=%d exact_target=1\n",
                interaction.row_index.has_value() ? 1 : 0,
                interaction.row_index.has_value()
                    ? (*interaction.row_index == 0u ? "main_menu" : "continue")
                    : "none",
                interaction.row_index.value_or(0u),
                interaction.input_suppressed ? 1 : 0
            );
            std::fflush(stderr);
        }
        return;
    }
    if (handle_main_menu_input_locked(
            rdram,
            context,
            live_object,
            descriptor,
            phase
        )) {
        return;
    }
    if (live_object != kFrontendObject || descriptor != kOptionsDescriptor ||
        phase != kOptionsPhase) {
        const MouseMenuContext mouse_context =
            g_mouse_menu_context.load(std::memory_order_acquire);
        const bool retain_completion =
            mouse_context == MouseMenuContext::Completion &&
            phase == kMissionCompletePhase;
        const bool retain_handoff =
            g_main_menu_handoff_pending.load(std::memory_order_acquire) &&
            (mouse_context == MouseMenuContext::Main ||
                mouse_context == MouseMenuContext::Completion);
        const bool retain_options =
            mouse_context == MouseMenuContext::Options &&
            phase == kOptionsPhase;
        if (mouse_context != MouseMenuContext::Pause &&
            mouse_context != MouseMenuContext::Main &&
            mouse_context != MouseMenuContext::LevelSelect &&
            !retain_options &&
            !retain_completion && !retain_handoff) {
            deactivate_menu_overlay();
        }
        return;
    }

    RowKind selected{};
    size_t selected_index = 0u;
    uint32_t selected_node = 0u;
    if (!selected_row_locked(
            rdram,
            selected,
            selected_index,
            selected_node
        )) {
        return;
    }

    const gpr current_pad = guest_address(kCurrentPad);
    const MenuOverlayLayout pointer_layout =
        keyboard_bindings_page(g_native_menu.page) ||
            controller_bindings_page(g_native_menu.page) ||
            g_native_menu.page == MenuPage::Gameplay
        ? MenuOverlayLayout::DenseBindings
        : MenuOverlayLayout::Standard;
    MenuPointerInteraction pointer =
        consume_menu_pointer_interaction_locked(
            pointer_layout,
            g_native_menu.row_count,
            g_native_menu.last_pointer_motion_revision
        );
    if (pointer.row_index.has_value()) {
        selected_index = *pointer.row_index;
        selected = g_native_menu.rows[selected_index];
        selected_node = node_address(g_native_menu, selected_index);
        write_guest_u32(
            rdram,
            kOptionsDescriptor + 0x38u,
            selected_node
        );
        g_native_menu.pending_vertical_direction = 0;
        g_native_menu.pending_vertical_node = 0u;
        MEM_H(6, current_pad) = 0;
    }
    apply_pointer_confirm_to_guest(rdram, pointer, current_pad, context);
    if (pointer.click_pressed && pointer.row_index.has_value() &&
        !pointer.input_suppressed) {
        g_native_menu.confirm_neutral_observed = true;
        g_native_menu.entry_confirm_suppressed = true;
        g_native_menu.confirm_armed = true;
    }
    if (pointer.pointer_moved && pointer.row_index.has_value()) {
        std::fprintf(
            stderr,
            "BUMBLE_GRAPHICS_OPTIONS stage=mouse_navigation"
            " source=%s page=%s row=%s index=%zu exact_target=1\n",
            pointer.click_pressed ? "mouse_click" : "mouse_hover",
            menu_page_name(g_native_menu.page),
            row_name(selected),
            selected_index
        );
        std::fflush(stderr);
    }
    if (pointer.click_pressed) {
        std::fprintf(
            stderr,
            "BUMBLE_MENU_POINTER stage=click menu=options page=%s"
            " row_hit=%d row=%s index=%zu"
            " input_suppressed=%d exact_target=1\n",
            menu_page_name(g_native_menu.page),
            pointer.row_index.has_value() ? 1 : 0,
            pointer.row_index.has_value() ? row_name(selected) : "none",
            pointer.row_index.has_value() ? selected_index : 0u,
            pointer.input_suppressed ? 1 : 0
        );
        std::fflush(stderr);
    }
    const uint16_t accepted = static_cast<uint16_t>(context->r2);
    const uint16_t held_buttons = MEM_HU(0, current_pad);
    const uint16_t pressed_buttons = MEM_HU(2, current_pad);
    const int16_t stick_x = MEM_H(4, current_pad);
    const int16_t stick_y = MEM_H(6, current_pad);

    const bool vertical_neutral = stick_y > -15 && stick_y < 15;
    if (stick_y <= -41 && !g_native_menu.vertical_latched) {
        g_native_menu.vertical_latched = true;
        g_native_menu.pending_vertical_direction = 1;
        g_native_menu.pending_vertical_node = selected_node;
    } else if (stick_y >= 41 && !g_native_menu.vertical_latched) {
        g_native_menu.vertical_latched = true;
        g_native_menu.pending_vertical_direction = -1;
        g_native_menu.pending_vertical_node = selected_node;
    } else if (vertical_neutral) {
        g_native_menu.vertical_latched = false;
    }

    const bool vertical_pending =
        g_native_menu.pending_vertical_direction != 0;
    const bool guest_already_navigated = vertical_pending &&
        selected_node != g_native_menu.pending_vertical_node;
    if (guest_already_navigated) {
        std::fprintf(
            stderr,
            "BUMBLE_GRAPHICS_OPTIONS stage=guest_menu_vertical_navigation"
            " source=guest_original guest_already_navigated=1"
            " direction=%d page=%s row=%s index=%zu"
            " node=0x%08" PRIX32 "\n",
            g_native_menu.pending_vertical_direction,
            menu_page_name(g_native_menu.page),
            row_name(selected),
            selected_index,
            selected_node
        );
        std::fflush(stderr);
        g_native_menu.pending_vertical_direction = 0;
        g_native_menu.pending_vertical_node = 0u;
    } else if (vertical_pending && vertical_neutral) {
        const int vertical_direction = g_native_menu.pending_vertical_direction;
        const size_t target_index = vertical_direction > 0
            ? std::min(selected_index + 1u, g_native_menu.row_count - 1u)
            : (selected_index == 0u ? 0u : selected_index - 1u);
        if (target_index != selected_index) {
            selected_index = target_index;
            selected = g_native_menu.rows[selected_index];
            selected_node = node_address(g_native_menu, selected_index);
            write_guest_u32(
                rdram,
                kOptionsDescriptor + 0x38u,
                selected_node
            );
            std::fprintf(
                stderr,
                "BUMBLE_GRAPHICS_OPTIONS stage=guest_menu_vertical_navigation"
                " source=native_fallback guest_already_navigated=0"
                " direction=%d page=%s row=%s index=%zu"
                " node=0x%08" PRIX32 "\n",
                vertical_direction,
                menu_page_name(g_native_menu.page),
                row_name(selected),
                selected_index,
                selected_node
            );
            std::fflush(stderr);
        }
        g_native_menu.pending_vertical_direction = 0;
        g_native_menu.pending_vertical_node = 0u;
    }
    g_native_menu.last_selected_node = selected_node;

    if ((pressed_buttons & kButtonB) != 0u) {
        if (g_native_menu.page != MenuPage::Root) {
            const MenuPage previous_page = g_native_menu.page;
            const MenuPage target = menu_parent_page(previous_page);
            MEM_H(2, current_pad) = static_cast<int16_t>(
                pressed_buttons & ~(kButtonA | kButtonB)
            );
            context->r2 = static_cast<gpr>(accepted & ~kButtonA);
            install_native_menu_locked(rdram, target, true);
            std::fprintf(
                stderr,
                "BUMBLE_GRAPHICS_OPTIONS stage=back_requested"
                " source=back_button from_page=%s to_page=%s\n",
                menu_page_name(previous_page),
                menu_page_name(target)
            );
            std::fflush(stderr);
            return;
        }

        for (size_t index = 0; index < g_native_menu.row_count; ++index) {
            if (g_native_menu.rows[index] != RowKind::Back) {
                continue;
            }
            selected_index = index;
            selected = RowKind::Back;
            selected_node = node_address(g_native_menu, index);
            g_native_menu.last_selected_node = selected_node;
            write_guest_u32(
                rdram,
                kOptionsDescriptor + 0x38u,
                selected_node
            );
            write_guest_u32(rdram, kOptionsDescriptor + 0x70u, 0u);
            MEM_H(2, current_pad) = static_cast<int16_t>(
                pressed_buttons & ~(kButtonA | kButtonB)
            );
            context->r2 = static_cast<gpr>(
                (accepted & ~(kButtonA | kButtonB)) | kButtonA
            );
            std::fprintf(
                stderr,
                "BUMBLE_GRAPHICS_OPTIONS stage=back_requested"
                " source=back_button from_page=root to_phase=0x%08" PRIX32
                " successor_owner=guest_menu\n",
                kMainMenuPhase
            );
            std::fflush(stderr);
            return;
        }
    }

    int direction = 0;
    bool consume_a = false;
    if (!g_native_menu.confirm_armed) {
        const bool accepted_a = (accepted & kButtonA) != 0u;
        const bool held_a = (held_buttons & kButtonA) != 0u;
        if (!accepted_a && !held_a) {
            g_native_menu.confirm_neutral_observed = true;
            if (g_native_menu.entry_confirm_suppressed ||
                std::chrono::steady_clock::now() >=
                    g_native_menu.confirm_fallback_not_before) {
                g_native_menu.confirm_armed = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_GRAPHICS_OPTIONS stage=entry_confirm_armed"
                    " carried_edge_suppressed=%d neutral_observed=1\n",
                    g_native_menu.entry_confirm_suppressed ? 1 : 0
                );
                std::fflush(stderr);
            }
        } else if (accepted_a) {
            if (g_native_menu.confirm_neutral_observed && held_a) {
                g_native_menu.confirm_armed = true;
                direction = 1;
                consume_a = true;
            } else {
                g_native_menu.entry_confirm_suppressed = true;
                consume_a = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_GRAPHICS_OPTIONS stage=entry_confirm_suppressed"
                    " held=0x%04" PRIX16 " pressed=0x%04" PRIX16
                    " neutral_observed=%d\n",
                    held_buttons,
                    pressed_buttons,
                    g_native_menu.confirm_neutral_observed ? 1 : 0
                );
                std::fflush(stderr);
            }
        }
    } else if ((accepted & kButtonA) != 0u) {
        direction = 1;
        consume_a = true;
    }

    if (direction == 0) {
        if (stick_x >= 41 && !g_native_menu.horizontal_latched) {
            direction = 1;
            g_native_menu.horizontal_latched = true;
        } else if (stick_x <= -41 && !g_native_menu.horizontal_latched) {
            direction = -1;
            g_native_menu.horizontal_latched = true;
        } else if (stick_x > -15 && stick_x < 15) {
            g_native_menu.horizontal_latched = false;
        }
    }

    const bool ending_credits_confirm =
        selected == RowKind::EndingCredits && consume_a && direction == 1 &&
        (accepted & kButtonA) != 0u;
    const bool high_scores_confirm = selected == RowKind::HighScores &&
        consume_a && direction == 1 && (accepted & kButtonA) != 0u &&
        bumble::game_completion_screen::open_high_scores_from_options(rdram);
    const bool root_back_confirm = g_native_menu.page == MenuPage::Root &&
        selected == RowKind::Back && consume_a && direction == 1 &&
        (accepted & kButtonA) != 0u;
    bumble::input_bindings::InputAction binding_action{};
    const bool binding_row =
        row_binding_action(selected, binding_action);
    const bool native_action_confirm = consume_a && direction == 1 &&
        (accepted & kButtonA) != 0u;
    const bool binding_confirm = native_action_confirm && binding_row;
    const bool reset_controls_confirm = native_action_confirm &&
        (selected == RowKind::ResetControls ||
         selected == RowKind::ResetKeyboardBindings ||
         selected == RowKind::ResetControllerBindings);
    const bool reset_joystick_confirm = native_action_confirm &&
        selected == RowKind::ResetJoystick;
    const bool clear_save_confirm = native_action_confirm &&
        selected == RowKind::ConfirmClearSaveData;
    const bool reset_defaults_confirm = native_action_confirm &&
        selected == RowKind::ConfirmResetDefaults;
    const bool page_confirm = consume_a && direction == 1 &&
        (accepted & kButtonA) != 0u &&
        (selected == RowKind::ControlsMenu ||
            selected == RowKind::InterfaceHudSettings ||
            selected == RowKind::GameplaySettings ||
            selected == RowKind::KeyboardBindingsMenu ||
            selected == RowKind::ControllerBindingsMenu ||
            selected == RowKind::MouseSettingsMenu ||
            selected == RowKind::JoystickSettingsMenu ||
            selected == RowKind::DisplaySettings ||
            selected == RowKind::RenderingSettings ||
            selected == RowKind::CheatsMenu ||
            selected == RowKind::CheatSettings ||
            selected == RowKind::ClearSaveDataMenu ||
            selected == RowKind::ResetDefaultsMenu ||
            (selected == RowKind::Back &&
                g_native_menu.page != MenuPage::Root));
    if (consume_a && !ending_credits_confirm && !high_scores_confirm && !root_back_confirm) {
        context->r2 = static_cast<gpr>(accepted & ~kButtonA);
    }
    write_guest_u32(rdram, kOptionsDescriptor + 0x70u, 0u);
    if (high_scores_confirm) {
        deactivate_menu_overlay();
    } else if (ending_credits_confirm) {
        deactivate_menu_overlay();
        std::fprintf(
            stderr,
            "BUMBLE_GRAPHICS_OPTIONS stage=ending_credits_requested"
            " row=ending_credits source=guest_menu_successor"
            " target_phase=0x%08" PRIX32
            " level=%" PRIu32 " level_identifier=\"46Outro\""
            " guest_state_writes=0 persisted=0\n",
            kEndingCreditsPhase,
            kEndingCreditsLevelIndex
        );
        std::fflush(stderr);
    } else if (root_back_confirm) {
        std::fprintf(
            stderr,
            "BUMBLE_GRAPHICS_OPTIONS stage=back_requested"
            " source=visible_row from_page=root to_phase=0x%08" PRIX32
            " successor_owner=guest_menu\n",
            kMainMenuPhase
        );
        std::fflush(stderr);
    } else if (binding_confirm) {
        const auto profile = binding_profile_for_page(g_native_menu.page);
        bumble::input_bindings::begin_capture(profile, binding_action);
        refresh_menu_strings_locked();
    } else if (reset_controls_confirm) {
        if (selected == RowKind::ResetKeyboardBindings) {
            bumble::input_bindings::reset_profile(
                bumble::input_bindings::BindingProfile::KeyboardMouse
            );
        } else if (selected == RowKind::ResetControllerBindings) {
            bumble::input_bindings::reset_profile(
                bumble::input_bindings::BindingProfile::Controller
            );
        } else {
            bumble::input_bindings::reset_all();
        }
        save_control_settings();
        refresh_menu_strings_locked();
    } else if (reset_joystick_confirm) {
        reset_joystick_settings();
        save_control_settings();
        refresh_menu_strings_locked();
    } else if (clear_save_confirm) {
        g_native_menu.clear_save_failed = !clear_save_data_locked();
        if (g_native_menu.clear_save_failed) {
            refresh_menu_strings_locked();
        } else {
            install_native_menu_locked(rdram, MenuPage::Root, true);
        }
    } else if (reset_defaults_confirm) {
        apply_settings(bumble::graphics_options::Settings{});
        install_native_menu_locked(rdram, MenuPage::Gameplay, true);
    } else if (page_confirm) {
        MenuPage target = MenuPage::Root;
        switch (selected) {
        case RowKind::ControlsMenu:
            target = MenuPage::Controls;
            break;
        case RowKind::InterfaceHudSettings:
            target = MenuPage::InterfaceHud;
            break;
        case RowKind::GameplaySettings:
            target = MenuPage::Gameplay;
            break;
        case RowKind::KeyboardBindingsMenu:
            target = MenuPage::KeyboardBindings;
            break;
        case RowKind::ControllerBindingsMenu:
            target = MenuPage::ControllerBindings;
            break;
        case RowKind::MouseSettingsMenu:
            target = MenuPage::MouseSettings;
            break;
        case RowKind::JoystickSettingsMenu:
            target = MenuPage::JoystickSettings;
            break;
        case RowKind::DisplaySettings:
            target = MenuPage::Display;
            break;
        case RowKind::RenderingSettings:
            target = MenuPage::Rendering;
            break;
        case RowKind::CheatsMenu:
            target = MenuPage::CheatsAndTests;
            break;
        case RowKind::CheatSettings:
            target = MenuPage::Cheats;
            break;
        case RowKind::ClearSaveDataMenu:
            target = MenuPage::ClearSaveData;
            break;
        case RowKind::ResetDefaultsMenu:
            target = MenuPage::ResetDefaults;
            break;
        case RowKind::Back:
            target = menu_parent_page(g_native_menu.page);
            break;
        default:
            break;
        }
        if (!install_native_menu_locked(rdram, target, true)) {
            std::fprintf(
                stderr,
                "BUMBLE_GRAPHICS_OPTIONS stage=guest_submenu_install_failed"
                " target=%s\n",
                menu_page_name(target)
            );
            std::fflush(stderr);
        }
    } else if (direction != 0 && !root_back_confirm &&
               selected != RowKind::EndingCredits && selected != RowKind::HighScores) {
        cycle_row_locked(selected, direction);
    }
}
