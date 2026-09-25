#include "native_widescreen.hpp"

#include "native_text_overlay_state.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cinttypes>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <string>
#include <stdexcept>
#include <vector>

#include "librecomp/addresses.hpp"
#include "native_campaign_levels.hpp"
#include "native_checkpoint_bridge.hpp"
#include "native_combat_feedback.hpp"
#include "native_electric_effect.hpp"
#include "native_gameplay_options.hpp"
#include "native_graphics_options.hpp"

#include "native_input_bindings.hpp"
#include "native_visible_ui_state.hpp"
#include "native_weapon_system.hpp"
#include "common/rt64_performance_profiler.h"
#include "common/rt64_bumble_ui.h"

namespace {
int64_t g_text_delay_remainder = 0;
uint32_t g_text_delay_speed = 1u;
}

extern "C" void bumble_scale_cutscene_text_delay(uint8_t* rdram, recomp_context* context) {
    const int32_t delay = MEM_W(0x10, context->r29);
    const uint32_t speed = bumble::graphics_options::cutscene_text_speed();
    if (speed != g_text_delay_speed) {
        g_text_delay_remainder = 0;
        g_text_delay_speed = speed;
    }
    if (delay > 0 && speed > 1u) {
        const int64_t total = delay + g_text_delay_remainder;
        const int32_t scaled = static_cast<int32_t>(std::max<int64_t>(1, total / speed));
        g_text_delay_remainder = std::max<int64_t>(-int64_t(speed), total - int64_t(scaled) * speed);
        MEM_W(0x10, context->r29) = scaled;
    }
}

namespace {

constexpr uint32_t kTerrainRowBounds = 0x801044E0u;
constexpr int32_t kMinimumCell = -32;
constexpr int32_t kMaximumCell = 31;
constexpr int32_t kTerrainGuardCells = 4;
constexpr double kNoFogTerrainFootprintScale = 2.0;
constexpr uint32_t kOriginalTerrainCandidateCapacity = 800u;
constexpr uint32_t kTerrainScratchCapacity = 64u * 64u;
constexpr uint32_t kTerrainCandidateBytes = 8u;
constexpr uint32_t kTerrainScratchBytes =
    kTerrainScratchCapacity * kTerrainCandidateBytes;
constexpr uint32_t kTerrainScratchGuardBytes = 64u;
constexpr uint8_t kTerrainScratchGuardValue = 0xA5u;
constexpr uint32_t kOriginalTerrainScratch = 0x800D5250u;
constexpr uint32_t kTerrainCellTable = 0x803CE000u;
constexpr uint32_t kTerrainMatrixCount = 0x80105228u;
constexpr uint32_t kTerrainCameraEyeZStackOffset = 0xD8u;
constexpr uint32_t kTerrainCameraCullZStackOffset = 0xFCu;
constexpr uint32_t kTerrainCameraEyeXStackOffset = 0xD0u;
constexpr uint32_t kTerrainCameraCullXStackOffset = 0xF4u;
constexpr uint32_t kTerrainCameraFocusXStackOffset = 0xDCu;
constexpr uint32_t kTerrainCameraFocusZStackOffset = 0xE4u;
constexpr float kTerrainCellSize = 160.0f;
constexpr double kTerrainRadiusAtOriginalAspect = 12.0;
constexpr double kTerrainForwardZEpsilon = 1.0e-3;
constexpr uint32_t kAuthoredMatrixCapacity = 700u;
constexpr uint32_t kExtendedMatrixCapacity = 8192u;
constexpr uint32_t kExtendedMatrixHeaderBytes = 0x100u;
constexpr uint32_t kExtendedMatrixBytes =
    kExtendedMatrixHeaderBytes + kExtendedMatrixCapacity * 0x40u;
constexpr uint32_t kExtendedDisplayListOffset = 0x00080200u;
constexpr uint32_t kExtendedDisplayListBytes = 0x00020000u;
constexpr uint32_t kExtendedFrameArenaBytes =
    kExtendedDisplayListOffset + kExtendedDisplayListBytes;
static_assert(kExtendedDisplayListOffset >= kExtendedMatrixBytes + 0x100u);
constexpr uint32_t kExtendedMatrixGuardBytes = 64u;
constexpr uint8_t kExtendedMatrixGuardValue = 0x6Du;
constexpr size_t kFrameMatrixArenaCount = 4u;
constexpr uint32_t kPostObjectMatrixQueueCapacity = 0xB4u;
static_assert(kPostObjectMatrixQueueCapacity < kAuthoredMatrixCapacity);
constexpr uint32_t kAuthoredTerrainPromotionMatrixCeiling =
    kAuthoredMatrixCapacity - kPostObjectMatrixQueueCapacity;
static_assert(kAuthoredTerrainPromotionMatrixCeiling == 520u);
constexpr uint32_t kNoFogDepthMatrixShareNumerator = 1u;
constexpr uint32_t kNoFogDepthMatrixShareDenominator = 2u;
static_assert(
    kNoFogDepthMatrixShareNumerator <
    kNoFogDepthMatrixShareDenominator
);
constexpr uint32_t kTerrainOppositeDepthGuardRows = 2u;
constexpr double kTerrainDepthDirectionHysteresis = 0.20;
constexpr double kOriginalAspect = 4.0 / 3.0;
constexpr uint32_t kDisplayListCursor = 0x80035F30u;
constexpr uint32_t kFrameMatrixBase = 0x80105224u;
constexpr uint32_t kFrameBaseFromMatrixBase = 0x68u;
constexpr uint32_t kDisplayListArenaOffset = 0xB068u;
// Reserve 16 bytes for E900/DF and 8 for the strict cursor < end check.
constexpr uint32_t kDisplayListArenaEndOffset = 0x14CA8u;
constexpr uint32_t kDisplayListFinalCommandReserve = 0x18u;
constexpr uint32_t kNativeDisplayListEndOffset =
    kDisplayListArenaEndOffset - kDisplayListFinalCommandReserve;
static_assert(kNativeDisplayListEndOffset == 0x14C90u);
constexpr uint32_t kDisplayListArenaBytes =
    kDisplayListArenaEndOffset - kDisplayListArenaOffset;
static_assert(kDisplayListArenaBytes == 0x9C40u);
constexpr uint32_t kUiScopeBeginCommandBytes = 0x60u;
constexpr uint32_t kUiNestedScopeEndCommandBytes = 0x48u;
constexpr uint32_t kUiRootScopeEndCommandBytes = 0x50u;
constexpr uint32_t kRdramSize = 0x00800000u;
constexpr uint32_t kScreenWidth = 320u;
constexpr uint32_t kScreenHeight = 240u;
constexpr uint32_t kPlayerHealthAddress = 0x800E92D8u;
constexpr uint32_t kPlayerMaximumHealthAddress = 0x800E9678u;
constexpr float kGameplayUiScale = 0.5f;
constexpr uint32_t kFrontendObject = 0x800FFF80u;
constexpr uint32_t kFrontendCurrentPhaseOffset = 0x00u;
constexpr uint32_t kFrontendPhaseOffset = 0x84u;
constexpr uint32_t kMainMenuPhase = 0x0Cu;
constexpr uint32_t kLevelSelectPhase = 0x0Eu;
constexpr uint32_t kOptionsMenuPhase = 0x0Fu;
constexpr uint32_t kMainMenuDescriptor = 0x800FC940u;
constexpr uint32_t kOptionsMenuDescriptor = 0x800FCC90u;
constexpr uint32_t kLevelSelectDescriptor = 0x800FD320u;
constexpr uint32_t kPauseMenuDescriptor = 0x800FD458u;
constexpr uint32_t kMissionCompleteDescriptor = 0x800FD648u;
constexpr uint32_t kPauseMenuVisible = 0x80100124u;
constexpr uint32_t kAnimatedTextScaleOffset = 0x110u;
constexpr float kMainMenuPulseAmplitudeScale = 0.0f;
constexpr uint32_t kMissionBriefingPhase = 0x18u;
constexpr uint32_t kMissionGameplayPhase = 0x19u;
constexpr uint32_t kMissionBriefingDescriptor = 0x800FD3F0u;
constexpr uint32_t kMissionBriefingDescriptorFlags = 0x00000023u;
constexpr uint32_t kLocalizedTextPointerTable = 0x8010013Cu;
constexpr char kMissionObjectivePrefix[] = "A forward";
constexpr char kMissionObjectiveContinuation[] = "Herd scout";
constexpr char kMissionTitlePrefix[] = "MISSION 1";
constexpr uint32_t kNativeTextMaximumBytes = 1024u;
constexpr size_t kBriefingTextSampleCapacity = 48u;
constexpr uint32_t kMissionTitleLocalizationIndex = 0xE3u;
constexpr uint32_t kMissionObjectiveLocalizationIndex = 0xE4u;
constexpr uint32_t kMissionTitleCallPc = 0x8009274Cu;
constexpr uint32_t kBriefingQueueCallPc = 0x80095588u;
constexpr uint32_t kBriefingShadowCallPc = 0x800A98F0u;
constexpr uint32_t kBriefingWhiteCallPc = 0x800A991Cu;
constexpr uint32_t kScriptUiObject = 0x800D735Cu;
constexpr uint32_t kBriefingPanelLeft = 0x801075F4u;
constexpr uint32_t kBriefingPanelRight = 0x801075FCu;
constexpr char kScriptPanelScopeOwner[] = "func_800928BC.script_panel";
constexpr char kScriptTextScopeOwner[] = "func_800926AC.complete_text";
constexpr char kBriefingPanelScopeOwner[] = "func_800A9580.briefing_panel";
constexpr char kBriefingLineScopeOwner[] = "func_800A9580.briefing_line";
constexpr char kDescriptorTextScopeOwner[] = "func_800AE9AC";
constexpr char kTextLineScopeOwner[] = "func_800B9748";
constexpr char kDirectTextScopeOwner[] = "func_800B8180";
constexpr char kGameplayRightHudScopeOwner[] =
    "func_800A718C.score_group";
constexpr char kGameplayStatusBarScopeOwner[] =
    "func_800A5C60.status_bar";
constexpr char kGameplayMissionGaugeScopeOwner[] =
    "func_800A5C60.mission_gauge";
constexpr char kGameplayHudRootScopeOwner[] =
    "func_800A5C60.gameplay_hud_root";
constexpr char kGameplayRadarScopeOwner[] =
    "func_800A5C60.radar";
constexpr char kGameplayWeaponAmmoScopeOwner[] =
    "func_800A5C60.weapon_ammo";
constexpr char kGameplayWeaponModelScopeOwner[] =
    "func_800A5C60.weapon_model";
constexpr char kGameplayKeyScopeOwner[] =
    "func_800A9E4C.key_group";
constexpr char kGameplayBonusScopeOwner[] =
    "func_800A6A08.bonus_group";
constexpr uint32_t kWeaponAmmoRightInset = 40u;
constexpr uint32_t kWeaponAmmoDestinationY = 135u;
constexpr uint32_t kWeaponSelectionStateBase = 0x800E92C0u;
constexpr uint32_t kWeaponSelectionStateStride = 0x54u;
constexpr uint32_t kWeaponAmmoOffset = 0x24u;
constexpr uint32_t kWeaponSelectionIndexOffset = 0x58u;
constexpr uint32_t kWeaponCarouselRequestBase = 0x800F5E40u;
constexpr uint32_t kWeaponCarouselStateBase = 0x800F5E48u;
constexpr uint32_t kWeaponCarouselLeftBase = 0x800F5E60u;
constexpr uint32_t kWeaponCarouselMiddleBase = 0x800F5E68u;
constexpr uint32_t kWeaponCarouselRightBase = 0x800F5E70u;
constexpr uint32_t kGameplayLeftStackX = RT64::BumbleUI::LeftStackX;
constexpr uint32_t kGameplayBonusDestinationY = 6u;
constexpr uint32_t kGameplayScoreDestinationY = 22u;
constexpr uint32_t kGameplayBonusValue = 0x800E9648u;
constexpr int32_t kRadarAnchorX = 80;
constexpr int32_t kWeaponModelAnchorX = 72;
constexpr uint32_t kWeaponListRightInset = 56u;
constexpr uint32_t kWeaponListSelectedY = 177u;
constexpr int32_t kWeaponListRowStep = 18;
constexpr float kWeaponListScale = 0.42f;
constexpr size_t kWeaponListVisibleRows = 5u;
constexpr uint64_t kWeaponListStableSlot = UINT64_C(0x574541504F4E4C53);
constexpr uint32_t kFrontendState = 0x800FFF80u;
constexpr uint32_t kFrontendWorldActiveOffset = 0x24u;
constexpr uint32_t kFrontendPlayerLayoutOffset = 0x0Cu;
constexpr uint32_t kSinglePlayerLayout = 1u;
constexpr uint32_t kWorldViewport = 0x800CC8C8u;
constexpr std::array<uint32_t, 4> kAuthoredWorldViewport{{
    0x025801A0u, 0x01FF0000u, 0x028001E0u, 0x01FF0000u,
}};
constexpr std::array<uint32_t, 4> kFullFrameWorldViewport{{
    0x028001E0u, 0x01FF0000u, 0x028001E0u, 0x01FF0000u,
}};
constexpr std::array<uint32_t, 2> kAuthoredWorldScissor{{
    0xED028040u, 0x004D8380u,
}};
constexpr std::array<uint32_t, 2> kFullFrameWorldScissor{{
    0xED000000u, 0x005003C0u,
}};
constexpr uint32_t kAuthoredSinglePlayerDepthClearUpper = 0xF64D8384u;
constexpr uint32_t kAuthoredSinglePlayerDepthClearLower = 0x00028040u;
constexpr uint32_t kFullFrameDepthClearUpper = 0xF64FC3C0u;
constexpr uint32_t kFullFrameDepthClearLower = 0x00000000u;
constexpr uint32_t kAuthoredSinglePlayerColorClearUpper = 0xF64D4384u;
constexpr uint32_t kAuthoredSinglePlayerColorClearLower = 0x00028040u;
constexpr uint32_t kFullFrameColorClearUpper = 0xF64FC3C0u;
constexpr uint32_t kFullFrameColorClearLower = 0x00000000u;
constexpr uint32_t kLevelBackgroundRgb = 0x800E952Cu;
constexpr uint32_t kAuthoredWorldAspectBits = 0x3FB851ECu; // 1.44f
constexpr uint32_t kFullFrameWorldAspectBits = 0x3FAAAAABu; // 4.0f / 3.0f
constexpr double kAuthoredProjectionFarCoefficient = 0.97;
constexpr double kAuthoredObjectBaseRadiusCoefficient = 0.5;
constexpr double kAuthoredStructuralDistanceScale = 1.0;
constexpr double kNoFogStructuralDistanceScale = 16.0;

// Use the recomp ABI for guest endianness; IDs are from rt64_extended_gbi.h.
constexpr uint32_t kExtendedOpcode = 0x64u;
constexpr uint32_t kSetRdramExtendedV1 = 0x00002Cu;
constexpr uint32_t kMatrixGroupV1 = 0x00000Cu;
constexpr uint32_t kPopMatrixGroupV1 = 0x00000Du;
thread_local bool g_world_camera_scope_open = false;
std::atomic_uint32_t g_world_camera_generation{0};
std::atomic_uint32_t g_script_camera_record{0};
thread_local uint32_t g_world_camera_phase = UINT32_MAX;
thread_local uint32_t g_world_camera_level = UINT32_MAX;
constexpr uint32_t kSetScissorV1 = 0x000005u;
constexpr uint32_t kSetRectAlignV1 = 0x000006u;
constexpr uint32_t kSetViewportAlignV1 = 0x000007u;
constexpr uint32_t kSetScissorAlignV1 = 0x000008u;
constexpr uint32_t kPushViewportV1 = 0x000015u;
constexpr uint32_t kPopViewportV1 = 0x000016u;
constexpr uint32_t kPushScissorV1 = 0x000017u;
constexpr uint32_t kPopScissorV1 = 0x000018u;
constexpr uint32_t kSetRectAspectV1 = 0x000033u;
constexpr uint32_t kHudBeginV1 = 0x000039u;
constexpr uint32_t kOriginLeft = 0x000u;
constexpr uint32_t kOriginRight = 0x400u;
constexpr uint32_t kOriginNone = 0x800u;
constexpr uint32_t kOriginHalfScale = 0x001u;
constexpr uint32_t kOriginAnchorTop = 0x002u;
constexpr uint32_t kOriginAnchorBottom = 0x004u;
constexpr uint32_t kOriginWeaponBottomRight = 0x008u;
constexpr uint32_t kOriginRadarBottomLeft = 0x010u;
constexpr uint32_t kOriginStatusTopLeft = 0x020u;
constexpr uint32_t kAspectAuto = 0u;
constexpr uint32_t kAspectAdjust = 2u;
constexpr int32_t kCenterSafeLeft = 96;
constexpr int32_t kCenterSafeRight = 224;
constexpr int32_t kFullWidthEdgeTolerance = 16;
constexpr size_t kUiScopeCapacity = 32u;

enum class UiOrigin : uint32_t {
    Left = kOriginLeft,
    Right = kOriginRight,
    None = kOriginNone,
};

enum class UiVerticalAnchor : uint8_t {
    Automatic,
    Top,
    Middle,
    Bottom,
};

struct UiAlignment {
    UiOrigin left = UiOrigin::None;
    UiOrigin right = UiOrigin::None;
    UiOrigin viewport = UiOrigin::None;
};

struct UiScope {
    UiAlignment alignment{};
    const char* owner = nullptr;
    bool half_scale = false;
    UiVerticalAnchor vertical_anchor = UiVerticalAnchor::Automatic;
    uint32_t extra_origin_flags = 0u;
};

struct MainMenuTextPulseOverride {
    uint32_t scale_address = 0u;
    uint32_t original_scale_word = 0u;
    bool active = false;
};

struct GuestDrawSuppression {
    uint32_t saved_cursor = 0u;
    bool active = false;
};

enum class DirectTextMetadataKind : uint8_t {
    CallerStackWord,
    EquivalentStackWords,
    AuthoredCenterSafe,
    InheritedOuterDescriptor,
};

struct DirectTextCallerSpec {
    uint32_t call_pc = 0;
    const char* owner = nullptr;
    DirectTextMetadataKind metadata_kind =
        DirectTextMetadataKind::AuthoredCenterSafe;
    uint32_t primary_offset = 0;
    uint32_t alternate_offset = 0;
};

constexpr std::array<DirectTextCallerSpec, 31> kDirectTextCallers{{
    {0x800AE03Cu, "func_800ADACC", DirectTextMetadataKind::AuthoredCenterSafe, 0x0u, 0x0u},
    {0x800AF47Cu, "func_800AF274", DirectTextMetadataKind::CallerStackWord, 0x78u, 0x0u},
    {0x800AF5C0u, "func_800AF274", DirectTextMetadataKind::CallerStackWord, 0x58u, 0x0u},
    {0x800AF720u, "func_800AF274", DirectTextMetadataKind::CallerStackWord, 0x98u, 0x0u},
    {0x800AFA34u, "func_800AF274", DirectTextMetadataKind::CallerStackWord, 0x58u, 0x0u},
    {0x800AFB68u, "func_800AF274", DirectTextMetadataKind::CallerStackWord, 0x78u, 0x0u},
    {0x800AFC9Cu, "func_800AF274", DirectTextMetadataKind::CallerStackWord, 0x98u, 0x0u},
    {0x800B0B84u, "func_800B08E4", DirectTextMetadataKind::CallerStackWord, 0x38u, 0x0u},
    {0x800B0D08u, "func_800B08E4", DirectTextMetadataKind::CallerStackWord, 0x38u, 0x0u},
    {0x800B0E8Cu, "func_800B08E4", DirectTextMetadataKind::CallerStackWord, 0x38u, 0x0u},
    {0x800B13B4u, "func_800B11B8", DirectTextMetadataKind::CallerStackWord, 0x78u, 0x0u},
    {0x800B14F8u, "func_800B11B8", DirectTextMetadataKind::CallerStackWord, 0x58u, 0x0u},
    {0x800B1658u, "func_800B11B8", DirectTextMetadataKind::CallerStackWord, 0x98u, 0x0u},
    {0x800B1968u, "func_800B11B8", DirectTextMetadataKind::CallerStackWord, 0x58u, 0x0u},
    {0x800B1A9Cu, "func_800B11B8", DirectTextMetadataKind::CallerStackWord, 0x78u, 0x0u},
    {0x800B1BD0u, "func_800B11B8", DirectTextMetadataKind::CallerStackWord, 0x98u, 0x0u},
    {0x800B37F8u, "func_800B35D4", DirectTextMetadataKind::CallerStackWord, 0x80u, 0x0u},
    {0x800B393Cu, "func_800B35D4", DirectTextMetadataKind::CallerStackWord, 0x60u, 0x0u},
    {0x800B3A90u, "func_800B35D4", DirectTextMetadataKind::CallerStackWord, 0xA0u, 0x0u},
    {0x800B3CE0u, "func_800B35D4", DirectTextMetadataKind::CallerStackWord, 0x60u, 0x0u},
    {0x800B3E14u, "func_800B35D4", DirectTextMetadataKind::CallerStackWord, 0xA0u, 0x0u},
    {0x800B3FACu, "func_800B35D4", DirectTextMetadataKind::CallerStackWord, 0x80u, 0x0u},
    {0x800B4300u, "func_800B35D4", DirectTextMetadataKind::CallerStackWord, 0x80u, 0x0u},
    {0x800B4440u, "func_800B35D4", DirectTextMetadataKind::EquivalentStackWords, 0x80u, 0xC0u},
    {0x800B4A40u, "func_800B477C", DirectTextMetadataKind::CallerStackWord, 0x38u, 0x0u},
    {0x800B4BC4u, "func_800B477C", DirectTextMetadataKind::CallerStackWord, 0x38u, 0x0u},
    {0x800B4D48u, "func_800B477C", DirectTextMetadataKind::CallerStackWord, 0x38u, 0x0u},
    {0x800B4F24u, "func_800B477C", DirectTextMetadataKind::CallerStackWord, 0x38u, 0x0u},
    {0x800B5B48u, "func_800B5638", DirectTextMetadataKind::CallerStackWord, 0x18u, 0x0u},
    {0x800B5CB0u, "func_800B5638", DirectTextMetadataKind::CallerStackWord, 0x18u, 0x0u},
    {0x800B98A8u, "func_800B9748", DirectTextMetadataKind::InheritedOuterDescriptor, 0x0u, 0x0u},
}};

struct PendingDirectTextCall {
    size_t caller_index = 0;
    uint32_t flags = 0;
    uint32_t face_rgba = 0xFFFFFFFFu;
    bool valid = false;
    bool flags_valid = false;
};

std::atomic_uint64_t g_render_size{0};
std::atomic_bool g_enabled{false};
std::atomic<float> g_fog_scale{0.0f};
std::atomic_uint64_t g_expansion_calls{0};
std::atomic_uint64_t g_no_fog_expansion_calls{0};
std::atomic_uint64_t g_terrain_scratch_verified_calls{0};
std::atomic_uint64_t g_terrain_scratch_failures{0};
std::atomic_uint64_t g_last_logged_size{0};
std::atomic_uint64_t g_last_publish_signature{UINT64_MAX};
std::atomic_bool g_completion_credit_text_logged{false};
std::atomic<float> g_visibility_camera_eye_x{0.0f};
std::atomic<float> g_visibility_camera_eye_z{0.0f};
std::atomic<float> g_visibility_camera_cull_x{0.0f};
std::atomic<float> g_visibility_camera_cull_z{0.0f};
std::atomic<float> g_visibility_camera_forward_x{0.0f};
std::atomic<float> g_visibility_camera_forward_z{0.0f};
std::atomic_uint64_t g_visibility_camera_generation{0};
std::atomic_bool g_visibility_camera_valid{false};
std::atomic_uint32_t g_terrain_sample_original_cells{0u};
std::atomic_uint32_t g_terrain_sample_candidate_cells{0u};
std::atomic_uint32_t g_terrain_sample_visible_cells{0u};
std::atomic_int32_t g_terrain_sample_first_row{0};
std::atomic_int32_t g_terrain_sample_last_row{0};
std::atomic_uint64_t g_terrain_sample_generation{0u};
std::atomic_bool g_terrain_sample_valid{false};
std::atomic_int g_terrain_primary_depth_step{0};
std::atomic_uint32_t g_logged_ui_origins{0};
std::atomic_uint64_t g_logged_direct_text_callers{0};
std::atomic_bool g_mission_briefing_queue_owner_logged{false};
std::atomic_bool g_mission_briefing_render_owner_logged{false};
std::atomic_uint64_t g_last_briefing_wrap_signature{UINT64_MAX};
std::atomic_bool g_main_menu_pulse_reduction_logged{false};
std::atomic_bool g_single_player_frame_scissor_logged{false};
std::atomic_bool g_single_player_depth_clear_logged{false};
std::atomic_bool g_single_player_color_clear_logged{false};
std::atomic_bool g_single_player_sky_clear_logged{false};
std::atomic_uint64_t g_last_projection_far_signature{UINT64_MAX};
std::atomic_uint64_t g_last_weapon_carousel_signature{UINT64_MAX};
std::atomic_uint32_t g_display_list_frame_base{0u};
std::atomic_uint32_t g_active_display_list_base{0u};
std::atomic_uint32_t g_active_display_list_end{0u};
std::atomic_uint32_t g_active_matrix_capacity{kAuthoredMatrixCapacity};
std::atomic_uint64_t g_extended_matrix_frame_count{0u};
std::atomic_uint64_t g_extended_matrix_failure_count{0u};
std::atomic_bool g_task_display_list_owner_logged{false};
std::atomic_bool g_extended_display_list_capacity_logged{false};
std::atomic_bool g_native_menu_prefix_suppression_logged{false};
std::atomic_bool g_legacy_level_entry_suppression_latched{false};
std::atomic_uint64_t g_legacy_level_entry_suppression_count{0u};
std::atomic_bool g_post_commit_level_select_suppression_latched{false};
std::atomic_uint64_t g_post_commit_level_select_suppression_count{0u};

struct FrameMatrixArena {
    uint8_t* rdram = nullptr;
    uint8_t* allocation = nullptr;
    uint32_t authored_frame_base = 0u;
    uint32_t guest_base = 0u;
};

std::array<FrameMatrixArena, kFrameMatrixArenaCount> g_frame_matrix_arenas{};
std::mutex g_frame_matrix_arena_mutex;

struct TerrainScratchState {
    uint8_t* rdram = nullptr;
    uint8_t* allocation = nullptr;
    uint32_t guest_base = 0;
    uint32_t expected_entries = 0;
    bool active = false;
    bool producer_relocated = false;
    bool consumer_relocated = false;
};

thread_local TerrainScratchState g_terrain_scratch{};
thread_local bool g_static_terrain_matrix_scope_open = false;
thread_local bool g_actor_matrix_scope_open = false;
thread_local uint32_t g_actor_matrix_owner = 0;
thread_local uint32_t g_model_role = 0;
thread_local bool g_model_part_scope_open = false;
struct ModelPartHistory {
    std::array<uint32_t, 4> key{};
    uint32_t id = 0;
};
struct ActorHistory {
    uint32_t address = 0;
    uint16_t serial = 0;
    uint32_t id = 0;
    std::vector<ModelPartHistory> parts;
};
std::array<ActorHistory, 512> g_actor_history{};
std::mutex g_actor_history_mutex;
uint32_t g_next_actor_id = 0x10000000u;

uint32_t allocate_actor_id() {
    if (g_next_actor_id == 0x40000000u) {
        throw std::runtime_error("Actor temporal identity exhausted");
    }
    return g_next_actor_id++;
}

uint32_t actor_history_id(uint8_t* rdram, uint32_t actor) {
    const uint16_t serial = MEM_HU(0x7E, static_cast<int32_t>(actor));
    const uint32_t slot = serial & 0x1FFu;
    const int32_t entry = static_cast<int32_t>(0x800E2408u + slot * 8u);
    if (static_cast<uint32_t>(MEM_W(4, entry)) != actor || MEM_HU(0, entry) != serial) {
        throw std::runtime_error("Actor registration does not own draw");
    }
    std::lock_guard lock(g_actor_history_mutex);
    auto& history = g_actor_history[slot];
    if (history.address != actor || history.serial != serial || history.id == 0) {
        history = {actor, serial, allocate_actor_id()};
    }
    return history.id;
}
std::mutex g_terrain_scratch_allocation_mutex;
thread_local std::array<UiScope, kUiScopeCapacity> g_ui_scopes{};
thread_local size_t g_ui_scope_depth = 0;
thread_local std::array<MainMenuTextPulseOverride, kUiScopeCapacity>
    g_main_menu_text_pulse_overrides{};
thread_local size_t g_main_menu_text_pulse_depth = 0u;
thread_local size_t g_main_menu_text_pulse_overflow_depth = 0u;
thread_local std::array<bool, kUiScopeCapacity> g_sprite_rect_scopes{};
thread_local std::array<bool, kUiScopeCapacity>
    g_sprite_rect_discard_commands{};
thread_local std::array<uint32_t, kUiScopeCapacity>
    g_sprite_rect_saved_cursors{};
thread_local size_t g_sprite_rect_depth = 0;
thread_local size_t g_sprite_rect_overflow_depth = 0;
thread_local std::array<bool, kUiScopeCapacity> g_direct_text_scopes{};
thread_local size_t g_direct_text_depth = 0;
thread_local size_t g_direct_text_overflow_depth = 0;
thread_local PendingDirectTextCall g_pending_direct_text_call{};
thread_local bool g_fullscreen_panel_scope_open = false;
thread_local GuestDrawSuppression g_native_menu_panel_suppression{};
thread_local GuestDrawSuppression g_native_menu_prefix_suppression{};
thread_local std::array<uint64_t, kBriefingTextSampleCapacity>
    g_briefing_text_sample_signatures{};
thread_local size_t g_briefing_text_sample_count = 0;
thread_local bool g_script_panel_scope_open = false;
thread_local bool g_script_text_scope_open = false;
thread_local bool g_script_native_text_active = false;
thread_local bool g_briefing_panel_scope_open = false;
thread_local bool g_briefing_line_scope_open = false;
thread_local bool g_briefing_native_line_active = false;
struct BriefingQueueEntry {
    uint32_t slot = 0u;
    uint32_t pointer = 0u;
    std::string text;
    std::vector<uint32_t> source_offsets;
    uint32_t text_offset = 0u;
};
std::mutex g_briefing_queue_mutex;
std::vector<BriefingQueueEntry> g_briefing_queue;
bool g_briefing_queue_valid = true;
std::string g_briefing_native_text;
thread_local bool g_briefing_native_aggregate_active = false;
thread_local bool g_briefing_handoff_ready = false;
thread_local bool g_gameplay_right_hud_scope_open = false;
thread_local bool g_gameplay_status_bar_scope_open = false;
thread_local bool g_gameplay_mission_gauge_scope_open = false;
thread_local GuestDrawSuppression g_gameplay_status_bar_suppression{};
thread_local bool g_gameplay_hud_root_scope_open = false;
thread_local bool g_gameplay_radar_scope_open = false;
thread_local bool g_gameplay_weapon_ammo_scope_open = false;
thread_local bool g_gameplay_weapon_model_scope_open = false;
thread_local bool g_gameplay_key_scope_open = false;
thread_local GuestDrawSuppression g_gameplay_weapon_neighbor_suppression{};
thread_local bool g_gameplay_bonus_scope_open = false;
thread_local GuestDrawSuppression g_gameplay_bonus_suppression{};
thread_local GuestDrawSuppression g_gameplay_lives_suppression{};
thread_local GuestDrawSuppression g_gameplay_timer_suppression{};
thread_local bool g_gameplay_lives_hidden = false;
thread_local GuestDrawSuppression g_campaign_grid_suppression{};
thread_local bool g_campaign_grid_render_active = false;
thread_local bool g_campaign_grid_render_logged = false;
thread_local GuestDrawSuppression g_native_menu_list_suppression{};
thread_local std::array<GuestDrawSuppression, kUiScopeCapacity>
    g_gameplay_text_suppressions{};
thread_local size_t g_gameplay_text_suppression_depth = 0u;
thread_local size_t g_gameplay_text_suppression_overflow_depth = 0u;
thread_local std::array<GuestDrawSuppression, kUiScopeCapacity>
    g_gameplay_number_suppressions{};
thread_local size_t g_gameplay_number_suppression_depth = 0u;
thread_local size_t g_gameplay_number_suppression_overflow_depth = 0u;
std::atomic_uint64_t g_display_list_budget_rejections{0};
std::atomic<uint32_t> g_last_world_aperture_state{UINT32_MAX};

gpr guest_address(uint32_t address) {
    return static_cast<gpr>(static_cast<int32_t>(address));
}

uint32_t low_guest_address(gpr address) {
    return static_cast<uint32_t>(address);
}

int32_t read_s32(uint8_t* rdram, uint32_t address) {
    return static_cast<int32_t>(MEM_W(0, guest_address(address)));
}

void write_s32(uint8_t* rdram, uint32_t address, int32_t value) {
    MEM_W(0, guest_address(address)) = value;
}

uint32_t read_u32(uint8_t* rdram, uint32_t address) {
    return static_cast<uint32_t>(MEM_W(0, guest_address(address)));
}

float read_f32(uint8_t* rdram, uint32_t address) {
    return std::bit_cast<float>(read_u32(rdram, address));
}

void record_weapon_carousel_context(uint8_t* rdram, uint32_t player_index) {
    if (rdram == nullptr || player_index >= 4u ||
        !RT64::PerformanceProfiler::captureEnabled()) {
        return;
    }

    const uint32_t player_word_offset = player_index * sizeof(uint32_t);
    const uint32_t state_offset = player_index * 12u;
    const uint32_t selected = read_u32(
        rdram,
        kWeaponSelectionStateBase +
            player_index * kWeaponSelectionStateStride +
            kWeaponSelectionIndexOffset
    );
    const uint32_t request = read_u32(
        rdram,
        kWeaponCarouselRequestBase + player_word_offset
    );
    const uint32_t state0 = read_u32(
        rdram,
        kWeaponCarouselStateBase + state_offset
    );
    const uint32_t state1 = read_u32(
        rdram,
        kWeaponCarouselStateBase + state_offset + sizeof(uint32_t)
    );
    const uint32_t state2 = read_u32(
        rdram,
        kWeaponCarouselStateBase + state_offset + 2u * sizeof(uint32_t)
    );
    const uint32_t left = read_u32(
        rdram,
        kWeaponCarouselLeftBase + player_word_offset
    );
    const uint32_t middle = read_u32(
        rdram,
        kWeaponCarouselMiddleBase + player_word_offset
    );
    const uint32_t right = read_u32(
        rdram,
        kWeaponCarouselRightBase + player_word_offset
    );
    const uint32_t draw_mask = 4u |
        ((left != right) ? 1u : 0u) |
        ((left != middle) ? 2u : 0u);
    const uint64_t packed_state =
        uint64_t(player_index & 0xFFu) |
        (uint64_t(selected & 0xFFu) << 8u) |
        (uint64_t(state0 & 0xFFu) << 16u) |
        (uint64_t(state1 & 0xFFu) << 24u) |
        (uint64_t(state2 & 0xFFu) << 32u) |
        (uint64_t(left & 0xFFu) << 40u) |
        (uint64_t(middle & 0xFFu) << 48u) |
        (uint64_t(right & 0xFFu) << 56u);
    const uint64_t signature = packed_state ^
        (uint64_t(request) * UINT64_C(0x9E3779B185EBCA87)) ^
        (uint64_t(draw_mask) << 60u);
    if (g_last_weapon_carousel_signature.exchange(
            signature,
            std::memory_order_acq_rel
        ) == signature) {
        return;
    }

    static constexpr std::array<const char*, 8> kDrawNames{{
        "Host.Context.HudWeapon.Invalid0",
        "Host.Context.HudWeapon.Invalid1",
        "Host.Context.HudWeapon.Invalid2",
        "Host.Context.HudWeapon.Invalid3",
        "Host.Context.HudWeapon.One",
        "Host.Context.HudWeapon.TwoLeft",
        "Host.Context.HudWeapon.TwoRight",
        "Host.Context.HudWeapon.Three",
    }};
    RT64::PerformanceProfiler::recordInstant(
        RT64::PerformanceCategory::Counter,
        kDrawNames[draw_mask],
        0u,
        packed_state,
        (uint64_t(request) << 32u) | draw_mask
    );
}

void write_u32(uint8_t* rdram, uint32_t address, uint32_t value) {
    MEM_W(0, guest_address(address)) = value;
}

bool physical_edge_ui_enabled(uint8_t* rdram) {
    if (!bumble::widescreen::extended_ui_enabled() || rdram == nullptr) {
        return false;
    }

    const uint32_t world_active = read_u32(
        rdram,
        kFrontendState + kFrontendWorldActiveOffset
    );
    if (world_active == 0u) {
        return true;
    }
    return read_u32(
        rdram,
        kFrontendState + kFrontendPlayerLayoutOffset
    ) == kSinglePlayerLayout;
}

std::string campaign_grid_name(
    const bumble::campaign_levels::Record& record
) {
    std::string name(record.identifier);
    if (name.size() >= 2u) {
        name.insert(2u, "  ");
    }
    std::transform(
        name.begin(),
        name.end(),
        name.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::toupper(character));
        }
    );
    return name;
}

bool wide_single_player_world_enabled(uint8_t* rdram) {
    return bumble::widescreen::horizontal_expansion_scale() > 1.000001 &&
        rdram != nullptr && read_u32(rdram, kFrontendState + kFrontendWorldActiveOffset) != 0u &&
        read_u32(rdram, kFrontendState + kFrontendPlayerLayoutOffset) == kSinglePlayerLayout;
}

bool single_player_ui_enabled(uint8_t* rdram) {
    if (!bumble::widescreen::extended_ui_enabled() || rdram == nullptr) {
        return false;
    }

    return read_u32(
        rdram,
        kFrontendState + kFrontendWorldActiveOffset
    ) != 0u && read_u32(
        rdram,
        kFrontendState + kFrontendPlayerLayoutOffset
    ) == kSinglePlayerLayout;
}

bool frontend_phase_is_current_or_target(
    uint8_t* rdram,
    uint32_t phase
) {
    return read_u32(
            rdram,
            kFrontendObject + kFrontendCurrentPhaseOffset
        ) == phase ||
        read_u32(rdram, kFrontendObject + kFrontendPhaseOffset) == phase;
}

bool main_menu_frontend_active(uint8_t* rdram) {
    return bumble::widescreen::extended_ui_enabled() && rdram != nullptr &&
        (frontend_phase_is_current_or_target(rdram, kMainMenuPhase) ||
            frontend_phase_is_current_or_target(rdram, kOptionsMenuPhase) ||
            read_u32(rdram, kPauseMenuVisible) != 0u);
}

bool compact_frontend_menu_enabled(uint8_t* rdram) {
    if (!bumble::widescreen::extended_ui_enabled() || rdram == nullptr) {
        return false;
    }
    return frontend_phase_is_current_or_target(rdram, kMainMenuPhase) ||
        frontend_phase_is_current_or_target(rdram, kOptionsMenuPhase);
}

UiVerticalAnchor compact_frontend_menu_vertical_anchor(uint8_t* rdram) {
    if (!bumble::widescreen::extended_ui_enabled() || rdram == nullptr) {
        return UiVerticalAnchor::Automatic;
    }
    if (frontend_phase_is_current_or_target(rdram, kMainMenuPhase)) {
        return UiVerticalAnchor::Bottom;
    }
    return frontend_phase_is_current_or_target(rdram, kOptionsMenuPhase)
        ? UiVerticalAnchor::Middle
        : UiVerticalAnchor::Automatic;
}

void begin_main_menu_text_pulse_override(
    uint8_t* rdram,
    recomp_context* context
) {
    if (g_main_menu_text_pulse_overflow_depth != 0u ||
        g_main_menu_text_pulse_depth >=
            g_main_menu_text_pulse_overrides.size()) {
        ++g_main_menu_text_pulse_overflow_depth;
        return;
    }

    MainMenuTextPulseOverride& scope =
        g_main_menu_text_pulse_overrides[g_main_menu_text_pulse_depth++];
    scope = {};
    if (!main_menu_frontend_active(rdram) || context == nullptr ||
        low_guest_address(context->r6) == 0u) {
        return;
    }

    const uint32_t animation_owner = low_guest_address(context->r4);
    if (animation_owner < UINT32_C(0x80000000) ||
        animation_owner >
            UINT32_C(0x84000000) - kAnimatedTextScaleOffset - 4u) {
        return;
    }
    const uint32_t scale_address =
        animation_owner + kAnimatedTextScaleOffset;
    const uint32_t original_word = read_u32(rdram, scale_address);
    const float original_scale = std::bit_cast<float>(original_word);
    if (!std::isfinite(original_scale) || original_scale <= 0.0f ||
        original_scale > 4.0f) {
        return;
    }

    const float stable_scale =
        1.0f + (original_scale - 1.0f) * kMainMenuPulseAmplitudeScale;
    scope.scale_address = scale_address;
    scope.original_scale_word = original_word;
    scope.active = true;
    write_u32(rdram, scale_address, std::bit_cast<uint32_t>(stable_scale));

    bool expected = false;
    if (g_main_menu_pulse_reduction_logged.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel
        )) {
        std::fprintf(
            stderr,
            "BUMBLE_UI stage=main_menu_text_pulse amplitude_scale=0.000"
            " coherent_word_scale=1 glyph_jitter=0"
            " clamp=1.000"
            " transition_owner=main_options_or_pause\n"
        );
        std::fflush(stderr);
    }
}

bool current_main_menu_text_pulse_override_active() {
    return g_main_menu_text_pulse_overflow_depth == 0u &&
        g_main_menu_text_pulse_depth != 0u &&
        g_main_menu_text_pulse_overrides[
            g_main_menu_text_pulse_depth - 1u
        ].active;
}

void end_main_menu_text_pulse_override(uint8_t* rdram) {
    if (g_main_menu_text_pulse_overflow_depth != 0u) {
        --g_main_menu_text_pulse_overflow_depth;
        return;
    }
    if (g_main_menu_text_pulse_depth == 0u) {
        return;
    }
    const MainMenuTextPulseOverride scope =
        g_main_menu_text_pulse_overrides[--g_main_menu_text_pulse_depth];
    if (scope.active && rdram != nullptr) {
        write_u32(rdram, scope.scale_address, scope.original_scale_word);
    }
}

bool expanded_single_player_world_enabled(uint8_t* rdram) {
    if (rdram == nullptr ||
        !bumble::widescreen::visibility_expansion_enabled()) {
        return false;
    }

    return read_u32(
        rdram,
        kFrontendState + kFrontendWorldActiveOffset
    ) != 0u && read_u32(
        rdram,
        kFrontendState + kFrontendPlayerLayoutOffset
    ) == kSinglePlayerLayout;
}

void write_world_viewport(
    uint8_t* rdram,
    const std::array<uint32_t, 4>& words
) {
    for (uint32_t index = 0; index < words.size(); ++index) {
        write_u32(rdram, kWorldViewport + index * 4u, words[index]);
    }
}

void log_world_aperture_state_once(uint32_t state) {
    const uint32_t previous = g_last_world_aperture_state.exchange(
        state,
        std::memory_order_acq_rel
    );
    if (previous == state) {
        return;
    }
    std::fprintf(
        stderr,
        "BUMBLE_WIDESCREEN stage=world_aperture mode=%s "
        "viewport=0x%08" PRIX32 "\n",
        state == 1u ? "wide_single_player" :
            (state == 2u ? "authored_split_screen" :
                (state == 3u ? "authored_fail_closed" : "authored_4x3")),
        kWorldViewport
    );
    std::fflush(stderr);
}

uint32_t terrain_row_address(int32_t row) {
    return kTerrainRowBounds + static_cast<uint32_t>(row + 64) * 8u;
}

bool terrain_scratch_guards_intact(const TerrainScratchState& scratch) {
    if (scratch.allocation == nullptr) {
        return false;
    }
    const uint8_t* suffix = scratch.allocation +
        kTerrainScratchGuardBytes + kTerrainScratchBytes;
    for (uint32_t i = 0; i < kTerrainScratchGuardBytes; ++i) {
        if (scratch.allocation[i] != kTerrainScratchGuardValue ||
            suffix[i] != kTerrainScratchGuardValue) {
            return false;
        }
    }
    return true;
}

bool prepare_terrain_scratch(uint8_t* rdram) {
    TerrainScratchState& scratch = g_terrain_scratch;
    scratch.active = false;
    scratch.producer_relocated = false;
    scratch.consumer_relocated = false;
    scratch.expected_entries = 0;

    if (scratch.rdram != rdram || scratch.allocation == nullptr) {
        constexpr size_t allocation_bytes =
            kTerrainScratchGuardBytes + kTerrainScratchBytes +
            kTerrainScratchGuardBytes;
        std::lock_guard lock(g_terrain_scratch_allocation_mutex);
        auto* allocation = static_cast<uint8_t*>(
            recomp::alloc(rdram, allocation_bytes)
        );
        if (allocation == nullptr) {
            return false;
        }
        const ptrdiff_t offset = allocation - rdram;
        if (offset < 0 ||
            static_cast<size_t>(offset) + allocation_bytes >
                recomp::mem_size ||
            static_cast<uint64_t>(offset) + kTerrainScratchGuardBytes >
                UINT32_C(0x7FFFFFFF)) {
            recomp::free(rdram, allocation);
            return false;
        }
        scratch.rdram = rdram;
        scratch.allocation = allocation;
        scratch.guest_base = UINT32_C(0x80000000) +
            static_cast<uint32_t>(offset) + kTerrainScratchGuardBytes;
    }

    std::memset(
        scratch.allocation,
        kTerrainScratchGuardValue,
        kTerrainScratchGuardBytes
    );
    std::memset(
        scratch.allocation + kTerrainScratchGuardBytes + kTerrainScratchBytes,
        kTerrainScratchGuardValue,
        kTerrainScratchGuardBytes
    );
    scratch.active = true;
    return true;
}

bool extended_matrix_guards_intact(const FrameMatrixArena& arena) {
    if (arena.allocation == nullptr) {
        return false;
    }
    const uint8_t* suffix = arena.allocation +
        kExtendedMatrixGuardBytes + kExtendedFrameArenaBytes;
    const uint8_t* matrix_display_list_guard = arena.allocation +
        kExtendedMatrixGuardBytes + kExtendedMatrixBytes;
    for (uint32_t i = 0; i < kExtendedMatrixGuardBytes; ++i) {
        if (arena.allocation[i] != kExtendedMatrixGuardValue ||
            suffix[i] != kExtendedMatrixGuardValue) {
            return false;
        }
    }
    for (uint32_t i = 0;
         i < kExtendedDisplayListOffset - kExtendedMatrixBytes;
         ++i) {
        if (matrix_display_list_guard[i] != kExtendedMatrixGuardValue) {
            return false;
        }
    }
    return true;
}

FrameMatrixArena* prepare_frame_matrix_arena(
    uint8_t* rdram,
    uint32_t authored_frame_base
) {
    if (rdram == nullptr || authored_frame_base < 0x80000000u) {
        return nullptr;
    }

    std::lock_guard lock(g_frame_matrix_arena_mutex);
    for (FrameMatrixArena& arena : g_frame_matrix_arenas) {
        if (arena.rdram == rdram &&
            arena.authored_frame_base == authored_frame_base) {
            return extended_matrix_guards_intact(arena) ? &arena : nullptr;
        }
    }

    FrameMatrixArena* empty = nullptr;
    for (FrameMatrixArena& arena : g_frame_matrix_arenas) {
        if (arena.allocation == nullptr) {
            empty = &arena;
            break;
        }
    }
    if (empty == nullptr) {
        return nullptr;
    }

    constexpr size_t allocation_bytes =
        kExtendedMatrixGuardBytes + kExtendedFrameArenaBytes +
        kExtendedMatrixGuardBytes;
    auto* allocation = static_cast<uint8_t*>(
        recomp::alloc(rdram, allocation_bytes)
    );
    if (allocation == nullptr) {
        return nullptr;
    }
    const ptrdiff_t offset = allocation - rdram;
    if (offset < 0 ||
        static_cast<size_t>(offset) + allocation_bytes > recomp::mem_size ||
        static_cast<uint64_t>(offset) + kExtendedMatrixGuardBytes +
                kExtendedFrameArenaBytes >
            UINT32_C(0x1FFFFFFF)) {
        recomp::free(rdram, allocation);
        return nullptr;
    }

    std::memset(
        allocation,
        kExtendedMatrixGuardValue,
        kExtendedMatrixGuardBytes
    );
    std::memset(
        allocation + kExtendedMatrixGuardBytes + kExtendedMatrixBytes,
        kExtendedMatrixGuardValue,
        kExtendedDisplayListOffset - kExtendedMatrixBytes
    );
    std::memset(
        allocation + kExtendedMatrixGuardBytes + kExtendedFrameArenaBytes,
        kExtendedMatrixGuardValue,
        kExtendedMatrixGuardBytes
    );
    empty->rdram = rdram;
    empty->allocation = allocation;
    empty->authored_frame_base = authored_frame_base;
    empty->guest_base = UINT32_C(0x80000000) +
        static_cast<uint32_t>(offset) + kExtendedMatrixGuardBytes;
    return empty;
}

bool read_terrain_bounds(
    uint8_t* rdram,
    int32_t row,
    int32_t& lower,
    int32_t& upper
) {
    const uint32_t address = terrain_row_address(row);
    upper = std::min(read_s32(rdram, address), kMaximumCell);
    lower = std::max(read_s32(rdram, address + 4u), kMinimumCell);
    return lower <= upper;
}

uint8_t terrain_sector_mask(uint8_t* rdram, int32_t row, int32_t column) {
    const uint32_t row_index = static_cast<uint32_t>(row - kMinimumCell);
    const uint32_t column_index =
        static_cast<uint32_t>(column - kMinimumCell);
    const uint32_t address = kTerrainCellTable + row_index * 64u * 8u +
        column_index * 8u;
    return static_cast<uint8_t>(MEM_BU(7, guest_address(address)));
}

uint32_t visible_cells_in_span(
    uint8_t* rdram,
    int32_t row,
    int32_t lower,
    int32_t upper,
    uint8_t sector_mask
) {
    uint32_t count = 0;
    for (int32_t column = lower; column <= upper; ++column) {
        if ((terrain_sector_mask(rdram, row, column) & sector_mask) != 0u) {
            ++count;
        }
    }
    return count;
}

uint64_t pack_size(uint32_t width, uint32_t height) {
    return (static_cast<uint64_t>(width) << 32) | height;
}

bool guest_rdram_address(uint32_t address, uint32_t alignment) {
    return (address & (alignment - 1u)) == 0u &&
        (address & 0xE0000000u) == 0x80000000u &&
        (address & 0x1FFFFFFFu) < kRdramSize;
}

bool recomp_rdram_address(
    uint32_t address,
    uint32_t bytes,
    uint32_t alignment
) {
    if (alignment == 0u || (alignment & (alignment - 1u)) != 0u ||
        (address & (alignment - 1u)) != 0u ||
        (address & 0xE0000000u) != 0x80000000u) {
        return false;
    }
    const uint64_t offset = address & 0x1FFFFFFFu;
    const uint64_t limit = recomp::mem_size;
    return offset <= limit && bytes <= limit - offset;
}

bool display_list_address(uint32_t address) {
    return recomp_rdram_address(address, 8u, 8u);
}

bool display_list_budget_available(
    uint8_t* rdram,
    uint32_t cursor,
    uint32_t bytes
) {
    if (rdram == nullptr || !display_list_address(cursor)) {
        return false;
    }

    uint32_t arena_start = g_active_display_list_base.load(
        std::memory_order_acquire
    );
    uint32_t arena_end = g_active_display_list_end.load(
        std::memory_order_acquire
    );
    bool active_arena = arena_end > arena_start &&
        recomp_rdram_address(
            arena_start,
            arena_end - arena_start,
            8u
        ) && cursor >= arena_start && cursor <= arena_end;
    if (!active_arena) {
        const uint32_t frame_base = g_display_list_frame_base.load(
            std::memory_order_acquire
        );
        const uint32_t frame_offset = frame_base & 0x1FFFFFFFu;
        if (!guest_rdram_address(frame_base, 8u) ||
            frame_offset + kDisplayListArenaEndOffset > kRdramSize) {
            return true;
        }
        arena_start = frame_base + kDisplayListArenaOffset;
        arena_end = frame_base + kDisplayListArenaEndOffset;
        if (cursor < arena_start || cursor > arena_end) {
            return true;
        }
    }

    const uint32_t native_end = arena_end - kDisplayListFinalCommandReserve -
        (g_world_camera_scope_open ? 8u : 0u);
    const bool available = cursor >= arena_start && cursor <= native_end &&
        bytes <= native_end - cursor;
    if (available) {
        return true;
    }

    const uint64_t rejection = g_display_list_budget_rejections.fetch_add(
        1u,
        std::memory_order_acq_rel
    ) + 1u;
    if (rejection <= 8u || std::has_single_bit(rejection)) {
        std::fprintf(
            stderr,
            "BUMBLE_WIDESCREEN stage=display_list_budget_rejected"
            " rejection=%" PRIu64 " cursor=0x%08" PRIX32
            " bytes=0x%08" PRIX32 " arena_start=0x%08" PRIX32
            " native_end=0x%08" PRIX32 " arena_end=0x%08" PRIX32
            " final_reserve=0x%08" PRIX32 "\n",
            rejection,
            cursor,
            bytes,
            arena_start,
            native_end,
            arena_end,
            kDisplayListFinalCommandReserve
        );
        std::fflush(stderr);
    }
    return false;
}

bool append_commands(
    uint8_t* rdram,
    std::initializer_list<uint32_t> words
) {
    if (rdram == nullptr || words.size() == 0 || (words.size() & 1u) != 0u) {
        return false;
    }

    const uint32_t cursor = static_cast<uint32_t>(
        MEM_W(0, guest_address(kDisplayListCursor))
    );
    const uint32_t bytes = static_cast<uint32_t>(words.size() * 4u);
    if (!display_list_address(cursor) ||
        !recomp_rdram_address(cursor, bytes, 8u) ||
        !display_list_budget_available(rdram, cursor, bytes)) {
        return false;
    }

    uint32_t offset = 0;
    for (const uint32_t word : words) {
        MEM_W(offset, guest_address(cursor)) = word;
        offset += 4u;
    }
    MEM_W(0, guest_address(kDisplayListCursor)) = cursor + bytes;
    return true;
}

bool current_display_list_budget_available(uint8_t* rdram, uint32_t bytes) {
    if (rdram == nullptr) {
        return false;
    }
    const uint32_t cursor = static_cast<uint32_t>(
        MEM_W(0, guest_address(kDisplayListCursor))
    );
    return display_list_budget_available(rdram, cursor, bytes);
}

uint32_t extended_command(uint32_t command) {
    return (kExtendedOpcode << 24) | command;
}

void begin_static_terrain_matrix_scope(uint8_t* rdram) {
    g_static_terrain_matrix_scope_open = append_commands(rdram, {
        extended_command(kMatrixGroupV1), 0u,
        1u, 0u,
    });
}

void end_static_terrain_matrix_scope(uint8_t* rdram) {
    if (!g_static_terrain_matrix_scope_open) {
        return;
    }
    append_commands(rdram, {
        extended_command(kPopMatrixGroupV1), 1u,
    });
    g_static_terrain_matrix_scope_open = false;
}

void close_actor_matrix_scope(uint8_t* rdram) {
    if (!g_actor_matrix_scope_open) {
        return;
    }
    append_commands(rdram, {
        extended_command(kPopMatrixGroupV1), 1u,
    });
    g_actor_matrix_scope_open = false;
    g_actor_matrix_owner = 0;
}

uint32_t pack_s16_pair(int32_t high, int32_t low) {
    return (static_cast<uint32_t>(static_cast<uint16_t>(high)) << 16) |
        static_cast<uint16_t>(low);
}

int32_t origin_compensation(UiOrigin origin) {
    const uint32_t value = static_cast<uint32_t>(origin);
    if (value >= kOriginNone) {
        return 0;
    }

    return -static_cast<int32_t>(
        (value * kScreenWidth * 4u) / kOriginRight
    );
}

void append_rect_alignment(
    uint8_t* rdram,
    const UiAlignment& alignment,
    bool half_scale = false,
    UiVerticalAnchor vertical_anchor = UiVerticalAnchor::Automatic,
    uint32_t extra_origin_flags = 0u
) {
    const uint32_t scale_flag = half_scale ? kOriginHalfScale : 0u;
    const uint32_t vertical_flag = vertical_anchor == UiVerticalAnchor::Top
        ? kOriginAnchorTop
        : (vertical_anchor == UiVerticalAnchor::Middle
            ? (kOriginAnchorTop | kOriginAnchorBottom)
            : (vertical_anchor == UiVerticalAnchor::Bottom
                ? kOriginAnchorBottom
                : 0u));
    const uint32_t flags = scale_flag | vertical_flag | extra_origin_flags;
    const uint32_t left =
        static_cast<uint32_t>(alignment.left) | flags;
    const uint32_t right =
        static_cast<uint32_t>(alignment.right) | flags;
    const uint32_t viewport =
        static_cast<uint32_t>(alignment.viewport) | flags;
    const int32_t left_offset = origin_compensation(alignment.left);
    const int32_t right_offset = origin_compensation(alignment.right);
    const int32_t viewport_offset = origin_compensation(alignment.viewport);
    append_commands(rdram, {
        extended_command(kSetRectAlignV1), left | (right << 12),
        pack_s16_pair(left_offset, 0), pack_s16_pair(right_offset, 0),
        extended_command(kSetViewportAlignV1), viewport,
        pack_s16_pair(viewport_offset, 0), 0u,
    });
}

void append_rect_aspect(uint8_t* rdram, uint32_t aspect) {
    append_commands(rdram, {
        extended_command(kSetRectAspectV1), aspect,
    });
}

void append_full_width_scissor(uint8_t* rdram) {
    append_commands(rdram, {
        extended_command(kSetScissorAlignV1),
        kOriginLeft | (kOriginRight << 12),
        0u, pack_s16_pair(-static_cast<int32_t>(kScreenWidth * 4u), 0),
        0u, (kScreenWidth * 4u << 16) | (kScreenHeight * 4u),
    });
}

void append_current_full_width_scissor(uint8_t* rdram) {
    append_commands(rdram, {
        extended_command(kSetScissorV1),
        kOriginLeft << 2 | kOriginRight << 14,
        0u, kScreenHeight * 4u,
    });
}

void append_push_2d_state(uint8_t* rdram) {
    append_commands(rdram, {
        extended_command(kPushViewportV1), 0u,
        extended_command(kPushScissorV1), 0u,
    });
}

void append_pop_2d_state(uint8_t* rdram) {
    append_commands(rdram, {
        extended_command(kPopViewportV1), 0u,
        extended_command(kPopScissorV1), 0u,
    });
}

UiAlignment single_origin_alignment(UiOrigin origin) {
    return {origin, origin, origin};
}

UiAlignment classify_pixel_x(int32_t x) {
    if (x < kCenterSafeLeft) {
        return single_origin_alignment(UiOrigin::Left);
    }
    if (x > kCenterSafeRight) {
        return single_origin_alignment(UiOrigin::Right);
    }
    return single_origin_alignment(UiOrigin::None);
}

UiAlignment classify_pixel_rect(int32_t x, int32_t width) {
    if (width > 0 &&
        x <= kFullWidthEdgeTolerance &&
        x + width >= static_cast<int32_t>(kScreenWidth) -
            kFullWidthEdgeTolerance) {
        return {UiOrigin::Left, UiOrigin::Right, UiOrigin::None};
    }
    return classify_pixel_x(x);
}

UiAlignment classify_text_justification(uint32_t flags, int32_t pixel_x) {
    if ((flags & 0x2u) != 0u) {
        return single_origin_alignment(UiOrigin::None);
    }
    if ((flags & 0x4u) != 0u) {
        return single_origin_alignment(UiOrigin::Right);
    }
    return classify_pixel_x(pixel_x);
}

UiAlignment classify_text_descriptor(
    uint8_t* rdram,
    uint32_t descriptor,
    int32_t& pixel_x,
    uint32_t& flags
) {
    pixel_x = static_cast<int32_t>(kScreenWidth / 2u);
    flags = 0u;
    if (!guest_rdram_address(descriptor, 2u) ||
        (descriptor & 0x1FFFFFFFu) + 12u > kRdramSize) {
        return single_origin_alignment(UiOrigin::None);
    }

    const uint32_t normalized_x = static_cast<uint16_t>(
        MEM_HU(0x4, guest_address(descriptor))
    );
    pixel_x = static_cast<int32_t>(
        (normalized_x * kScreenWidth + 0x7FFFu) / 0xFFFFu
    );
    flags = static_cast<uint32_t>(MEM_W(0x8, guest_address(descriptor)));

    return classify_text_justification(flags, pixel_x);
}

const DirectTextCallerSpec* find_direct_text_caller(
    uint32_t call_pc,
    size_t& caller_index
) {
    for (size_t i = 0; i < kDirectTextCallers.size(); ++i) {
        if (kDirectTextCallers[i].call_pc == call_pc) {
            caller_index = i;
            return &kDirectTextCallers[i];
        }
    }
    caller_index = kDirectTextCallers.size();
    return nullptr;
}

bool read_guest_word(uint8_t* rdram, uint32_t address, uint32_t& value) {
    if (rdram == nullptr || !guest_rdram_address(address, 4u) ||
        (address & 0x1FFFFFFFu) + 4u > kRdramSize) {
        return false;
    }
    value = static_cast<uint32_t>(MEM_W(0, guest_address(address)));
    return true;
}

bool add_guest_offset(uint32_t base, uint32_t offset, uint32_t& address) {
    if (base > UINT32_MAX - offset) {
        return false;
    }
    address = base + offset;
    return true;
}

bool copy_guest_text_preview(
    uint8_t* rdram,
    uint32_t address,
    char* output,
    size_t output_capacity,
    size_t& output_length
) {
    output_length = 0u;
    if (output != nullptr && output_capacity != 0u) {
        output[0] = '\0';
    }
    if (rdram == nullptr || output == nullptr || output_capacity < 2u ||
        !guest_rdram_address(address, 1u)) {
        return false;
    }
    const uint32_t rdram_offset = address & 0x1FFFFFFFu;
    const uint32_t available = kRdramSize - rdram_offset;
    const uint32_t scan_size = std::min(
        std::min(
            kNativeTextMaximumBytes,
            static_cast<uint32_t>(output_capacity - 1u)
        ),
        available
    );
    for (uint32_t index = 0; index < scan_size; ++index) {
        const uint8_t value = static_cast<uint8_t>(MEM_BU(
            static_cast<int32_t>(index),
            guest_address(address)
        ));
        if (value == 0u) {
            break;
        }
        output[output_length++] = value >= 0x20u && value <= 0x7Eu &&
                value != static_cast<uint8_t>('"') &&
                value != static_cast<uint8_t>('\\')
            ? static_cast<char>(value)
            : '.';
    }
    output[output_length] = '\0';
    return output_length != 0u;
}

uint32_t localization_index_for_entry(
    uint8_t* rdram,
    uint32_t localization_entry
) {
    uint32_t pointer_table = 0u;
    if (!read_guest_word(rdram, kLocalizedTextPointerTable, pointer_table) ||
        localization_entry < pointer_table ||
        ((localization_entry - pointer_table) & 0x3u) != 0u) {
        return UINT32_MAX;
    }
    return (localization_entry - pointer_table) / sizeof(uint32_t);
}

uint64_t briefing_text_sample_signature(
    uint32_t owner_tag,
    uint32_t node,
    uint32_t text_pointer,
    const char* preview,
    size_t preview_length
) {
    uint64_t value = UINT64_C(1469598103934665603);
    const auto mix = [&value](uint8_t byte) {
        value ^= byte;
        value *= UINT64_C(1099511628211);
    };
    for (const uint32_t word : {owner_tag, node, text_pointer}) {
        for (uint32_t shift = 0u; shift < 32u; shift += 8u) {
            mix(static_cast<uint8_t>(word >> shift));
        }
    }
    for (size_t index = 0; index < preview_length; ++index) {
        mix(static_cast<uint8_t>(preview[index]));
    }
    return value;
}

void publish_visibility_camera_xz(
    float eye_x,
    float eye_z,
    float cull_x,
    float cull_z,
    bool valid
) {
    // Readers accept a packet only between matching even sequence values.
    g_visibility_camera_generation.fetch_add(1u, std::memory_order_acq_rel);
    g_visibility_camera_eye_x.store(eye_x, std::memory_order_relaxed);
    g_visibility_camera_eye_z.store(eye_z, std::memory_order_relaxed);
    g_visibility_camera_cull_x.store(cull_x, std::memory_order_relaxed);
    g_visibility_camera_cull_z.store(cull_z, std::memory_order_relaxed);
    g_visibility_camera_forward_x.store(
        cull_x - eye_x,
        std::memory_order_relaxed
    );
    g_visibility_camera_forward_z.store(
        cull_z - eye_z,
        std::memory_order_relaxed
    );
    g_visibility_camera_valid.store(valid, std::memory_order_relaxed);
    g_visibility_camera_generation.fetch_add(1u, std::memory_order_release);
}

void publish_terrain_visibility_sample(
    uint32_t original_cells,
    uint32_t candidate_cells,
    uint32_t visible_cells,
    int32_t first_row,
    int32_t last_row,
    bool valid
) {
    g_terrain_sample_generation.fetch_add(1u, std::memory_order_acq_rel);
    g_terrain_sample_original_cells.store(
        original_cells,
        std::memory_order_relaxed
    );
    g_terrain_sample_candidate_cells.store(
        candidate_cells,
        std::memory_order_relaxed
    );
    g_terrain_sample_visible_cells.store(
        visible_cells,
        std::memory_order_relaxed
    );
    g_terrain_sample_first_row.store(first_row, std::memory_order_relaxed);
    g_terrain_sample_last_row.store(last_row, std::memory_order_relaxed);
    g_terrain_sample_valid.store(valid, std::memory_order_relaxed);
    g_terrain_sample_generation.fetch_add(1u, std::memory_order_release);
}

bool remember_briefing_text_sample(uint64_t signature) {
    for (size_t index = 0; index < g_briefing_text_sample_count; ++index) {
        if (g_briefing_text_sample_signatures[index] == signature) {
            return false;
        }
    }
    if (g_briefing_text_sample_count >=
        g_briefing_text_sample_signatures.size()) {
        return false;
    }
    g_briefing_text_sample_signatures[g_briefing_text_sample_count++] =
        signature;
    return true;
}

bool read_direct_text_flags(
    uint8_t* rdram,
    const recomp_context* context,
    uint32_t offset,
    uint32_t& flags
) {
    uint32_t address = 0;
    return add_guest_offset(low_guest_address(context->r29), offset, address) &&
        read_guest_word(rdram, address, flags);
}

const char* direct_text_metadata_kind_name(DirectTextMetadataKind kind) {
    switch (kind) {
    case DirectTextMetadataKind::CallerStackWord: return "caller_stack_word";
    case DirectTextMetadataKind::EquivalentStackWords:
        return "equivalent_stack_words";
    case DirectTextMetadataKind::AuthoredCenterSafe:
        return "authored_center_safe";
    case DirectTextMetadataKind::InheritedOuterDescriptor:
        return "inherited_outer_descriptor";
    }
    return "unknown";
}

const char* origin_name(UiOrigin origin) {
    switch (origin) {
    case UiOrigin::Left: return "left";
    case UiOrigin::Right: return "right";
    case UiOrigin::None: return "center_safe";
    }
    return "unknown";
}

const char* alignment_name(const UiAlignment& alignment) {
    if (alignment.left == UiOrigin::Left &&
        alignment.right == UiOrigin::Right) {
        return "full_width";
    }
    if (alignment.left == alignment.right) {
        return origin_name(alignment.left);
    }
    return "mixed";
}

uint32_t alignment_index(const UiAlignment& alignment) {
    if (alignment.left == UiOrigin::Left &&
        alignment.right == UiOrigin::Right) {
        return 3u;
    }
    if (alignment.left == UiOrigin::Left) {
        return 0u;
    }
    if (alignment.left == UiOrigin::Right) {
        return 1u;
    }
    return 2u;
}

void log_ui_origin_once(
    const char* owner,
    const UiAlignment& alignment,
    int32_t x
) {
    const uint32_t owner_group = std::strcmp(owner, "func_800A6824") == 0 ? 1u :
        (std::strcmp(owner, "func_800A6A08") == 0 ? 2u :
            (std::strcmp(owner, "func_800AE9AC") == 0 ? 3u :
                (std::strcmp(owner, "func_800B9748") == 0 ? 4u :
                    (std::strcmp(owner, "func_800B8180") == 0 ? 5u :
                        (std::strcmp(owner, kScriptPanelScopeOwner) == 0 ? 6u :
                            (std::strcmp(owner, kBriefingPanelScopeOwner) == 0
                                ? 7u : 0u))))));
    const uint32_t bit = 1u << (owner_group * 4u + alignment_index(alignment));
    const uint32_t previous = g_logged_ui_origins.fetch_or(
        bit,
        std::memory_order_acq_rel
    );
    if ((previous & bit) != 0u) {
        return;
    }
    std::fprintf(
        stderr,
        "BUMBLE_WIDESCREEN stage=ui_anchor owner=%s origin=%s x=%" PRId32 "\n",
        owner,
        alignment_name(alignment),
        x
    );
    std::fflush(stderr);
}

void log_direct_text_caller_once(
    size_t caller_index,
    const DirectTextCallerSpec* caller,
    uint32_t call_pc,
    bool flags_valid,
    uint32_t flags,
    int32_t pixel_x,
    const UiAlignment& alignment,
    bool inherited_scope
) {
    const uint64_t bit = caller_index < kDirectTextCallers.size()
        ? UINT64_C(1) << caller_index
        : UINT64_C(1) << 63u;
    const uint64_t previous = g_logged_direct_text_callers.fetch_or(
        bit,
        std::memory_order_acq_rel
    );
    if ((previous & bit) != 0u) {
        return;
    }

    std::fprintf(
        stderr,
        "BUMBLE_WIDESCREEN stage=direct_text_caller owner=%s "
        "call_pc=0x%08" PRIX32 " source=%s flags_valid=%u "
        "flags=0x%08" PRIX32 " x=%" PRId32 " origin=%s inherited=%u\n",
        caller == nullptr ? "unknown" : caller->owner,
        call_pc,
        caller == nullptr
            ? "unknown"
            : direct_text_metadata_kind_name(caller->metadata_kind),
        flags_valid ? 1u : 0u,
        flags,
        pixel_x,
        alignment_name(alignment),
        inherited_scope ? 1u : 0u
    );
    std::fflush(stderr);
}

void log_briefing_queue_text(
    uint8_t* rdram,
    uint32_t text_pointer,
    uint32_t localization_entry
) {
    if (rdram == nullptr) {
        return;
    }
    const uint32_t live_phase = read_u32(
        rdram,
        kFrontendObject + kFrontendPhaseOffset
    );

    const uint32_t localization_index = localization_index_for_entry(
        rdram,
        localization_entry
    );

    std::array<char, kNativeTextMaximumBytes + 1u> preview{};
    size_t preview_length = 0u;
    if (!copy_guest_text_preview(
            rdram,
            text_pointer,
            preview.data(),
            preview.size(),
            preview_length
        )) {
        return;
    }
    const bool objective_prefix = std::strncmp(
        preview.data(),
        kMissionObjectivePrefix,
        sizeof(kMissionObjectivePrefix) - 1u
    ) == 0;
    const bool objective_continuation = std::strstr(
        preview.data(),
        kMissionObjectiveContinuation
    ) != nullptr;
    const char* content = objective_prefix || objective_continuation
        ? "mission_objective"
        : "unclassified";

    const uint64_t signature = briefing_text_sample_signature(
        kBriefingQueueCallPc,
        localization_index,
        text_pointer,
        preview.data(),
        preview_length
    );
    if (remember_briefing_text_sample(signature)) {
        std::fprintf(
            stderr,
            "BUMBLE_WIDESCREEN stage=briefing_queue_text"
            " owner=func_80095558 call_pc=0x%08" PRIX32
            " queue_owner=func_800A9214 live_phase=0x%08" PRIX32
            " localization_index=0x%08" PRIX32
            " localization_entry=0x%08" PRIX32
            " text_pointer=0x%08" PRIX32
            " content=%s preview=\"%s\"\n",
            kBriefingQueueCallPc,
            live_phase,
            localization_index,
            localization_entry,
            text_pointer,
            content,
            preview.data()
        );
        std::fflush(stderr);
    }

    if (localization_index != kMissionObjectiveLocalizationIndex ||
        !objective_prefix) {
        return;
    }
    bool expected = false;
    if (!g_mission_briefing_queue_owner_logged.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        )) {
        return;
    }
    std::fprintf(
        stderr,
        "BUMBLE_WIDESCREEN stage=mission_briefing_text_owner"
        " owner=func_80095558 call_pc=0x%08" PRIX32
        " queue_owner=func_800A9214 live_phase=0x%08" PRIX32
        " localization_index=0x%08" PRIX32
        " text_pointer=0x%08" PRIX32
        " content=mission_objective\n",
        kBriefingQueueCallPc,
        live_phase,
        localization_index,
        text_pointer
    );
    std::fflush(stderr);
}

void log_briefing_render_pass(
    uint8_t* rdram,
    uint32_t call_pc,
    uint32_t text_pointer,
    uint32_t submitted_x,
    uint32_t submitted_y,
    uint32_t authored_x,
    uint32_t authored_y,
    const UiAlignment& alignment
) {
    if (rdram == nullptr ||
        (call_pc != kBriefingShadowCallPc &&
            call_pc != kBriefingWhiteCallPc)) {
        return;
    }
    const uint32_t live_phase = read_u32(
        rdram,
        kFrontendObject + kFrontendPhaseOffset
    );
    if (live_phase != kMissionBriefingPhase &&
        live_phase != kMissionGameplayPhase) {
        return;
    }

    std::array<char, kNativeTextMaximumBytes + 1u> preview{};
    size_t preview_length = 0u;
    if (!copy_guest_text_preview(
            rdram,
            text_pointer,
            preview.data(),
            preview.size(),
            preview_length
        )) {
        return;
    }
    const bool mission_objective =
        std::strstr(preview.data(), kMissionObjectivePrefix) != nullptr ||
        std::strstr(preview.data(), kMissionObjectiveContinuation) != nullptr;
    const char* pass = call_pc == kBriefingShadowCallPc ? "shadow" : "white";
    const uint64_t signature = briefing_text_sample_signature(
        call_pc,
        (submitted_x << 16u) | (submitted_y & 0xFFFFu),
        text_pointer,
        preview.data(),
        preview_length
    );
    if (remember_briefing_text_sample(signature)) {
        std::fprintf(
            stderr,
            "BUMBLE_WIDESCREEN stage=briefing_render_pass"
            " owner=func_800A9580 call_pc=0x%08" PRIX32
            " renderer=func_800AA2B0 pass=%s"
            " live_phase=0x%08" PRIX32
            " text_pointer=0x%08" PRIX32
            " submitted_x=%" PRIu32 " submitted_y=%" PRIu32
            " authored_x=%" PRIu32 " authored_y=%" PRIu32
            " origin=%s"
            " content=%s preview=\"%s\"\n",
            call_pc,
            pass,
            live_phase,
            text_pointer,
            submitted_x,
            submitted_y,
            authored_x,
            authored_y,
            alignment_name(alignment),
            mission_objective ? "mission_objective" : "unclassified",
            preview.data()
        );
        std::fflush(stderr);
    }

    if (live_phase != kMissionBriefingPhase ||
        call_pc != kBriefingWhiteCallPc ||
        !mission_objective) {
        return;
    }
    bool expected = false;
    if (!g_mission_briefing_render_owner_logged.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        )) {
        return;
    }
    std::fprintf(
        stderr,
        "BUMBLE_WIDESCREEN stage=mission_briefing_render_owner"
        " owner=func_800A9580 call_pc=0x%08" PRIX32
        " renderer=func_800AA2B0 glyph_owner=func_800A6824"
        " live_phase=0x%08" PRIX32
        " text_pointer=0x%08" PRIX32
        " authored_x=%" PRIu32 " authored_y=%" PRIu32 " origin=%s"
        " content=mission_objective\n",
        call_pc,
        live_phase,
        text_pointer,
        authored_x,
        authored_y,
        alignment_name(alignment)
    );
    std::fflush(stderr);
}

bool ui_scope_can_begin(uint8_t* rdram) {
    return g_ui_scope_depth < g_ui_scopes.size() &&
        current_display_list_budget_available(
            rdram,
            kUiScopeBeginCommandBytes
        );
}

bool begin_ui_scope(
    uint8_t* rdram,
    const char* owner,
    const UiAlignment& alignment,
    int32_t x,
    bool half_scale = false,
    UiVerticalAnchor vertical_anchor = UiVerticalAnchor::Automatic,
    uint32_t extra_origin_flags = 0u
) {
    if (!ui_scope_can_begin(rdram)) {
        return false;
    }

    g_ui_scopes[g_ui_scope_depth++] = {
        alignment,
        owner,
        half_scale,
        vertical_anchor,
        extra_origin_flags,
    };
    append_push_2d_state(rdram);
    append_full_width_scissor(rdram);
    append_current_full_width_scissor(rdram);
    append_rect_aspect(rdram, kAspectAdjust);
    append_rect_alignment(
        rdram,
        alignment,
        half_scale,
        vertical_anchor,
        extra_origin_flags
    );
    log_ui_origin_once(owner, alignment, x);
    return true;
}

bool begin_pixel_anchored_rect(
    uint8_t* rdram,
    const char* owner,
    int32_t x,
    int32_t width = 0,
    bool half_scale = false
) {
    const UiAlignment alignment = width > 0
        ? classify_pixel_rect(x, width)
        : classify_pixel_x(x);
    return begin_ui_scope(rdram, owner, alignment, x, half_scale);
}

void reset_scissor_alignment(uint8_t* rdram) {
    append_commands(rdram, {
        extended_command(kSetScissorAlignV1),
        kOriginNone | (kOriginNone << 12),
        0u, 0u,
        0u, (kScreenWidth * 4u << 16) | (kScreenHeight * 4u),
    });
}

void end_ui_scope(uint8_t* rdram, const char* owner) {
    if (g_ui_scope_depth == 0u) {
        return;
    }

    const UiScope& current = g_ui_scopes[g_ui_scope_depth - 1u];
    if (current.owner == nullptr || std::strcmp(current.owner, owner) != 0) {
        std::fprintf(
            stderr,
            "BUMBLE_WIDESCREEN stage=ui_scope_mismatch expected=%s actual=%s\n",
            current.owner == nullptr ? "none" : current.owner,
            owner
        );
        std::fflush(stderr);
        return;
    }

    const uint32_t command_bytes = g_ui_scope_depth == 1u
        ? kUiRootScopeEndCommandBytes
        : kUiNestedScopeEndCommandBytes;
    if (!current_display_list_budget_available(rdram, command_bytes)) {
        --g_ui_scope_depth;
        return;
    }

    --g_ui_scope_depth;
    append_pop_2d_state(rdram);
    if (g_ui_scope_depth != 0u) {
        append_full_width_scissor(rdram);
        append_rect_alignment(
            rdram,
            g_ui_scopes[g_ui_scope_depth - 1u].alignment,
            g_ui_scopes[g_ui_scope_depth - 1u].half_scale,
            g_ui_scopes[g_ui_scope_depth - 1u].vertical_anchor,
            g_ui_scopes[g_ui_scope_depth - 1u].extra_origin_flags
        );
        return;
    }

    append_rect_alignment(rdram, single_origin_alignment(UiOrigin::None));
    append_rect_aspect(rdram, kAspectAuto);
    reset_scissor_alignment(rdram);
}

bumble::text_overlay::HorizontalAnchor gameplay_horizontal_anchor(
    int32_t x
) {
    if (g_ui_scope_depth != 0u) {
        const char* owner = g_ui_scopes[g_ui_scope_depth - 1u].owner;
        if (owner != nullptr &&
            std::strcmp(owner, kGameplayWeaponAmmoScopeOwner) == 0) {
            return bumble::text_overlay::HorizontalAnchor::RightInset;
        }
        if (owner != nullptr &&
            (std::strcmp(owner, kGameplayRightHudScopeOwner) == 0 ||
                std::strcmp(owner, kGameplayStatusBarScopeOwner) == 0)) {
            return bumble::text_overlay::HorizontalAnchor::Left;
        }
    }
    if (x < kCenterSafeLeft) {
        return bumble::text_overlay::HorizontalAnchor::Left;
    }
    if (x > kCenterSafeRight) {
        return bumble::text_overlay::HorizontalAnchor::Right;
    }
    return bumble::text_overlay::HorizontalAnchor::Center;
}

bumble::text_overlay::VerticalAnchor gameplay_vertical_anchor(int32_t y) {
    if (g_ui_scope_depth != 0u) {
        const char* owner = g_ui_scopes[g_ui_scope_depth - 1u].owner;
        if (owner != nullptr &&
            std::strcmp(owner, kGameplayWeaponAmmoScopeOwner) == 0) {
            return bumble::text_overlay::VerticalAnchor::Bottom;
        }
    }
    if (y < static_cast<int32_t>(kScreenHeight / 3u)) {
        return bumble::text_overlay::VerticalAnchor::Top;
    }
    if (y > static_cast<int32_t>((kScreenHeight * 2u) / 3u)) {
        return bumble::text_overlay::VerticalAnchor::Bottom;
    }
    return bumble::text_overlay::VerticalAnchor::Center;
}

uint32_t pack_text_rgba(
    uint32_t red,
    uint32_t green,
    uint32_t blue
) {
    return ((red & 0xFFu) << 24u) |
        ((green & 0xFFu) << 16u) |
        ((blue & 0xFFu) << 8u) | 0xFFu;
}

bool publish_gameplay_weapon_list(uint8_t* rdram, uint32_t player_index) {
    if (rdram == nullptr || player_index >= 4u ||
        !bumble::text_overlay::renderer_ready()) {
        return false;
    }

    const uint32_t weapon_count = bumble::weapon_system::weapon_count();
    if (weapon_count == 0u || weapon_count > 32u) {
        return false;
    }
    const uint32_t weapon_state = kWeaponSelectionStateBase +
        player_index * kWeaponSelectionStateStride;
    const uint32_t selected = read_u32(
        rdram,
        weapon_state + kWeaponSelectionIndexOffset
    );
    if (selected >= weapon_count) {
        return false;
    }

    std::array<uint32_t, 32> owned{};
    size_t owned_count = 0u;
    size_t selected_position = 0u;
    for (uint32_t weapon = 0u; weapon < weapon_count; ++weapon) {
        const bool available = weapon == selected || read_u32(
            rdram,
            weapon_state + kWeaponAmmoOffset + weapon * sizeof(uint32_t)
        ) != 0u;
        if (!available) {
            continue;
        }
        if (weapon == selected) {
            selected_position = owned_count;
        }
        owned[owned_count++] = weapon;
    }
    if (owned_count == 0u) {
        return false;
    }

    const size_t visible_rows = std::min(owned_count, kWeaponListVisibleRows);
    const size_t selected_row = visible_rows / 2u;
    bool published = true;
    for (size_t row = 0u; row < visible_rows; ++row) {
        const ptrdiff_t relative = static_cast<ptrdiff_t>(row) -
            static_cast<ptrdiff_t>(selected_row);
        const ptrdiff_t wrapped = (
            static_cast<ptrdiff_t>(selected_position) + relative +
            static_cast<ptrdiff_t>(owned_count)
        ) % static_cast<ptrdiff_t>(owned_count);
        const uint32_t weapon = owned[static_cast<size_t>(wrapped)];
        std::string label = weapon == selected ? "> " : "  ";
        label += bumble::weapon_system::hud_label(weapon);
        const int32_t row_y = static_cast<int32_t>(kWeaponListSelectedY) +
            static_cast<int32_t>(relative) * kWeaponListRowStep;
        published = bumble::text_overlay::observe(
            bumble::text_overlay::TextKind::GameplayHud,
            kWeaponListRightInset,
            static_cast<uint32_t>(row_y),
            label.c_str(),
            bumble::text_overlay::HorizontalAnchor::RightTextInset,
            bumble::text_overlay::VerticalAnchor::Bottom,
            kWeaponListScale,
            pack_text_rgba(255u, 244u, 216u),
            pack_text_rgba(0u, 0u, 0u),
            kWeaponListStableSlot + row + 1u
        ) && published;
    }
    return published;
}

bool publish_native_menu_replacement(
    uint8_t* rdram,
    uint32_t descriptor
) {
    return (descriptor == kMainMenuDescriptor ||
            descriptor == kOptionsMenuDescriptor ||
            descriptor == kPauseMenuDescriptor ||
            descriptor == kMissionCompleteDescriptor) &&
        bumble::graphics_options::native_menu_overlay_active(
            rdram,
            descriptor
        ) &&
        bumble::graphics_options::publish_native_menu_overlay(
            rdram,
            descriptor
        );
}

template <size_t Capacity>
GuestDrawSuppression* push_guest_draw_suppression(
    std::array<GuestDrawSuppression, Capacity>& suppressions,
    size_t& depth,
    size_t& overflow_depth
) {
    if (overflow_depth != 0u || depth >= suppressions.size()) {
        ++overflow_depth;
        return nullptr;
    }
    GuestDrawSuppression& suppression = suppressions[depth++];
    suppression = {};
    return &suppression;
}

template <size_t Capacity>
void pop_guest_draw_suppression(
    uint8_t* rdram,
    std::array<GuestDrawSuppression, Capacity>& suppressions,
    size_t& depth,
    size_t& overflow_depth
) {
    if (overflow_depth != 0u) {
        --overflow_depth;
        return;
    }
    if (depth == 0u) {
        return;
    }
    GuestDrawSuppression suppression = suppressions[--depth];
    suppressions[depth] = {};
    if (suppression.active && rdram != nullptr) {
        MEM_W(0, guest_address(kDisplayListCursor)) = suppression.saved_cursor;
    }
}

} // namespace

void bumble::widescreen::invalidate_world_camera_history() {
    g_script_camera_record.store(0, std::memory_order_relaxed);
    g_world_camera_generation.fetch_add(1, std::memory_order_relaxed);
}

void bumble::widescreen::invalidate_scene_history() {
    invalidate_world_camera_history();
    bumble::electric_effect::reset_scene();
    std::lock_guard lock(g_actor_history_mutex);
    g_actor_history = {};
}

extern "C" void bumble_invalidate_world_camera_history(uint8_t*, recomp_context*) {
    bumble::widescreen::invalidate_world_camera_history();
}

extern "C" void bumble_publish_script_camera_history(uint8_t*, recomp_context* context) {
    const uint32_t record = static_cast<uint32_t>(context->r16);
    if (g_script_camera_record.exchange(record, std::memory_order_relaxed) != record) {
        g_world_camera_generation.fetch_add(1, std::memory_order_relaxed);
    }
}

extern "C" void bumble_end_world_camera_scope(uint8_t* rdram, recomp_context*) noexcept(false) {
    if (!g_world_camera_scope_open) return;
    g_world_camera_scope_open = false;
    if (!append_commands(rdram, {extended_command(kPopMatrixGroupV1), 0x101u})) {
        throw std::runtime_error("World camera matrix scope overflow");
    }
}

void bumble::widescreen::publish_render_size(
    uint32_t width,
    uint32_t height,
    bool enabled
) {
    const uint64_t packed = pack_size(width, height);
    g_render_size.store(packed, std::memory_order_release);
    g_enabled.store(enabled, std::memory_order_release);
    const uint64_t signature =
        packed ^ (enabled ? UINT64_C(0x8000000000000000) : 0u);
    uint64_t previous =
        g_last_publish_signature.load(std::memory_order_acquire);
    if (previous != signature &&
        g_last_publish_signature.compare_exchange_strong(
            previous,
            signature,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        )) {
        const double aspect = height == 0
            ? 0.0
            : static_cast<double>(width) / height;
        const double scale = enabled
            ? std::max(aspect / kOriginalAspect, 1.0)
            : 1.0;
        std::fprintf(
            stderr,
            "BUMBLE_WIDESCREEN stage=aspect_configured enabled=%d"
            " window_width=%" PRIu32 " window_height=%" PRIu32
            " target_aspect=%.6f source_aspect=1.333333 scale=%.6f\n",
            enabled ? 1 : 0,
            width,
            height,
            aspect,
            scale
        );
        std::fflush(stderr);
    }
}

double bumble::widescreen::horizontal_expansion_scale() {
    if (!g_enabled.load(std::memory_order_acquire)) {
        return 1.0;
    }

    const uint64_t packed = g_render_size.load(std::memory_order_acquire);
    const uint32_t width = static_cast<uint32_t>(packed >> 32);
    const uint32_t height = static_cast<uint32_t>(packed);
    if (width == 0 || height == 0) {
        return 1.0;
    }

    const double target_aspect = static_cast<double>(width) / height;
    return std::max(target_aspect / kOriginalAspect, 1.0);
}

void bumble::widescreen::publish_fog_scale(float scale) {
    g_fog_scale.store(
        std::clamp(scale, 0.0f, 1.0f),
        std::memory_order_release
    );
}

float bumble::widescreen::fog_scale() {
    return g_fog_scale.load(std::memory_order_acquire);
}

bumble::widescreen::VisibilityCameraXZ
bumble::widescreen::visibility_camera_xz() {
    VisibilityCameraXZ snapshot{};
    for (uint32_t attempt = 0; attempt < 2u; ++attempt) {
        const uint64_t generation_before =
            g_visibility_camera_generation.load(std::memory_order_acquire);
        if ((generation_before & 1u) != 0u) {
            continue;
        }
        snapshot.eye_x =
            g_visibility_camera_eye_x.load(std::memory_order_relaxed);
        snapshot.eye_z =
            g_visibility_camera_eye_z.load(std::memory_order_relaxed);
        snapshot.cull_x =
            g_visibility_camera_cull_x.load(std::memory_order_relaxed);
        snapshot.cull_z =
            g_visibility_camera_cull_z.load(std::memory_order_relaxed);
        snapshot.forward_x =
            g_visibility_camera_forward_x.load(std::memory_order_relaxed);
        snapshot.forward_z =
            g_visibility_camera_forward_z.load(std::memory_order_relaxed);
        const bool valid =
            g_visibility_camera_valid.load(std::memory_order_relaxed);
        const uint64_t generation_after =
            g_visibility_camera_generation.load(std::memory_order_acquire);
        if (generation_before == generation_after &&
            (generation_after & 1u) == 0u) {
            snapshot.generation = generation_after;
            snapshot.valid = valid;
            return snapshot;
        }
    }
    return {};
}

bumble::widescreen::TerrainVisibilitySample
bumble::widescreen::terrain_visibility_sample() {
    TerrainVisibilitySample snapshot{};
    for (uint32_t attempt = 0u; attempt < 2u; ++attempt) {
        const uint64_t generation_before =
            g_terrain_sample_generation.load(std::memory_order_acquire);
        if ((generation_before & 1u) != 0u) {
            continue;
        }
        snapshot.original_cells = g_terrain_sample_original_cells.load(
            std::memory_order_relaxed
        );
        snapshot.candidate_cells = g_terrain_sample_candidate_cells.load(
            std::memory_order_relaxed
        );
        snapshot.visible_cells = g_terrain_sample_visible_cells.load(
            std::memory_order_relaxed
        );
        snapshot.first_row = g_terrain_sample_first_row.load(
            std::memory_order_relaxed
        );
        snapshot.last_row = g_terrain_sample_last_row.load(
            std::memory_order_relaxed
        );
        const bool valid = g_terrain_sample_valid.load(
            std::memory_order_relaxed
        );
        const uint64_t generation_after =
            g_terrain_sample_generation.load(std::memory_order_acquire);
        if (generation_before == generation_after &&
            (generation_after & 1u) == 0u) {
            snapshot.generation = generation_after;
            snapshot.valid = valid;
            return snapshot;
        }
    }
    return {};
}

bool bumble::widescreen::visibility_expansion_enabled() {
    return horizontal_expansion_scale() > 1.000001 ||
        fog_scale() < 0.999999f ||
        (g_enabled.load(std::memory_order_acquire) &&
         bumble::graphics_options::modern_lighting_enabled());
}

bool bumble::widescreen::extended_ui_enabled() {
    const uint64_t size = g_render_size.load(std::memory_order_acquire);
    return uint32_t(size >> 32) != 0 && uint32_t(size) != 0;
}

uint32_t bumble::widescreen::display_list_frame_base() {
    return g_display_list_frame_base.load(std::memory_order_acquire);
}

uint32_t bumble::widescreen::display_list_arena_base() {
    return g_active_display_list_base.load(std::memory_order_acquire);
}

uint32_t bumble::widescreen::display_list_arena_end() {
    return g_active_display_list_end.load(std::memory_order_acquire);
}

uint32_t bumble::widescreen::matrix_arena_capacity() {
    return g_active_matrix_capacity.load(std::memory_order_acquire);
}

uint64_t bumble::widescreen::legacy_level_entry_suppression_count() {
    return g_legacy_level_entry_suppression_count.load(
        std::memory_order_acquire
    );
}

uint64_t bumble::widescreen::post_commit_level_select_suppression_count() {
    return g_post_commit_level_select_suppression_count.load(
        std::memory_order_acquire
    );
}

extern "C" void bumble_prepare_extended_matrix_arena(
    uint8_t* rdram,
    recomp_context* context
) {
    bumble::text_overlay::begin_frame_observations();
    bumble::combat_feedback::observe_frame(rdram);
    bumble::game_completion_screen::observe_frame();
    g_active_matrix_capacity.store(
        kAuthoredMatrixCapacity,
        std::memory_order_release
    );
    g_active_display_list_base.store(0u, std::memory_order_release);
    g_active_display_list_end.store(0u, std::memory_order_release);
    if (rdram == nullptr || context == nullptr) {
        return;
    }

    const uint32_t authored_frame_base = static_cast<uint32_t>(context->r23);
    const uint32_t authored_matrix_base = authored_frame_base +
        kFrameBaseFromMatrixBase;
    const uint32_t authored_cursor = authored_frame_base +
        kDisplayListArenaOffset;
    const uint32_t live_matrix_base = read_u32(rdram, kFrameMatrixBase);
    const uint32_t live_cursor = read_u32(rdram, kDisplayListCursor);
    if (!guest_rdram_address(authored_frame_base, 8u) ||
        (authored_frame_base & 0x1FFFFFFFu) +
                kDisplayListArenaEndOffset >
            kRdramSize ||
        live_matrix_base != authored_matrix_base ||
        live_cursor != authored_cursor) {
        return;
    }

    g_display_list_frame_base.store(
        authored_frame_base,
        std::memory_order_release
    );
    g_active_display_list_base.store(
        authored_cursor,
        std::memory_order_release
    );
    g_active_display_list_end.store(
        authored_frame_base + kDisplayListArenaEndOffset,
        std::memory_order_release
    );
    if (!expanded_single_player_world_enabled(rdram)) {
        return;
    }

    FrameMatrixArena* arena = prepare_frame_matrix_arena(
        rdram,
        authored_frame_base
    );
    if (arena == nullptr || !extended_matrix_guards_intact(*arena)) {
        const uint64_t failure = g_extended_matrix_failure_count.fetch_add(
            1u,
            std::memory_order_acq_rel
        ) + 1u;
        if (failure <= 4u || std::has_single_bit(failure)) {
            std::fprintf(
                stderr,
                "BUMBLE_MATRIX_ARENA stage=extended_allocation_failed"
                " failure=%" PRIu64 " authored_frame_base=0x%08" PRIX32
                " fallback_capacity=%" PRIu32 "\n",
                failure,
                authored_frame_base,
                kAuthoredMatrixCapacity
            );
            std::fflush(stderr);
        }
        return;
    }

    const uint32_t extended_display_list_base = arena->guest_base +
        kExtendedDisplayListOffset;
    const uint32_t extended_display_list_end = extended_display_list_base +
        kExtendedDisplayListBytes;
    if (!recomp_rdram_address(
            extended_display_list_base,
            kExtendedDisplayListBytes,
            8u
        )) {
        return;
    }

    // Enable full KSEG0 offsets before any extended matrix or vertex pointer.
    write_u32(
        rdram,
        extended_display_list_base,
        extended_command(kSetRdramExtendedV1)
    );
    write_u32(rdram, extended_display_list_base + 4u, 1u);
    write_u32(
        rdram,
        kDisplayListCursor,
        extended_display_list_base + 8u
    );
    write_u32(rdram, kFrameMatrixBase, arena->guest_base);
    g_active_display_list_base.store(
        extended_display_list_base,
        std::memory_order_release
    );
    g_active_display_list_end.store(
        extended_display_list_end,
        std::memory_order_release
    );
    g_active_matrix_capacity.store(
        kExtendedMatrixCapacity,
        std::memory_order_release
    );

    const uint64_t frame = g_extended_matrix_frame_count.fetch_add(
        1u,
        std::memory_order_acq_rel
    ) + 1u;
    if (frame <= 4u || std::has_single_bit(frame)) {
        std::fprintf(
            stderr,
            "BUMBLE_MATRIX_ARENA stage=extended_frame_ready"
            " frame=%" PRIu64
            " authored_frame_base=0x%08" PRIX32
            " matrix_guest_base=0x%08" PRIX32
            " matrix_payload_bytes=%" PRIu32
            " matrix_capacity=%" PRIu32
            " display_list_guest_base=0x%08" PRIX32
            " display_list_capacity_bytes=%" PRIu32
            " display_list_cursor=0x%08" PRIX32
            " extended_rdram_command=0x%08" PRIX32
            " guards_intact=1\n",
            frame,
            authored_frame_base,
            arena->guest_base,
            kExtendedMatrixBytes,
            kExtendedMatrixCapacity,
            extended_display_list_base,
            kExtendedDisplayListBytes,
            extended_display_list_base + 8u,
            extended_command(kSetRdramExtendedV1)
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_restore_task_display_list_owner(
    uint8_t* rdram,
    recomp_context* context
) {
    bumble::text_overlay::commit_frame_observations();
    if (g_briefing_handoff_ready) {
        g_briefing_handoff_ready = false;
        bumble::graphics_options::mark_main_menu_handoff_ready();
    }
    if (rdram == nullptr || context == nullptr ||
        g_active_matrix_capacity.load(std::memory_order_acquire) !=
            kExtendedMatrixCapacity) {
        return;
    }

    const uint32_t frame_base = g_display_list_frame_base.load(
        std::memory_order_acquire
    );
    const uint32_t active_display_list_base =
        g_active_display_list_base.load(std::memory_order_acquire);
    const uint32_t active_display_list_end =
        g_active_display_list_end.load(std::memory_order_acquire);
    const uint32_t extended_matrix_base = read_u32(rdram, kFrameMatrixBase);
    const uint32_t live_cursor = read_u32(rdram, kDisplayListCursor);
    if (!guest_rdram_address(frame_base, 8u) ||
        (frame_base & 0x1FFFFFFFu) + kDisplayListArenaEndOffset >
            kRdramSize ||
        active_display_list_end <= active_display_list_base ||
        !recomp_rdram_address(
            active_display_list_base,
            active_display_list_end - active_display_list_base,
            8u
        ) ||
        low_guest_address(context->r7) != extended_matrix_base ||
        live_cursor < active_display_list_base ||
        live_cursor > active_display_list_end ||
        active_display_list_base < 0x80000000u + 0xB000u) {
        return;
    }

    // Set a3 so a3+0xB000 points to the extended commands; leave D_80105224 unchanged.
    const uint32_t task_address_base = active_display_list_base - 0xB000u;
    context->r7 = guest_address(task_address_base);

    bool expected = false;
    if (g_task_display_list_owner_logged.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        )) {
        std::fprintf(
            stderr,
            "BUMBLE_MATRIX_ARENA stage=task_display_list_owner_restored"
            " extended_matrix_base=0x%08" PRIX32
            " task_address_base=0x%08" PRIX32
            " task_display_list=0x%08" PRIX32
            " display_list_bytes=%" PRIu32
            " matrix_global_mutated=0 register_mutated=a3\n",
            extended_matrix_base,
            task_address_base,
            active_display_list_base,
            live_cursor - active_display_list_base
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_guard_extended_display_list_capacity(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr ||
        g_active_matrix_capacity.load(std::memory_order_acquire) !=
            kExtendedMatrixCapacity) {
        return;
    }

    const uint32_t arena_start = g_active_display_list_base.load(
        std::memory_order_acquire
    );
    const uint32_t arena_end = g_active_display_list_end.load(
        std::memory_order_acquire
    );
    const uint32_t cursor = read_u32(rdram, kDisplayListCursor);
    if (arena_end <= arena_start || cursor < arena_start ||
        cursor >= arena_end ||
        !recomp_rdram_address(arena_start, arena_end - arena_start, 8u)) {
        return;
    }

    context->r4 = guest_address(1u);
    bool expected = false;
    if (g_extended_display_list_capacity_logged.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        )) {
        std::fprintf(
            stderr,
            "BUMBLE_MATRIX_ARENA stage=extended_display_list_capacity"
            " arena_start=0x%08" PRIX32
            " arena_end=0x%08" PRIX32
            " cursor=0x%08" PRIX32
            " bytes_used=%" PRIu32
            " capacity_bytes=%" PRIu32
            " register_mutated=a0 guest_memory_mutated=0\n",
            arena_start,
            arena_end,
            cursor,
            cursor - arena_start,
            arena_end - arena_start
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_configure_single_player_frame_scissor(
    uint8_t* rdram,
    recomp_context* context
) {
    if (context == nullptr || !wide_single_player_world_enabled(rdram)) {
        return;
    }

    if (static_cast<uint32_t>(context->r18) != kAuthoredWorldScissor[0] ||
        static_cast<uint32_t>(context->r20) != kAuthoredWorldScissor[1]) {
        return;
    }
    context->r18 = guest_address(kFullFrameWorldScissor[0]);
    context->r20 = guest_address(kFullFrameWorldScissor[1]);

    bool expected = false;
    if (g_single_player_frame_scissor_logged.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        )) {
        std::fprintf(
            stderr,
            "BUMBLE_WIDESCREEN stage=single_player_frame_scissor"
            " pc=0x80054678 s2=0x%08" PRIX32 " s4=0x%08" PRIX32 "\n",
            static_cast<uint32_t>(context->r18),
            static_cast<uint32_t>(context->r20)
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_configure_single_player_depth_clear(
    uint8_t* rdram,
    recomp_context* context
) {
    if (context == nullptr || !wide_single_player_world_enabled(rdram)) {
        return;
    }

    if (static_cast<uint32_t>(context->r5) !=
            kAuthoredSinglePlayerDepthClearUpper ||
        static_cast<uint32_t>(context->r4) !=
            kAuthoredSinglePlayerDepthClearLower) {
        return;
    }
    context->r5 = guest_address(kFullFrameDepthClearUpper);
    context->r4 = guest_address(kFullFrameDepthClearLower);

    bool expected = false;
    if (g_single_player_depth_clear_logged.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        )) {
        std::fprintf(
            stderr,
            "BUMBLE_WIDESCREEN stage=single_player_depth_clear"
            " pc=0x80054928 a1=0x%08" PRIX32 " a0=0x%08" PRIX32 "\n",
            static_cast<uint32_t>(context->r5),
            static_cast<uint32_t>(context->r4)
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_force_wide_sky_background_fill(
    uint8_t* rdram,
    recomp_context* context
) {
    if (context == nullptr || !wide_single_player_world_enabled(rdram)) {
        return;
    }

    const uint32_t old_branch_value = static_cast<uint32_t>(context->r2);
    const bool forced_live_level_fill = old_branch_value == 0u;
    if (forced_live_level_fill) {
        context->r2 = guest_address(1u);
    }
    const uint32_t new_branch_value = static_cast<uint32_t>(context->r2);

    bool expected = false;
    if (g_single_player_sky_clear_logged.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        )) {
        std::fprintf(
            stderr,
            "BUMBLE_WIDESCREEN stage=single_player_sky_background_fill"
            " pc=0x800549A4 branch_register=v0"
            " old_value=%" PRIu32 " new_value=%" PRIu32
            " action=%s"
            " level_rgb=0x%08" PRIX32
            " pack_owner=guest_0x800549B8_0x80054A00"
            " wide_single_player_only=1 pvs_mutated=0"
            " matrices_consumed=0 guest_memory_mutated=0\n",
            old_branch_value,
            new_branch_value,
            forced_live_level_fill
                ? "forced_live_level_color"
                : "authored_live_level_color_already_enabled",
            read_u32(rdram, kLevelBackgroundRgb)
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_configure_single_player_color_clear(
    uint8_t* rdram,
    recomp_context* context
) {
    if (context == nullptr || !wide_single_player_world_enabled(rdram)) {
        return;
    }

    if (static_cast<uint32_t>(context->r6) !=
            kAuthoredSinglePlayerColorClearUpper ||
        static_cast<uint32_t>(context->r5) !=
            kAuthoredSinglePlayerColorClearLower) {
        return;
    }

    context->r6 = guest_address(kFullFrameColorClearUpper);
    context->r5 = guest_address(kFullFrameColorClearLower);

    bool expected = false;
    if (g_single_player_color_clear_logged.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        )) {
        std::fprintf(
            stderr,
            "BUMBLE_WIDESCREEN stage=single_player_color_clear"
            " pc=0x80054A54 a2=0x%08" PRIX32 " a1=0x%08" PRIX32 "\n",
            static_cast<uint32_t>(context->r6),
            static_cast<uint32_t>(context->r5)
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_configure_single_player_world_aperture(
    uint8_t* rdram,
    recomp_context* context
) noexcept(false) {
    if (rdram == nullptr || context == nullptr) {
        return;
    }

    const uint32_t viewport_command = low_guest_address(context->r8);
    const uint32_t scissor_command = low_guest_address(context->r3);
    const bool exact_commands =
        display_list_address(viewport_command) &&
        display_list_address(scissor_command) &&
        read_u32(rdram, viewport_command) == 0xDC080008u &&
        read_u32(rdram, viewport_command + 4u) == kWorldViewport &&
        read_u32(rdram, scissor_command) == kAuthoredWorldScissor[0] &&
        read_u32(rdram, scissor_command + 4u) == kAuthoredWorldScissor[1] &&
        static_cast<uint32_t>(context->r7) == kAuthoredWorldAspectBits;

    const bool visibility_requested =
        bumble::widescreen::visibility_expansion_enabled();
    if (exact_commands) {
        if (g_world_camera_scope_open || !current_display_list_budget_available(rdram, 24u)) {
            throw std::runtime_error("Invalid world camera matrix scope");
        }
        constexpr uint32_t CameraInterpolation = 1u | (1u << 1u) |
            (1u << 3u) | (1u << 5u) | (1u << 7u) | (1u << 9u) | (1u << 11u);
        const uint32_t phase = read_u32(rdram, kFrontendObject + kFrontendCurrentPhaseOffset);
        const uint32_t level = read_u32(rdram, kFrontendObject + 0x10u);
        if (phase != g_world_camera_phase || level != g_world_camera_level) {
            bumble::widescreen::invalidate_scene_history();
            g_world_camera_phase = phase;
            g_world_camera_level = level;
        }
        const uint32_t cameraId = 0x40000000u |
            (g_world_camera_generation.load(std::memory_order_relaxed) & 0x3FFFFFFFu);
        g_world_camera_scope_open = append_commands(rdram, {
            extended_command(kMatrixGroupV1), cameraId, CameraInterpolation, 0u
        });
    }
    const float authored_far = context->f0.fl;
    const float fog_scale = std::clamp(
        bumble::widescreen::fog_scale(),
        0.0f,
        1.0f
    );
    double projection_far_scale = 1.0;
    float expanded_far = authored_far;
    if (visibility_requested && exact_commands &&
        std::isfinite(authored_far) && authored_far > 0.0f) {
        const double structural_distance_scale = fog_scale <= 0.000001f
            ? kNoFogStructuralDistanceScale
            : kAuthoredStructuralDistanceScale;
        projection_far_scale = std::max(
            1.0,
            (kAuthoredObjectBaseRadiusCoefficient *
                structural_distance_scale) /
                kAuthoredProjectionFarCoefficient
        );
        const double expanded_far_double =
            static_cast<double>(authored_far) * projection_far_scale;
        if (std::isfinite(expanded_far_double) &&
            expanded_far_double <=
                static_cast<double>(std::numeric_limits<float>::max())) {
            expanded_far = static_cast<float>(expanded_far_double);
            context->f0.fl = expanded_far;
        }
        else {
            projection_far_scale = 1.0;
            expanded_far = authored_far;
        }
    }

    const uint64_t projection_signature =
        (static_cast<uint64_t>(std::bit_cast<uint32_t>(authored_far)) << 32u) |
        std::bit_cast<uint32_t>(expanded_far);
    uint64_t prior_projection_signature =
        g_last_projection_far_signature.load(std::memory_order_acquire);
    if (prior_projection_signature != projection_signature &&
        g_last_projection_far_signature.compare_exchange_strong(
            prior_projection_signature,
            projection_signature,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        )) {
        std::fprintf(
            stderr,
            "BUMBLE_WIDESCREEN stage=single_player_projection_far"
            " authored_far=%.6f expanded_far=%.6f scale=%.6f"
            " fog_scale=%.6f visibility_expansion=%d"
            " policy=match_large_structural_base_radius_1_to_16"
            " draw_calls_added=0 matrices_added=0\n",
            static_cast<double>(authored_far),
            static_cast<double>(expanded_far),
            projection_far_scale,
            static_cast<double>(fog_scale),
            visibility_requested ? 1 : 0
        );
        std::fflush(stderr);
    }

    const bool wide_requested = bumble::widescreen::horizontal_expansion_scale() > 1.000001;
    const bool expand = wide_requested && exact_commands;
    write_world_viewport(
        rdram,
        expand ? kFullFrameWorldViewport : kAuthoredWorldViewport
    );
    if (!expand) {
        log_world_aperture_state_once(wide_requested ? 3u : 0u);
        return;
    }

    write_u32(rdram, scissor_command, kFullFrameWorldScissor[0]);
    write_u32(rdram, scissor_command + 4u, kFullFrameWorldScissor[1]);
    context->r7 = static_cast<gpr>(
        static_cast<int32_t>(kFullFrameWorldAspectBits)
    );
    log_world_aperture_state_once(1u);
}

extern "C" void bumble_restore_split_screen_world_aperture(
    uint8_t* rdram,
    recomp_context*
) {
    if (rdram == nullptr) {
        return;
    }

    write_world_viewport(rdram, kAuthoredWorldViewport);
    log_world_aperture_state_once(2u);
}

extern "C" void bumble_expand_terrain_visibility(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr) {
        return;
    }

    publish_terrain_visibility_sample(0u, 0u, 0u, 0, 0, false);

    g_terrain_scratch.active = false;
    g_terrain_scratch.producer_relocated = false;
    g_terrain_scratch.consumer_relocated = false;
    g_terrain_scratch.expected_entries = 0;

    const int32_t authored_first_row = static_cast<int32_t>(context->r11);
    const int32_t authored_last_row = static_cast<int32_t>(context->r30);
    if (authored_first_row < kMinimumCell ||
        authored_first_row > kMaximumCell ||
        authored_last_row < kMinimumCell ||
        authored_last_row > kMaximumCell ||
        authored_first_row > authored_last_row) {
        return;
    }

    const float fog_scale = std::clamp(
        bumble::widescreen::fog_scale(),
        0.0f,
        1.0f
    );
    const bool no_fog = fog_scale <= 0.000001f;
    const double scale = no_fog
        ? std::max(
            bumble::widescreen::horizontal_expansion_scale(),
            kNoFogTerrainFootprintScale
        )
        : bumble::widescreen::horizontal_expansion_scale();
    if (!bumble::widescreen::visibility_expansion_enabled()) {
        return;
    }

    const uint32_t sector_mask_address =
        static_cast<uint32_t>(context->r29) + 0x100u;
    if (!guest_rdram_address(sector_mask_address, 1u)) {
        return;
    }
    MEM_B(0x100, context->r29) = 0xFFu;
    const uint8_t sector_mask = static_cast<uint8_t>(
        MEM_BU(0x100, context->r29)
    );

    const bool relocated = prepare_terrain_scratch(rdram);
    if (!relocated) {
        const uint64_t failures = g_terrain_scratch_failures.fetch_add(
            1,
            std::memory_order_acq_rel
        ) + 1;
        if (failures == 1u) {
            std::fprintf(
                stderr,
                "BUMBLE_WIDESCREEN stage=terrain_scratch_allocation_failed"
                " fallback_capacity=%" PRIu32 "\n",
                kOriginalTerrainCandidateCapacity
            );
            std::fflush(stderr);
        }
    }
    const uint32_t candidate_capacity = relocated
        ? kTerrainScratchCapacity
        : kOriginalTerrainCandidateCapacity;

    std::array<int32_t, 64> old_lowers{};
    std::array<int32_t, 64> old_uppers{};
    std::array<int32_t, 64> new_lowers{};
    std::array<int32_t, 64> new_uppers{};
    std::array<int32_t, 64> target_lowers{};
    std::array<int32_t, 64> target_uppers{};
    std::array<bool, 64> valid_rows{};
    old_uppers.fill(-1);
    new_uppers.fill(-1);
    uint32_t original_cells = 0;
    uint32_t original_visible_cells = 0;
    uint32_t desired_cells = 0;
    for (int32_t row = authored_first_row;
         row <= authored_last_row;
         ++row) {
        const size_t index = static_cast<size_t>(row - kMinimumCell);
        int32_t old_lower = 0;
        int32_t old_upper = -1;
        if (!read_terrain_bounds(
                rdram,
                row,
                old_lower,
                old_upper
            )) {
            continue;
        }
        valid_rows[index] = true;
        old_lowers[index] = new_lowers[index] = old_lower;
        old_uppers[index] = new_uppers[index] = old_upper;
        original_cells += static_cast<uint32_t>(old_upper - old_lower + 1);
        original_visible_cells += visible_cells_in_span(
            rdram,
            row,
            old_lower,
            old_upper,
            sector_mask
        );

        const double center = (static_cast<double>(old_lower) + old_upper) * 0.5;
        const double half_span =
            (static_cast<double>(old_upper) - old_lower + 1.0) * 0.5;
        target_lowers[index] = std::max(
            kMinimumCell,
            static_cast<int32_t>(std::floor(center - half_span * scale)) -
                kTerrainGuardCells
        );
        target_uppers[index] = std::min(
            kMaximumCell,
            static_cast<int32_t>(std::ceil(center + half_span * scale)) +
                kTerrainGuardCells
        );
        desired_cells += static_cast<uint32_t>(
            target_uppers[index] - target_lowers[index] + 1
        );
    }

    const uint32_t matrix_capacity =
        bumble::widescreen::matrix_arena_capacity();
    const bool extended_matrix_arena =
        matrix_capacity == kExtendedMatrixCapacity;
    const uint32_t terrain_promotion_matrix_ceiling =
        matrix_capacity > kPostObjectMatrixQueueCapacity
            ? matrix_capacity - kPostObjectMatrixQueueCapacity
            : 0u;
    const int32_t live_matrix_count = read_s32(rdram, kTerrainMatrixCount);
    const uint32_t matrix_slots_available =
        live_matrix_count >= 0 &&
        static_cast<uint32_t>(live_matrix_count) <
            terrain_promotion_matrix_ceiling
            ? terrain_promotion_matrix_ceiling -
                static_cast<uint32_t>(live_matrix_count)
            : 0u;
    const auto authored_valid_rows = valid_rows;
    const auto authored_target_lowers = target_lowers;
    const auto authored_target_uppers = target_uppers;

    const uint32_t stack_pointer = low_guest_address(context->r29);
    const uint32_t eye_x_address =
        stack_pointer + kTerrainCameraEyeXStackOffset;
    const uint32_t eye_z_address =
        stack_pointer + kTerrainCameraEyeZStackOffset;
    const uint32_t cull_x_address =
        stack_pointer + kTerrainCameraCullXStackOffset;
    const uint32_t cull_z_address =
        stack_pointer + kTerrainCameraCullZStackOffset;
    const uint32_t focus_x_address =
        stack_pointer + kTerrainCameraFocusXStackOffset;
    const uint32_t focus_z_address =
        stack_pointer + kTerrainCameraFocusZStackOffset;
    float camera_eye_x = 0.0f;
    float camera_eye_z = 0.0f;
    float camera_cull_x = 0.0f;
    float camera_cull_z = 0.0f;
    float camera_focus_x = 0.0f;
    float camera_focus_z = 0.0f;
    double camera_forward_x = 0.0;
    double camera_forward_z = 0.0;
    bool camera_position_xz_valid = false;
    bool camera_focus_xz_valid = false;
    bool camera_packet_xz_valid = false;
    if (guest_rdram_address(eye_x_address, 4u) &&
        guest_rdram_address(eye_z_address, 4u) &&
        guest_rdram_address(cull_x_address, 4u) &&
        guest_rdram_address(cull_z_address, 4u) &&
        guest_rdram_address(focus_x_address, 4u) &&
        guest_rdram_address(focus_z_address, 4u)) {
        camera_eye_x = read_f32(rdram, eye_x_address);
        camera_eye_z = read_f32(rdram, eye_z_address);
        camera_cull_x = read_f32(rdram, cull_x_address);
        camera_cull_z = read_f32(rdram, cull_z_address);
        camera_focus_x = read_f32(rdram, focus_x_address);
        camera_focus_z = read_f32(rdram, focus_z_address);
        camera_forward_x = static_cast<double>(camera_cull_x) -
            static_cast<double>(camera_eye_x);
        camera_forward_z = static_cast<double>(camera_cull_z) -
            static_cast<double>(camera_eye_z);
        camera_position_xz_valid = std::isfinite(camera_eye_x) &&
            std::isfinite(camera_eye_z);
        camera_focus_xz_valid = std::isfinite(camera_focus_x) &&
            std::isfinite(camera_focus_z);
        camera_packet_xz_valid = camera_position_xz_valid &&
            std::isfinite(camera_cull_x) &&
            std::isfinite(camera_cull_z) &&
            std::isfinite(camera_forward_x) &&
            std::isfinite(camera_forward_z) &&
            camera_forward_x * camera_forward_x +
                camera_forward_z * camera_forward_z >
                kTerrainForwardZEpsilon * kTerrainForwardZEpsilon;
    }

    bool stable_footprint_promoted = false;
    uint32_t stable_footprint_visible_cells = 0u;
    uint32_t stable_footprint_candidate_cells = 0u;
    int32_t stable_first_row = authored_first_row;
    int32_t stable_last_row = authored_last_row;
    int32_t stable_radius_cells = 0;
    if (camera_focus_xz_valid && (no_fog ||
            bumble::graphics_options::modern_lighting_enabled())) {
        static const auto disk_half_widths = [] {
            std::array<std::array<int32_t, 37>, 37> widths{};
            for (int32_t radius = 1; radius <= 36; ++radius) {
                for (int32_t row = 0; row <= radius; ++row) {
                    widths[radius][row] = static_cast<int32_t>(std::floor(
                        std::sqrt(static_cast<double>(radius * radius - row * row))));
                }
            }
            return widths;
        }();
        const int32_t camera_column = std::clamp(
            static_cast<int32_t>(std::lround(
                static_cast<double>(camera_focus_x) / kTerrainCellSize
            )),
            kMinimumCell,
            kMaximumCell
        );
        const int32_t camera_row = std::clamp(
            static_cast<int32_t>(std::lround(
                static_cast<double>(camera_focus_z) / kTerrainCellSize
            )),
            kMinimumCell,
            kMaximumCell
        );
        stable_radius_cells = std::clamp(
            static_cast<int32_t>(std::ceil(
                kTerrainRadiusAtOriginalAspect *
                bumble::widescreen::horizontal_expansion_scale() *
                (no_fog ? 2.0 : 1.0)
            )) + kTerrainGuardCells,
            1,
            36
        );
        if (!relocated) {
            stable_radius_cells = std::min(stable_radius_cells, 16);
        }
        const int32_t disk_first_row = std::max(
            kMinimumCell,
            camera_row - stable_radius_cells
        );
        const int32_t disk_last_row = std::min(
            kMaximumCell,
            camera_row + stable_radius_cells
        );
        const auto compose_disk = [&](bool retain_authored) {
            valid_rows = retain_authored ? authored_valid_rows : decltype(valid_rows){};
            new_lowers = retain_authored && !no_fog ? authored_target_lowers : old_lowers;
            new_uppers = retain_authored && !no_fog ? authored_target_uppers : old_uppers;
            target_lowers = authored_target_lowers;
            target_uppers = authored_target_uppers;
            stable_first_row = retain_authored
                ? std::min(disk_first_row, authored_first_row) : disk_first_row;
            stable_last_row = retain_authored
                ? std::max(disk_last_row, authored_last_row) : disk_last_row;
            stable_footprint_candidate_cells = 0u;
            stable_footprint_visible_cells = 0u;
            for (int32_t row = disk_first_row;
                 row <= disk_last_row;
                 ++row) {
                const int32_t row_delta = row - camera_row;
                const int32_t half_width = disk_half_widths[stable_radius_cells][std::abs(row_delta)];
                const int32_t lower = std::max(
                    kMinimumCell,
                    camera_column - half_width
                );
                const int32_t upper = std::min(
                    kMaximumCell,
                    camera_column + half_width
                );
                const size_t index = static_cast<size_t>(
                    row - kMinimumCell
                );
                new_lowers[index] = target_lowers[index] = retain_authored && authored_valid_rows[index]
                    ? std::min(no_fog ? old_lowers[index] : authored_target_lowers[index], lower) : lower;
                new_uppers[index] = target_uppers[index] = retain_authored && authored_valid_rows[index]
                    ? std::max(no_fog ? old_uppers[index] : authored_target_uppers[index], upper) : upper;
                valid_rows[index] = true;
            }
            for (int32_t row = stable_first_row; row <= stable_last_row; ++row) {
                const size_t index = static_cast<size_t>(row - kMinimumCell);
                if (!valid_rows[index]) {
                    continue;
                }
                const int32_t lower = new_lowers[index];
                const int32_t upper = new_uppers[index];
                stable_footprint_candidate_cells += static_cast<uint32_t>(
                    upper - lower + 1
                );
                stable_footprint_visible_cells += visible_cells_in_span(
                    rdram,
                    row,
                    lower,
                    upper,
                    sector_mask
                );
            }
            return
                stable_footprint_candidate_cells <= candidate_capacity &&
                stable_footprint_visible_cells <=
                    matrix_slots_available;
        };
        stable_footprint_promoted = compose_disk(true) ||
            (no_fog && compose_disk(false));
        if (stable_footprint_promoted) {
            desired_cells = stable_footprint_candidate_cells;
            target_lowers = new_lowers;
            target_uppers = new_uppers;
        }
        else {
            valid_rows = authored_valid_rows;
            new_lowers = old_lowers;
            new_uppers = old_uppers;
            target_lowers = authored_target_lowers;
            target_uppers = authored_target_uppers;
        }
    }

    uint32_t candidate_cells = stable_footprint_promoted
        ? stable_footprint_candidate_cells
        : original_cells;
    uint32_t visible_cells = stable_footprint_promoted
        ? stable_footprint_visible_cells
        : original_visible_cells;
    uint32_t matrix_slots_remaining =
        visible_cells < matrix_slots_available
            ? matrix_slots_available - visible_cells
            : 0u;

    int32_t first_row = stable_footprint_promoted
        ? stable_first_row
        : authored_first_row;
    int32_t last_row = stable_footprint_promoted
        ? stable_last_row
        : authored_last_row;
    uint32_t negative_depth_rows_added = stable_footprint_promoted
        ? static_cast<uint32_t>(std::max(
            authored_first_row - stable_first_row,
            0
        ))
        : 0u;
    uint32_t positive_depth_rows_added = stable_footprint_promoted
        ? static_cast<uint32_t>(std::max(
            stable_last_row - authored_last_row,
            0
        ))
        : 0u;
    uint32_t depth_rows_added =
        negative_depth_rows_added + positive_depth_rows_added;
    const char* depth_direction = stable_footprint_promoted
        ? "rotation_invariant_focus_disk"
        : "none";
    const char* depth_direction_source = stable_footprint_promoted
        ? "camera_focus_not_direction"
        : (no_fog ? "camera_packet_fail_closed" : "not_requested");
    int32_t depth_step = 0;
    if (no_fog && camera_packet_xz_valid &&
        !stable_footprint_promoted) {
                const double forward_length = std::hypot(
                    camera_forward_x,
                    camera_forward_z
                );
                const double normalized_forward_z =
                    camera_forward_z / forward_length;
                const int32_t previous_depth_step =
                    g_terrain_primary_depth_step.load(
                        std::memory_order_relaxed
                    );
                if (normalized_forward_z >
                    kTerrainDepthDirectionHysteresis) {
                    depth_step = 1;
                    depth_direction_source =
                        "camera_packet_forward_z_positive_hysteresis";
                }
                else if (normalized_forward_z <
                    -kTerrainDepthDirectionHysteresis) {
                    depth_step = -1;
                    depth_direction_source =
                        "camera_packet_forward_z_negative_hysteresis";
                }
                else {
                    depth_step = previous_depth_step;
                    depth_direction_source = depth_step == 0
                        ? "camera_packet_sideways_no_history"
                        : "camera_packet_sideways_retained_history";
                }
                if (depth_step != 0) {
                    g_terrain_primary_depth_step.store(
                        depth_step,
                        std::memory_order_relaxed
                    );
                }
    }
    publish_visibility_camera_xz(
        camera_eye_x,
        camera_eye_z,
        camera_cull_x,
        camera_cull_z,
        camera_packet_xz_valid
    );

    const size_t authored_first_index = static_cast<size_t>(
        authored_first_row - kMinimumCell
    );
    const size_t authored_last_index = static_cast<size_t>(
        authored_last_row - kMinimumCell
    );
    const bool depth_growth_eligible = !stable_footprint_promoted && no_fog &&
        depth_step != 0 &&
        valid_rows[authored_first_index] && valid_rows[authored_last_index];
    const uint32_t initial_promotion_matrix_slots = matrix_slots_remaining;
    const uint32_t depth_matrix_slots_reserved = depth_growth_eligible
        ? (matrix_slots_remaining * kNoFogDepthMatrixShareNumerator +
            kNoFogDepthMatrixShareDenominator - 1u) /
            kNoFogDepthMatrixShareDenominator
        : 0u;
    uint32_t lateral_phase_matrix_slots =
        matrix_slots_remaining - depth_matrix_slots_reserved;
    uint32_t negative_depth_phase_matrix_slots = 0;
    uint32_t positive_depth_phase_matrix_slots = 0;
    uint32_t lateral_matrix_slots_used = 0;
    uint32_t depth_matrix_slots_used = 0;
    uint32_t recycled_lateral_matrix_slots_used = 0;

    auto reserve_cell = [&] (
        int32_t row,
        int32_t column,
        uint32_t* phase_matrix_slots
    ) {
        if (candidate_cells >= candidate_capacity) {
            return false;
        }
        const uint32_t matrix_cost =
            (terrain_sector_mask(rdram, row, column) & sector_mask) != 0u
                ? 1u
                : 0u;
        if (matrix_cost > matrix_slots_remaining ||
            (phase_matrix_slots != nullptr &&
             matrix_cost > *phase_matrix_slots)) {
            return false;
        }
        ++candidate_cells;
        visible_cells += matrix_cost;
        matrix_slots_remaining -= matrix_cost;
        if (phase_matrix_slots != nullptr) {
            *phase_matrix_slots -= matrix_cost;
        }
        return true;
    };

    auto expand_authored_lateral_rows = [&](uint32_t* phase_matrix_slots) {
        bool progress = candidate_cells < candidate_capacity;
        while (progress) {
            progress = false;
            for (int32_t row = authored_first_row;
                 row <= authored_last_row;
                 ++row) {
                const size_t index = static_cast<size_t>(
                    row - kMinimumCell
                );
                if (!valid_rows[index]) {
                    continue;
                }
                const int32_t lower_candidate = new_lowers[index] - 1;
                if (new_lowers[index] > target_lowers[index] &&
                    reserve_cell(row, lower_candidate, phase_matrix_slots)) {
                    new_lowers[index] = lower_candidate;
                    progress = true;
                }
                const int32_t upper_candidate = new_uppers[index] + 1;
                if (new_uppers[index] < target_uppers[index] &&
                    reserve_cell(row, upper_candidate, phase_matrix_slots)) {
                    new_uppers[index] = upper_candidate;
                    progress = true;
                }
            }
        }
    };

    const uint32_t before_lateral_matrix_slots = matrix_slots_remaining;
    expand_authored_lateral_rows(&lateral_phase_matrix_slots);
    lateral_matrix_slots_used =
        before_lateral_matrix_slots - matrix_slots_remaining;

    if (depth_growth_eligible) {
        const uint32_t before_depth_matrix_slots = matrix_slots_remaining;
        depth_direction = depth_step > 0
            ? "primary_positive_guard_negative"
            : "primary_negative_guard_positive";
        bool negative_open = first_row > kMinimumCell;
        bool positive_open = last_row < kMaximumCell;
        if (negative_open && positive_open) {
            const uint32_t opposite_guard_slots =
                depth_matrix_slots_reserved / 4u;
            if (depth_step > 0) {
                negative_depth_phase_matrix_slots = opposite_guard_slots;
                positive_depth_phase_matrix_slots =
                    depth_matrix_slots_reserved - opposite_guard_slots;
            }
            else {
                positive_depth_phase_matrix_slots = opposite_guard_slots;
                negative_depth_phase_matrix_slots =
                    depth_matrix_slots_reserved - opposite_guard_slots;
            }
        }
        else if (negative_open) {
            negative_depth_phase_matrix_slots = depth_matrix_slots_reserved;
        }
        else if (positive_open) {
            positive_depth_phase_matrix_slots = depth_matrix_slots_reserved;
        }

        auto append_depth_row = [&](int32_t step) {
            uint32_t& direction_matrix_slots = step > 0
                ? positive_depth_phase_matrix_slots
                : negative_depth_phase_matrix_slots;
            if (direction_matrix_slots == 0u) {
                return false;
            }
            const int32_t previous_row = step > 0 ? last_row : first_row;
            const int32_t next_row = previous_row + step;
            if (next_row < kMinimumCell || next_row > kMaximumCell) {
                return false;
            }
            const size_t previous_index = static_cast<size_t>(
                previous_row - kMinimumCell
            );
            if (!valid_rows[previous_index]) {
                return false;
            }
            const int32_t prior_row = previous_row - step;
            const bool prior_valid = prior_row >= kMinimumCell &&
                prior_row <= kMaximumCell &&
                valid_rows[static_cast<size_t>(
                    prior_row - kMinimumCell
                )];
            const size_t prior_index = prior_valid
                ? static_cast<size_t>(prior_row - kMinimumCell)
                : previous_index;
            const int32_t lower_delta = prior_valid
                ? new_lowers[previous_index] - new_lowers[prior_index]
                : 0;
            const int32_t upper_delta = prior_valid
                ? new_uppers[previous_index] - new_uppers[prior_index]
                : 0;
            const int32_t desired_lower = std::max(
                kMinimumCell,
                std::min(
                    new_lowers[previous_index],
                    new_lowers[previous_index] + lower_delta
                ) - 1
            );
            const int32_t desired_upper = std::min(
                kMaximumCell,
                std::max(
                    new_uppers[previous_index],
                    new_uppers[previous_index] + upper_delta
                ) + 1
            );
            const int32_t center = std::clamp(
                (desired_lower + desired_upper) / 2,
                kMinimumCell,
                kMaximumCell
            );
            if (!reserve_cell(
                    next_row,
                    center,
                    &direction_matrix_slots
                )) {
                return false;
            }

            const size_t next_index = static_cast<size_t>(
                next_row - kMinimumCell
            );
            valid_rows[next_index] = true;
            old_lowers[next_index] = center;
            old_uppers[next_index] = center - 1;
            new_lowers[next_index] = center;
            new_uppers[next_index] = center;

            bool row_progress = true;
            while (row_progress) {
                row_progress = false;
                const int32_t lower_candidate =
                    new_lowers[next_index] - 1;
                if (new_lowers[next_index] > desired_lower &&
                    reserve_cell(
                        next_row,
                        lower_candidate,
                        &direction_matrix_slots
                    )) {
                    new_lowers[next_index] = lower_candidate;
                    row_progress = true;
                }
                const int32_t upper_candidate =
                    new_uppers[next_index] + 1;
                if (new_uppers[next_index] < desired_upper &&
                    reserve_cell(
                        next_row,
                        upper_candidate,
                        &direction_matrix_slots
                    )) {
                    new_uppers[next_index] = upper_candidate;
                    row_progress = true;
                }
            }

            if (step > 0) {
                last_row = next_row;
                ++positive_depth_rows_added;
            }
            else {
                first_row = next_row;
                ++negative_depth_rows_added;
            }
            ++depth_rows_added;

            return new_lowers[next_index] == desired_lower &&
                new_uppers[next_index] == desired_upper &&
                direction_matrix_slots != 0u &&
                candidate_cells < candidate_capacity;
        };

        const int32_t primary_step = depth_step;
        const int32_t opposite_step = -depth_step;
        bool& primary_open = primary_step > 0
            ? positive_open
            : negative_open;
        bool& opposite_open = opposite_step > 0
            ? positive_open
            : negative_open;
        uint32_t& primary_direction_slots = primary_step > 0
            ? positive_depth_phase_matrix_slots
            : negative_depth_phase_matrix_slots;
        uint32_t& opposite_direction_slots = opposite_step > 0
            ? positive_depth_phase_matrix_slots
            : negative_depth_phase_matrix_slots;

        uint32_t opposite_guard_rows_added = 0;
        while (opposite_open && opposite_direction_slots != 0u &&
               opposite_guard_rows_added <
                    kTerrainOppositeDepthGuardRows &&
               candidate_cells < candidate_capacity) {
            const uint32_t before = candidate_cells;
            opposite_open = append_depth_row(opposite_step);
            if (candidate_cells == before) {
                break;
            }
            ++opposite_guard_rows_added;
        }

        primary_direction_slots += opposite_direction_slots;
        opposite_direction_slots = 0u;
        while (primary_open && primary_direction_slots != 0u &&
               candidate_cells < candidate_capacity) {
            const uint32_t before = candidate_cells;
            primary_open = append_depth_row(primary_step);
            if (candidate_cells == before) {
                break;
            }
        }
        depth_matrix_slots_used =
            before_depth_matrix_slots - matrix_slots_remaining;
    }

    if (matrix_slots_remaining != 0u &&
        candidate_cells < candidate_capacity) {
        const uint32_t before_recycled_lateral_slots = matrix_slots_remaining;
        expand_authored_lateral_rows(nullptr);
        recycled_lateral_matrix_slots_used =
            before_recycled_lateral_slots - matrix_slots_remaining;
    }

    uint32_t rows_expanded = 0;
    const uint32_t cells_added = candidate_cells - original_cells;
    for (int32_t row = first_row; row <= last_row; ++row) {
        const size_t index = static_cast<size_t>(row - kMinimumCell);
        if (!valid_rows[index] ||
            (new_lowers[index] == old_lowers[index] &&
             new_uppers[index] == old_uppers[index])) {
            continue;
        }
        const uint32_t row_address = terrain_row_address(row);
        write_s32(rdram, row_address, new_uppers[index]);
        write_s32(rdram, row_address + 4u, new_lowers[index]);
        ++rows_expanded;
    }
    context->r11 = guest_address(static_cast<uint32_t>(first_row));
    context->r30 = guest_address(static_cast<uint32_t>(last_row));

    if (relocated) {
        g_terrain_scratch.expected_entries = candidate_cells;
    }
    publish_terrain_visibility_sample(
        original_cells,
        candidate_cells,
        visible_cells,
        first_row,
        last_row,
        true
    );
    const char* allocation_policy = stable_footprint_promoted
        ? "rotation_invariant_focus_disk"
        : (depth_growth_eligible
            ? "balanced_lateral_hysteretic_depth_with_opposite_guard"
            : "lateral_only");
    const bool expansion_capped = !stable_footprint_promoted &&
        (candidate_cells >= candidate_capacity ||
         visible_cells >= matrix_slots_available);

    if (no_fog) {
        const uint64_t no_fog_count = g_no_fog_expansion_calls.fetch_add(
            1,
            std::memory_order_acq_rel
        ) + 1;
        if (no_fog_count == 1u) {
            std::fprintf(
                stderr,
                "BUMBLE_WIDESCREEN stage=terrain_sector_mask_relaxed"
                " fog_scale=%.6f terrain_footprint_scale=%.6f"
                " guard_cells=%" PRId32
                " authored_first_row=%" PRId32
                " authored_last_row=%" PRId32 " sector_mask=0xFF"
                " depth_direction_source=%s depth_direction=%s"
                " camera_vector_valid=%d camera_packet_xz_valid=%d"
                " camera_eye_x=%.6f camera_eye_z=%.6f"
                " camera_focus_x=%.6f camera_focus_z=%.6f"
                " camera_cull_x=%.6f camera_cull_z=%.6f"
                " camera_forward_x=%.6f camera_forward_z=%.6f"
                " depth_growth_eligible=%d"
                " depth_rows_added=%" PRIu32
                " negative_depth_rows_added=%" PRIu32
                " positive_depth_rows_added=%" PRIu32
                " effective_first_row=%" PRId32
                " effective_last_row=%" PRId32
                " candidate_capacity=%" PRIu32
                " matrix_capacity=%" PRIu32
                " terrain_promotion_matrix_ceiling=%" PRIu32
                " downstream_matrix_reserve=%" PRIu32
                " matrix_count_before=%" PRId32
                " allocation_policy=%s"
                " promotion_matrix_slots_initial=%" PRIu32
                " depth_matrix_slots_reserved=%" PRIu32
                " lateral_matrix_slots_used=%" PRIu32
                " depth_matrix_slots_used=%" PRIu32
                " recycled_lateral_matrix_slots_used=%" PRIu32
                " camera_sign_controls_primary_depth_rows=%d"
                " depth_direction_hysteresis=%.6f"
                " opposite_depth_guard_row_cap=%" PRIu32
                " stable_footprint_promoted=%d"
                " stable_radius_cells=%" PRId32
                " extended_matrix_arena=%d"
                " global_draw_distance_mutated=0\n",
                static_cast<double>(fog_scale),
                scale,
                kTerrainGuardCells,
                authored_first_row,
                authored_last_row,
                depth_direction_source,
                depth_direction,
                depth_step != 0 ? 1 : 0,
                camera_packet_xz_valid ? 1 : 0,
                static_cast<double>(camera_eye_x),
                static_cast<double>(camera_eye_z),
                static_cast<double>(camera_focus_x),
                static_cast<double>(camera_focus_z),
                static_cast<double>(camera_cull_x),
                static_cast<double>(camera_cull_z),
                camera_forward_x,
                camera_forward_z,
                depth_growth_eligible ? 1 : 0,
                depth_rows_added,
                negative_depth_rows_added,
                positive_depth_rows_added,
                first_row,
                last_row,
                candidate_capacity,
                matrix_capacity,
                terrain_promotion_matrix_ceiling,
                kPostObjectMatrixQueueCapacity,
                live_matrix_count,
                allocation_policy,
                initial_promotion_matrix_slots,
                depth_matrix_slots_reserved,
                lateral_matrix_slots_used,
                depth_matrix_slots_used,
                recycled_lateral_matrix_slots_used,
                stable_footprint_promoted ? 0 : 1,
                kTerrainDepthDirectionHysteresis,
                kTerrainOppositeDepthGuardRows,
                stable_footprint_promoted ? 1 : 0,
                stable_radius_cells,
                extended_matrix_arena ? 1 : 0
            );
            std::fflush(stderr);
        }
    }

    const uint64_t count =
        g_expansion_calls.fetch_add(1, std::memory_order_acq_rel) + 1;
    const uint64_t packed_size =
        g_render_size.load(std::memory_order_acquire);
    uint64_t last_size = g_last_logged_size.load(std::memory_order_acquire);
    const bool size_changed = last_size != packed_size &&
        g_last_logged_size.compare_exchange_strong(
            last_size,
            packed_size,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        );
    if (count == 1 || size_changed) {
        std::fprintf(
            stderr,
            "BUMBLE_WIDESCREEN stage=terrain_visibility_expanded"
            " count=%" PRIu64 " window_width=%" PRIu32
            " window_height=%" PRIu32 " scale=%.6f guard_cells=%" PRId32
            " first_row=%" PRId32 " last_row=%" PRId32
            " rows_expanded=%" PRIu32 " cells_added=%" PRIu32
            " original_cells=%" PRIu32 " desired_cells=%" PRIu32
            " candidate_cells=%" PRIu32
            " visible_cells=%" PRIu32
            " candidate_capacity=%" PRIu32
            " matrix_capacity=%" PRIu32
            " terrain_promotion_matrix_ceiling=%" PRIu32
            " downstream_matrix_reserve=%" PRIu32
            " matrix_slots_available=%" PRIu32
            " allocation_policy=%s"
            " promotion_matrix_slots_initial=%" PRIu32
            " depth_matrix_slots_reserved=%" PRIu32
            " lateral_matrix_slots_used=%" PRIu32
            " depth_matrix_slots_used=%" PRIu32
            " recycled_lateral_matrix_slots_used=%" PRIu32
            " depth_rows_added=%" PRIu32
            " negative_depth_rows_added=%" PRIu32
            " positive_depth_rows_added=%" PRIu32
            " camera_sign_controls_primary_depth_rows=%d"
            " depth_direction_hysteresis=%.6f"
            " opposite_depth_guard_row_cap=%" PRIu32
            " stable_footprint_promoted=%d"
            " stable_radius_cells=%" PRId32
            " extended_matrix_arena=%d capped=%d\n",
            count,
            static_cast<uint32_t>(packed_size >> 32),
            static_cast<uint32_t>(packed_size),
            scale,
            kTerrainGuardCells,
            first_row,
            last_row,
            rows_expanded,
            cells_added,
            original_cells,
            desired_cells,
            candidate_cells,
            visible_cells,
            candidate_capacity,
            matrix_capacity,
            terrain_promotion_matrix_ceiling,
            kPostObjectMatrixQueueCapacity,
            matrix_slots_available,
            allocation_policy,
            initial_promotion_matrix_slots,
            depth_matrix_slots_reserved,
            lateral_matrix_slots_used,
            depth_matrix_slots_used,
            recycled_lateral_matrix_slots_used,
            depth_rows_added,
            negative_depth_rows_added,
            positive_depth_rows_added,
            stable_footprint_promoted ? 0 : 1,
            kTerrainDepthDirectionHysteresis,
            kTerrainOppositeDepthGuardRows,
            stable_footprint_promoted ? 1 : 0,
            stable_radius_cells,
            extended_matrix_arena ? 1 : 0,
            expansion_capped ? 1 : 0
        );
        std::fflush(stderr);
    }

    begin_static_terrain_matrix_scope(rdram);
}

extern "C" void bumble_relocate_terrain_visibility_build_scratch(
    uint8_t* rdram,
    recomp_context* context
) {
    TerrainScratchState& scratch = g_terrain_scratch;
    if (rdram == nullptr || context == nullptr || !scratch.active ||
        scratch.rdram != rdram) {
        return;
    }
    context->r10 = guest_address(scratch.guest_base);
    scratch.producer_relocated = true;
}

extern "C" void bumble_begin_actor_matrix_group(
    uint8_t* rdram,
    recomp_context* context
) {
    close_actor_matrix_scope(rdram);
    if (rdram == nullptr || context == nullptr) {
        return;
    }

    const uint32_t actor = static_cast<uint32_t>(context->r17);
    if (actor == 0u || actor == UINT32_MAX ||
        !recomp_rdram_address(actor, 0x8Cu, 4u) ||
        !current_display_list_budget_available(rdram, 24u)) {
        return;
    }

    constexpr uint32_t Push = 1u << 0u;
    constexpr uint32_t Decompose = 1u << 2u;
    constexpr uint32_t AutoPosition = 2u << 3u;
    constexpr uint32_t AutoRotation = 2u << 5u;
    constexpr uint32_t AutoScale = 2u << 7u;
    constexpr uint32_t AutoSkew = 2u << 9u;
    constexpr uint32_t AutoPerspective = 2u << 11u;
    constexpr uint32_t AutoVertex = 2u << 13u;
    constexpr uint32_t AutoTile = 2u << 15u;
    constexpr uint32_t AutoLookAt = 2u << 24u;
    constexpr uint32_t AutoTexcoord = 2u << 22u;
    constexpr uint32_t ActorInterpolation =
        Push | Decompose | AutoPosition | AutoRotation | AutoScale |
        AutoSkew | AutoPerspective | AutoVertex | AutoTile |
        AutoLookAt | AutoTexcoord;

    g_actor_matrix_scope_open = append_commands(rdram, {
        extended_command(kMatrixGroupV1), actor_history_id(rdram, actor),
        ActorInterpolation, 0u,
    });
    if (g_actor_matrix_scope_open) g_actor_matrix_owner = actor;
}

extern "C" void bumble_register_actor_history(uint8_t* rdram, recomp_context* context) {
    const uint32_t actor = static_cast<uint32_t>(context->r5);
    const uint16_t serial = MEM_HU(0x7E, static_cast<int32_t>(actor));
    std::lock_guard lock(g_actor_history_mutex);
    g_actor_history[serial & 0x1FFu] = {actor, serial, allocate_actor_id()};
}

extern "C" void bumble_unregister_actor_history(uint8_t* rdram, recomp_context* context) {
    const uint32_t actor = static_cast<uint32_t>(context->r5);
    bumble::electric_effect::release_actor(actor);
    const uint16_t serial = MEM_HU(0x7E, static_cast<int32_t>(actor));
    std::lock_guard lock(g_actor_history_mutex);
    auto& history = g_actor_history[serial & 0x1FFu];
    if (history.address == actor && history.serial == serial) history = {};
}

extern "C" void bumble_set_model_role(uint32_t role) {
    g_model_role = role;
}

extern "C" void bumble_end_model_part(uint8_t* rdram) noexcept(false) {
    if (g_model_part_scope_open) {
        if (!append_commands(rdram, {extended_command(kPopMatrixGroupV1), 1u})) {
            throw std::runtime_error("Model part matrix scope overflow");
        }
        g_model_part_scope_open = false;
    }
}

extern "C" void bumble_begin_model_part(uint8_t* rdram, recomp_context* context) noexcept(false) {
    if (!g_actor_matrix_scope_open) return;
    if (g_model_part_scope_open || !current_display_list_budget_available(rdram, 48u)) {
        throw std::runtime_error("Invalid model part matrix scope");
    }
    const uint32_t model = static_cast<uint32_t>(context->r19);
    const uint32_t child = static_cast<uint32_t>(context->r17);
    const uint32_t displayLists = read_u32(rdram, model);
    const std::array<uint32_t, 4> key{g_model_role, model, child,
        read_u32(rdram, displayLists + child * 4u)};
    const uint16_t serial = MEM_HU(0x7E, static_cast<int32_t>(g_actor_matrix_owner));
    uint32_t id = 0;
    {
        std::lock_guard lock(g_actor_history_mutex);
        auto& history = g_actor_history[serial & 0x1FFu];
        if (history.address != g_actor_matrix_owner || history.serial != serial) {
            throw std::runtime_error("Model part outlived actor registration");
        }
        auto part = std::find_if(history.parts.begin(), history.parts.end(),
            [&](const auto& candidate) { return candidate.key == key; });
        if (part == history.parts.end()) {
            history.parts.push_back({key, allocate_actor_id()});
            id = history.parts.back().id;
        }
        else id = part->id;
    }
    constexpr uint32_t PartInterpolation = 1u | (1u << 2u) |
        (2u << 3u) | (2u << 5u) | (2u << 7u) | (2u << 9u) |
        (2u << 11u) | (2u << 13u) | (2u << 15u) |
        (2u << 22u) | (2u << 24u);
    g_model_part_scope_open = append_commands(rdram, {
        extended_command(kMatrixGroupV1), id, PartInterpolation, 0u});
}

extern "C" void bumble_end_actor_matrix_group(uint8_t* rdram) {
    close_actor_matrix_scope(rdram);
}

extern "C" void bumble_relocate_terrain_visibility_consume_scratch(
    uint8_t* rdram,
    recomp_context* context
) {
    TerrainScratchState& scratch = g_terrain_scratch;
    if (rdram == nullptr || context == nullptr || !scratch.active ||
        scratch.rdram != rdram || !scratch.producer_relocated) {
        return;
    }

    const uint32_t expected_end = scratch.guest_base +
        scratch.expected_entries * kTerrainCandidateBytes;
    const bool producer_valid =
        scratch.expected_entries <= kTerrainScratchCapacity &&
        low_guest_address(context->r9) == expected_end &&
        terrain_scratch_guards_intact(scratch);
    context->r10 = guest_address(scratch.guest_base);
    if (!producer_valid) {
        context->r9 = guest_address(scratch.guest_base);
        g_terrain_scratch_failures.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    scratch.consumer_relocated = true;
}

extern "C" void bumble_finish_terrain_visibility_scratch(
    uint8_t* rdram,
    recomp_context* context
) {
    end_static_terrain_matrix_scope(rdram);

    TerrainScratchState& scratch = g_terrain_scratch;
    if (rdram == nullptr || context == nullptr || !scratch.active ||
        scratch.rdram != rdram || !scratch.producer_relocated) {
        return;
    }
    const uint32_t expected_end = scratch.guest_base +
        scratch.expected_entries * kTerrainCandidateBytes;
    const bool verified = scratch.consumer_relocated &&
        low_guest_address(context->r9) == expected_end &&
        low_guest_address(context->r10) == expected_end &&
        terrain_scratch_guards_intact(scratch);
    if (verified) {
        const uint64_t calls = g_terrain_scratch_verified_calls.fetch_add(
            1,
            std::memory_order_acq_rel
        ) + 1;
        if (calls == 1u) {
            std::fprintf(
                stderr,
                "BUMBLE_WIDESCREEN stage=terrain_scratch_verified"
                " guest_base=0x%08" PRIX32
                " entries=%" PRIu32 " capacity=%" PRIu32
                " payload_bytes=%" PRIu32 " guard_bytes=%" PRIu32
                " producer_pc=0x800859BC consumer_pc=0x80085C70"
                " finish_pc=0x80085D94 guards_intact=1\n",
                scratch.guest_base,
                scratch.expected_entries,
                kTerrainScratchCapacity,
                kTerrainScratchBytes,
                kTerrainScratchGuardBytes
            );
            std::fflush(stderr);
        }
    }
    else {
        g_terrain_scratch_failures.fetch_add(1, std::memory_order_acq_rel);
    }
    scratch.active = false;
}

extern "C" void bumble_ui_begin_fullscreen_panel(
    uint8_t* rdram,
    recomp_context* context
) {
    g_native_menu_panel_suppression = {};
    if (rdram != nullptr && context != nullptr) {
        const uint32_t descriptor = low_guest_address(context->r23);
        uint32_t saved_cursor = 0u;
        if (read_guest_word(rdram, kDisplayListCursor, saved_cursor) &&
            publish_native_menu_replacement(rdram, descriptor)) {
            g_native_menu_panel_suppression.saved_cursor = saved_cursor;
            g_native_menu_panel_suppression.active = true;
            return;
        }
    }
    if (!physical_edge_ui_enabled(rdram) ||
        g_fullscreen_panel_scope_open ||
        g_ui_scope_depth >= g_ui_scopes.size()) {
        return;
    }

    g_fullscreen_panel_scope_open = begin_pixel_anchored_rect(
        rdram,
        "func_800AC204.fullscreen_panel",
        0,
        static_cast<int32_t>(kScreenWidth)
    );
}

extern "C" void bumble_ui_end_fullscreen_panel(
    uint8_t* rdram,
    recomp_context* context
) {
    if (g_native_menu_panel_suppression.active) {
        if (rdram != nullptr) {
            MEM_W(0, guest_address(kDisplayListCursor)) =
                g_native_menu_panel_suppression.saved_cursor;
        }
        g_native_menu_panel_suppression = {};
    } else if (g_fullscreen_panel_scope_open) {
        end_ui_scope(rdram, "func_800AC204.fullscreen_panel");
        g_fullscreen_panel_scope_open = false;
    }

    g_native_menu_prefix_suppression = {};
    if (rdram != nullptr && context != nullptr) {
        const uint32_t descriptor = low_guest_address(context->r23);
        uint32_t saved_cursor = 0u;
        if (read_guest_word(rdram, kDisplayListCursor, saved_cursor) &&
            publish_native_menu_replacement(rdram, descriptor)) {
            g_native_menu_prefix_suppression.saved_cursor = saved_cursor;
            g_native_menu_prefix_suppression.active = true;
        }
    }
}

extern "C" void bumble_ui_begin_texrect(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!physical_edge_ui_enabled(rdram) || context == nullptr) {
        return;
    }
    const int32_t x = static_cast<int32_t>(MEM_W(0x10, context->r29));
    begin_pixel_anchored_rect(
        rdram,
        "func_800AE344",
        x,
        0,
        single_player_ui_enabled(rdram)
    );
}

extern "C" void bumble_ui_end_texrect(
    uint8_t* rdram,
    recomp_context*
) {
    if (physical_edge_ui_enabled(rdram)) {
        end_ui_scope(rdram, "func_800AE344");
    }
}

extern "C" void bumble_ui_setup_2d_scissor(
    uint8_t* rdram,
    recomp_context*
) {
    if (physical_edge_ui_enabled(rdram)) {
        append_full_width_scissor(rdram);
    }
}

extern "C" void bumble_ui_observe_briefing_queue_text(
    uint8_t* rdram,
    recomp_context* context
) {
    if (context == nullptr) {
        return;
    }
    log_briefing_queue_text(
        rdram,
        low_guest_address(context->r4),
        low_guest_address(context->r2)
    );
}

extern "C" void bumble_ui_begin_script_panel(
    uint8_t* rdram,
    recomp_context*
) {
    if (!physical_edge_ui_enabled(rdram) ||
        g_script_panel_scope_open ||
        g_ui_scope_depth >= g_ui_scopes.size()) {
        return;
    }

    uint32_t script_object = 0u;
    uint32_t descriptor = 0u;
    uint32_t panel_left_word = 0u;
    uint32_t panel_right_word = 0u;
    if (!read_guest_word(rdram, kScriptUiObject, script_object) ||
        !read_guest_word(rdram, script_object, descriptor) ||
        !read_guest_word(rdram, descriptor, panel_left_word) ||
        !read_guest_word(rdram, descriptor + 8u, panel_right_word) ||
        (panel_left_word & 0xFF000000u) == 0xAA000000u ||
        (panel_right_word & 0xFF000000u) == 0xAA000000u) {
        return;
    }
    const int32_t authored_left =
        static_cast<int32_t>(panel_left_word & 0x3FFu);
    const int32_t authored_right =
        static_cast<int32_t>(panel_right_word & 0x3FFu);
    const int32_t authored_width = authored_right - authored_left;
    if (authored_width <= 0) {
        return;
    }
    g_script_panel_scope_open = begin_ui_scope(
        rdram,
        kScriptPanelScopeOwner,
        {UiOrigin::Left, UiOrigin::Right, UiOrigin::None},
        authored_left
    );
}

extern "C" void bumble_ui_end_script_panel(
    uint8_t* rdram,
    recomp_context*
) {
    if (!g_script_panel_scope_open) {
        return;
    }
    end_ui_scope(rdram, kScriptPanelScopeOwner);
    g_script_panel_scope_open = false;
}

extern "C" void bumble_ui_begin_script_text(
    uint8_t* rdram,
    recomp_context* context
) {
    if (context == nullptr || g_script_text_scope_open ||
        g_script_native_text_active) {
        return;
    }
    const uint32_t text_pointer = low_guest_address(context->r4);
    const uint32_t localization_entry = low_guest_address(context->r3);
    const uint32_t localization_index = localization_index_for_entry(
        rdram,
        localization_entry
    );
    const int32_t x = static_cast<uint16_t>(context->r5);
    const uint32_t y = static_cast<uint16_t>(context->r6);
    const UiAlignment alignment = classify_pixel_x(x);

    std::array<char, kNativeTextMaximumBytes + 1u> preview{};
    size_t preview_length = 0u;
    if (copy_guest_text_preview(
            rdram,
            text_pointer,
            preview.data(),
            preview.size(),
            preview_length
        )) {
        const uint64_t stable_slot =
            0x5343524950540000ull |
            static_cast<uint64_t>(text_pointer);
        g_script_native_text_active = bumble::text_overlay::observe(
            bumble::text_overlay::TextKind::Script,
            static_cast<uint32_t>(std::max<int32_t>(x, 0)),
            y,
            preview.data(),
            bumble::text_overlay::HorizontalAnchor::Panel,
            bumble::text_overlay::VerticalAnchor::Authored,
            1.0f,
            0xFFFFFFFFu,
            0x000000FFu,
            stable_slot
        );
        const bool mission_title =
            localization_index == kMissionTitleLocalizationIndex &&
            std::strncmp(
                preview.data(),
                kMissionTitlePrefix,
                sizeof(kMissionTitlePrefix) - 1u
            ) == 0;
        const uint64_t signature = briefing_text_sample_signature(
            kMissionTitleCallPc,
            localization_index,
            text_pointer,
            preview.data(),
            preview_length
        );
        if (remember_briefing_text_sample(signature)) {
            std::fprintf(
                stderr,
                "BUMBLE_WIDESCREEN stage=script_text_group"
                " owner=func_800926AC call_pc=0x%08" PRIX32
                " renderer=func_800AA3B8 localization_index=0x%08" PRIX32
                " text_pointer=0x%08" PRIX32
                " x=%" PRId32 " y=%" PRIu32 " origin=%s"
                " content=%s preview=\"%s\"\n",
                kMissionTitleCallPc,
                localization_index,
                text_pointer,
                x,
                y,
                alignment_name(alignment),
                mission_title ? "mission_title" : "unclassified",
                preview.data()
            );
            std::fflush(stderr);
        }
    }

    if (!physical_edge_ui_enabled(rdram) ||
        g_ui_scope_depth >= g_ui_scopes.size()) {
        return;
    }
    g_script_text_scope_open = begin_ui_scope(
        rdram,
        kScriptTextScopeOwner,
        alignment,
        x
    );
}

extern "C" void bumble_ui_end_script_text(
    uint8_t* rdram,
    recomp_context*
) {
    g_script_native_text_active = false;
    if (!g_script_text_scope_open) {
        return;
    }
    end_ui_scope(rdram, kScriptTextScopeOwner);
    g_script_text_scope_open = false;
}

extern "C" void bumble_reset_briefing_text(uint8_t*, recomp_context*) {
    g_text_delay_remainder = 0;
    std::scoped_lock lock(g_briefing_queue_mutex);
    g_briefing_queue.clear();
    g_briefing_native_text.clear();
    g_briefing_queue_valid = true;
    g_briefing_native_aggregate_active = false;
}

extern "C" void bumble_queue_briefing_text(
    uint8_t* rdram, recomp_context* context
) {
    if (rdram == nullptr || context == nullptr) {
        return;
    }
    std::scoped_lock lock(g_briefing_queue_mutex);
    if (read_u32(rdram, 0x8010761Cu) == 0u &&
        read_u32(rdram, 0x80107A20u) == 0u) {
        g_briefing_queue.clear();
        g_briefing_native_text.clear();
        g_briefing_queue_valid = true;
    }
    if (!g_briefing_queue_valid) {
        return;
    }
    BriefingQueueEntry entry;
    entry.slot = (static_cast<uint32_t>(context->r4) - 1u) & 255u;
    entry.pointer = low_guest_address(context->r6);
    constexpr uint32_t maximum_bytes = 16384u;
    bool terminated = false;
    for (uint32_t offset = 0u; offset < maximum_bytes; ++offset) {
        if (!recomp_rdram_address(entry.pointer, offset + 1u, 1u)) {
            break;
        }
        const uint8_t byte = static_cast<uint8_t>(MEM_BU(
            static_cast<int32_t>(offset), guest_address(entry.pointer)));
        if (byte == 0u) {
            terminated = true;
            break;
        }
        if (byte == 7u) {
            // Skip the three-byte delay; the guest executes it.
            if (!recomp_rdram_address(entry.pointer, offset + 4u, 1u)) {
                break;
            }
            offset += 3u;
            continue;
        }
        if (byte == 8u) {
            continue; // Script side effect stays with func_800A9CE0.
        }
        if (byte == '\n' || (byte >= 0x20u && byte <= 0x7Eu)) {
            entry.text.push_back(static_cast<char>(byte));
            entry.source_offsets.push_back(offset);
        }
    }
    if (!terminated) {
        // Never suppress guest text when the complete string is unavailable.
        g_briefing_queue_valid = false;
        return;
    }
    const auto reused = std::find_if(g_briefing_queue.begin(),
        g_briefing_queue.end(), [&](const BriefingQueueEntry& old) {
            return old.slot == entry.slot;
        });
    if (reused != g_briefing_queue.end()) {
        g_briefing_queue.erase(g_briefing_queue.begin(), std::next(reused));
    }
    g_briefing_queue.push_back(std::move(entry));
    g_briefing_native_text.clear();
    for (BriefingQueueEntry& queued : g_briefing_queue) {
        if (!g_briefing_native_text.empty() && !queued.text.empty() &&
            !std::isspace(static_cast<unsigned char>(g_briefing_native_text.back())) &&
            !std::isspace(static_cast<unsigned char>(queued.text.front()))) {
            g_briefing_native_text.push_back(' ');
        }
        queued.text_offset = static_cast<uint32_t>(g_briefing_native_text.size());
        g_briefing_native_text += queued.text;
    }
    g_briefing_queue_valid = g_briefing_native_text.size() <= maximum_bytes;
}

extern "C" void bumble_ui_begin_briefing_panel(
    uint8_t* rdram,
    recomp_context*
) {
    const bool wide_ui = physical_edge_ui_enabled(rdram);
    g_briefing_native_aggregate_active = false;
    if (wide_ui) {
        std::scoped_lock lock(g_briefing_queue_mutex);
        if (g_briefing_queue_valid && !g_briefing_native_text.empty()) {
            const uint32_t cursor = read_u32(rdram, 0x80107A24u);
            const uint32_t slot = read_u32(rdram, 0x80107A28u) & 255u;
            const auto entry = std::find_if(g_briefing_queue.begin(),
                g_briefing_queue.end(), [&](const BriefingQueueEntry& item) {
                    return item.slot == slot;
                });
            if (entry != g_briefing_queue.end() && cursor >= entry->pointer) {
                uint32_t visible = entry->text_offset + static_cast<uint32_t>(
                    std::upper_bound(entry->source_offsets.begin(),
                        entry->source_offsets.end(), cursor - entry->pointer) -
                    entry->source_offsets.begin());
                if (entry == g_briefing_queue.begin() && cursor == entry->pointer &&
                    read_u32(rdram, 0x80107A2Cu) == 0u) {
                    visible = 0u;
                }
                const int32_t left = read_s32(rdram, kBriefingPanelLeft);
                const int32_t top = read_s32(rdram, 0x801075F8u);
                const int32_t width = read_s32(rdram, kBriefingPanelRight) - left;
                const int32_t height = read_s32(rdram, 0x80107600u) - top;
                if (left >= 0 && top >= 0 && width > 8 && height > 8) {
                    g_briefing_native_aggregate_active =
                        bumble::text_overlay::observe_briefing(left + 4u, top + 4u,
                            width - 8u, height - 8u, g_briefing_native_text, visible);
                    g_briefing_handoff_ready |= g_briefing_native_aggregate_active;
                }
            }
        }
    }
    if (!wide_ui ||
        g_briefing_panel_scope_open ||
        g_ui_scope_depth >= g_ui_scopes.size()) {
        return;
    }

    const int32_t panel_left = read_s32(rdram, kBriefingPanelLeft);
    const int32_t panel_right = read_s32(rdram, kBriefingPanelRight);
    const int32_t panel_width = panel_right - panel_left;
    if (panel_width <= 0) {
        return;
    }
    g_briefing_panel_scope_open = begin_ui_scope(
        rdram,
        kBriefingPanelScopeOwner,
        {UiOrigin::Left, UiOrigin::Right, UiOrigin::None},
        panel_left
    );
}

extern "C" void bumble_ui_end_briefing_panel(
    uint8_t* rdram,
    recomp_context*
) {
    g_briefing_native_aggregate_active = false;
    if (!g_briefing_panel_scope_open) {
        return;
    }
    end_ui_scope(rdram, kBriefingPanelScopeOwner);
    g_briefing_panel_scope_open = false;
}

extern "C" void bumble_expand_briefing_wrap_width(
    uint8_t* rdram,
    recomp_context* context
) {
    (void)context;
    if (!physical_edge_ui_enabled(rdram)) {
        return;
    }

    const int32_t authored_width =
        read_s32(rdram, kBriefingPanelRight) -
        read_s32(rdram, kBriefingPanelLeft);
    const double expansion = bumble::widescreen::horizontal_expansion_scale();
    if (authored_width <= 0 || !std::isfinite(expansion) ||
        expansion <= 1.000001) {
        return;
    }
    const int64_t added_width = static_cast<int64_t>(std::llround(
        (expansion - 1.0) * static_cast<double>(kScreenWidth)
    ));
    const int32_t expanded_width = static_cast<int32_t>(std::clamp<int64_t>(
        static_cast<int64_t>(authored_width) + added_width,
        authored_width,
        512
    ));
    if (expanded_width == authored_width) {
        return;
    }
    write_s32(rdram, 0x80107A44u, expanded_width);

    const uint64_t signature =
        (static_cast<uint64_t>(static_cast<uint32_t>(authored_width)) << 32u) |
        static_cast<uint32_t>(expanded_width);
    uint64_t previous = g_last_briefing_wrap_signature.load(
        std::memory_order_acquire
    );
    if (previous != signature &&
        g_last_briefing_wrap_signature.compare_exchange_strong(
            previous,
            signature,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        )) {
        std::fprintf(
            stderr,
            "BUMBLE_WIDESCREEN stage=briefing_wrap_expanded"
            " authored_width=%" PRId32 " expanded_width=%" PRId32
            " horizontal_scale=%.6f full_panel_width=1\n",
            authored_width,
            expanded_width,
            expansion
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_ui_begin_briefing_line(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t call_pc
) {
    if (context == nullptr || g_briefing_line_scope_open ||
        g_briefing_native_line_active) {
        return;
    }
    if (g_briefing_native_aggregate_active) {
        g_briefing_native_line_active = true;
        return;
    }
    const uint32_t text_pointer = low_guest_address(context->r4);
    const uint32_t submitted_x = static_cast<uint16_t>(context->r5);
    const uint32_t submitted_y = static_cast<uint16_t>(context->r6);
    const bool shadow = call_pc == kBriefingShadowCallPc;
    const uint32_t authored_x = shadow && submitted_x != 0u
        ? submitted_x - 1u
        : submitted_x;
    const uint32_t authored_y = shadow && submitted_y != 0u
        ? submitted_y - 1u
        : submitted_y;
    const UiAlignment alignment = classify_pixel_x(
        static_cast<int32_t>(authored_x)
    );
    std::array<char, kNativeTextMaximumBytes + 1u> native_text{};
    size_t native_text_length = 0u;
    if (copy_guest_text_preview(
            rdram,
            text_pointer,
            native_text.data(),
            native_text.size(),
            native_text_length
        )) {
        g_briefing_native_line_active =
            g_briefing_native_aggregate_active
                ? bumble::text_overlay::renderer_ready()
                : bumble::text_overlay::observe(
                    bumble::text_overlay::TextKind::Briefing,
                    authored_x,
                    authored_y,
                    native_text.data(),
                    bumble::text_overlay::HorizontalAnchor::Panel,
                    bumble::text_overlay::VerticalAnchor::Authored,
                    1.0f,
                    0xFFFFFFFFu,
                    0x000000FFu
                );
    }
    log_briefing_render_pass(
        rdram,
        call_pc,
        text_pointer,
        submitted_x,
        submitted_y,
        authored_x,
        authored_y,
        alignment
    );

    if (!physical_edge_ui_enabled(rdram) ||
        g_ui_scope_depth >= g_ui_scopes.size()) {
        return;
    }
    g_briefing_line_scope_open = begin_ui_scope(
        rdram,
        kBriefingLineScopeOwner,
        alignment,
        static_cast<int32_t>(authored_x)
    );
}

extern "C" void bumble_ui_end_briefing_line(
    uint8_t* rdram,
    recomp_context*
) {
    g_briefing_native_line_active = false;
    if (!g_briefing_line_scope_open) {
        return;
    }
    end_ui_scope(rdram, kBriefingLineScopeOwner);
    g_briefing_line_scope_open = false;
}

extern "C" void bumble_ui_begin_text(
    uint8_t* rdram,
    recomp_context* context
) {
    begin_main_menu_text_pulse_override(rdram, context);
    if (!physical_edge_ui_enabled(rdram) || context == nullptr) {
        return;
    }
    int32_t pixel_x = 0;
    uint32_t flags = 0u;
    const UiAlignment alignment = classify_text_descriptor(
        rdram,
        static_cast<uint32_t>(context->r5),
        pixel_x,
        flags
    );
    (void)flags;
    const bool compact_frontend_menu =
        compact_frontend_menu_enabled(rdram);
    begin_ui_scope(
        rdram,
        "func_800AE9AC",
        alignment,
        pixel_x,
        single_player_ui_enabled(rdram) ||
            compact_frontend_menu,
        compact_frontend_menu
            ? compact_frontend_menu_vertical_anchor(rdram)
            : UiVerticalAnchor::Automatic
    );
}

extern "C" void bumble_ui_reduce_main_menu_pulse(
    uint8_t*,
    recomp_context* context
) {
    if (context == nullptr ||
        !current_main_menu_text_pulse_override_active() ||
        !std::isfinite(context->f4.fl)) {
        return;
    }

    context->f4.fl = 0.0f;
}

extern "C" void bumble_ui_end_text(
    uint8_t* rdram,
    recomp_context*
) {
    if (physical_edge_ui_enabled(rdram)) {
        end_ui_scope(rdram, "func_800AE9AC");
    }
    end_main_menu_text_pulse_override(rdram);
}

extern "C" void bumble_ui_begin_sprite_rect(
    uint8_t* rdram,
    recomp_context* context
) {
    if (context == nullptr) {
        return;
    }

    if (g_sprite_rect_overflow_depth != 0u ||
        g_sprite_rect_depth >= g_sprite_rect_scopes.size()) {
        ++g_sprite_rect_overflow_depth;
        return;
    }

    const int32_t x = static_cast<int16_t>(context->r4);
    const int32_t width = static_cast<int16_t>(context->r6);
    const UiScope* outer = g_ui_scope_depth == 0u
        ? nullptr
        : &g_ui_scopes[g_ui_scope_depth - 1u];
    const bool complete_text_group = outer != nullptr &&
        outer->owner != nullptr &&
        (std::strcmp(outer->owner, kScriptTextScopeOwner) == 0 ||
            std::strcmp(outer->owner, kBriefingLineScopeOwner) == 0 ||
            std::strcmp(outer->owner, kDescriptorTextScopeOwner) == 0 ||
            std::strcmp(outer->owner, kTextLineScopeOwner) == 0 ||
            std::strcmp(outer->owner, kDirectTextScopeOwner) == 0 ||
            std::strcmp(outer->owner, kGameplayRightHudScopeOwner) == 0 ||
            std::strcmp(outer->owner, kGameplayKeyScopeOwner) == 0 ||
            std::strcmp(outer->owner, kGameplayWeaponAmmoScopeOwner) == 0);
    const bool discard_native_text_commands =
        g_script_native_text_active || g_briefing_native_line_active;
    if (!discard_native_text_commands && !physical_edge_ui_enabled(rdram)) {
        return;
    }
    bool scope_opened = false;
    uint32_t saved_cursor = 0u;
    bool discard_commands = false;
    if (discard_native_text_commands &&
        read_guest_word(rdram, kDisplayListCursor, saved_cursor)) {
        discard_commands = true;
    }
    else if (complete_text_group) {
    }
    else {
        scope_opened = begin_pixel_anchored_rect(
            rdram,
            "func_800A6824",
            x,
            width,
            single_player_ui_enabled(rdram)
        );
    }
    g_sprite_rect_scopes[g_sprite_rect_depth] = scope_opened;
    g_sprite_rect_discard_commands[g_sprite_rect_depth] = discard_commands;
    g_sprite_rect_saved_cursors[g_sprite_rect_depth] = saved_cursor;
    ++g_sprite_rect_depth;
}

extern "C" void bumble_ui_end_sprite_rect(
    uint8_t* rdram,
    recomp_context*
) {
    if (g_sprite_rect_overflow_depth != 0u) {
        --g_sprite_rect_overflow_depth;
        return;
    }
    if (g_sprite_rect_depth == 0u) {
        return;
    }

    --g_sprite_rect_depth;
    const bool scope_opened =
        g_sprite_rect_scopes[g_sprite_rect_depth];
    const bool discard_commands =
        g_sprite_rect_discard_commands[g_sprite_rect_depth];
    if (discard_commands) {
        MEM_W(0, guest_address(kDisplayListCursor)) =
            g_sprite_rect_saved_cursors[g_sprite_rect_depth];
    }
    if (scope_opened) {
        end_ui_scope(rdram, "func_800A6824");
    }
}

extern "C" void bumble_ui_begin_hud_rect_batch(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!physical_edge_ui_enabled(rdram) || context == nullptr) {
        return;
    }

    uint32_t saved_cursor = 0u;
    std::array<char, 32> bonus{};
    const int32_t bonus_value = std::max(
        read_s32(rdram, kGameplayBonusValue),
        0
    );
    std::snprintf(
        bonus.data(),
        bonus.size(),
        "BONUS: %" PRId32,
        bonus_value
    );
    if (single_player_ui_enabled(rdram) &&
        read_guest_word(rdram, kDisplayListCursor, saved_cursor) &&
        bumble::text_overlay::observe(
            bumble::text_overlay::TextKind::GameplayHud,
            kGameplayLeftStackX,
            kGameplayBonusDestinationY,
            bonus.data(),
            bumble::text_overlay::HorizontalAnchor::Left,
            bumble::text_overlay::VerticalAnchor::Top,
            kGameplayUiScale,
            pack_text_rgba(255u, 144u, 0u)
        )) {
        g_gameplay_bonus_suppression.saved_cursor = saved_cursor;
        g_gameplay_bonus_suppression.active = true;
        return;
    }

    g_gameplay_bonus_scope_open = begin_pixel_anchored_rect(
        rdram,
        kGameplayBonusScopeOwner,
        static_cast<int16_t>(context->r4),
        0,
        single_player_ui_enabled(rdram)
    );
}

extern "C" void bumble_ui_end_hud_rect_batch(
    uint8_t* rdram,
    recomp_context*
) {
    if (g_gameplay_bonus_suppression.active) {
        if (rdram != nullptr) {
            MEM_W(0, guest_address(kDisplayListCursor)) =
                g_gameplay_bonus_suppression.saved_cursor;
        }
        g_gameplay_bonus_suppression = {};
        return;
    }
    if (g_gameplay_bonus_scope_open) {
        end_ui_scope(rdram, kGameplayBonusScopeOwner);
        g_gameplay_bonus_scope_open = false;
    }
}

extern "C" void bumble_ui_begin_gameplay_hud_root(
    uint8_t* rdram,
    recomp_context*
) {
    append_commands(rdram, {extended_command(kHudBeginV1), 0u});

    if (!single_player_ui_enabled(rdram) ||
        g_gameplay_hud_root_scope_open ||
        g_ui_scope_depth >= g_ui_scopes.size()) {
        return;
    }

    g_gameplay_hud_root_scope_open = begin_ui_scope(
        rdram,
        kGameplayHudRootScopeOwner,
        single_origin_alignment(UiOrigin::None),
        static_cast<int32_t>(kScreenWidth / 2u),
        true,
        UiVerticalAnchor::Top
    );
}

extern "C" void bumble_ui_end_gameplay_hud_root(
    uint8_t* rdram,
    recomp_context*
) {
    append_commands(rdram, {extended_command(kHudBeginV1), 1u});
    if (!g_gameplay_hud_root_scope_open) {
        return;
    }
    end_ui_scope(rdram, kGameplayHudRootScopeOwner);
    g_gameplay_hud_root_scope_open = false;
}

extern "C" void bumble_ui_begin_gameplay_radar(
    uint8_t* rdram,
    recomp_context*
) {
    if (!g_gameplay_hud_root_scope_open ||
        g_gameplay_radar_scope_open ||
        g_ui_scope_depth >= g_ui_scopes.size()) {
        return;
    }

    g_gameplay_radar_scope_open = begin_ui_scope(
        rdram,
        kGameplayRadarScopeOwner,
        single_origin_alignment(UiOrigin::Left),
        kRadarAnchorX,
        true,
        UiVerticalAnchor::Bottom,
        kOriginRadarBottomLeft
    );
}

extern "C" void bumble_ui_end_gameplay_radar(
    uint8_t* rdram,
    recomp_context*
) {
    if (!g_gameplay_radar_scope_open) {
        return;
    }
    end_ui_scope(rdram, kGameplayRadarScopeOwner);
    g_gameplay_radar_scope_open = false;
}

extern "C" void bumble_ui_begin_gameplay_weapon_model(
    uint8_t* rdram,
    recomp_context* context
) {
    record_weapon_carousel_context(
        rdram,
        (context != nullptr) ? static_cast<uint32_t>(context->r4) : UINT32_MAX
    );
    if (!g_gameplay_hud_root_scope_open ||
        g_gameplay_weapon_model_scope_open ||
        g_ui_scope_depth >= g_ui_scopes.size()) {
        return;
    }

    const uint32_t player_index = context != nullptr
        ? static_cast<uint32_t>(context->r4)
        : UINT32_MAX;
    (void)publish_gameplay_weapon_list(rdram, player_index);

    g_gameplay_weapon_model_scope_open = begin_ui_scope(
        rdram,
        kGameplayWeaponModelScopeOwner,
        single_origin_alignment(UiOrigin::Right),
        kWeaponModelAnchorX,
        true,
        UiVerticalAnchor::Bottom,
        kOriginWeaponBottomRight
    );
}

extern "C" void bumble_ui_end_gameplay_weapon_model(
    uint8_t* rdram,
    recomp_context*
) {
    if (g_gameplay_weapon_neighbor_suppression.active) {
        if (rdram != nullptr) {
            MEM_W(0, guest_address(kDisplayListCursor)) =
                g_gameplay_weapon_neighbor_suppression.saved_cursor;
        }
        g_gameplay_weapon_neighbor_suppression = {};
    }
    if (!g_gameplay_weapon_model_scope_open) {
        return;
    }
    end_ui_scope(rdram, kGameplayWeaponModelScopeOwner);
    g_gameplay_weapon_model_scope_open = false;
}

extern "C" void bumble_ui_begin_gameplay_weapon_neighbor(
    uint8_t* rdram,
    recomp_context*
) {
    if (!g_gameplay_weapon_model_scope_open ||
        !single_player_ui_enabled(rdram) ||
        g_gameplay_weapon_neighbor_suppression.active) {
        return;
    }

    uint32_t saved_cursor = 0u;
    if (!read_guest_word(rdram, kDisplayListCursor, saved_cursor)) {
        return;
    }
    g_gameplay_weapon_neighbor_suppression.saved_cursor = saved_cursor;
    g_gameplay_weapon_neighbor_suppression.active = true;
}

extern "C" void bumble_ui_end_gameplay_weapon_neighbor(
    uint8_t* rdram,
    recomp_context*
) {
    if (!g_gameplay_weapon_neighbor_suppression.active) {
        return;
    }
    if (rdram != nullptr) {
        MEM_W(0, guest_address(kDisplayListCursor)) =
            g_gameplay_weapon_neighbor_suppression.saved_cursor;
    }
    g_gameplay_weapon_neighbor_suppression = {};
}

extern "C" void bumble_ui_begin_gameplay_key_group(
    uint8_t* rdram,
    recomp_context*
) {
    if (!single_player_ui_enabled(rdram) ||
        g_gameplay_key_scope_open ||
        g_ui_scope_depth >= g_ui_scopes.size()) {
        return;
    }

    g_gameplay_key_scope_open = begin_ui_scope(
        rdram,
        kGameplayKeyScopeOwner,
        single_origin_alignment(UiOrigin::Right),
        210,
        true,
        UiVerticalAnchor::Top
    );
}

extern "C" void bumble_ui_end_gameplay_key_group(
    uint8_t* rdram,
    recomp_context*
) {
    if (!g_gameplay_key_scope_open) {
        return;
    }
    end_ui_scope(rdram, kGameplayKeyScopeOwner);
    g_gameplay_key_scope_open = false;
}

extern "C" void bumble_ui_begin_gameplay_right_hud(
    uint8_t* rdram,
    recomp_context*
) {
    if (!physical_edge_ui_enabled(rdram) ||
        g_gameplay_right_hud_scope_open ||
        g_ui_scope_depth >= g_ui_scopes.size()) {
        return;
    }

    g_gameplay_right_hud_scope_open = begin_ui_scope(
        rdram,
        kGameplayRightHudScopeOwner,
        single_origin_alignment(UiOrigin::Left),
        static_cast<int32_t>(kGameplayLeftStackX),
        true,
        UiVerticalAnchor::Top
    );
}

extern "C" void bumble_ui_end_gameplay_right_hud(
    uint8_t* rdram,
    recomp_context*
) {
    if (!g_gameplay_right_hud_scope_open) {
        return;
    }
    end_ui_scope(rdram, kGameplayRightHudScopeOwner);
    g_gameplay_right_hud_scope_open = false;
}

extern "C" void bumble_ui_begin_gameplay_lives(
    uint8_t* rdram,
    recomp_context*
) {
    if (!single_player_ui_enabled(rdram) ||
        !bumble::text_overlay::renderer_ready() ||
        g_gameplay_lives_hidden) {
        return;
    }

    uint32_t saved_cursor = 0u;
    if (!read_guest_word(rdram, kDisplayListCursor, saved_cursor)) {
        return;
    }
    g_gameplay_lives_suppression.saved_cursor = saved_cursor;
    g_gameplay_lives_suppression.active = true;
    g_gameplay_lives_hidden = true;
}

extern "C" void bumble_ui_end_gameplay_lives(
    uint8_t* rdram,
    recomp_context*
) {
    if (!g_gameplay_lives_hidden) {
        return;
    }
    if (g_gameplay_lives_suppression.active && rdram != nullptr) {
        MEM_W(0, guest_address(kDisplayListCursor)) =
            g_gameplay_lives_suppression.saved_cursor;
    }
    g_gameplay_lives_suppression = {};
    g_gameplay_lives_hidden = false;
}

extern "C" void bumble_ui_begin_gameplay_weapon_ammo(
    uint8_t* rdram,
    recomp_context*
) {
    if (!g_gameplay_hud_root_scope_open ||
        g_gameplay_weapon_ammo_scope_open ||
        g_ui_scope_depth >= g_ui_scopes.size()) {
        return;
    }

    g_gameplay_weapon_ammo_scope_open = begin_ui_scope(
        rdram,
        kGameplayWeaponAmmoScopeOwner,
        single_origin_alignment(UiOrigin::Right),
        static_cast<int32_t>(kWeaponAmmoRightInset),
        true,
        UiVerticalAnchor::Bottom,
        kOriginWeaponBottomRight
    );
}

extern "C" void bumble_ui_end_gameplay_weapon_ammo(
    uint8_t* rdram,
    recomp_context*
) {
    if (!g_gameplay_weapon_ammo_scope_open) {
        return;
    }
    end_ui_scope(rdram, kGameplayWeaponAmmoScopeOwner);
    g_gameplay_weapon_ammo_scope_open = false;
}

extern "C" void bumble_ui_begin_gameplay_mission_gauge(
    uint8_t* rdram,
    recomp_context*
) {
    if (!physical_edge_ui_enabled(rdram) ||
        g_gameplay_mission_gauge_scope_open ||
        read_u32(rdram, 0x800D7364u) == 0u) {
        return;
    }
    g_gameplay_mission_gauge_scope_open = begin_ui_scope(
        rdram,
        kGameplayMissionGaugeScopeOwner,
        single_origin_alignment(UiOrigin::Right),
        212,
        true,
        UiVerticalAnchor::Top
    );
}

extern "C" void bumble_ui_end_gameplay_mission_gauge(
    uint8_t* rdram,
    recomp_context*
) {
    if (g_gameplay_mission_gauge_scope_open) {
        end_ui_scope(rdram, kGameplayMissionGaugeScopeOwner);
        g_gameplay_mission_gauge_scope_open = false;
    }
}

extern "C" void bumble_ui_begin_gameplay_status_bar(
    uint8_t* rdram,
    recomp_context*
) {
    if (single_player_ui_enabled(rdram) &&
        bumble::graphics_options::honeycomb_health_enabled() &&
        bumble::text_overlay::renderer_ready()) {
        const float current_health = read_f32(rdram, kPlayerHealthAddress);
        const float maximum_health = read_f32(
            rdram,
            kPlayerMaximumHealthAddress
        );
        uint32_t saved_cursor = 0u;
        if (std::isfinite(current_health) && std::isfinite(maximum_health) &&
            maximum_health > 0.0f &&
            read_guest_word(rdram, kDisplayListCursor, saved_cursor)) {
            const float fill = std::clamp(
                current_health / maximum_health,
                0.0f,
                1.0f
            );
            const uint32_t cells =
                bumble::native_checkpoint::half_player_health_active()
                ? 5u
                : 10u;
            if (bumble::text_overlay::observe_health_honeycomb(
                    18u,
                    20u,
                    fill,
                    cells
                )) {
                g_gameplay_status_bar_suppression.saved_cursor = saved_cursor;
                g_gameplay_status_bar_suppression.active = true;
                return;
            }
        }
    }
    if (!physical_edge_ui_enabled(rdram) ||
        g_gameplay_status_bar_scope_open ||
        g_ui_scope_depth >= g_ui_scopes.size()) {
        return;
    }

    g_gameplay_status_bar_scope_open = begin_ui_scope(
        rdram,
        kGameplayStatusBarScopeOwner,
        single_origin_alignment(UiOrigin::Left),
        static_cast<int32_t>(kGameplayLeftStackX),
        true,
        UiVerticalAnchor::Top,
        kOriginStatusTopLeft
    );
}

extern "C" void bumble_ui_end_gameplay_status_bar(
    uint8_t* rdram,
    recomp_context*
) {
    if (g_gameplay_status_bar_suppression.active) {
        if (rdram != nullptr) {
            MEM_W(0, guest_address(kDisplayListCursor)) =
                g_gameplay_status_bar_suppression.saved_cursor;
        }
        g_gameplay_status_bar_suppression = {};
        return;
    }
    if (!g_gameplay_status_bar_scope_open) {
        return;
    }
    end_ui_scope(rdram, kGameplayStatusBarScopeOwner);
    g_gameplay_status_bar_scope_open = false;
}

extern "C" void bumble_ui_begin_gameplay_text(
    uint8_t* rdram,
    recomp_context* context
) {
    GuestDrawSuppression* suppression = push_guest_draw_suppression(
        g_gameplay_text_suppressions,
        g_gameplay_text_suppression_depth,
        g_gameplay_text_suppression_overflow_depth
    );
    if (suppression == nullptr || context == nullptr ||
        !single_player_ui_enabled(rdram) ||
        g_script_native_text_active || g_briefing_native_line_active) {
        return;
    }

    uint32_t saved_cursor = 0u;
    if (!read_guest_word(rdram, kDisplayListCursor, saved_cursor)) {
        return;
    }
    std::array<char, kNativeTextMaximumBytes + 1u> native_text{};
    size_t native_text_length = 0u;
    if (!copy_guest_text_preview(
            rdram,
            low_guest_address(context->r4),
            native_text.data(),
            native_text.size(),
            native_text_length
        )) {
        return;
    }

    const uint32_t x = static_cast<uint16_t>(context->r5);
    const uint32_t y = static_cast<uint16_t>(context->r6);
    const uint32_t red = static_cast<uint32_t>(context->r7) & 0xFFu;
    const uint32_t green =
        static_cast<uint32_t>(MEM_W(0x10, context->r29)) & 0xFFu;
    const uint32_t blue =
        static_cast<uint32_t>(MEM_W(0x14, context->r29)) & 0xFFu;
    const char* scope_owner = g_ui_scope_depth != 0u
        ? g_ui_scopes[g_ui_scope_depth - 1u].owner
        : nullptr;
    const bool weapon_ammo = scope_owner != nullptr &&
        std::strcmp(scope_owner, kGameplayWeaponAmmoScopeOwner) == 0;
    const bool gameplay_score = scope_owner != nullptr &&
        std::strcmp(scope_owner, kGameplayRightHudScopeOwner) == 0;
    const uint32_t submitted_x = weapon_ammo
        ? kWeaponAmmoRightInset
        : (gameplay_score ? kGameplayLeftStackX : x);
    const uint32_t submitted_y = weapon_ammo
        ? kWeaponAmmoDestinationY
        : (gameplay_score ? kGameplayScoreDestinationY : y);
    suppression->active = bumble::text_overlay::observe(
        bumble::text_overlay::TextKind::GameplayHud,
        submitted_x,
        submitted_y,
        native_text.data(),
        gameplay_horizontal_anchor(static_cast<int32_t>(submitted_x)),
        gameplay_vertical_anchor(static_cast<int32_t>(submitted_y)),
        kGameplayUiScale,
        pack_text_rgba(red, green, blue)
    );
    suppression->saved_cursor = saved_cursor;
}

extern "C" void bumble_ui_end_gameplay_text(
    uint8_t* rdram,
    recomp_context*
) {
    pop_guest_draw_suppression(
        rdram,
        g_gameplay_text_suppressions,
        g_gameplay_text_suppression_depth,
        g_gameplay_text_suppression_overflow_depth
    );
}

extern "C" void bumble_ui_begin_gameplay_number(
    uint8_t* rdram,
    recomp_context* context
) {
    GuestDrawSuppression* suppression = push_guest_draw_suppression(
        g_gameplay_number_suppressions,
        g_gameplay_number_suppression_depth,
        g_gameplay_number_suppression_overflow_depth
    );
    if (suppression == nullptr || context == nullptr ||
        !single_player_ui_enabled(rdram) ||
        g_gameplay_lives_hidden || g_gameplay_timer_suppression.active) {
        return;
    }

    int32_t value = static_cast<int32_t>(context->r6);
    if (value < 0) {
        return;
    }
    value = std::min(value, 99);
    uint32_t saved_cursor = 0u;
    if (!read_guest_word(rdram, kDisplayListCursor, saved_cursor)) {
        return;
    }
    std::array<char, 4> number{};
    std::snprintf(number.data(), number.size(), "%02" PRId32, value);
    const uint32_t x = static_cast<uint16_t>(context->r4);
    const uint32_t y = static_cast<uint16_t>(context->r5);
    const uint32_t style = static_cast<uint32_t>(context->r7);
    const uint32_t face_rgba = style == 1u
        ? pack_text_rgba(96u, 255u, 0u)
        : pack_text_rgba(255u, 255u, 255u);
    suppression->active = bumble::text_overlay::observe(
        bumble::text_overlay::TextKind::GameplayHud,
        x,
        y,
        number.data(),
        gameplay_horizontal_anchor(static_cast<int32_t>(x)),
        gameplay_vertical_anchor(static_cast<int32_t>(y)),
        kGameplayUiScale,
        face_rgba
    );
    suppression->saved_cursor = saved_cursor;
}

extern "C" void bumble_ui_end_gameplay_number(
    uint8_t* rdram,
    recomp_context*
) {
    pop_guest_draw_suppression(
        rdram,
        g_gameplay_number_suppressions,
        g_gameplay_number_suppression_depth,
        g_gameplay_number_suppression_overflow_depth
    );
}

extern "C" void bumble_ui_begin_gameplay_timer(
    uint8_t* rdram,
    recomp_context* context
) {
    g_gameplay_timer_suppression = {};
    if (context == nullptr || !single_player_ui_enabled(rdram)) {
        return;
    }

    const uint32_t timer = low_guest_address(context->r4);
    uint32_t active = 0u;
    uint32_t minutes_word = 0u;
    uint32_t seconds_word = 0u;
    uint32_t saved_cursor = 0u;
    if (!read_guest_word(rdram, timer + 0x20u, active) || active == 0u ||
        !read_guest_word(rdram, timer + 0x28u, minutes_word) ||
        !read_guest_word(rdram, timer + 0x2Cu, seconds_word) ||
        !read_guest_word(rdram, kDisplayListCursor, saved_cursor)) {
        return;
    }

    const int32_t minutes = static_cast<int32_t>(minutes_word);
    const int32_t seconds = static_cast<int32_t>(seconds_word);
    if (minutes < 0 || seconds < 0) {
        return;
    }

    std::array<char, 8> text{};
    std::snprintf(
        text.data(),
        text.size(),
        "%02" PRId32 ":%02" PRId32,
        std::min(minutes, 99),
        std::min(seconds, 99)
    );
    const uint32_t x = static_cast<uint16_t>(context->r5);
    const uint32_t y = static_cast<uint16_t>(context->r6);
    constexpr int32_t kTimerGroupCenterOffset = 28;
    g_gameplay_timer_suppression.active =
        bumble::text_overlay::observe(
            bumble::text_overlay::TextKind::GameplayHud,
            x,
            y,
            text.data(),
            gameplay_horizontal_anchor(
                static_cast<int32_t>(x) + kTimerGroupCenterOffset
            ),
            gameplay_vertical_anchor(static_cast<int32_t>(y)),
            kGameplayUiScale,
            pack_text_rgba(255u, 255u, 255u)
        );
    g_gameplay_timer_suppression.saved_cursor = saved_cursor;
}

extern "C" void bumble_ui_end_gameplay_timer(
    uint8_t* rdram,
    recomp_context*
) {
    if (g_gameplay_timer_suppression.active && rdram != nullptr) {
        MEM_W(0, guest_address(kDisplayListCursor)) =
            g_gameplay_timer_suppression.saved_cursor;
    }
    g_gameplay_timer_suppression = {};
}

extern "C" void bumble_ui_begin_text_line(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!physical_edge_ui_enabled(rdram) || context == nullptr) {
        return;
    }

    int32_t pixel_x = 0;
    uint32_t flags = 0u;
    const UiAlignment alignment = classify_text_descriptor(
        rdram,
        static_cast<uint32_t>(context->r5),
        pixel_x,
        flags
    );
    (void)flags;
    const bool compact_frontend_menu =
        compact_frontend_menu_enabled(rdram);
    begin_ui_scope(
        rdram,
        "func_800B9748",
        alignment,
        pixel_x,
        single_player_ui_enabled(rdram) ||
            compact_frontend_menu,
        compact_frontend_menu
            ? compact_frontend_menu_vertical_anchor(rdram)
            : UiVerticalAnchor::Automatic
    );
}

extern "C" void bumble_ui_end_text_line(
    uint8_t* rdram,
    recomp_context*
) {
    if (physical_edge_ui_enabled(rdram)) {
        end_ui_scope(rdram, "func_800B9748");
    }
}

extern "C" void bumble_ui_begin_native_menu_list(
    uint8_t* rdram,
    recomp_context* context
) {
    g_native_menu_list_suppression = {};
    if (g_native_menu_prefix_suppression.active) {
        if (rdram != nullptr) {
            MEM_W(0, guest_address(kDisplayListCursor)) =
                g_native_menu_prefix_suppression.saved_cursor;
            if (!g_native_menu_prefix_suppression_logged.exchange(
                    true,
                    std::memory_order_acq_rel
                )) {
                std::fprintf(
                    stderr,
                    "BUMBLE_UI stage=native_menu_legacy_prefix"
                    " guest_prefix_rewound=1"
                    " replacement=full_height_output_resolution\n"
                );
                std::fflush(stderr);
            }
        }
        g_native_menu_prefix_suppression = {};
    }
    if (rdram == nullptr || context == nullptr) {
        return;
    }

    const uint32_t descriptor = low_guest_address(context->r23);
    uint32_t saved_cursor = 0u;
    if (!read_guest_word(rdram, kDisplayListCursor, saved_cursor) ||
        !publish_native_menu_replacement(rdram, descriptor)) {
        return;
    }
    g_native_menu_list_suppression.saved_cursor = saved_cursor;
    g_native_menu_list_suppression.active = true;
}

extern "C" void bumble_ui_end_native_menu_list(
    uint8_t* rdram,
    recomp_context*
) {
    if (!g_native_menu_list_suppression.active) {
        return;
    }

    if (rdram != nullptr) {
        MEM_W(0, guest_address(kDisplayListCursor)) =
            g_native_menu_list_suppression.saved_cursor;
    }
    g_native_menu_list_suppression = {};
}

extern "C" void bumble_ui_begin_campaign_level_grid(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr) {
        return;
    }
    const bool campaign_grid_active =
        bumble::native_checkpoint::campaign_grid_active() &&
        low_guest_address(context->r5) == kLevelSelectDescriptor &&
        frontend_phase_is_current_or_target(rdram, kLevelSelectPhase);
    const bool campaign_selection_committed =
        bumble::native_checkpoint::campaign_selection_committed_index() != 0u;
    const uint32_t descriptor = context != nullptr
        ? low_guest_address(context->r5)
        : 0u;
    const bool post_commit_level_select =
        rdram != nullptr && campaign_selection_committed &&
        frontend_phase_is_current_or_target(rdram, kLevelSelectPhase) &&
        descriptor == kLevelSelectDescriptor;
    const bool briefing_level_entry =
        rdram != nullptr && context != nullptr &&
        campaign_selection_committed &&
        frontend_phase_is_current_or_target(rdram, kMissionBriefingPhase) &&
        descriptor == kMissionBriefingDescriptor &&
        read_u32(
            rdram,
            kMissionBriefingDescriptor + 0x1Cu
        ) == kMissionBriefingDescriptorFlags &&
        read_u32(rdram, kMissionBriefingDescriptor + 0x38u) == 0u;
    const bool legacy_level_entry =
        post_commit_level_select || briefing_level_entry;
    if (!legacy_level_entry) {
        g_legacy_level_entry_suppression_latched.store(
            false,
            std::memory_order_release
        );
    }
    if (!post_commit_level_select) {
        g_post_commit_level_select_suppression_latched.store(
            false,
            std::memory_order_release
        );
    }
    if (!campaign_grid_active && !legacy_level_entry) {
        g_campaign_grid_render_logged = false;
        return;
    }
    if (g_campaign_grid_render_active ||
        !bumble::text_overlay::renderer_ready()) {
        return;
    }
    uint32_t saved_cursor = 0u;
    if (!read_guest_word(rdram, kDisplayListCursor, saved_cursor)) {
        return;
    }
    g_campaign_grid_suppression.saved_cursor = saved_cursor;
    g_campaign_grid_suppression.active = true;
    g_campaign_grid_render_active = true;
    if (legacy_level_entry &&
        !g_legacy_level_entry_suppression_latched.exchange(
            true,
            std::memory_order_acq_rel
        )) {
        const uint64_t count =
            g_legacy_level_entry_suppression_count.fetch_add(
                1u,
                std::memory_order_acq_rel
            ) + 1u;
        std::fprintf(
            stderr,
            "BUMBLE_CAMPAIGN_GRID stage=legacy_level_entry_hidden"
            " count=%" PRIu64 " phase=0x%08" PRIX32
            " descriptor=0x%08" PRIX32
            " load_state_preserved=1 overlay_rewound=1\n",
            count,
            post_commit_level_select
                ? kLevelSelectPhase
                : kMissionBriefingPhase,
            descriptor
        );
        std::fflush(stderr);
    }
    if (post_commit_level_select &&
        !g_post_commit_level_select_suppression_latched.exchange(
            true,
            std::memory_order_acq_rel
        )) {
        g_post_commit_level_select_suppression_count.fetch_add(
            1u,
            std::memory_order_acq_rel
        );
    }
}

extern "C" void bumble_ui_end_campaign_level_grid(
    uint8_t* rdram,
    recomp_context*
) {
    if (!g_campaign_grid_render_active) {
        return;
    }
    if (!bumble::native_checkpoint::campaign_grid_active()) {
        if (g_campaign_grid_suppression.active && rdram != nullptr) {
            MEM_W(0, guest_address(kDisplayListCursor)) =
                g_campaign_grid_suppression.saved_cursor;
        }
        g_campaign_grid_suppression = {};
        g_campaign_grid_render_active = false;
        return;
    }

    using bumble::text_overlay::HorizontalAnchor;
    using bumble::text_overlay::TextKind;
    using bumble::text_overlay::VerticalAnchor;
    constexpr uint32_t kWhite = 0xFFF4D8FFu;
    constexpr uint32_t kGrey = 0x777777FFu;
    constexpr uint32_t kYellow = 0xFFD64AFFu;
    constexpr uint32_t kBlack = 0x000000FFu;
    bool accepted = bumble::text_overlay::observe_menu_backdrop(
        0x162A3DDBu,
        TextKind::Menu
    );
    accepted &= bumble::text_overlay::observe(
        TextKind::Menu,
        118u,
        13u,
        "LEVEL SELECT",
        HorizontalAnchor::Center,
        VerticalAnchor::Authored,
        0.88f,
        kYellow,
        kBlack
    );

    const uint32_t selected_slot = std::min<uint32_t>(
        bumble::native_checkpoint::campaign_grid_selected_slot(),
        static_cast<uint32_t>(
            bumble::campaign_levels::kMissionSelectLevelIndices.size() - 1u
        )
    );
    const uint32_t progress_level =
        bumble::native_checkpoint::campaign_grid_progress_level();
    const bool back_hovered =
        bumble::native_checkpoint::campaign_grid_back_hovered();
    for (size_t slot = 0u;
         slot < bumble::campaign_levels::kMissionSelectLevelIndices.size();
         ++slot) {
        const uint32_t level_index =
            bumble::campaign_levels::kMissionSelectLevelIndices[slot];
        const auto* record = bumble::campaign_levels::find(level_index);
        if (record == nullptr) {
            accepted = false;
            continue;
        }
        std::string text = campaign_grid_name(*record);
        const bool selected = !back_hovered && slot == selected_slot;
        if (selected) {
            text.insert(0u, "> ");
        } else {
            text.insert(0u, "  ");
        }
        const bool unlocked =
            bumble::graphics_options::unlock_all_levels_enabled() ||
            level_index <= progress_level;
        const uint32_t column = static_cast<uint32_t>(
            slot / bumble::campaign_levels::kMissionGridRows);
        const uint32_t row = static_cast<uint32_t>(
            slot % bumble::campaign_levels::kMissionGridRows);
        accepted &= bumble::text_overlay::observe(
            TextKind::Menu,
            column == 0u ? 32u : 174u,
            bumble::campaign_levels::kMissionGridFirstY +
                row * bumble::campaign_levels::kMissionGridRowSpacing,
            text.c_str(),
            HorizontalAnchor::Center,
            VerticalAnchor::Authored,
            0.64f,
            unlocked ? (selected ? kYellow : kWhite) : kGrey,
            kBlack
        );
    }
    accepted &= bumble::text_overlay::observe(
        TextKind::Menu,
        118u,
        214u,
        back_hovered ? "> BACK" : "BACK",
        HorizontalAnchor::Center,
        VerticalAnchor::Authored,
        0.64f,
        back_hovered ? kYellow : kWhite,
        kBlack
    );
    std::string controls;
#if defined(BUMBLE_HEADLESS_GRAPHICS_OPTIONS_STUB)
    controls = "ENTER SELECT    ESC BACK    GREY = LOCKED";
#else
    const std::string keyboard_confirm =
        bumble::input_bindings::binding_text(
            bumble::input_bindings::BindingProfile::KeyboardMouse,
            bumble::input_bindings::InputAction::MenuConfirm
        );
    const std::string keyboard_back =
        bumble::input_bindings::binding_text(
            bumble::input_bindings::BindingProfile::KeyboardMouse,
            bumble::input_bindings::InputAction::MenuBack
        );
    controls = keyboard_confirm + " SELECT    ESC / " + keyboard_back +
        " BACK    GREY = LOCKED";
#endif
    accepted &= bumble::text_overlay::observe(
        TextKind::Menu,
        91u,
        232u,
        controls.c_str(),
        HorizontalAnchor::Center,
        VerticalAnchor::Authored,
        0.44f,
        kWhite,
        kBlack
    );

    if (accepted && g_campaign_grid_suppression.active && rdram != nullptr) {
        MEM_W(0, guest_address(kDisplayListCursor)) =
            g_campaign_grid_suppression.saved_cursor;
        if (!g_campaign_grid_render_logged) {
            std::fprintf(
                stderr,
                "BUMBLE_CAMPAIGN_GRID stage=rendered labels=%zu"
                " original_selector_rewound=1 renderer=high_resolution\n",
                bumble::campaign_levels::kMissionSelectLevelIndices.size() + 3u
            );
            std::fflush(stderr);
            g_campaign_grid_render_logged = true;
        }
    }
    g_campaign_grid_suppression = {};
    g_campaign_grid_render_active = false;
}

extern "C" void bumble_ui_publish_direct_text_call(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t call_pc
) {
    g_pending_direct_text_call = {};
    if (context == nullptr || g_campaign_grid_render_active) {
        return;
    }

    size_t caller_index = kDirectTextCallers.size();
    const DirectTextCallerSpec* caller = find_direct_text_caller(
        call_pc,
        caller_index
    );
    if (caller == nullptr) {
        return;
    }

    if (!physical_edge_ui_enabled(rdram)) {
        return;
    }

    PendingDirectTextCall pending{};
    pending.caller_index = caller_index;
    pending.face_rgba = static_cast<uint32_t>(context->r4);
    pending.valid = true;
    switch (caller->metadata_kind) {
    case DirectTextMetadataKind::CallerStackWord:
        pending.flags_valid = read_direct_text_flags(
            rdram,
            context,
            caller->primary_offset,
            pending.flags
        );
        break;
    case DirectTextMetadataKind::EquivalentStackWords: {
        uint32_t alternate_flags = 0u;
        const bool primary_valid = read_direct_text_flags(
            rdram,
            context,
            caller->primary_offset,
            pending.flags
        );
        const bool alternate_valid = read_direct_text_flags(
            rdram,
            context,
            caller->alternate_offset,
            alternate_flags
        );
        pending.flags_valid = primary_valid && alternate_valid &&
            (pending.flags & 0x6u) == (alternate_flags & 0x6u);
        break;
    }
    case DirectTextMetadataKind::AuthoredCenterSafe:
    case DirectTextMetadataKind::InheritedOuterDescriptor:
        break;
    }
    g_pending_direct_text_call = pending;
}

extern "C" void bumble_ui_begin_direct_text(
    uint8_t* rdram,
    recomp_context* context
) {
    if (!physical_edge_ui_enabled(rdram) || context == nullptr) {
        g_pending_direct_text_call = {};
        return;
    }

    const PendingDirectTextCall pending = g_pending_direct_text_call;
    g_pending_direct_text_call = {};

    if (g_direct_text_depth >= g_direct_text_scopes.size() ||
        g_direct_text_overflow_depth != 0u) {
        ++g_direct_text_overflow_depth;
        return;
    }

    const size_t caller_index = pending.valid
        ? pending.caller_index
        : kDirectTextCallers.size();
    const DirectTextCallerSpec* caller = caller_index < kDirectTextCallers.size()
        ? &kDirectTextCallers[caller_index]
        : nullptr;

    const bool completion_credit_call =
        caller != nullptr &&
        (caller->call_pc == 0x800B5B48u ||
            caller->call_pc == 0x800B5CB0u) &&
        bumble::game_completion_screen::active();
    if (completion_credit_call) {
        const uint32_t text_pointer = low_guest_address(context->r4);
        float credit_x = 0.0f;
        float credit_y = 0.0f;
        const uint32_t credit_x_bits = static_cast<uint32_t>(context->r5);
        const uint32_t credit_y_bits = static_cast<uint32_t>(context->r6);
        std::memcpy(&credit_x, &credit_x_bits, sizeof(credit_x));
        std::memcpy(&credit_y, &credit_y_bits, sizeof(credit_y));
        std::array<char, kNativeTextMaximumBytes + 1u> preview{};
        size_t preview_length = 0u;
        if (std::isfinite(credit_x) && std::isfinite(credit_y) &&
            copy_guest_text_preview(
                rdram,
                text_pointer,
                preview.data(),
                preview.size(),
                preview_length
            )) {
            const uint32_t x = static_cast<uint32_t>(std::clamp<int32_t>(
                static_cast<int32_t>(std::lround(credit_x)),
                0,
                static_cast<int32_t>(kScreenWidth)
            ));
            const uint32_t y = static_cast<uint32_t>(std::clamp<int32_t>(
                static_cast<int32_t>(std::lround(credit_y)),
                0,
                static_cast<int32_t>(kScreenHeight)
            ));
            const uint64_t stable_slot =
                0x4352454449540000ull |
                static_cast<uint64_t>(text_pointer);
            const bool accepted = bumble::text_overlay::observe(
                bumble::text_overlay::TextKind::Credits,
                x,
                y,
                preview.data(),
                bumble::text_overlay::HorizontalAnchor::Right,
                bumble::text_overlay::VerticalAnchor::Authored,
                0.86f,
                pending.face_rgba,
                0x000000FFu,
                stable_slot
            );
            if (accepted &&
                !g_completion_credit_text_logged.exchange(
                    true,
                    std::memory_order_acq_rel
                )) {
                std::fprintf(
                    stderr,
                    "BUMBLE_GAME_COMPLETION_SCREEN"
                    " stage=first_credit_observed"
                    " owner=func_800B5638 call_pc=0x%08" PRIX32
                    " text_pointer=0x%08" PRIX32
                    " x=%" PRIu32 " y=%" PRIu32
                    " rgba=0x%08" PRIX32
                    " stable_slot=1 preview=\"%s\"\n",
                    caller->call_pc,
                    text_pointer,
                    x,
                    y,
                    pending.face_rgba,
                    preview.data()
                );
                std::fflush(stderr);
            }
        }
    }

    const bool start_scope = g_ui_scope_depth == 0u;
    const size_t scope_index = g_direct_text_depth++;
    g_direct_text_scopes[scope_index] = false;

    float x = 0.0f;
    const uint32_t x_bits = static_cast<uint32_t>(context->r5);
    std::memcpy(&x, &x_bits, sizeof(x));
    const int32_t pixel_x = std::isfinite(x)
        ? static_cast<int32_t>(std::lround(x))
        : static_cast<int32_t>(kScreenWidth / 2u);

    const uint32_t flags = pending.flags;
    const bool flags_valid = pending.valid && pending.flags_valid;
    UiAlignment alignment = single_origin_alignment(UiOrigin::None);
    if (!start_scope) {
        alignment = g_ui_scopes[g_ui_scope_depth - 1u].alignment;
    }
    else if (caller != nullptr &&
        caller->metadata_kind == DirectTextMetadataKind::AuthoredCenterSafe) {
        alignment = single_origin_alignment(UiOrigin::None);
    }
    else if (flags_valid) {
        alignment = classify_text_justification(flags, pixel_x);
    }

    log_direct_text_caller_once(
        caller_index,
        caller,
        caller == nullptr ? 0u : caller->call_pc,
        flags_valid,
        flags,
        pixel_x,
        alignment,
        !start_scope
    );
    if (!start_scope) {
        return;
    }

    const bool compact_frontend_menu =
        compact_frontend_menu_enabled(rdram);
    g_direct_text_scopes[scope_index] = begin_ui_scope(
        rdram,
        "func_800B8180",
        alignment,
        pixel_x,
        single_player_ui_enabled(rdram) ||
            compact_frontend_menu,
        compact_frontend_menu
            ? compact_frontend_menu_vertical_anchor(rdram)
            : UiVerticalAnchor::Automatic
    );
}

extern "C" void bumble_ui_end_direct_text(
    uint8_t* rdram,
    recomp_context*
) {
    if (!physical_edge_ui_enabled(rdram)) {
        return;
    }

    if (g_direct_text_overflow_depth != 0u) {
        --g_direct_text_overflow_depth;
        return;
    }
    if (g_direct_text_depth == 0u) {
        return;
    }

    const bool end_scope = g_direct_text_scopes[--g_direct_text_depth];
    if (end_scope) {
        end_ui_scope(rdram, "func_800B8180");
    }
}
