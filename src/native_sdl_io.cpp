#include "native_sdl_io.hpp"

#if defined(_WIN32)
#include <Windows.h>
#endif

#include <SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <json/json.hpp>

#include "native_checkpoint_bridge.hpp"
#include "native_graphics_options.hpp"
#include "native_input_bindings.hpp"
#include "native_key_codes.hpp"
#include "native_level_editor.hpp"
#include "native_modern_controls.hpp"
#include "native_pi_dma_bridge.hpp"
#include "native_rsp_task_bridge.hpp"
#include "native_rt64_renderer.hpp"
#include "native_text_overlay_state.hpp"
#include "native_widescreen.hpp"

namespace {

constexpr uint16_t kButtonA = 0x8000;
constexpr uint16_t kButtonB = 0x4000;
constexpr uint16_t kButtonZ = 0x2000;
constexpr uint16_t kButtonStart = 0x1000;
constexpr uint16_t kDpadUp = 0x0800;
constexpr uint16_t kDpadDown = 0x0400;
constexpr uint16_t kDpadLeft = 0x0200;
constexpr uint16_t kDpadRight = 0x0100;
constexpr uint16_t kButtonL = 0x0020;
constexpr uint16_t kButtonR = 0x0010;
constexpr uint16_t kCDown = 0x0004;
constexpr uint16_t kCLeft = 0x0002;
constexpr uint16_t kCRight = 0x0001;
constexpr Sint16 kMovementStickDeadzone = 7000;
constexpr float kN64StickMagnitude = 80.0f / 127.0f;
constexpr float kReplayStickMagnitude = kN64StickMagnitude;
constexpr Sint16 kCButtonThreshold = 16000;
constexpr size_t kReplayControllerCount = 2;
constexpr size_t kReplayControlCount = 12;
constexpr uint32_t kKeyboardVirtualKeyCount = 256u;
constexpr uint32_t kKeyboardWordBits = 64u;
constexpr size_t kKeyboardWordCount =
    kKeyboardVirtualKeyCount / kKeyboardWordBits;
constexpr uint64_t kAudioViRate = 60u;
constexpr uint64_t kAudioGameplayPrebufferViIntervals = 6u;
constexpr uint64_t kAudioTransitionPrebufferViIntervals = 18u;
constexpr uint32_t kAudioOutputFrequency = 48000u;
constexpr int32_t kAudioGainDivisor = 2;

constexpr bool directly_selectable_campaign_index(uint32_t level_index) {
    return (level_index >= 2u && level_index <= 15u) ||
        (level_index >= 17u && level_index <= 20u);
}

struct ControllerSnapshot {
    bool connected = false;
    bool physical_rumble = false;
    ultramodern::input::Pak connected_pak = ultramodern::input::Pak::None;
    uint16_t buttons = 0;
    float x = 0.0f;
    float y = 0.0f;
};

enum class ReplayControl {
    A,
    B,
    Z,
    Start,
    StickUp,
    StickDown,
    StickLeft,
    StickRight,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
};

struct ScheduledReplayEvent {
    uint64_t tick = 0;
    size_t controller = 0;
    bool down = false;
    ReplayControl control = ReplayControl::A;
};

struct GameOwnedReplayEvent {
    size_t controller = 0;
    ReplayControl control = ReplayControl::A;
    uint32_t observed_phase = 0;
    uint64_t reference_tick = 0;
    uint64_t deadline_tick = 0;
    bool deadline_reported = false;
};

struct MenuSelectionReplayEvent {
    size_t controller = 0;
    ReplayControl control = ReplayControl::StickDown;
    uint32_t observed_phase = 0;
    uint32_t descriptor = 0;
    uint32_t from_item = 0;
    uint32_t to_item = 0;
    uint64_t deadline_tick = 0;
    bool requested = false;
    bool acknowledged = false;
    bool deadline_reported = false;
};

struct TwoPlayerReplayProbe {
    size_t controller = 1;
    ReplayControl control = ReplayControl::StickUp;
    uint64_t not_before_tick = 0;
    uint64_t hold_polls = 0;
    uint64_t armed_tick = 0;
    bool armed = false;
    bool released = false;
    bool expected_positive = false;
    bool applied = false;
};

struct LaterProgressionReplay {
    uint32_t target_level_index = 2;
    uint32_t cheat_phase = 0;
    uint64_t cheat_deadline_tick = 0;
    uint64_t selector_deadline_tick = 0;
    uint64_t commit_deadline_tick = 0;
    uint64_t player_deadline_tick = 0;
    std::vector<ReplayControl> cheat_sequence;
    size_t cheat_cursor = 0;
    uint32_t cheat_progress_baseline = 0;
    bool cheat_started = false;
    bool cheat_direction_down = false;
    bool cheat_waiting_ack = false;
    bool cheat_attempt_complete = false;
    bool cheat_z_down = false;
    bool main_menu_a_down = false;
    bool main_menu_acked = false;
    uint32_t main_menu_confirm_baseline = 0;
    bool selector_pulse_down = false;
    bool commit_a_down = false;
    bool commit_a_sent = false;
    bool control_phase_e_a_down = false;
    bool control_phase_e_acked = false;
    uint32_t control_phase_e_confirm_baseline = 0;
    bool pi_capture_started = false;
    bool capture_drain_started = false;
    bool capture_drain_wait_logged = false;
    bool deadline_reported = false;
};

struct MissionOneCompletionReplay {
    bool withhold_one_owner = false;
    bool require_profile_save = false;
    bool require_mission2_checkpoint = false;
    uint64_t completion_deadline_tick = 0;
    uint64_t negative_stable_since_tick = 0;
    uint64_t result_visible_since_tick = 0;
    uint64_t post_stage_started_tick = 0;
    uint32_t selector_confirm_baseline = 0;
    uint32_t success_confirm_baseline = 0;
    uint32_t save_confirm_baseline = 0;
    uint32_t save_slot_confirm_baseline = 0;
    uint32_t save_success_confirm_baseline = 0;
    uint32_t continue_confirm_baseline = 0;
    uint32_t mission2_phase18_confirm_baseline = 0;
    uint32_t mission2_phase19_confirm_baseline = 0;
    bool selector_a_down = false;
    bool selector_acked = false;
    bool success_a_down = false;
    bool post_route_started = false;
    bool save_navigation_down = false;
    bool save_navigation_acked = false;
    bool save_a_down = false;
    bool save_acked = false;
    bool save_slot_a_down = false;
    bool save_slot_acked = false;
    bool save_success_a_down = false;
    bool save_success_acked = false;
    bool continue_navigation_down = false;
    bool continue_navigation_acked = false;
    bool continue_a_down = false;
    bool continue_acked = false;
    bool mission2_phase18_a_down = false;
    bool mission2_phase18_acked = false;
    bool mission2_phase19_a_down = false;
    bool mission2_phase19_acked = false;
    bool pi_capture_started = false;
    bool capture_drain_started = false;
    bool capture_drain_wait_logged = false;
    bool capture_contract_logged = false;
};

struct ProfileResumeReplay {
    uint64_t completion_deadline_tick = 0;
    uint64_t profile_reopen_ready_tick = 0;
    uint32_t selector_confirm_baseline = 0;
    bool selector_a_down = false;
    bool selector_acked = false;
    bool pi_capture_started = false;
    bool capture_drain_started = false;
    bool capture_drain_wait_logged = false;
    bool capture_contract_logged = false;
};

struct ReplayObservation {
    uint64_t tick = 0;
    std::string label;
};

struct ReplayState {
    int schema_version = 1;
    size_t replay_controller_count = 1;
    bool configured = false;
    bool armed = false;
    bool completion_logged = false;
    bool semantic_success = false;
    bool require_mission1_checkpoint = false;
    bool require_two_player_checkpoint = false;
    bool require_mission2_checkpoint = false;
    ultramodern::input::Pak replay_connected_pak = ultramodern::input::Pak::None;
    std::array<ultramodern::input::Pak, 4> replay_connected_paks{};
    std::optional<ReplayControl> masked_control;
    std::optional<MenuSelectionReplayEvent> selection_event;
    std::optional<TwoPlayerReplayProbe> two_player_probe;
    std::optional<LaterProgressionReplay> later_progression;
    std::optional<MissionOneCompletionReplay> mission1_completion;
    std::optional<ProfileResumeReplay> profile_resume;
    std::string manifest_path;
    uint64_t tick = 0;
    uint64_t success_earliest_tick = 0;
    uint64_t end_tick = 0;
    size_t scheduled_cursor = 0;
    size_t game_event_cursor = 0;
    size_t observation_cursor = 0;
    bool game_event_button_down = false;
    bool deadline_missed = false;
    std::optional<bool> campaign_grid_exit_clean;
    bool legacy_level_entry_hidden = false;
    bool post_commit_level_select_hidden = false;
    uint32_t game_event_confirm_baseline = 0;
    ControllerSnapshot snapshot{.connected = true};
    std::array<ControllerSnapshot, 4> snapshots{};
    std::vector<ScheduledReplayEvent> scheduled_events;
    std::vector<GameOwnedReplayEvent> game_owned_events;
    std::vector<ReplayObservation> observations;
};

std::mutex g_controller_mutex;
std::atomic_bool g_quit_requested{false};
std::atomic_bool g_replay_configured{false};
std::array<SDL_GameController*, 4> g_controllers{};
std::array<ControllerSnapshot, 4> g_snapshots{};
std::array<ultramodern::input::Pak, 4> g_connected_paks{
    ultramodern::input::Pak::ControllerPak,
    ultramodern::input::Pak::None,
    ultramodern::input::Pak::None,
    ultramodern::input::Pak::None,
};
bool g_rescan_requested = false;
bool g_modern_visuals_toggle_down = false;
bool g_keyboard_visuals_toggle_down = false;
ReplayState g_replay{};
std::atomic_bool g_replay_complete{false};
std::atomic_bool g_replay_semantic_success{false};
std::atomic_uint64_t g_replay_tick{0};
std::atomic_uint32_t g_replay_completed_game_events{0};

std::mutex g_audio_mutex;
SDL_AudioDeviceID g_audio_device = 0;
SDL_AudioStream* g_audio_stream = nullptr;
uint32_t g_audio_frequency = 0;
uint32_t g_audio_output_frequency = 0;
uint16_t g_audio_device_buffer_frames = 0;
uint64_t g_audio_prebuffer_target_frames = 0;
uint64_t g_audio_transition_prebuffer_target_frames = 0;
bool g_audio_playback_started = false;
bool g_audio_transition_reservoir_armed = false;
uint64_t g_audio_queue_calls_current = 0;
uint64_t g_audio_playback_starts_current = 0;
uint64_t g_audio_underruns_current = 0;
uint64_t g_audio_reprimes_current = 0;
uint64_t g_audio_transition_rebuffers_current = 0;
uint64_t g_audio_min_started_queue_frames = std::numeric_limits<uint64_t>::max();
uint64_t g_audio_input_frames_current = 0;
uint64_t g_audio_output_frames_current = 0;
bool g_audio_conversion_log_emitted = false;
std::vector<int16_t> g_audio_input_buffer;
std::vector<int16_t> g_audio_resample_buffer;

uint64_t active_audio_prebuffer_target_frames() {
    const bool transition_reservoir = g_audio_transition_reservoir_armed &&
        bumble::modern_controls::enabled() &&
        !bumble::modern_controls::gameplay_input_active();
    return transition_reservoir
        ? g_audio_transition_prebuffer_target_frames
        : g_audio_prebuffer_target_frames;
}

void release_audio_transition_reservoir_locked() {
    if (!g_audio_transition_reservoir_armed ||
        !bumble::modern_controls::gameplay_input_active()) {
        return;
    }
    g_audio_transition_reservoir_armed = false;
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=audio_transition_reservoir_released"
        " gameplay_target_frames=%llu pitch_stretch=0\n",
        static_cast<unsigned long long>(g_audio_prebuffer_target_frames)
    );
    std::fflush(stderr);
}

void arm_audio_transition_reservoir() {
    std::lock_guard lock(g_audio_mutex);
    if (g_audio_device == 0 || g_audio_transition_reservoir_armed ||
        bumble::modern_controls::gameplay_input_active()) {
        return;
    }

    g_audio_transition_reservoir_armed = true;
    ++g_audio_transition_rebuffers_current;
    const uint64_t queued_frames =
        SDL_GetQueuedAudioSize(g_audio_device) / (sizeof(int16_t) * 2u);
    const bool needs_rebuffer =
        queued_frames < g_audio_transition_prebuffer_target_frames;
    if (needs_rebuffer && g_audio_playback_started) {
        SDL_PauseAudioDevice(g_audio_device, 1);
        g_audio_playback_started = false;
    }
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=audio_transition_reservoir_armed"
        " queued_frames=%llu target_frames=%llu paused_for_fill=%d"
        " pitch_stretch=0\n",
        static_cast<unsigned long long>(queued_frames),
        static_cast<unsigned long long>(g_audio_transition_prebuffer_target_frames),
        needs_rebuffer ? 1 : 0
    );
    std::fflush(stderr);
}

std::atomic_uint64_t g_input_polls{0};
std::atomic_uint64_t g_input_transitions{0};
std::atomic_uint32_t g_device_info_queries{0};
std::atomic_uint64_t g_audio_samples{0};
std::atomic_uint64_t g_audio_consumed_samples{0};
uint64_t g_audio_submitted_bytes_current = 0;
uint64_t g_audio_observed_consumed_bytes_current = 0;
bool g_audio_consumption_logged = false;
bool g_initialized = false;

std::array<std::atomic_uint64_t, kKeyboardWordCount> g_keyboard_down{};
std::array<std::atomic_uint64_t, kKeyboardWordCount>
    g_keyboard_pressed_since_poll{};
thread_local std::array<uint64_t, kKeyboardWordCount>
    g_keyboard_pressed_for_poll{};
std::atomic_bool g_keyboard_pause_context_active{false};
std::atomic_bool g_keyboard_pause_confirm_requires_release{false};
std::atomic_uint8_t g_keyboard_pause_neutral_polls{0u};
std::atomic_bool g_controller_pause_context_active{false};
std::atomic_bool g_controller_pause_confirm_requires_release{false};
std::atomic_uint8_t g_controller_pause_neutral_polls{0u};
std::atomic_uint32_t g_mouse_buttons_down{0u};
std::atomic_uint32_t g_mouse_buttons_pressed_since_poll{0u};
thread_local uint32_t g_mouse_buttons_pressed_for_poll = 0u;

struct MouseWheelPulses {
    std::array<int, 64> directions{};
    size_t head = 0u;
    size_t count = 0u;
    int remainder = 0;
    bool release = false;
    constexpr void clear() { *this = {}; }
    constexpr void add(int delta) {
        remainder += std::clamp(delta, -7680, 7680);
        while (remainder >= 120 || remainder <= -120) {
            const int direction = remainder > 0 ? 1 : -1;
            remainder -= direction * 120;
            if (count < directions.size()) {
                directions[(head + count++) % directions.size()] = direction;
            }
        }
    }
    constexpr int poll() {
        if (release) { release = false; return 0; }
        if (count == 0u) { return 0; }
        const int direction = directions[head];
        head = (head + 1u) % directions.size();
        --count;
        release = true;
        return direction;
    }
};
constexpr bool mouse_wheel_pulse_contract() {
    MouseWheelPulses wheel;
    wheel.add(60);
    if (wheel.poll() != 0) return false;
    wheel.add(300);
    wheel.add(-120);
    for (int expected : {1, 0, 1, 0, 1, 0, -1, 0, 0}) {
        if (wheel.poll() != expected) return false;
    }
    wheel.add(7680);
    wheel.add(7680);
    if (wheel.count != 64u) return false;
    wheel.clear();
    return wheel.poll() == 0 && wheel.remainder == 0 && !wheel.release;
}
static_assert(mouse_wheel_pulse_contract());
std::mutex g_mouse_wheel_mutex;
MouseWheelPulses g_mouse_wheel;
uint64_t g_mouse_wheel_binding_revision = 0u;
std::atomic_size_t g_connected_controller_count{0u};

uint32_t portable_key_code(SDL_Keycode code) {
    if (code >= SDLK_a && code <= SDLK_z) {
        return static_cast<uint32_t>('A' + (code - SDLK_a));
    }
    if (code >= SDLK_0 && code <= SDLK_9) {
        return static_cast<uint32_t>('0' + (code - SDLK_0));
    }
    switch (code) {
    case SDLK_BACKSPACE: return bumble::key::Backspace;
    case SDLK_TAB: return bumble::key::Tab;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return bumble::key::Enter;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT: return bumble::key::Shift;
    case SDLK_LCTRL:
    case SDLK_RCTRL: return bumble::key::Control;
    case SDLK_LALT:
    case SDLK_RALT: return bumble::key::Alt;
    case SDLK_ESCAPE: return bumble::key::Escape;
    case SDLK_SPACE: return bumble::key::Space;
    case SDLK_PAGEUP: return bumble::key::PageUp;
    case SDLK_PAGEDOWN: return bumble::key::PageDown;
    case SDLK_END: return bumble::key::End;
    case SDLK_HOME: return bumble::key::Home;
    case SDLK_LEFT: return bumble::key::Left;
    case SDLK_UP: return bumble::key::Up;
    case SDLK_RIGHT: return bumble::key::Right;
    case SDLK_DOWN: return bumble::key::Down;
    case SDLK_DELETE: return bumble::key::Delete;
    case SDLK_F3: return bumble::key::F3;
    default: return 0u;
    }
}

bool key_down(int virtual_key) {
    if (virtual_key < 0 ||
        static_cast<uint32_t>(virtual_key) >= kKeyboardVirtualKeyCount) {
        return false;
    }
    const uint32_t key = static_cast<uint32_t>(virtual_key);
    const uint64_t mask = UINT64_C(1) << (key % kKeyboardWordBits);
    const size_t word_index = key / kKeyboardWordBits;
    return ((g_keyboard_down[word_index].load(std::memory_order_acquire) |
             g_keyboard_pressed_for_poll[word_index]) & mask) != 0;
}

constexpr uint32_t mouse_button_mask(
    bumble::input_bindings::MouseButton button
) {
    return UINT32_C(1) << (static_cast<uint32_t>(button) - 1u);
}

bool mouse_button_down(bumble::input_bindings::MouseButton button) {
    const uint32_t mask = mouse_button_mask(button);
    return ((g_mouse_buttons_down.load(std::memory_order_acquire) |
             g_mouse_buttons_pressed_for_poll) & mask) != 0u;
}

bool physical_keyboard_mouse_neutral() {
    for (const std::atomic_uint64_t& word : g_keyboard_down) {
        if (word.load(std::memory_order_acquire) != 0u) {
            return false;
        }
    }
    return g_mouse_buttons_down.load(std::memory_order_acquire) == 0u;
}

bool keyboard_binding_down(uint16_t binding) {
    using namespace bumble::input_bindings;
    if (binding == 0u) {
        return false;
    }
    if (is_mouse_binding(binding)) {
        return mouse_button_down(mouse_button_from_binding(binding));
    }
    return key_down(static_cast<int>(binding));
}

bool keyboard_action_down(
    const bumble::input_bindings::Settings& settings,
    bumble::input_bindings::InputAction action,
    bool include_mouse_bindings = true
) {
    const auto& bindings =
        settings.keyboard_mouse[static_cast<size_t>(action)];
    return std::any_of(
        bindings.begin(),
        bindings.end(),
        [include_mouse_bindings](uint16_t binding) {
            return (include_mouse_bindings ||
                    !bumble::input_bindings::is_mouse_binding(binding)) &&
                keyboard_binding_down(binding);
        }
    );
}

const char* replay_control_name(ReplayControl control) {
    switch (control) {
    case ReplayControl::A:
        return "A";
    case ReplayControl::B:
        return "B";
    case ReplayControl::Z:
        return "Z";
    case ReplayControl::Start:
        return "START";
    case ReplayControl::StickUp:
        return "STICK_U";
    case ReplayControl::StickDown:
        return "STICK_D";
    case ReplayControl::StickLeft:
        return "STICK_L";
    case ReplayControl::StickRight:
        return "STICK_R";
    case ReplayControl::DpadUp:
        return "DPAD_U";
    case ReplayControl::DpadDown:
        return "DPAD_D";
    case ReplayControl::DpadLeft:
        return "DPAD_L";
    case ReplayControl::DpadRight:
        return "DPAD_R";
    }
    return "UNKNOWN";
}

ReplayControl parse_replay_control(
    const std::string& value,
    bool allow_extended_directions = false
) {
    if (value == "A") return ReplayControl::A;
    if (value == "B") return ReplayControl::B;
    if (value == "Z") return ReplayControl::Z;
    if (value == "START") return ReplayControl::Start;
    if (value == "STICK_U") return ReplayControl::StickUp;
    if (allow_extended_directions && value == "STICK_D") return ReplayControl::StickDown;
    if (allow_extended_directions && value == "STICK_L") return ReplayControl::StickLeft;
    if (allow_extended_directions && value == "STICK_R") return ReplayControl::StickRight;
    if (allow_extended_directions && value == "DPAD_U") return ReplayControl::DpadUp;
    if (allow_extended_directions && value == "DPAD_D") return ReplayControl::DpadDown;
    if (allow_extended_directions && value == "DPAD_L") return ReplayControl::DpadLeft;
    if (allow_extended_directions && value == "DPAD_R") return ReplayControl::DpadRight;
    throw std::runtime_error("unsupported replay control: " + value);
}

const char* connected_pak_name(ultramodern::input::Pak connected_pak) {
    using Pak = ultramodern::input::Pak;
    switch (connected_pak) {
    case Pak::None:
        return "None";
    case Pak::RumblePak:
        return "RumblePak";
    case Pak::ControllerPak:
        return "ControllerPak";
    }
    return "Unknown";
}

ultramodern::input::Pak parse_connected_pak(
    const std::string& value,
    bool allow_none = false
) {
    using Pak = ultramodern::input::Pak;
    if (allow_none && value == "None") return Pak::None;
    if (value == "RumblePak") return Pak::RumblePak;
    if (value == "ControllerPak") return Pak::ControllerPak;
    throw std::runtime_error("unsupported replay connected_pak: " + value);
}

uint32_t parse_replay_u32(const nlohmann::json& value, const char* field) {
    uint64_t parsed = 0;
    if (value.is_string()) {
        size_t consumed = 0;
        const std::string text = value.get<std::string>();
        parsed = std::stoull(text, &consumed, 0);
        if (consumed != text.size()) {
            throw std::runtime_error(std::string("invalid replay integer in ") + field);
        }
    } else if (value.is_number_unsigned()) {
        parsed = value.get<uint64_t>();
    } else if (value.is_number_integer()) {
        const int64_t signed_value = value.get<int64_t>();
        if (signed_value < 0) {
            throw std::runtime_error(std::string("negative replay integer in ") + field);
        }
        parsed = static_cast<uint64_t>(signed_value);
    } else {
        throw std::runtime_error(std::string("non-integer replay field: ") + field);
    }
    if (parsed > UINT32_MAX) {
        throw std::runtime_error(std::string("replay integer out of range in ") + field);
    }
    return static_cast<uint32_t>(parsed);
}

void apply_replay_control(ControllerSnapshot& snapshot, ReplayControl control, bool down) {
    const auto apply_button = [&](uint16_t mask) {
        if (down) {
            snapshot.buttons |= mask;
        } else {
            snapshot.buttons &= static_cast<uint16_t>(~mask);
        }
    };

    switch (control) {
    case ReplayControl::A:
        apply_button(kButtonA);
        break;
    case ReplayControl::B:
        apply_button(kButtonB);
        break;
    case ReplayControl::Z:
        apply_button(kButtonZ);
        break;
    case ReplayControl::Start:
        apply_button(kButtonStart);
        break;
    case ReplayControl::StickUp:
        snapshot.y = down ? kReplayStickMagnitude : 0.0f;
        break;
    case ReplayControl::StickDown:
        snapshot.y = down ? -kReplayStickMagnitude : 0.0f;
        break;
    case ReplayControl::StickLeft:
        snapshot.x = down ? -kReplayStickMagnitude : 0.0f;
        break;
    case ReplayControl::StickRight:
        snapshot.x = down ? kReplayStickMagnitude : 0.0f;
        break;
    case ReplayControl::DpadUp:
        apply_button(kDpadUp);
        break;
    case ReplayControl::DpadDown:
        apply_button(kDpadDown);
        break;
    case ReplayControl::DpadLeft:
        apply_button(kDpadLeft);
        break;
    case ReplayControl::DpadRight:
        apply_button(kDpadRight);
        break;
    }
}

bool replay_control_is_masked(ReplayControl control) {
    return g_replay.masked_control.has_value() && *g_replay.masked_control == control;
}

bool replay_owns_controller(size_t controller) {
    if (!g_replay.configured) {
        return false;
    }
    if (g_replay.schema_version == 1 || g_replay.schema_version == 3 ||
        g_replay.schema_version == 4 || g_replay.schema_version == 5) {
        return controller == 0;
    }
    return controller < g_replay.replay_controller_count;
}

bool load_replay_manifest_locked(const char* manifest_path, const char* masked_control) {
    try {
        std::ifstream input(manifest_path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("could not open replay manifest");
        }

        nlohmann::json document;
        input >> document;
        const int schema_version = document.at("schema_version").get<int>();
        if (schema_version != 1 && schema_version != 2 && schema_version != 3 &&
            schema_version != 4 && schema_version != 5) {
            throw std::runtime_error(
                "replay schema_version must be 1, 2, 3, 4, or 5"
            );
        }
        if (document.at("clock").at("kind").get<std::string>() != "native_input_poll" ||
            document.at("clock").at("origin").get<std::string>() !=
                "first_osContStartReadData_after_game_init") {
            throw std::runtime_error("replay clock identity is not native input-poll origin");
        }

        ReplayState loaded{};
        loaded.schema_version = schema_version;
        loaded.configured = true;
        loaded.manifest_path = manifest_path;
        loaded.replay_connected_paks.fill(ultramodern::input::Pak::None);

        if (schema_version == 1 || schema_version == 3 ||
            schema_version == 4 || schema_version == 5) {
            if (document.at("controller").get<int>() != 0 ||
                !document.at("physical_input_suppressed").get<bool>()) {
                throw std::runtime_error("replay must exclusively own native controller 0");
            }
            loaded.replay_controller_count = 1;
            loaded.snapshot.connected = true;
            const std::string connected_pak = document.at("connected_pak").get<std::string>();
            loaded.replay_connected_pak = parse_connected_pak(connected_pak);
            loaded.replay_connected_paks[0] = loaded.replay_connected_pak;
            loaded.snapshot.connected_pak = loaded.replay_connected_pak;
            loaded.require_mission1_checkpoint =
                schema_version == 1 || schema_version == 4
                ? document.at("require_mission1_checkpoint").get<bool>()
                : false;
            loaded.require_mission2_checkpoint =
                schema_version == 3 || schema_version == 5
                ? document.at("require_mission2_checkpoint").get<bool>()
                : false;
            if (schema_version == 1 && loaded.require_mission1_checkpoint &&
                loaded.replay_connected_pak !=
                    ultramodern::input::Pak::ControllerPak) {
                throw std::runtime_error(
                    "Level Select Mission 1 replay requires ControllerPak"
                );
            }
            if (schema_version == 3 &&
                loaded.replay_connected_pak != ultramodern::input::Pak::RumblePak) {
                throw std::runtime_error(
                    "campaign progression replay connected_pak must be RumblePak"
                );
            }
            if (schema_version == 4 &&
                (!loaded.require_mission1_checkpoint ||
                 loaded.replay_connected_pak !=
                    ultramodern::input::Pak::ControllerPak)) {
                throw std::runtime_error(
                    "normal Mission 1 completion replay requires ControllerPak "
                    "and the exact Mission 1 checkpoint"
                );
            }
            if (schema_version == 5 &&
                (!loaded.require_mission2_checkpoint ||
                 loaded.replay_connected_pak !=
                    ultramodern::input::Pak::ControllerPak)) {
                throw std::runtime_error(
                    "saved-profile Mission 2 resume replay requires "
                    "ControllerPak and the exact Mission 2 checkpoint"
                );
            }
        } else {
            if (!document.at("physical_input_suppressed").get<bool>()) {
                throw std::runtime_error(
                    "replay schema 2 must suppress all physical controller input"
                );
            }
            const nlohmann::json& controllers = document.at("controllers");
            if (!controllers.is_array() || controllers.size() != kReplayControllerCount) {
                throw std::runtime_error(
                    "replay schema 2 must exclusively own native controllers 0 and 1"
                );
            }
            loaded.replay_controller_count = kReplayControllerCount;
            for (size_t controller = 0; controller < kReplayControllerCount; ++controller) {
                const nlohmann::json& descriptor = controllers.at(controller);
                if (descriptor.at("index").get<size_t>() != controller) {
                    throw std::runtime_error(
                        "replay schema 2 controller indices must be ordered 0 and 1"
                    );
                }
                const ultramodern::input::Pak connected_pak = parse_connected_pak(
                    descriptor.at("connected_pak").get<std::string>(),
                    true
                );
                loaded.replay_connected_paks[controller] = connected_pak;
                loaded.snapshots[controller] = ControllerSnapshot{
                    .connected = true,
                    .connected_pak = connected_pak,
                };
            }
            if (loaded.replay_connected_paks[0] != ultramodern::input::Pak::RumblePak ||
                loaded.replay_connected_paks[1] != ultramodern::input::Pak::None) {
                throw std::runtime_error(
                    "replay schema 2 requires RumblePak on controller 0 and None on controller 1"
                );
            }
            loaded.require_two_player_checkpoint =
                document.at("require_two_player_checkpoint").get<bool>();
            if (!loaded.require_two_player_checkpoint) {
                throw std::runtime_error(
                    "replay schema 2 must require the two-player checkpoint"
                );
            }
        }

        loaded.end_tick = document.at("end_tick").get<uint64_t>();
        if (loaded.end_tick == 0 || loaded.end_tick > 1000000) {
            throw std::runtime_error("replay end_tick is out of range");
        }
        if (schema_version == 1) {
            loaded.success_earliest_tick =
                document.at("success_earliest_tick").get<uint64_t>();
            if (loaded.success_earliest_tick == 0 ||
                loaded.success_earliest_tick >= loaded.end_tick) {
                throw std::runtime_error(
                    "schema 1 replay success boundary must precede end_tick"
                );
            }
        }

        const std::string mask = masked_control == nullptr ? "" : masked_control;
        if (!mask.empty() && mask != "NONE") {
            loaded.masked_control = parse_replay_control(mask, schema_version != 1);
        }
        if (schema_version == 3 && loaded.masked_control.has_value() &&
            *loaded.masked_control != ReplayControl::DpadLeft) {
            throw std::runtime_error(
                "later progression replay permits only the selective DPAD_L control mask"
            );
        }
        if (schema_version == 5 && loaded.masked_control.has_value()) {
            throw std::runtime_error(
                "saved-profile Mission 2 resume replay does not permit a control mask"
            );
        }

        if (schema_version == 1 || schema_version == 3 ||
            schema_version == 4 || schema_version == 5) {
            uint64_t previous_tick = 0;
            std::array<bool, kReplayControlCount> scheduled_down{};
            for (const nlohmann::json& item : document.at("scheduled_events")) {
                ScheduledReplayEvent event{};
                event.tick = item.at("tick").get<uint64_t>();
                event.down = item.at("op").get<std::string>() == "down";
                if (!event.down && item.at("op").get<std::string>() != "up") {
                    throw std::runtime_error("scheduled replay op must be down or up");
                }
                if (item.at("order").get<int>() != 0 || event.tick == 0 ||
                    event.tick <= previous_tick || event.tick > loaded.end_tick) {
                    throw std::runtime_error("scheduled replay events are not strictly ordered");
                }
                event.control = parse_replay_control(
                    item.at("control").get<std::string>(),
                    schema_version == 1 || schema_version == 3 ||
                        schema_version == 4
                );
                const size_t control_index = static_cast<size_t>(event.control);
                if (event.down == scheduled_down[control_index]) {
                    throw std::runtime_error("scheduled replay control transitions are unbalanced");
                }
                scheduled_down[control_index] = event.down;
                previous_tick = event.tick;
                loaded.scheduled_events.push_back(event);
            }
            for (bool down : scheduled_down) {
                if (down) {
                    throw std::runtime_error("scheduled replay leaves a control held");
                }
            }
        } else {
            uint64_t previous_tick = 0;
            int previous_order = -1;
            std::array<std::array<bool, kReplayControlCount>, kReplayControllerCount>
                scheduled_down{};
            for (const nlohmann::json& item : document.at("scheduled_events")) {
                ScheduledReplayEvent event{};
                event.tick = item.at("tick").get<uint64_t>();
                const int order = item.at("order").get<int>();
                const int controller = item.at("controller").get<int>();
                if (controller < 0 ||
                    controller >= static_cast<int>(kReplayControllerCount)) {
                    throw std::runtime_error(
                        "scheduled replay controller must be 0 or 1"
                    );
                }
                event.controller = static_cast<size_t>(controller);
                const std::string operation = item.at("op").get<std::string>();
                event.down = operation == "down";
                if (!event.down && operation != "up") {
                    throw std::runtime_error("scheduled replay op must be down or up");
                }
                if (event.tick == 0 || event.tick > loaded.end_tick || order < 0 ||
                    event.tick < previous_tick ||
                    (event.tick == previous_tick && order <= previous_order)) {
                    throw std::runtime_error("scheduled replay events are not strictly ordered");
                }
                if (event.tick != previous_tick) {
                    previous_order = -1;
                }
                event.control = parse_replay_control(
                    item.at("control").get<std::string>(),
                    true
                );
                const size_t control_index = static_cast<size_t>(event.control);
                if (event.down == scheduled_down[event.controller][control_index]) {
                    throw std::runtime_error("scheduled replay control transitions are unbalanced");
                }
                scheduled_down[event.controller][control_index] = event.down;
                previous_tick = event.tick;
                previous_order = order;
                loaded.scheduled_events.push_back(event);
            }
            for (const auto& controller_controls : scheduled_down) {
                for (bool down : controller_controls) {
                    if (down) {
                        throw std::runtime_error("scheduled replay leaves a control held");
                    }
                }
            }
        }

        uint64_t previous_reference = 0;
        for (const nlohmann::json& item : document.at("game_owned_events")) {
            GameOwnedReplayEvent event{};
            if (schema_version == 2) {
                const int controller = item.at("controller").get<int>();
                if (controller < 0 ||
                    controller >= static_cast<int>(kReplayControllerCount)) {
                    throw std::runtime_error("game-owned replay controller must be 0 or 1");
                }
                event.controller = static_cast<size_t>(controller);
            }
            event.control = parse_replay_control(
                item.at("control").get<std::string>(),
                schema_version == 2
            );
            event.observed_phase = parse_replay_u32(item.at("observed_phase"), "observed_phase");
            event.reference_tick = item.at("reference_tick").get<uint64_t>();
            event.deadline_tick = item.at("deadline_tick").get<uint64_t>();
            const bool accepted_frontend_control =
                event.control == ReplayControl::A ||
                (schema_version == 1 &&
                 event.control == ReplayControl::B &&
                 event.observed_phase == 0x00000018u);
            if (!accepted_frontend_control ||
                event.reference_tick <= previous_reference ||
                event.deadline_tick < event.reference_tick ||
                event.deadline_tick >= loaded.end_tick) {
                throw std::runtime_error("game-owned replay events have invalid ordering or deadline");
            }
            previous_reference = event.reference_tick;
            loaded.game_owned_events.push_back(event);
        }
        if (loaded.game_owned_events.empty()) {
            throw std::runtime_error("replay has no game-owned event sequence");
        }

        if (schema_version == 2) {
            const nlohmann::json& selection = document.at("selection_event");
            MenuSelectionReplayEvent event{};
            const int controller = selection.at("controller").get<int>();
            if (controller < 0 ||
                controller >= static_cast<int>(kReplayControllerCount)) {
                throw std::runtime_error("selection replay controller must be 0 or 1");
            }
            event.controller = static_cast<size_t>(controller);
            event.control = parse_replay_control(
                selection.at("control").get<std::string>(),
                true
            );
            event.observed_phase = parse_replay_u32(
                selection.at("observed_phase"),
                "selection observed_phase"
            );
            event.descriptor = parse_replay_u32(
                selection.at("descriptor"),
                "selection descriptor"
            );
            event.from_item = parse_replay_u32(
                selection.at("from_item"),
                "selection from_item"
            );
            event.to_item = parse_replay_u32(
                selection.at("to_item"),
                "selection to_item"
            );
            event.deadline_tick = selection.at("deadline_tick").get<uint64_t>();
            if (event.controller != 0 || event.control != ReplayControl::StickDown ||
                event.observed_phase != 0x0C || event.descriptor != 0x800FC940 ||
                event.from_item != 0x800FC820 || event.to_item != 0x800FC848 ||
                event.deadline_tick == 0 || event.deadline_tick >= loaded.end_tick) {
                throw std::runtime_error("selection replay event does not match the reviewed two-player route");
            }
            loaded.selection_event = event;

            const nlohmann::json& probe = document.at("two_player_probe");
            TwoPlayerReplayProbe replay_probe{};
            const int probe_controller = probe.at("controller").get<int>();
            if (probe_controller < 0 ||
                probe_controller >= static_cast<int>(kReplayControllerCount)) {
                throw std::runtime_error("two-player probe controller must be 0 or 1");
            }
            replay_probe.controller = static_cast<size_t>(probe_controller);
            replay_probe.control = parse_replay_control(
                probe.at("control").get<std::string>(),
                true
            );
            replay_probe.not_before_tick = probe.at("not_before_tick").get<uint64_t>();
            replay_probe.hold_polls = probe.at("hold_polls").get<uint64_t>();
            if (replay_probe.controller != 1 ||
                replay_probe.control != ReplayControl::StickUp ||
                replay_probe.not_before_tick != 1550 ||
                replay_probe.hold_polls != 30) {
                throw std::runtime_error(
                    "two-player replay probe must hold controller 1 STICK_U for 30 polls at or after tick 1550"
                );
            }
            loaded.two_player_probe = replay_probe;
        }

        if (schema_version == 3) {
            const nlohmann::json& route = document.at("later_progression");
            LaterProgressionReplay later{};
            later.target_level_index = route.value(
                "target_level_index",
                2u
            );
            later.cheat_phase = parse_replay_u32(
                route.at("cheat_phase"),
                "later progression cheat_phase"
            );
            later.cheat_deadline_tick =
                route.at("cheat_deadline_tick").get<uint64_t>();
            later.selector_deadline_tick =
                route.at("selector_deadline_tick").get<uint64_t>();
            later.commit_deadline_tick =
                route.at("commit_deadline_tick").get<uint64_t>();
            later.player_deadline_tick =
                route.at("player_deadline_tick").get<uint64_t>();

            constexpr std::array<ReplayControl, 12> expected_sequence{
                ReplayControl::DpadRight,
                ReplayControl::DpadDown,
                ReplayControl::DpadDown,
                ReplayControl::DpadRight,
                ReplayControl::DpadRight,
                ReplayControl::DpadUp,
                ReplayControl::DpadDown,
                ReplayControl::DpadLeft,
                ReplayControl::DpadLeft,
                ReplayControl::DpadUp,
                ReplayControl::DpadRight,
                ReplayControl::DpadRight,
            };
            const nlohmann::json& steps = route.at("cheat_sequence");
            if (!steps.is_array() || steps.size() != expected_sequence.size()) {
                throw std::runtime_error(
                    "later progression cheat sequence must contain exactly 12 steps"
                );
            }
            for (size_t index = 0; index < expected_sequence.size(); ++index) {
                const nlohmann::json& step = steps.at(index);
                const ReplayControl control = parse_replay_control(
                    step.at("control").get<std::string>(),
                    true
                );
                const bool hold_z = step.at("hold_z").get<bool>();
                if (control != expected_sequence[index] || hold_z != (index < 4)) {
                    throw std::runtime_error(
                        "later progression cheat sequence does not match reviewed game table"
                    );
                }
                later.cheat_sequence.push_back(control);
            }
            if (parse_replay_control(
                    route.at("mission_selection_control").get<std::string>(),
                    true) != ReplayControl::StickUp ||
                later.cheat_phase != 0x0Cu ||
                later.cheat_deadline_tick == 0 ||
                later.selector_deadline_tick <= later.cheat_deadline_tick ||
                later.commit_deadline_tick <= later.selector_deadline_tick ||
                later.player_deadline_tick <= later.commit_deadline_tick ||
                later.player_deadline_tick >= loaded.end_tick ||
                !directly_selectable_campaign_index(
                    later.target_level_index
                ) ||
                (loaded.require_mission2_checkpoint &&
                 later.target_level_index != 2u)) {
                throw std::runtime_error(
                    "Invalid campaign replay route or deadlines."
                );
            }
            if (loaded.game_owned_events.size() != 4u ||
                loaded.game_owned_events.back().observed_phase != 0x0Bu) {
                throw std::runtime_error(
                    "Campaign replay must start after the four title confirmations."
                );
            }
            loaded.later_progression = std::move(later);
        }

        if (schema_version == 4) {
            const nlohmann::json& route = document.at("mission1_completion");
            MissionOneCompletionReplay completion{};
            completion.withhold_one_owner =
                route.at("withhold_one_owner").get<bool>();
            completion.require_profile_save =
                route.at("require_profile_save").get<bool>();
            completion.require_mission2_checkpoint =
                route.at("require_mission2_checkpoint").get<bool>();
            completion.completion_deadline_tick =
                route.at("completion_deadline_tick").get<uint64_t>();
            constexpr std::array<uint32_t, 8> kExpectedMissionOnePhases{
                0x08u, 0x09u, 0x0Au, 0x0Bu, 0x0Cu, 0x0Eu, 0x18u, 0x19u,
            };
            bool exact_phases = loaded.game_owned_events.size() ==
                kExpectedMissionOnePhases.size();
            if (exact_phases) {
                for (size_t index = 0;
                     index < kExpectedMissionOnePhases.size(); ++index) {
                    exact_phases = exact_phases &&
                        loaded.game_owned_events[index].observed_phase ==
                            kExpectedMissionOnePhases[index];
                }
            }
            const bool exact_native_menu_entry =
                exact_phases && loaded.scheduled_events.size() == 4u &&
                loaded.scheduled_events[0].control == ReplayControl::A &&
                loaded.scheduled_events[0].down &&
                loaded.scheduled_events[1].control == ReplayControl::A &&
                !loaded.scheduled_events[1].down &&
                loaded.scheduled_events[2].control == ReplayControl::StickDown &&
                loaded.scheduled_events[2].down &&
                loaded.scheduled_events[3].control == ReplayControl::StickDown &&
                !loaded.scheduled_events[3].down &&
                loaded.scheduled_events[3].tick <
                    loaded.game_owned_events[4].reference_tick;
            if (!exact_phases || completion.require_profile_save ||
                 !completion.require_mission2_checkpoint ||
                 completion.completion_deadline_tick <=
                    loaded.game_owned_events.back().deadline_tick ||
                 completion.completion_deadline_tick > loaded.end_tick ||
                 !exact_native_menu_entry ||
                 loaded.masked_control.has_value()) {
                throw std::runtime_error(
                    "Invalid Mission 1 completion replay route."
                );
            }
            loaded.mission1_completion = completion;
        }

        if (schema_version == 5) {
            const nlohmann::json& selection = document.at("selection_event");
            MenuSelectionReplayEvent selection_event{};
            selection_event.controller = static_cast<size_t>(
                selection.at("controller").get<int>()
            );
            selection_event.control = parse_replay_control(
                selection.at("control").get<std::string>(),
                true
            );
            selection_event.observed_phase = parse_replay_u32(
                selection.at("observed_phase"),
                "saved-profile selection observed_phase"
            );
            selection_event.descriptor = parse_replay_u32(
                selection.at("descriptor"),
                "saved-profile selection descriptor"
            );
            selection_event.from_item = parse_replay_u32(
                selection.at("from_item"),
                "saved-profile selection from_item"
            );
            selection_event.to_item = parse_replay_u32(
                selection.at("to_item"),
                "saved-profile selection to_item"
            );
            selection_event.deadline_tick =
                selection.at("deadline_tick").get<uint64_t>();
            if (selection_event.controller != 0u ||
                selection_event.control != ReplayControl::StickDown ||
                selection_event.observed_phase != 0x0Cu ||
                selection_event.descriptor != 0x800FC940u ||
                selection_event.from_item != 0x800FC820u ||
                selection_event.to_item != 0x800FC870u ||
                selection_event.deadline_tick == 0u ||
                selection_event.deadline_tick >= loaded.end_tick) {
                throw std::runtime_error(
                    "saved-profile replay selection must hold STICK_D from "
                    "the main-menu campaign item to Buck's Load Game item"
                );
            }
            loaded.selection_event = selection_event;

            const nlohmann::json& route = document.at("profile_resume");
            ProfileResumeReplay resume{};
            resume.completion_deadline_tick =
                route.at("completion_deadline_tick").get<uint64_t>();
            constexpr std::array<uint32_t, 8> kExpectedResumePhases{
                0x08u, 0x09u, 0x0Au, 0x0Bu, 0x0Cu, 0x16u, 0x18u, 0x19u,
            };
            bool exact_phases = loaded.game_owned_events.size() ==
                kExpectedResumePhases.size();
            if (exact_phases) {
                for (size_t index = 0; index < kExpectedResumePhases.size(); ++index) {
                    exact_phases = exact_phases &&
                        loaded.game_owned_events[index].observed_phase ==
                            kExpectedResumePhases[index];
                }
            }
            if (!exact_phases || !loaded.require_mission2_checkpoint ||
                resume.completion_deadline_tick <=
                    loaded.game_owned_events.back().deadline_tick ||
                resume.completion_deadline_tick > loaded.end_tick ||
                selection_event.deadline_tick >=
                    loaded.game_owned_events[4].deadline_tick ||
                !loaded.scheduled_events.empty()) {
                throw std::runtime_error(
                    "Invalid saved-profile resume replay route."
                );
            }
            loaded.profile_resume = resume;
        }

        uint64_t previous_observation = 0;
        for (const nlohmann::json& item : document.at("observations")) {
            ReplayObservation observation{};
            observation.tick = item.at("tick").get<uint64_t>();
            observation.label = item.at("label").get<std::string>();
            if (observation.tick == 0 || observation.tick <= previous_observation ||
                observation.tick >= loaded.end_tick || observation.label.empty()) {
                throw std::runtime_error("replay observations are invalid or unordered");
            }
            previous_observation = observation.tick;
            loaded.observations.push_back(std::move(observation));
        }

        g_replay = std::move(loaded);
        g_replay_configured.store(true, std::memory_order_release);
        g_replay_complete.store(false, std::memory_order_release);
        g_replay_semantic_success.store(false, std::memory_order_release);
        g_replay_tick.store(0, std::memory_order_release);
        g_replay_completed_game_events.store(0, std::memory_order_release);
        bumble::native_checkpoint::configure_mission1_completion_replay(
            schema_version == 4,
            schema_version == 4 &&
                g_replay.mission1_completion->withhold_one_owner
        );
        if (schema_version == 1) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_config_loaded path=%s clock=native_input_poll"
                " physical_input_suppressed=1 scheduled_events=%zu game_owned_events=%zu"
                " observations=%zu success_earliest_tick=%llu"
                " end_tick=%llu connected_pak=%s masked_control=%s\n",
                g_replay.manifest_path.c_str(),
                g_replay.scheduled_events.size(),
                g_replay.game_owned_events.size(),
                g_replay.observations.size(),
                static_cast<unsigned long long>(
                    g_replay.success_earliest_tick
                ),
                static_cast<unsigned long long>(g_replay.end_tick),
                connected_pak_name(g_replay.replay_connected_pak),
                g_replay.masked_control.has_value()
                    ? replay_control_name(*g_replay.masked_control)
                    : "NONE"
            );
        } else if (schema_version == 2) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_config_loaded path=%s clock=native_input_poll"
                " schema=2 controllers=2 physical_input_suppressed=1 scheduled_events=%zu"
                " game_owned_events=%zu observations=%zu end_tick=%llu"
                " controller0_pak=%s controller1_pak=%s selection=1 probe=1"
                " require_two_player=1 masked_control=%s\n",
                g_replay.manifest_path.c_str(),
                g_replay.scheduled_events.size(),
                g_replay.game_owned_events.size(),
                g_replay.observations.size(),
                static_cast<unsigned long long>(g_replay.end_tick),
                connected_pak_name(g_replay.replay_connected_paks[0]),
                connected_pak_name(g_replay.replay_connected_paks[1]),
                g_replay.masked_control.has_value()
                    ? replay_control_name(*g_replay.masked_control)
                    : "NONE"
            );
        } else if (schema_version == 3) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_config_loaded path=%s clock=native_input_poll"
                " schema=3 controllers=1 physical_input_suppressed=1 scheduled_events=%zu"
                " game_owned_events=%zu observations=%zu end_tick=%llu"
                " controller0_pak=%s level_select_steps=12"
                " target_level=%" PRIu32 " require_mission2=%d"
                " masked_control=%s\n",
                g_replay.manifest_path.c_str(),
                g_replay.scheduled_events.size(),
                g_replay.game_owned_events.size(),
                g_replay.observations.size(),
                static_cast<unsigned long long>(g_replay.end_tick),
                connected_pak_name(g_replay.replay_connected_pak),
                g_replay.later_progression->target_level_index,
                g_replay.require_mission2_checkpoint ? 1 : 0,
                g_replay.masked_control.has_value()
                    ? replay_control_name(*g_replay.masked_control)
                    : "NONE"
            );
        } else if (schema_version == 4) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_config_loaded path=%s"
                " clock=native_input_poll schema=4 controllers=1"
                " physical_input_suppressed=1 scheduled_events=%zu"
                " game_owned_events=%zu observations=%zu end_tick=%llu"
                " controller0_pak=%s normal_mission1_completion=1"
                " withhold_one_owner=%d completion_deadline_tick=%llu"
                " require_profile_save=%d require_mission2=%d"
                " counter_result_level_writes=0\n",
                g_replay.manifest_path.c_str(),
                g_replay.scheduled_events.size(),
                g_replay.game_owned_events.size(),
                g_replay.observations.size(),
                static_cast<unsigned long long>(g_replay.end_tick),
                connected_pak_name(g_replay.replay_connected_pak),
                g_replay.mission1_completion->withhold_one_owner ? 1 : 0,
                static_cast<unsigned long long>(
                    g_replay.mission1_completion->completion_deadline_tick
                ),
                g_replay.mission1_completion->require_profile_save ? 1 : 0,
                g_replay.mission1_completion->require_mission2_checkpoint
                    ? 1
                    : 0
            );
        } else {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_config_loaded path=%s"
                " clock=native_input_poll schema=5 controllers=1"
                " physical_input_suppressed=1 scheduled_events=%zu"
                " game_owned_events=%zu observations=%zu end_tick=%llu"
                " controller0_pak=%s saved_profile_resume=1"
                " require_profile_reopen=1 require_mission2=1"
                " completion_deadline_tick=%llu guest_state_writes=0\n",
                g_replay.manifest_path.c_str(),
                g_replay.scheduled_events.size(),
                g_replay.game_owned_events.size(),
                g_replay.observations.size(),
                static_cast<unsigned long long>(g_replay.end_tick),
                connected_pak_name(g_replay.replay_connected_pak),
                static_cast<unsigned long long>(
                    g_replay.profile_resume->completion_deadline_tick
                )
            );
        }
        std::fflush(stderr);
        return true;
    } catch (const std::exception& error) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_config_failed error=%s\n",
            error.what()
        );
        std::fflush(stderr);
        return false;
    }
}

void advance_schema1_replay_locked() {
    if (!g_replay.configured) {
        return;
    }
    if (!g_replay.armed) {
        g_replay.snapshot = ControllerSnapshot{
            .connected = true,
            .connected_pak = g_replay.replay_connected_pak,
        };
        return;
    }

    ++g_replay.tick;
    g_replay_tick.store(g_replay.tick, std::memory_order_release);

    while (g_replay.scheduled_cursor < g_replay.scheduled_events.size() &&
           g_replay.scheduled_events[g_replay.scheduled_cursor].tick == g_replay.tick) {
        const ScheduledReplayEvent& event =
            g_replay.scheduled_events[g_replay.scheduled_cursor++];
        const bool masked = replay_control_is_masked(event.control);
        if (!masked) {
            apply_replay_control(g_replay.snapshot, event.control, event.down);
        }
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_event tick=%llu source=scheduled"
            " op=%s control=%s applied=%d\n",
            static_cast<unsigned long long>(g_replay.tick),
            event.down ? "down" : "up",
            replay_control_name(event.control),
            masked ? 0 : 1
        );
        std::fflush(stderr);
    }

    bool released_game_event = false;
    const bool descriptor_selector_pending =
        (g_replay.schema_version == 4 &&
         g_replay.mission1_completion.has_value() &&
         g_replay.game_event_cursor >= 6u &&
         !g_replay.mission1_completion->selector_acked) ||
        (g_replay.schema_version == 5 &&
         g_replay.profile_resume.has_value() &&
         g_replay.game_event_cursor >= 6u &&
         !g_replay.profile_resume->selector_acked);
    const bool profile_load_selection_pending =
        g_replay.schema_version == 5 &&
        g_replay.profile_resume.has_value() &&
        g_replay.selection_event.has_value() &&
        g_replay.game_event_cursor == 4u &&
        !g_replay.selection_event->acknowledged;
    if (!descriptor_selector_pending && !profile_load_selection_pending &&
        g_replay.game_event_cursor < g_replay.game_owned_events.size()) {
        GameOwnedReplayEvent& event =
            g_replay.game_owned_events[g_replay.game_event_cursor];
        if (replay_control_is_masked(event.control)) {
            if (g_replay.tick == 1) {
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_game_sequence_masked control=%s count=%zu\n",
                    replay_control_name(event.control),
                    g_replay.game_owned_events.size()
                );
                std::fflush(stderr);
            }
        } else if (g_replay.tick > event.deadline_tick) {
            if (!event.deadline_reported) {
                event.deadline_reported = true;
                g_replay.deadline_missed = true;
                const bool button_was_down = g_replay.game_event_button_down;
                if (button_was_down) {
                    apply_replay_control(g_replay.snapshot, event.control, false);
                    g_replay.game_event_button_down = false;
                    released_game_event = true;
                }
                const uint32_t phase =
                    bumble::native_checkpoint::last_frontend_phase();
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_game_event_deadline_missed tick=%llu"
                    " control=%s expected_phase=0x%08" PRIX32 " last_phase=0x%08" PRIX32
                    " deadline_tick=%llu button_was_down=%d\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    replay_control_name(event.control),
                    event.observed_phase,
                    phase,
                    static_cast<unsigned long long>(event.deadline_tick),
                    button_was_down ? 1 : 0
                );
                std::fflush(stderr);
            }
        } else if (g_replay.game_event_button_down) {
            const uint32_t confirm_count =
                bumble::native_checkpoint::frontend_confirm_count();
            const bool campaign_commit =
                event.observed_phase == 0x0000000Eu &&
                bumble::native_checkpoint::campaign_selection_committed_index() !=
                    0u;
            const uint32_t accepted_phase = campaign_commit
                ? event.observed_phase
                : bumble::native_checkpoint::last_accepted_frontend_phase();
            if (confirm_count > g_replay.game_event_confirm_baseline ||
                campaign_commit) {
                apply_replay_control(g_replay.snapshot, event.control, false);
                g_replay.game_event_button_down = false;
                released_game_event = true;
                const bool phase_matches = accepted_phase == event.observed_phase;
                if (!phase_matches) {
                    g_replay.deadline_missed = true;
                }
                ++g_replay.game_event_cursor;
                g_replay_completed_game_events.store(
                    static_cast<uint32_t>(g_replay.game_event_cursor),
                    std::memory_order_release
                );
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_game_event_ack tick=%llu control=%s"
                    " accepted_count=%" PRIu32 " expected_phase=0x%08" PRIX32
                    " accepted_phase=0x%08" PRIX32 " phase_matches=%d op=up\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    replay_control_name(event.control),
                    confirm_count,
                    event.observed_phase,
                    accepted_phase,
                    phase_matches ? 1 : 0
                );
                std::fflush(stderr);
            }
        } else {
            const uint32_t phase = bumble::native_checkpoint::last_frontend_phase();
            if (phase == event.observed_phase &&
                g_replay.tick >= event.reference_tick &&
                !released_game_event) {
                g_replay.game_event_confirm_baseline =
                    bumble::native_checkpoint::frontend_confirm_count();
                apply_replay_control(g_replay.snapshot, event.control, true);
                g_replay.game_event_button_down = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_game_event_request tick=%llu control=%s"
                    " observed_phase=0x%08" PRIX32 " reference_tick=%llu"
                    " deadline_tick=%llu op=down\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    replay_control_name(event.control),
                    phase,
                    static_cast<unsigned long long>(event.reference_tick),
                    static_cast<unsigned long long>(event.deadline_tick)
                );
                std::fflush(stderr);
            }
        }
    }

    while (g_replay.observation_cursor < g_replay.observations.size() &&
           g_replay.observations[g_replay.observation_cursor].tick == g_replay.tick) {
        const ReplayObservation& observation =
            g_replay.observations[g_replay.observation_cursor++];
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_observation tick=%llu label=%s"
            " frontend_accepts=%" PRIu32 " frontend_phase=0x%08" PRIX32
            " mission1_player=%d\n",
            static_cast<unsigned long long>(g_replay.tick),
            observation.label.c_str(),
            bumble::native_checkpoint::frontend_confirm_count(),
            bumble::native_checkpoint::last_frontend_phase(),
            bumble::native_checkpoint::mission1_player_observed() ? 1 : 0
        );
        std::fflush(stderr);
    }

    if (!g_replay.campaign_grid_exit_clean.value_or(false) &&
        bumble::native_checkpoint::campaign_selection_committed_index() != 0u) {
        const auto observations = bumble::text_overlay::active_observations();
        const bool menu_text_cleared = std::none_of(
            observations.begin(),
            observations.end(),
            [](const bumble::text_overlay::Observation& observation) {
                return observation.kind == bumble::text_overlay::TextKind::Menu;
            }
        );
        const bool clean =
            !bumble::native_checkpoint::campaign_grid_active() &&
            menu_text_cleared;
        if (clean) {
            g_replay.campaign_grid_exit_clean = true;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_campaign_grid_exit"
                " tick=%llu grid_active=0 menu_text_cleared=1 clean=1\n",
                static_cast<unsigned long long>(g_replay.tick)
            );
            std::fflush(stderr);
        }
    }
    const bool legacy_draw_suppressed =
        bumble::widescreen::legacy_level_entry_suppression_count() != 0u;
    const bool native_composition_handoff_completed =
        bumble::graphics_options::main_menu_handoff_completion_count() != 0u;
    if (!g_replay.legacy_level_entry_hidden &&
        (legacy_draw_suppressed || native_composition_handoff_completed)) {
        g_replay.legacy_level_entry_hidden = true;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_legacy_level_entry_hidden"
            " tick=%llu phase=0x00000018 load_state_preserved=1 source=%s\n",
            static_cast<unsigned long long>(g_replay.tick),
            legacy_draw_suppressed
                ? "legacy_draw_suppression"
                : "native_composition_handoff"
        );
        std::fflush(stderr);
    }
    const bool post_commit_phase_not_presented =
        g_replay.campaign_grid_exit_clean.value_or(false) &&
        bumble::native_checkpoint::last_frontend_phase() != 0x0000000Eu;
    if (!g_replay.post_commit_level_select_hidden &&
        (bumble::widescreen::post_commit_level_select_suppression_count() !=
            0u || post_commit_phase_not_presented)) {
        g_replay.post_commit_level_select_hidden = true;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE"
            " stage=replay_post_commit_level_select_hidden"
            " tick=%llu source=%s\n",
            static_cast<unsigned long long>(g_replay.tick),
            post_commit_phase_not_presented
                ? "retained_modern_composition"
                : "legacy_draw_suppression"
        );
        std::fflush(stderr);
    }

    const bool hard_completion_deadline =
        g_replay.tick >= g_replay.end_tick;
    const bool schema_one_success_window =
        g_replay.schema_version == 1 &&
        g_replay.tick >= g_replay.success_earliest_tick;
    if ((hard_completion_deadline || schema_one_success_window) &&
        !g_replay.completion_logged) {
        const uint32_t accepts = bumble::native_checkpoint::frontend_confirm_count();
        const bool masked_a = g_replay.masked_control == ReplayControl::A;
        const bool all_game_events =
            g_replay.game_event_cursor == g_replay.game_owned_events.size();
        const bool mission1 = bumble::native_checkpoint::mission1_player_observed();
        const bool campaign_grid_exit_clean =
            g_replay.campaign_grid_exit_clean.value_or(false);
        const size_t expected_frontend_accepts =
            g_replay.game_owned_events.size() -
            std::count_if(
                g_replay.game_owned_events.begin(),
                g_replay.game_owned_events.end(),
                [](const GameOwnedReplayEvent& event) {
                    return event.observed_phase == 0x0000000Eu;
                }
            );
        g_replay.semantic_success = !g_replay.deadline_missed &&
            (masked_a ? (accepts == 0 && !mission1) :
                         (all_game_events && accepts == expected_frontend_accepts &&
                         campaign_grid_exit_clean &&
                         g_replay.legacy_level_entry_hidden &&
                         g_replay.post_commit_level_select_hidden &&
                         (!g_replay.require_mission1_checkpoint || mission1)));
        if (!g_replay.semantic_success && !hard_completion_deadline) {
            return;
        }
        g_replay.completion_logged = true;
        g_replay_semantic_success.store(g_replay.semantic_success, std::memory_order_release);
        g_replay_complete.store(true, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_complete tick=%llu"
            " completed_game_events=%zu expected_game_events=%zu frontend_accepts=%" PRIu32
            " mission1_player=%d campaign_grid_exit_clean=%d"
            " legacy_level_entry_hidden=%d"
            " post_commit_level_select_hidden=%d"
            " deadline_missed=%d semantic_success=%d masked_control=%s\n",
            static_cast<unsigned long long>(g_replay.tick),
            g_replay.game_event_cursor,
            g_replay.game_owned_events.size(),
            accepts,
            mission1 ? 1 : 0,
            campaign_grid_exit_clean ? 1 : 0,
            g_replay.legacy_level_entry_hidden ? 1 : 0,
            g_replay.post_commit_level_select_hidden ? 1 : 0,
            g_replay.deadline_missed ? 1 : 0,
            g_replay.semantic_success ? 1 : 0,
            g_replay.masked_control.has_value()
                ? replay_control_name(*g_replay.masked_control)
                : "NONE"
        );
        std::fflush(stderr);
    }
}

bool replay_pi_dma_contract_passed(
    const bumble::native_pi_dma::CaptureStats& stats
) {
    return stats.transfers > 0u &&
        stats.transfers == stats.receipts &&
        stats.successful_starts == stats.transfers &&
        stats.successful_receives == stats.receipts &&
        stats.associated_receipts == stats.receipts &&
        stats.verified_copies == stats.transfers &&
        stats.start_failures == 0u &&
        stats.receive_failures == 0u &&
        stats.unassociated_receipts == 0u &&
        stats.receipt_call_site_mismatches == 0u &&
        stats.copy_mismatches == 0u &&
        stats.source_bounds_failures == 0u &&
        stats.destination_bounds_failures == 0u &&
        stats.bounds_failures == 0u &&
        stats.alignment_failures == 0u &&
        stats.unknown_call_sites == 0u &&
        stats.unknown_receipt_pcs == 0u &&
        stats.blocking_transfers > 0u &&
        stats.chunked_transfers > 0u &&
        stats.audio_page_transfers > 0u &&
        stats.blocking_transfers == stats.blocking_receipts &&
        stats.chunked_transfers == stats.chunked_receipts &&
        stats.audio_page_transfers == stats.audio_page_receipts &&
        stats.unknown_transfers == 0u &&
        stats.unknown_receipts == 0u &&
        stats.pending_transfers == 0u;
}

bool replay_rsp_task_contract_passed(
    const bumble::native_rsp_task::CaptureStats& stats
) {
    return stats.contract_passed &&
        stats.fully_correlated_audio > 0u &&
        stats.fully_correlated_graphics > 0u &&
        stats.snapshot_mismatches == 0u &&
        stats.unexpected_exits == 0u &&
        stats.pending_sampled_lifecycles == 0u;
}

void advance_schema4_replay_locked() {
    advance_schema1_replay_locked();
    if (!g_replay.configured || !g_replay.armed ||
        g_replay.completion_logged ||
        !g_replay.mission1_completion.has_value()) {
        return;
    }

    MissionOneCompletionReplay& completion = *g_replay.mission1_completion;
    constexpr uint32_t kMissionSelectorPhase = 0x0Eu;
    constexpr uint32_t kMissionSelectorDescriptor = 0x800FE508u;
    constexpr uint32_t kMissionResultPhase = 0x1Au;
    constexpr uint32_t kMissionResultDescriptor = 0x800FD648u;
    constexpr uint32_t kSaveGameItem = 0x800FD5F8u;
    constexpr uint32_t kContinueItem = 0x800FD620u;
    constexpr uint32_t kSaveProfilePhase = 0x1Fu;
    constexpr uint32_t kSaveProfileDescriptor = 0x800FDA10u;
    constexpr uint32_t kSaveSuccessPhase = 0x20u;
    constexpr uint32_t kSaveSuccessDescriptor = 0x800FDB58u;
    if (!completion.selector_acked && g_replay.game_event_cursor >= 6u) {
        if (bumble::native_checkpoint::campaign_selection_committed_index() ==
            1u) {
            completion.selector_acked = true;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission1_selector_confirm_ack"
                " tick=%llu accepted_phase=0x0000000E accepted=1"
                " route=native_level_grid_commit schema=4\n",
                static_cast<unsigned long long>(g_replay.tick)
            );
            std::fflush(stderr);
        } else if (!completion.selector_a_down &&
            bumble::native_checkpoint::last_frontend_phase() ==
                kMissionSelectorPhase &&
            bumble::native_checkpoint::last_frontend_descriptor() ==
                kMissionSelectorDescriptor) {
            completion.selector_confirm_baseline =
                bumble::native_checkpoint::frontend_confirm_count();
            apply_replay_control(g_replay.snapshot, ReplayControl::A, true);
            completion.selector_a_down = true;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission1_selector_confirm_request"
                " tick=%llu phase=0x0000000E descriptor=0x800FE508"
                " op=down schema=4\n",
                static_cast<unsigned long long>(g_replay.tick)
            );
            std::fflush(stderr);
        } else if (completion.selector_a_down &&
            bumble::native_checkpoint::frontend_confirm_count() >
                completion.selector_confirm_baseline) {
            const bool accepted =
                bumble::native_checkpoint::last_accepted_frontend_phase() ==
                    kMissionSelectorPhase;
            apply_replay_control(g_replay.snapshot, ReplayControl::A, false);
            completion.selector_a_down = false;
            completion.selector_acked = accepted;
            g_replay.deadline_missed = g_replay.deadline_missed || !accepted;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission1_selector_confirm_ack"
                " tick=%llu accepted_phase=0x%08" PRIX32
                " accepted=%d op=up schema=4\n",
                static_cast<unsigned long long>(g_replay.tick),
                bumble::native_checkpoint::last_accepted_frontend_phase(),
                accepted ? 1 : 0
            );
            std::fflush(stderr);
        }
    }
    const bool success_frontend =
        bumble::native_checkpoint::mission1_success_frontend_observed();
    const bool level_increment =
        bumble::native_checkpoint::mission1_level_increment_observed();
    const bool profile_saved =
        bumble::native_checkpoint::mission1_profile_save_observed();
    const uint32_t post_phase =
        bumble::native_checkpoint::last_frontend_phase();
    const uint32_t post_descriptor =
        bumble::native_checkpoint::last_frontend_descriptor();
    const uint32_t post_item =
        bumble::native_checkpoint::last_frontend_descriptor_item();
    const uint32_t post_confirms =
        bumble::native_checkpoint::frontend_confirm_count();
    const bool result_menu_visible = success_frontend &&
        post_phase == kMissionResultPhase &&
        post_descriptor == kMissionResultDescriptor &&
        (post_item == kSaveGameItem || post_item == kContinueItem);
    if (result_menu_visible && completion.result_visible_since_tick == 0u) {
        completion.result_visible_since_tick = g_replay.tick;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_mission1_result_hold_started"
            " tick=%llu phase=0x0000001A descriptor=0x800FD648"
            " hold_polls=480 purpose=delayed_visibility_proof schema=4\n",
            static_cast<unsigned long long>(g_replay.tick)
        );
        std::fflush(stderr);
    }
    const bool result_hold_complete =
        completion.result_visible_since_tick != 0u &&
        g_replay.tick >= completion.result_visible_since_tick + 480u;
    if (completion.require_profile_save && success_frontend) {
        if (!level_increment && !completion.success_a_down &&
            bumble::native_checkpoint::last_frontend_phase() == 0x1Au) {
            completion.success_confirm_baseline =
                bumble::native_checkpoint::frontend_confirm_count();
            apply_replay_control(g_replay.snapshot, ReplayControl::A, true);
            completion.success_a_down = true;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission1_success_confirm_request"
                " tick=%llu phase=0x0000001A op=down schema=4\n",
                static_cast<unsigned long long>(g_replay.tick)
            );
            std::fflush(stderr);
        } else if (completion.success_a_down &&
            (bumble::native_checkpoint::frontend_confirm_count() >
                completion.success_confirm_baseline || level_increment)) {
            apply_replay_control(g_replay.snapshot, ReplayControl::A, false);
            completion.success_a_down = false;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission1_success_confirm_ack"
                " tick=%llu phase=0x%08" PRIX32
                " accepted_phase=0x%08" PRIX32 " op=up schema=4\n",
                static_cast<unsigned long long>(g_replay.tick),
                bumble::native_checkpoint::last_frontend_phase(),
                bumble::native_checkpoint::last_accepted_frontend_phase()
            );
            std::fflush(stderr);
        }
    }

    if (completion.require_profile_save &&
        !completion.withhold_one_owner && level_increment &&
        !completion.post_route_started && !completion.success_a_down) {
        completion.post_route_started = true;
        completion.post_stage_started_tick = g_replay.tick;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_mission1_post_route_started"
            " tick=%llu phase=0x%08" PRIX32
            " descriptor=0x%08" PRIX32 " item=0x%08" PRIX32
            " target=save_then_continue schema=4\n",
            static_cast<unsigned long long>(g_replay.tick),
            post_phase,
            post_descriptor,
            post_item
        );
        std::fflush(stderr);
    }

    if (completion.post_route_started && !completion.save_navigation_acked) {
        if (post_phase == kMissionResultPhase &&
            post_descriptor == kMissionResultDescriptor &&
            post_item == kSaveGameItem) {
            if (completion.save_navigation_down) {
                apply_replay_control(
                    g_replay.snapshot,
                    ReplayControl::StickUp,
                    false
                );
                completion.save_navigation_down = false;
            }
            completion.save_navigation_acked = true;
            completion.post_stage_started_tick = g_replay.tick;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission1_save_selection_ack"
                " tick=%llu phase=0x0000001A descriptor=0x800FD648"
                " from_item=0x800FD620 item=0x800FD5F8 control=STICK_U"
                " op=up schema=4\n",
                static_cast<unsigned long long>(g_replay.tick)
            );
            std::fflush(stderr);
        } else if (!completion.save_navigation_down &&
            post_phase == kMissionResultPhase &&
            post_descriptor == kMissionResultDescriptor &&
            post_item == kContinueItem) {
            apply_replay_control(
                g_replay.snapshot,
                ReplayControl::StickUp,
                true
            );
            completion.save_navigation_down = true;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission1_save_selection_request"
                " tick=%llu phase=0x0000001A descriptor=0x800FD648"
                " from_item=0x800FD620 to_item=0x800FD5F8"
                " control=STICK_U op=down schema=4\n",
                static_cast<unsigned long long>(g_replay.tick)
            );
            std::fflush(stderr);
        }
    }

    if (completion.save_navigation_acked && !completion.save_acked) {
        if (!completion.save_a_down &&
            post_phase == kMissionResultPhase &&
            post_descriptor == kMissionResultDescriptor &&
            post_item == kSaveGameItem) {
            completion.save_confirm_baseline = post_confirms;
            apply_replay_control(g_replay.snapshot, ReplayControl::A, true);
            completion.save_a_down = true;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission1_save_confirm_request"
                " tick=%llu phase=0x0000001A descriptor=0x800FD648"
                " item=0x800FD5F8 op=down schema=4\n",
                static_cast<unsigned long long>(g_replay.tick)
            );
            std::fflush(stderr);
        } else if (completion.save_a_down &&
            post_confirms > completion.save_confirm_baseline) {
            const bool accepted =
                bumble::native_checkpoint::last_accepted_frontend_phase() ==
                    kMissionResultPhase;
            apply_replay_control(g_replay.snapshot, ReplayControl::A, false);
            completion.save_a_down = false;
            completion.save_acked = accepted;
            completion.post_stage_started_tick = g_replay.tick;
            g_replay.deadline_missed = g_replay.deadline_missed || !accepted;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission1_save_confirm_ack"
                " tick=%llu accepted_phase=0x%08" PRIX32
                " accepted=%d op=up schema=4\n",
                static_cast<unsigned long long>(g_replay.tick),
                bumble::native_checkpoint::last_accepted_frontend_phase(),
                accepted ? 1 : 0
            );
            std::fflush(stderr);
        }
    }

    if (completion.save_acked && !completion.save_slot_acked &&
        !profile_saved) {
        if (!completion.save_slot_a_down &&
            post_phase == kSaveProfilePhase &&
            post_descriptor == kSaveProfileDescriptor &&
            post_item != 0u && post_item != UINT32_MAX) {
            completion.save_slot_confirm_baseline = post_confirms;
            apply_replay_control(g_replay.snapshot, ReplayControl::A, true);
            completion.save_slot_a_down = true;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission1_save_slot_request"
                " tick=%llu phase=0x0000001F descriptor=0x800FDA10"
                " item=0x%08" PRIX32 " slot=default_empty op=down schema=4\n",
                static_cast<unsigned long long>(g_replay.tick),
                post_item
            );
            std::fflush(stderr);
        } else if (completion.save_slot_a_down &&
            post_confirms > completion.save_slot_confirm_baseline) {
            const bool accepted =
                bumble::native_checkpoint::last_accepted_frontend_phase() ==
                    kSaveProfilePhase;
            apply_replay_control(g_replay.snapshot, ReplayControl::A, false);
            completion.save_slot_a_down = false;
            completion.save_slot_acked = accepted;
            completion.post_stage_started_tick = g_replay.tick;
            g_replay.deadline_missed = g_replay.deadline_missed || !accepted;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission1_save_slot_ack"
                " tick=%llu accepted_phase=0x%08" PRIX32
                " accepted=%d slot=default_empty op=up schema=4\n",
                static_cast<unsigned long long>(g_replay.tick),
                bumble::native_checkpoint::last_accepted_frontend_phase(),
                accepted ? 1 : 0
            );
            std::fflush(stderr);
        }
    }

    if (completion.require_profile_save && profile_saved &&
        !completion.pi_capture_started) {
        if (completion.save_slot_a_down) {
            apply_replay_control(g_replay.snapshot, ReplayControl::A, false);
            completion.save_slot_a_down = false;
        }
        if (!completion.save_slot_acked) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission1_save_slot_ack"
                " tick=%llu accepted_phase=0x0000001F"
                " accepted=1 slot=default_empty op=up"
                " evidence=profile_write_reread schema=4\n",
                static_cast<unsigned long long>(g_replay.tick)
            );
        }
        completion.save_slot_acked = true;
        bumble::native_pi_dma::set_capture_enabled(true);
        completion.pi_capture_started = true;
        completion.post_stage_started_tick = g_replay.tick;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_mission1_mission2_capture_started"
            " tick=%llu profile_save=1 capture_boundary=post_save_pre_continue"
            " schema=4\n",
            static_cast<unsigned long long>(g_replay.tick)
        );
        std::fflush(stderr);
    }

    if (!completion.require_profile_save && result_hold_complete &&
        !completion.pi_capture_started) {
        bumble::native_pi_dma::set_capture_enabled(true);
        completion.pi_capture_started = true;
        completion.post_stage_started_tick = g_replay.tick;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_mission1_mission2_capture_started"
            " tick=%llu profile_save=0 capture_boundary=post_result_hold_pre_continue"
            " schema=4\n",
            static_cast<unsigned long long>(g_replay.tick)
        );
        std::fflush(stderr);
    }

    if (profile_saved && !completion.save_success_acked) {
        if (!completion.save_success_a_down &&
            post_phase == kSaveSuccessPhase &&
            post_descriptor == kSaveSuccessDescriptor) {
            completion.save_success_confirm_baseline = post_confirms;
            apply_replay_control(g_replay.snapshot, ReplayControl::A, true);
            completion.save_success_a_down = true;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission1_save_success_request"
                " tick=%llu phase=0x00000020 descriptor=0x800FDB58"
                " target_phase=0x00000018 op=down schema=4\n",
                static_cast<unsigned long long>(g_replay.tick)
            );
            std::fflush(stderr);
        } else if (completion.save_success_a_down &&
            post_confirms > completion.save_success_confirm_baseline) {
            const bool accepted =
                bumble::native_checkpoint::last_accepted_frontend_phase() ==
                    kSaveSuccessPhase;
            apply_replay_control(g_replay.snapshot, ReplayControl::A, false);
            completion.save_success_a_down = false;
            completion.save_success_acked = accepted;
            completion.post_stage_started_tick = g_replay.tick;
            g_replay.deadline_missed = g_replay.deadline_missed || !accepted;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission1_save_success_ack"
                " tick=%llu accepted_phase=0x%08" PRIX32
                " accepted=%d target_phase=0x00000018 op=up schema=4\n",
                static_cast<unsigned long long>(g_replay.tick),
                bumble::native_checkpoint::last_accepted_frontend_phase(),
                accepted ? 1 : 0
            );
            std::fflush(stderr);
        }
    }

    const bool continue_route_ready = completion.require_profile_save
        ? profile_saved
        : result_hold_complete;
    if (continue_route_ready && !completion.continue_acked) {
        if (completion.save_success_acked &&
            (post_phase == 0x18u || post_phase == 0x19u ||
             bumble::native_checkpoint::mission2_player_observed())) {
            completion.continue_acked = true;
            completion.post_stage_started_tick = g_replay.tick;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission1_continue_ack"
                " tick=%llu route=save_success_card phase=0x%08" PRIX32
                " schema=4\n",
                static_cast<unsigned long long>(g_replay.tick),
                post_phase
            );
            std::fflush(stderr);
        } else if (post_phase == 0x18u || post_phase == 0x19u ||
            bumble::native_checkpoint::mission2_player_observed()) {
            completion.continue_acked = true;
            completion.post_stage_started_tick = g_replay.tick;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission1_continue_ack"
                " tick=%llu route=save_successor_auto phase=0x%08" PRIX32
                " schema=4\n",
                static_cast<unsigned long long>(g_replay.tick),
                post_phase
            );
            std::fflush(stderr);
        } else if (!completion.continue_navigation_acked &&
            post_phase == kMissionResultPhase &&
            post_descriptor == kMissionResultDescriptor) {
            if (post_item == kContinueItem) {
                if (completion.continue_navigation_down) {
                    apply_replay_control(
                        g_replay.snapshot,
                        ReplayControl::StickDown,
                        false
                    );
                    completion.continue_navigation_down = false;
                }
                completion.continue_navigation_acked = true;
                completion.post_stage_started_tick = g_replay.tick;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_mission1_continue_selection_ack"
                    " tick=%llu phase=0x0000001A descriptor=0x800FD648"
                    " item=0x800FD620 control=STICK_D op=up schema=4\n",
                    static_cast<unsigned long long>(g_replay.tick)
                );
                std::fflush(stderr);
            } else if (post_item == kSaveGameItem &&
                !completion.continue_navigation_down) {
                apply_replay_control(
                    g_replay.snapshot,
                    ReplayControl::StickDown,
                    true
                );
                completion.continue_navigation_down = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_mission1_continue_selection_request"
                    " tick=%llu phase=0x0000001A descriptor=0x800FD648"
                    " from_item=0x800FD5F8 to_item=0x800FD620"
                    " control=STICK_D op=down schema=4\n",
                    static_cast<unsigned long long>(g_replay.tick)
                );
                std::fflush(stderr);
            }
        }
        if (completion.continue_navigation_acked &&
            !completion.continue_a_down &&
            post_phase == kMissionResultPhase &&
            post_descriptor == kMissionResultDescriptor &&
            post_item == kContinueItem) {
            completion.continue_confirm_baseline = post_confirms;
            apply_replay_control(g_replay.snapshot, ReplayControl::A, true);
            completion.continue_a_down = true;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission1_continue_confirm_request"
                " tick=%llu phase=0x0000001A descriptor=0x800FD648"
                " item=0x800FD620 op=down schema=4\n",
                static_cast<unsigned long long>(g_replay.tick)
            );
            std::fflush(stderr);
        } else if (completion.continue_a_down &&
            post_confirms > completion.continue_confirm_baseline) {
            const bool accepted =
                bumble::native_checkpoint::last_accepted_frontend_phase() ==
                    kMissionResultPhase;
            apply_replay_control(g_replay.snapshot, ReplayControl::A, false);
            completion.continue_a_down = false;
            completion.continue_acked = accepted;
            completion.post_stage_started_tick = g_replay.tick;
            g_replay.deadline_missed = g_replay.deadline_missed || !accepted;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission1_continue_confirm_ack"
                " tick=%llu accepted_phase=0x%08" PRIX32
                " accepted=%d op=up schema=4\n",
                static_cast<unsigned long long>(g_replay.tick),
                bumble::native_checkpoint::last_accepted_frontend_phase(),
                accepted ? 1 : 0
            );
            if (accepted) {
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_mission1_continue_ack"
                    " tick=%llu route=result_menu_confirm"
                    " phase=0x0000001A schema=4\n",
                    static_cast<unsigned long long>(g_replay.tick)
                );
            }
            std::fflush(stderr);
        }
    }

    if (continue_route_ready && completion.continue_acked &&
        !completion.mission2_phase18_acked) {
        if (!completion.mission2_phase18_a_down && post_phase == 0x18u) {
            completion.mission2_phase18_confirm_baseline = post_confirms;
            apply_replay_control(g_replay.snapshot, ReplayControl::A, true);
            completion.mission2_phase18_a_down = true;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission2_normal_phase18_request"
                " tick=%llu op=down schema=4\n",
                static_cast<unsigned long long>(g_replay.tick)
            );
            std::fflush(stderr);
        } else if (completion.mission2_phase18_a_down &&
            post_confirms > completion.mission2_phase18_confirm_baseline) {
            const bool accepted =
                bumble::native_checkpoint::last_accepted_frontend_phase() == 0x18u;
            apply_replay_control(g_replay.snapshot, ReplayControl::A, false);
            completion.mission2_phase18_a_down = false;
            completion.mission2_phase18_acked = accepted;
            g_replay.deadline_missed = g_replay.deadline_missed || !accepted;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission2_normal_phase18_ack"
                " tick=%llu accepted=%d op=up schema=4\n",
                static_cast<unsigned long long>(g_replay.tick),
                accepted ? 1 : 0
            );
            std::fflush(stderr);
        }
    }
    if (continue_route_ready && completion.continue_acked &&
        completion.mission2_phase18_acked &&
        !completion.mission2_phase19_acked) {
        if (!completion.mission2_phase19_a_down && post_phase == 0x19u) {
            completion.mission2_phase19_confirm_baseline = post_confirms;
            apply_replay_control(g_replay.snapshot, ReplayControl::A, true);
            completion.mission2_phase19_a_down = true;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission2_normal_phase19_request"
                " tick=%llu op=down schema=4\n",
                static_cast<unsigned long long>(g_replay.tick)
            );
            std::fflush(stderr);
        } else if (completion.mission2_phase19_a_down &&
            post_confirms > completion.mission2_phase19_confirm_baseline) {
            const bool accepted =
                bumble::native_checkpoint::last_accepted_frontend_phase() == 0x19u;
            apply_replay_control(g_replay.snapshot, ReplayControl::A, false);
            completion.mission2_phase19_a_down = false;
            completion.mission2_phase19_acked = accepted;
            g_replay.deadline_missed = g_replay.deadline_missed || !accepted;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission2_normal_phase19_ack"
                " tick=%llu accepted=%d op=up schema=4\n",
                static_cast<unsigned long long>(g_replay.tick),
                accepted ? 1 : 0
            );
            std::fflush(stderr);
        }
    }

    const uint32_t installers =
        bumble::native_checkpoint::mission1_installer_count();
    const uint32_t owners =
        bumble::native_checkpoint::mission1_registered_owner_count();
    const uint32_t terminals =
        bumble::native_checkpoint::mission1_terminal_count();
    const uint32_t counter =
        bumble::native_checkpoint::mission1_counter_value();
    const uint32_t delay =
        bumble::native_checkpoint::mission1_delay_value();
    const bool success_callback =
        bumble::native_checkpoint::mission1_success_callback_observed();
    const bool mission2_player =
        bumble::native_checkpoint::mission2_player_observed();
    const bool mission2_screen =
        bumble::rt64_renderer::mission2_screen_presented();
    if (completion.pi_capture_started && mission2_player && mission2_screen &&
        !completion.capture_drain_started) {
        bumble::native_rsp_task::begin_capture_drain();
        bumble::native_pi_dma::begin_capture_drain();
        completion.capture_drain_started = true;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_mission1_capture_drain_started"
            " tick=%llu boundary=post_mission2_present"
            " mission2_player=1 mission2_screen=1 schema=4\n",
            static_cast<unsigned long long>(g_replay.tick)
        );
        std::fflush(stderr);
    }
    const bumble::native_pi_dma::CaptureStats pi_stats =
        bumble::native_pi_dma::capture_stats();
    const bumble::native_rsp_task::CaptureStats rsp_stats =
        bumble::native_rsp_task::capture_stats();
    const bool pi_dma_contract = completion.pi_capture_started &&
        completion.capture_drain_started &&
        replay_pi_dma_contract_passed(pi_stats);
    const bool rsp_task_contract = completion.pi_capture_started &&
        completion.capture_drain_started &&
        replay_rsp_task_contract_passed(rsp_stats);
    const bool capture_contract = pi_dma_contract && rsp_task_contract;
    if (completion.capture_drain_started && !capture_contract &&
        !completion.capture_drain_wait_logged) {
        completion.capture_drain_wait_logged = true;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_mission1_capture_drain_wait"
            " tick=%llu pending_pi_transfers=%llu"
            " pending_rsp_sampled_lifecycles=%llu"
            " pi_contract=%d rsp_contract=%d schema=4\n",
            static_cast<unsigned long long>(g_replay.tick),
            static_cast<unsigned long long>(pi_stats.pending_transfers),
            static_cast<unsigned long long>(
                rsp_stats.pending_sampled_lifecycles
            ),
            pi_dma_contract ? 1 : 0,
            rsp_task_contract ? 1 : 0
        );
        std::fflush(stderr);
    }
    const bool topology = installers == 11u && owners == 13u;
    const bool all_frontend =
        g_replay.game_event_cursor == g_replay.game_owned_events.size() &&
        completion.selector_acked &&
        bumble::native_checkpoint::mission1_player_observed() &&
        bumble::modern_controls::gameplay_input_active();
    const bool result_route = completion.require_profile_save
        ? profile_saved && completion.save_navigation_acked &&
            completion.save_acked && completion.save_slot_acked &&
            completion.save_success_acked
        : result_hold_complete;
    const bool positive_ready = !completion.withhold_one_owner && topology &&
        all_frontend && terminals == 13u && counter == 0u && delay == 0u &&
        success_callback && success_frontend && level_increment &&
        result_route &&
        completion.continue_acked && completion.mission2_phase18_acked &&
        completion.mission2_phase19_acked && mission2_player &&
        mission2_screen && capture_contract;

    const bool negative_shape = completion.withhold_one_owner && topology &&
        all_frontend && terminals == 12u && counter == 1u && delay == 20u &&
        !success_callback && !success_frontend && !level_increment &&
        !profile_saved && !mission2_player && !mission2_screen &&
        !completion.pi_capture_started;
    if (negative_shape) {
        if (completion.negative_stable_since_tick == 0u) {
            completion.negative_stable_since_tick = g_replay.tick;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_mission1_negative_stability_started"
                " tick=%llu terminal=12 counter=1 required_stable_polls=600"
                " schema=4\n",
                static_cast<unsigned long long>(g_replay.tick)
            );
            std::fflush(stderr);
        }
    } else {
        completion.negative_stable_since_tick = 0u;
    }
    const bool negative_ready = negative_shape &&
        g_replay.tick >= completion.negative_stable_since_tick + 600u;
    const bool deadline =
        g_replay.tick >= completion.completion_deadline_tick;
    if (!positive_ready && !negative_ready && !deadline) {
        return;
    }

    if (completion.success_a_down) {
        apply_replay_control(g_replay.snapshot, ReplayControl::A, false);
        completion.success_a_down = false;
    }
    if (completion.selector_a_down) {
        apply_replay_control(g_replay.snapshot, ReplayControl::A, false);
        completion.selector_a_down = false;
    }
    if (completion.save_a_down || completion.save_slot_a_down ||
        completion.save_success_a_down ||
        completion.continue_a_down || completion.mission2_phase18_a_down ||
        completion.mission2_phase19_a_down) {
        apply_replay_control(g_replay.snapshot, ReplayControl::A, false);
    }
    if (completion.save_navigation_down) {
        apply_replay_control(
            g_replay.snapshot,
            ReplayControl::StickUp,
            false
        );
    }
    if (completion.continue_navigation_down) {
        apply_replay_control(
            g_replay.snapshot,
            ReplayControl::StickDown,
            false
        );
    }
    apply_replay_control(g_replay.snapshot, ReplayControl::Z, false);
    bumble::modern_controls::set_movement_input(0.0f, 0.0f);
    if (completion.pi_capture_started) {
        bumble::native_pi_dma::set_capture_enabled(false);
    }
    const bumble::native_rsp_task::CaptureStats final_rsp_stats =
        completion.pi_capture_started
            ? bumble::native_rsp_task::finalize_capture()
            : rsp_stats;
    const bumble::native_pi_dma::CaptureStats final_pi_stats =
        bumble::native_pi_dma::capture_stats();
    if (completion.pi_capture_started &&
        !completion.capture_contract_logged) {
        completion.capture_contract_logged = true;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_mission1_pi_dma_contract schema=4"
            " transfers=%llu receipts=%llu verified_copies=%llu"
            " copy_mismatches=%llu blocking_transfers=%llu"
            " chunked_transfers=%llu audio_page_transfers=%llu"
            " excluded_unadmitted_receipts=%llu"
            " excluded_pre_admission_receipts=%llu"
            " excluded_post_boundary_receipts=%llu"
            " pending_transfers=%llu contract_passed=%d\n",
            static_cast<unsigned long long>(final_pi_stats.transfers),
            static_cast<unsigned long long>(final_pi_stats.receipts),
            static_cast<unsigned long long>(final_pi_stats.verified_copies),
            static_cast<unsigned long long>(final_pi_stats.copy_mismatches),
            static_cast<unsigned long long>(
                final_pi_stats.blocking_transfers
            ),
            static_cast<unsigned long long>(
                final_pi_stats.chunked_transfers
            ),
            static_cast<unsigned long long>(
                final_pi_stats.audio_page_transfers
            ),
            static_cast<unsigned long long>(
                final_pi_stats.excluded_unadmitted_receipts
            ),
            static_cast<unsigned long long>(
                final_pi_stats.excluded_pre_admission_receipts
            ),
            static_cast<unsigned long long>(
                final_pi_stats.excluded_post_boundary_receipts
            ),
            static_cast<unsigned long long>(
                final_pi_stats.pending_transfers
            ),
            pi_dma_contract ? 1 : 0
        );
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_mission1_rsp_task_contract schema=4"
            " fully_correlated_audio=%llu fully_correlated_graphics=%llu"
            " snapshot_mismatches=%llu unexpected_exits=%llu"
            " pending_sampled_lifecycles=%llu contract_passed=%d\n",
            static_cast<unsigned long long>(
                final_rsp_stats.fully_correlated_audio
            ),
            static_cast<unsigned long long>(
                final_rsp_stats.fully_correlated_graphics
            ),
            static_cast<unsigned long long>(
                final_rsp_stats.snapshot_mismatches
            ),
            static_cast<unsigned long long>(final_rsp_stats.unexpected_exits),
            static_cast<unsigned long long>(
                final_rsp_stats.pending_sampled_lifecycles
            ),
            rsp_task_contract ? 1 : 0
        );
        std::fflush(stderr);
    }
    g_replay.completion_logged = true;
    g_replay.deadline_missed = g_replay.deadline_missed ||
        (deadline && !positive_ready && !negative_ready);
    g_replay.semantic_success = !g_replay.deadline_missed &&
        (completion.withhold_one_owner ? negative_ready : positive_ready);
    g_replay_semantic_success.store(
        g_replay.semantic_success,
        std::memory_order_release
    );
    g_replay_complete.store(true, std::memory_order_release);
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=replay_complete tick=%llu schema=4"
        " controllers=1 completed_game_events=%zu expected_game_events=%zu"
        " mission1_player=%d installers=%" PRIu32
        " registered_owners=%" PRIu32 " terminal_owners=%" PRIu32
        " counter=%" PRIu32 " delay=%" PRIu32
        " success_callback=%d success_frontend=%d level_increment=%d"
        " profile_save=%d save_selection=%d save_confirm=%d save_slot=%d"
        " save_success=%d"
        " continue=%d mission2_phase18=%d mission2_phase19=%d"
        " mission2_player=%d mission2_screen=%d"
        " pi_dma_contract=%d rsp_task_contract=%d"
        " pi_transfers=%llu rsp_audio=%llu rsp_graphics=%llu"
        " withhold_one_owner=%d negative_stable_polls=%llu"
        " deadline_missed=%d semantic_success=%d"
        " physical_input_suppressed=1 counter_result_level_writes=0\n",
        static_cast<unsigned long long>(g_replay.tick),
        g_replay.game_event_cursor,
        g_replay.game_owned_events.size(),
        bumble::native_checkpoint::mission1_player_observed() ? 1 : 0,
        installers,
        owners,
        terminals,
        counter,
        delay,
        success_callback ? 1 : 0,
        success_frontend ? 1 : 0,
        level_increment ? 1 : 0,
        profile_saved ? 1 : 0,
        completion.save_navigation_acked ? 1 : 0,
        completion.save_acked ? 1 : 0,
        completion.save_slot_acked ? 1 : 0,
        completion.save_success_acked ? 1 : 0,
        completion.continue_acked ? 1 : 0,
        completion.mission2_phase18_acked ? 1 : 0,
        completion.mission2_phase19_acked ? 1 : 0,
        mission2_player ? 1 : 0,
        mission2_screen ? 1 : 0,
        pi_dma_contract ? 1 : 0,
        rsp_task_contract ? 1 : 0,
        static_cast<unsigned long long>(final_pi_stats.transfers),
        static_cast<unsigned long long>(
            final_rsp_stats.fully_correlated_audio
        ),
        static_cast<unsigned long long>(
            final_rsp_stats.fully_correlated_graphics
        ),
        completion.withhold_one_owner ? 1 : 0,
        completion.negative_stable_since_tick == 0u ? 0ull :
            static_cast<unsigned long long>(
                g_replay.tick - completion.negative_stable_since_tick
            ),
        g_replay.deadline_missed ? 1 : 0,
        g_replay.semantic_success ? 1 : 0
    );
    std::fflush(stderr);
}

void advance_schema5_replay_locked() {
    advance_schema1_replay_locked();
    if (!g_replay.configured || !g_replay.armed ||
        g_replay.completion_logged || !g_replay.profile_resume.has_value()) {
        return;
    }

    ProfileResumeReplay& resume = *g_replay.profile_resume;
    constexpr uint32_t kMissionSelectorPhase = 0x2Bu;
    constexpr uint32_t kMissionSelectorDescriptor = 0x800FE508u;
    const bool profile_reopened =
        bumble::native_checkpoint::mission1_profile_reopen_observed();
    const bool selector_is_mission2 =
        bumble::native_checkpoint::campaign_selector_index() == 2u;

    if (g_replay.selection_event.has_value() &&
        !g_replay.selection_event->acknowledged) {
        MenuSelectionReplayEvent& selection = *g_replay.selection_event;
        const uint32_t phase =
            bumble::native_checkpoint::last_frontend_phase();
        const uint32_t descriptor =
            bumble::native_checkpoint::last_frontend_descriptor();
        const uint32_t item =
            bumble::native_checkpoint::last_frontend_descriptor_item();
        if (g_replay.tick > selection.deadline_tick) {
            if (!selection.deadline_reported) {
                selection.deadline_reported = true;
                g_replay.deadline_missed = true;
                if (selection.requested) {
                    apply_replay_control(
                        g_replay.snapshot,
                        selection.control,
                        false
                    );
                }
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_profile_resume_load_selection_deadline_missed"
                    " tick=%llu phase=0x%08" PRIX32
                    " descriptor=0x%08" PRIX32 " item=0x%08" PRIX32
                    " requested=%d deadline_tick=%llu schema=5\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    phase,
                    descriptor,
                    item,
                    selection.requested ? 1 : 0,
                    static_cast<unsigned long long>(selection.deadline_tick)
                );
                std::fflush(stderr);
            }
        } else if (selection.requested &&
            phase == selection.observed_phase &&
            descriptor == selection.descriptor &&
            item == selection.to_item) {
            apply_replay_control(g_replay.snapshot, selection.control, false);
            selection.acknowledged = true;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_profile_resume_load_selection_ack"
                " tick=%llu control=STICK_D phase=0x0000000C"
                " descriptor=0x800FC940 item=0x800FC870"
                " expected_item=0x800FC870 target_state=22 op=up schema=5\n",
                static_cast<unsigned long long>(g_replay.tick)
            );
            std::fflush(stderr);
        } else if (!selection.requested &&
            phase == selection.observed_phase &&
            descriptor == selection.descriptor &&
            item == selection.from_item) {
            apply_replay_control(g_replay.snapshot, selection.control, true);
            selection.requested = true;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_profile_resume_load_selection_request"
                " tick=%llu control=STICK_D phase=0x0000000C"
                " descriptor=0x800FC940 item=0x800FC820"
                " expected_item=0x800FC870 target_state=22"
                " deadline_tick=%llu op=down schema=5\n",
                static_cast<unsigned long long>(g_replay.tick),
                static_cast<unsigned long long>(selection.deadline_tick)
            );
            std::fflush(stderr);
        }
    }

    const bool mission_selector_ready = profile_reopened &&
        selector_is_mission2 &&
        bumble::native_checkpoint::last_frontend_phase() ==
            kMissionSelectorPhase &&
        bumble::native_checkpoint::last_frontend_descriptor() ==
            kMissionSelectorDescriptor;
    if (mission_selector_ready && !resume.pi_capture_started) {
        bumble::native_pi_dma::set_capture_enabled(true);
        resume.pi_capture_started = true;
        resume.profile_reopen_ready_tick = g_replay.tick;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_profile_resume_capture_started"
            " tick=%llu profile_reopen=1 restored_level=2"
            " selector_index=2"
            " capture_boundary=post_profile_reopen_pre_mission2_selector"
            " schema=5\n",
            static_cast<unsigned long long>(g_replay.tick)
        );
        std::fflush(stderr);
    }

    if (!resume.selector_acked && g_replay.game_event_cursor >= 6u) {
        if (!resume.selector_a_down && mission_selector_ready &&
            resume.pi_capture_started &&
            g_replay.tick > resume.profile_reopen_ready_tick) {
            resume.selector_confirm_baseline =
                bumble::native_checkpoint::frontend_confirm_count();
            apply_replay_control(g_replay.snapshot, ReplayControl::A, true);
            resume.selector_a_down = true;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_profile_resume_selector_request"
                " tick=%llu phase=0x0000002B descriptor=0x800FE508"
                " selector_index=2 profile_reopen=1 op=down schema=5\n",
                static_cast<unsigned long long>(g_replay.tick)
            );
            std::fflush(stderr);
        } else if (resume.selector_a_down &&
            bumble::native_checkpoint::frontend_confirm_count() >
                resume.selector_confirm_baseline) {
            const bool accepted =
                bumble::native_checkpoint::last_accepted_frontend_phase() ==
                    kMissionSelectorPhase && profile_reopened &&
                selector_is_mission2;
            apply_replay_control(g_replay.snapshot, ReplayControl::A, false);
            resume.selector_a_down = false;
            resume.selector_acked = accepted;
            g_replay.deadline_missed = g_replay.deadline_missed || !accepted;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_profile_resume_selector_ack"
                " tick=%llu accepted_phase=0x%08" PRIX32
                " selector_index=%" PRIu32
                " profile_reopen=%d accepted=%d op=up schema=5\n",
                static_cast<unsigned long long>(g_replay.tick),
                bumble::native_checkpoint::last_accepted_frontend_phase(),
                bumble::native_checkpoint::campaign_selector_index(),
                profile_reopened ? 1 : 0,
                accepted ? 1 : 0
            );
            std::fflush(stderr);
        }
    }

    const bool mission2_player =
        bumble::native_checkpoint::mission2_player_observed();
    const bool mission2_screen =
        bumble::rt64_renderer::mission2_screen_presented();
    if (resume.pi_capture_started && mission2_player && mission2_screen &&
        !resume.capture_drain_started) {
        bumble::native_rsp_task::begin_capture_drain();
        bumble::native_pi_dma::begin_capture_drain();
        resume.capture_drain_started = true;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_profile_resume_capture_drain_started"
            " tick=%llu boundary=post_mission2_present"
            " mission2_player=1 mission2_screen=1 schema=5\n",
            static_cast<unsigned long long>(g_replay.tick)
        );
        std::fflush(stderr);
    }

    const bumble::native_pi_dma::CaptureStats pi_stats =
        bumble::native_pi_dma::capture_stats();
    const bumble::native_rsp_task::CaptureStats rsp_stats =
        bumble::native_rsp_task::capture_stats();
    const bool pi_dma_contract = resume.pi_capture_started &&
        resume.capture_drain_started &&
        replay_pi_dma_contract_passed(pi_stats);
    const bool rsp_task_contract = resume.pi_capture_started &&
        resume.capture_drain_started &&
        replay_rsp_task_contract_passed(rsp_stats);
    const bool capture_contract = pi_dma_contract && rsp_task_contract;
    if (resume.capture_drain_started && !capture_contract &&
        !resume.capture_drain_wait_logged) {
        resume.capture_drain_wait_logged = true;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_profile_resume_capture_drain_wait"
            " tick=%llu pending_pi_transfers=%llu"
            " pending_rsp_sampled_lifecycles=%llu"
            " pi_contract=%d rsp_contract=%d schema=5\n",
            static_cast<unsigned long long>(g_replay.tick),
            static_cast<unsigned long long>(pi_stats.pending_transfers),
            static_cast<unsigned long long>(
                rsp_stats.pending_sampled_lifecycles
            ),
            pi_dma_contract ? 1 : 0,
            rsp_task_contract ? 1 : 0
        );
        std::fflush(stderr);
    }

    const bool all_frontend =
        g_replay.game_event_cursor == g_replay.game_owned_events.size() &&
        resume.selector_acked;
    const bool ready = !g_replay.deadline_missed && all_frontend &&
        profile_reopened && mission2_player && mission2_screen &&
        !bumble::native_checkpoint::level_select_cheat_complete() &&
        capture_contract;
    const bool deadline = g_replay.tick >= resume.completion_deadline_tick;
    if (!ready && !deadline) {
        return;
    }

    if (resume.selector_a_down || g_replay.game_event_button_down) {
        apply_replay_control(g_replay.snapshot, ReplayControl::A, false);
        resume.selector_a_down = false;
        g_replay.game_event_button_down = false;
    }
    if (resume.pi_capture_started) {
        bumble::native_pi_dma::set_capture_enabled(false);
    }
    const bumble::native_rsp_task::CaptureStats final_rsp_stats =
        resume.pi_capture_started
            ? bumble::native_rsp_task::finalize_capture()
            : rsp_stats;
    const bumble::native_pi_dma::CaptureStats final_pi_stats =
        bumble::native_pi_dma::capture_stats();
    if (resume.pi_capture_started && !resume.capture_contract_logged) {
        resume.capture_contract_logged = true;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_profile_resume_pi_dma_contract"
            " schema=5 transfers=%llu receipts=%llu verified_copies=%llu"
            " copy_mismatches=%llu blocking_transfers=%llu"
            " chunked_transfers=%llu audio_page_transfers=%llu"
            " excluded_unadmitted_receipts=%llu"
            " excluded_pre_admission_receipts=%llu"
            " excluded_post_boundary_receipts=%llu"
            " pending_transfers=%llu contract_passed=%d\n",
            static_cast<unsigned long long>(final_pi_stats.transfers),
            static_cast<unsigned long long>(final_pi_stats.receipts),
            static_cast<unsigned long long>(final_pi_stats.verified_copies),
            static_cast<unsigned long long>(final_pi_stats.copy_mismatches),
            static_cast<unsigned long long>(final_pi_stats.blocking_transfers),
            static_cast<unsigned long long>(final_pi_stats.chunked_transfers),
            static_cast<unsigned long long>(final_pi_stats.audio_page_transfers),
            static_cast<unsigned long long>(
                final_pi_stats.excluded_unadmitted_receipts
            ),
            static_cast<unsigned long long>(
                final_pi_stats.excluded_pre_admission_receipts
            ),
            static_cast<unsigned long long>(
                final_pi_stats.excluded_post_boundary_receipts
            ),
            static_cast<unsigned long long>(final_pi_stats.pending_transfers),
            pi_dma_contract ? 1 : 0
        );
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_profile_resume_rsp_task_contract"
            " schema=5 fully_correlated_audio=%llu"
            " fully_correlated_graphics=%llu snapshot_mismatches=%llu"
            " unexpected_exits=%llu pending_sampled_lifecycles=%llu"
            " contract_passed=%d\n",
            static_cast<unsigned long long>(
                final_rsp_stats.fully_correlated_audio
            ),
            static_cast<unsigned long long>(
                final_rsp_stats.fully_correlated_graphics
            ),
            static_cast<unsigned long long>(final_rsp_stats.snapshot_mismatches),
            static_cast<unsigned long long>(final_rsp_stats.unexpected_exits),
            static_cast<unsigned long long>(
                final_rsp_stats.pending_sampled_lifecycles
            ),
            rsp_task_contract ? 1 : 0
        );
        std::fflush(stderr);
    }

    g_replay.completion_logged = true;
    g_replay.deadline_missed = g_replay.deadline_missed ||
        (deadline && !ready);
    g_replay.semantic_success = ready && !g_replay.deadline_missed;
    g_replay_semantic_success.store(
        g_replay.semantic_success,
        std::memory_order_release
    );
    g_replay_complete.store(true, std::memory_order_release);
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=replay_complete tick=%llu schema=5"
        " controllers=1 completed_game_events=%zu expected_game_events=%zu"
        " profile_reopen=%d selector_index=%" PRIu32
        " selector_ack=%d mission2_player=%d mission2_screen=%d"
        " cheat_complete=%d pi_dma_contract=%d rsp_task_contract=%d"
        " pi_transfers=%llu rsp_audio=%llu rsp_graphics=%llu"
        " deadline_missed=%d semantic_success=%d"
        " physical_input_suppressed=1 guest_state_writes=0\n",
        static_cast<unsigned long long>(g_replay.tick),
        g_replay.game_event_cursor,
        g_replay.game_owned_events.size(),
        profile_reopened ? 1 : 0,
        bumble::native_checkpoint::campaign_selector_index(),
        resume.selector_acked ? 1 : 0,
        mission2_player ? 1 : 0,
        mission2_screen ? 1 : 0,
        bumble::native_checkpoint::level_select_cheat_complete() ? 1 : 0,
        pi_dma_contract ? 1 : 0,
        rsp_task_contract ? 1 : 0,
        static_cast<unsigned long long>(final_pi_stats.transfers),
        static_cast<unsigned long long>(
            final_rsp_stats.fully_correlated_audio
        ),
        static_cast<unsigned long long>(
            final_rsp_stats.fully_correlated_graphics
        ),
        g_replay.deadline_missed ? 1 : 0,
        g_replay.semantic_success ? 1 : 0
    );
    std::fflush(stderr);
}

void advance_schema2_replay_locked() {
    if (!g_replay.configured) {
        return;
    }
    if (!g_replay.armed) {
        for (size_t controller = 0;
             controller < g_replay.replay_controller_count;
             ++controller) {
            g_replay.snapshots[controller] = ControllerSnapshot{
                .connected = true,
                .connected_pak = g_replay.replay_connected_paks[controller],
            };
        }
        return;
    }

    ++g_replay.tick;
    g_replay_tick.store(g_replay.tick, std::memory_order_release);

    while (g_replay.scheduled_cursor < g_replay.scheduled_events.size() &&
           g_replay.scheduled_events[g_replay.scheduled_cursor].tick == g_replay.tick) {
        const ScheduledReplayEvent& event =
            g_replay.scheduled_events[g_replay.scheduled_cursor++];
        const bool masked = replay_control_is_masked(event.control);
        if (!masked) {
            apply_replay_control(
                g_replay.snapshots[event.controller],
                event.control,
                event.down
            );
        }
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_event tick=%llu source=scheduled"
            " controller=%zu op=%s control=%s applied=%d schema=2\n",
            static_cast<unsigned long long>(g_replay.tick),
            event.controller,
            event.down ? "down" : "up",
            replay_control_name(event.control),
            masked ? 0 : 1
        );
        std::fflush(stderr);
    }

    if (g_replay.selection_event.has_value() &&
        !g_replay.selection_event->acknowledged) {
        MenuSelectionReplayEvent& selection = *g_replay.selection_event;
        const uint32_t phase = bumble::native_checkpoint::last_frontend_phase();
        const uint32_t descriptor =
            bumble::native_checkpoint::last_frontend_descriptor();
        const uint32_t item =
            bumble::native_checkpoint::last_frontend_descriptor_item();
        if (g_replay.tick > selection.deadline_tick) {
            if (!selection.deadline_reported) {
                selection.deadline_reported = true;
                g_replay.deadline_missed = true;
                if (selection.requested) {
                    apply_replay_control(
                        g_replay.snapshots[selection.controller],
                        selection.control,
                        false
                    );
                }
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_selection_deadline_missed tick=%llu"
                    " controller=%zu control=%s expected_phase=0x%08" PRIX32
                    " last_phase=0x%08" PRIX32 " expected_descriptor=0x%08" PRIX32
                    " last_descriptor=0x%08" PRIX32 " from_item=0x%08" PRIX32
                    " to_item=0x%08" PRIX32 " last_item=0x%08" PRIX32
                    " requested=%d deadline_tick=%llu schema=2\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    selection.controller,
                    replay_control_name(selection.control),
                    selection.observed_phase,
                    phase,
                    selection.descriptor,
                    descriptor,
                    selection.from_item,
                    selection.to_item,
                    item,
                    selection.requested ? 1 : 0,
                    static_cast<unsigned long long>(selection.deadline_tick)
                );
                std::fflush(stderr);
            }
        } else if (selection.requested) {
            if (phase == selection.observed_phase &&
                descriptor == selection.descriptor && item == selection.to_item) {
                apply_replay_control(
                    g_replay.snapshots[selection.controller],
                    selection.control,
                    false
                );
                selection.acknowledged = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_selection_ack tick=%llu"
                    " controller=%zu control=%s phase=0x%08" PRIX32
                    " descriptor=0x%08" PRIX32 " item=0x%08" PRIX32
                    " expected_item=0x%08" PRIX32 " op=up schema=2\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    selection.controller,
                    replay_control_name(selection.control),
                    phase,
                    descriptor,
                    item,
                    selection.to_item
                );
                std::fflush(stderr);
            }
        } else if (phase == selection.observed_phase &&
                   descriptor == selection.descriptor && item == selection.from_item) {
            const bool masked = replay_control_is_masked(selection.control);
            if (!masked) {
                apply_replay_control(
                    g_replay.snapshots[selection.controller],
                    selection.control,
                    true
                );
            }
            selection.requested = true;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_selection_request tick=%llu"
                " controller=%zu control=%s phase=0x%08" PRIX32
                " descriptor=0x%08" PRIX32 " item=0x%08" PRIX32
                " expected_item=0x%08" PRIX32 " deadline_tick=%llu"
                " op=down applied=%d schema=2\n",
                static_cast<unsigned long long>(g_replay.tick),
                selection.controller,
                replay_control_name(selection.control),
                phase,
                descriptor,
                item,
                selection.to_item,
                static_cast<unsigned long long>(selection.deadline_tick),
                masked ? 0 : 1
            );
            std::fflush(stderr);
        }
    }

    bool released_game_event = false;
    if (g_replay.game_event_cursor < g_replay.game_owned_events.size()) {
        GameOwnedReplayEvent& event =
            g_replay.game_owned_events[g_replay.game_event_cursor];
        if (replay_control_is_masked(event.control)) {
            if (g_replay.tick == 1) {
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_game_sequence_masked control=%s count=%zu"
                    " controller=%zu schema=2\n",
                    replay_control_name(event.control),
                    g_replay.game_owned_events.size(),
                    event.controller
                );
                std::fflush(stderr);
            }
        } else if (g_replay.tick > event.deadline_tick) {
            if (!event.deadline_reported) {
                event.deadline_reported = true;
                g_replay.deadline_missed = true;
                const bool button_was_down = g_replay.game_event_button_down;
                if (button_was_down) {
                    apply_replay_control(
                        g_replay.snapshots[event.controller],
                        event.control,
                        false
                    );
                    g_replay.game_event_button_down = false;
                    released_game_event = true;
                }
                const uint32_t phase =
                    bumble::native_checkpoint::last_frontend_phase();
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_game_event_deadline_missed tick=%llu"
                    " controller=%zu control=%s expected_phase=0x%08" PRIX32
                    " last_phase=0x%08" PRIX32 " deadline_tick=%llu"
                    " button_was_down=%d schema=2\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    event.controller,
                    replay_control_name(event.control),
                    event.observed_phase,
                    phase,
                    static_cast<unsigned long long>(event.deadline_tick),
                    button_was_down ? 1 : 0
                );
                std::fflush(stderr);
            }
        } else if (g_replay.game_event_button_down) {
            const uint32_t confirm_count =
                bumble::native_checkpoint::frontend_confirm_count();
            const uint32_t accepted_phase =
                bumble::native_checkpoint::last_accepted_frontend_phase();
            if (confirm_count > g_replay.game_event_confirm_baseline) {
                apply_replay_control(
                    g_replay.snapshots[event.controller],
                    event.control,
                    false
                );
                g_replay.game_event_button_down = false;
                released_game_event = true;
                const bool phase_matches = accepted_phase == event.observed_phase;
                if (!phase_matches) {
                    g_replay.deadline_missed = true;
                }
                ++g_replay.game_event_cursor;
                g_replay_completed_game_events.store(
                    static_cast<uint32_t>(g_replay.game_event_cursor),
                    std::memory_order_release
                );
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_game_event_ack tick=%llu"
                    " controller=%zu control=%s accepted_count=%" PRIu32
                    " expected_phase=0x%08" PRIX32 " accepted_phase=0x%08" PRIX32
                    " phase_matches=%d op=up schema=2\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    event.controller,
                    replay_control_name(event.control),
                    confirm_count,
                    event.observed_phase,
                    accepted_phase,
                    phase_matches ? 1 : 0
                );
                std::fflush(stderr);
            }
        } else {
            const uint32_t phase = bumble::native_checkpoint::last_frontend_phase();
            bool selection_ready = true;
            if (g_replay.selection_event.has_value() &&
                event.observed_phase == g_replay.selection_event->observed_phase) {
                const MenuSelectionReplayEvent& selection = *g_replay.selection_event;
                selection_ready = selection.acknowledged &&
                    bumble::native_checkpoint::last_frontend_descriptor() ==
                        selection.descriptor &&
                    bumble::native_checkpoint::last_frontend_descriptor_item() ==
                        selection.to_item;
            }
            if (phase == event.observed_phase &&
                g_replay.tick >= event.reference_tick && selection_ready &&
                !released_game_event) {
                g_replay.game_event_confirm_baseline =
                    bumble::native_checkpoint::frontend_confirm_count();
                apply_replay_control(
                    g_replay.snapshots[event.controller],
                    event.control,
                    true
                );
                g_replay.game_event_button_down = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_game_event_request tick=%llu"
                    " controller=%zu control=%s observed_phase=0x%08" PRIX32
                    " reference_tick=%llu deadline_tick=%llu selection_ready=%d"
                    " op=down schema=2\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    event.controller,
                    replay_control_name(event.control),
                    phase,
                    static_cast<unsigned long long>(event.reference_tick),
                    static_cast<unsigned long long>(event.deadline_tick),
                    selection_ready ? 1 : 0
                );
                std::fflush(stderr);
            }
        }
    }

    if (g_replay.two_player_probe.has_value()) {
        TwoPlayerReplayProbe& probe = *g_replay.two_player_probe;
        if (!probe.armed && g_replay.tick >= probe.not_before_tick &&
            bumble::native_checkpoint::two_player_state2_ready()) {
            probe.armed = true;
            probe.armed_tick = g_replay.tick;
            probe.expected_positive = !replay_control_is_masked(probe.control);
            bumble::native_checkpoint::arm_two_player_control_observation(
                probe.expected_positive
            );
            if (probe.expected_positive) {
                apply_replay_control(
                    g_replay.snapshots[probe.controller],
                    probe.control,
                    true
                );
                probe.applied = true;
            }
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_two_player_probe_armed tick=%llu"
                " controller=%zu control=%s masked=%d expect_positive=%d applied=%d"
                " not_before_tick=%llu hold_polls=%llu state2_ready=1 schema=2\n",
                static_cast<unsigned long long>(g_replay.tick),
                probe.controller,
                replay_control_name(probe.control),
                probe.expected_positive ? 0 : 1,
                probe.expected_positive ? 1 : 0,
                probe.applied ? 1 : 0,
                static_cast<unsigned long long>(probe.not_before_tick),
                static_cast<unsigned long long>(probe.hold_polls)
            );
            std::fflush(stderr);
        } else if (probe.armed && !probe.released &&
                   g_replay.tick - probe.armed_tick >= probe.hold_polls) {
            if (probe.applied) {
                apply_replay_control(
                    g_replay.snapshots[probe.controller],
                    probe.control,
                    false
                );
            }
            probe.released = true;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_two_player_probe_release tick=%llu"
                " controller=%zu control=%s expect_positive=%d applied=%d"
                " held_polls=%llu op=up schema=2\n",
                static_cast<unsigned long long>(g_replay.tick),
                probe.controller,
                replay_control_name(probe.control),
                probe.expected_positive ? 1 : 0,
                probe.applied ? 1 : 0,
                static_cast<unsigned long long>(g_replay.tick - probe.armed_tick)
            );
            std::fflush(stderr);
        }
    }

    while (g_replay.observation_cursor < g_replay.observations.size() &&
           g_replay.observations[g_replay.observation_cursor].tick == g_replay.tick) {
        const ReplayObservation& observation =
            g_replay.observations[g_replay.observation_cursor++];
        const bool selection_requested = g_replay.selection_event.has_value() &&
            g_replay.selection_event->requested;
        const bool selection_acked = g_replay.selection_event.has_value() &&
            g_replay.selection_event->acknowledged;
        const bool probe_armed = g_replay.two_player_probe.has_value() &&
            g_replay.two_player_probe->armed;
        const bool probe_released = g_replay.two_player_probe.has_value() &&
            g_replay.two_player_probe->released;
        const bool probe_expected_positive = g_replay.two_player_probe.has_value() &&
            g_replay.two_player_probe->expected_positive;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_observation tick=%llu label=%s"
            " schema=2 controllers=2 frontend_accepts=%" PRIu32
            " frontend_phase=0x%08" PRIX32
            " selection_requested=%d selection_acked=%d probe_armed=%d"
            " probe_released=%d probe_expected_positive=%d two_player_ready=%d"
            " two_player_checkpoint=%d two_player_positive_control=%d\n",
            static_cast<unsigned long long>(g_replay.tick),
            observation.label.c_str(),
            bumble::native_checkpoint::frontend_confirm_count(),
            bumble::native_checkpoint::last_frontend_phase(),
            selection_requested ? 1 : 0,
            selection_acked ? 1 : 0,
            probe_armed ? 1 : 0,
            probe_released ? 1 : 0,
            probe_expected_positive ? 1 : 0,
            bumble::native_checkpoint::two_player_state2_ready() ? 1 : 0,
            bumble::native_checkpoint::two_player_checkpoint_observed() ? 1 : 0,
            bumble::native_checkpoint::two_player_positive_control_observed() ? 1 : 0
        );
        std::fflush(stderr);
    }

    if (g_replay.tick >= g_replay.end_tick && !g_replay.completion_logged) {
        g_replay.completion_logged = true;
        const uint32_t accepts = bumble::native_checkpoint::frontend_confirm_count();
        const bool all_game_events =
            g_replay.game_event_cursor == g_replay.game_owned_events.size();
        const bool selection_requested = g_replay.selection_event.has_value() &&
            g_replay.selection_event->requested;
        const bool selection_acked = g_replay.selection_event.has_value() &&
            g_replay.selection_event->acknowledged;
        const bool probe_armed = g_replay.two_player_probe.has_value() &&
            g_replay.two_player_probe->armed;
        const bool probe_released = g_replay.two_player_probe.has_value() &&
            g_replay.two_player_probe->released;
        const bool probe_expected_positive = g_replay.two_player_probe.has_value() &&
            g_replay.two_player_probe->expected_positive;
        const bool two_player_ready =
            bumble::native_checkpoint::two_player_state2_ready();
        const bool two_player_checkpoint =
            bumble::native_checkpoint::two_player_checkpoint_observed();
        const bool positive_control =
            bumble::native_checkpoint::two_player_positive_control_observed();
        const bool probe_result_matches = probe_expected_positive
            ? positive_control
            : !positive_control;
        g_replay.semantic_success = !g_replay.deadline_missed &&
            all_game_events && accepts == g_replay.game_owned_events.size() &&
            selection_requested && selection_acked && probe_armed && probe_released &&
            (!g_replay.require_two_player_checkpoint || two_player_checkpoint) &&
            probe_result_matches;
        g_replay_semantic_success.store(g_replay.semantic_success, std::memory_order_release);
        g_replay_complete.store(true, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_complete tick=%llu schema=2 controllers=2"
            " completed_game_events=%zu expected_game_events=%zu frontend_accepts=%" PRIu32
            " selection_requested=%d selection_acked=%d probe_armed=%d"
            " probe_released=%d probe_expected_positive=%d two_player_ready=%d"
            " two_player_checkpoint=%d two_player_positive_control=%d"
            " probe_result_matches=%d deadline_missed=%d semantic_success=%d"
            " masked_control=%s\n",
            static_cast<unsigned long long>(g_replay.tick),
            g_replay.game_event_cursor,
            g_replay.game_owned_events.size(),
            accepts,
            selection_requested ? 1 : 0,
            selection_acked ? 1 : 0,
            probe_armed ? 1 : 0,
            probe_released ? 1 : 0,
            probe_expected_positive ? 1 : 0,
            two_player_ready ? 1 : 0,
            two_player_checkpoint ? 1 : 0,
            positive_control ? 1 : 0,
            probe_result_matches ? 1 : 0,
            g_replay.deadline_missed ? 1 : 0,
            g_replay.semantic_success ? 1 : 0,
            g_replay.masked_control.has_value()
                ? replay_control_name(*g_replay.masked_control)
                : "NONE"
        );
        std::fflush(stderr);
    }
}

void advance_schema3_replay_locked() {
    if (!g_replay.configured || !g_replay.later_progression.has_value()) {
        return;
    }
    if (!g_replay.armed) {
        g_replay.snapshot = ControllerSnapshot{
            .connected = true,
            .connected_pak = g_replay.replay_connected_pak,
        };
        return;
    }

    ++g_replay.tick;
    g_replay_tick.store(g_replay.tick, std::memory_order_release);
    LaterProgressionReplay& later = *g_replay.later_progression;

    while (g_replay.scheduled_cursor < g_replay.scheduled_events.size() &&
           g_replay.scheduled_events[g_replay.scheduled_cursor].tick == g_replay.tick) {
        const ScheduledReplayEvent& event =
            g_replay.scheduled_events[g_replay.scheduled_cursor++];
        const bool masked = replay_control_is_masked(event.control);
        if (!masked) {
            apply_replay_control(g_replay.snapshot, event.control, event.down);
        }
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_event tick=%llu source=scheduled"
            " op=%s control=%s applied=%d schema=3\n",
            static_cast<unsigned long long>(g_replay.tick),
            event.down ? "down" : "up",
            replay_control_name(event.control),
            masked ? 0 : 1
        );
        std::fflush(stderr);
    }

    bool released_game_event = false;
    if (g_replay.game_event_cursor < g_replay.game_owned_events.size()) {
        GameOwnedReplayEvent& event =
            g_replay.game_owned_events[g_replay.game_event_cursor];
        if (g_replay.tick > event.deadline_tick) {
            if (!event.deadline_reported) {
                event.deadline_reported = true;
                g_replay.deadline_missed = true;
                const bool button_was_down = g_replay.game_event_button_down;
                if (button_was_down) {
                    apply_replay_control(g_replay.snapshot, event.control, false);
                    g_replay.game_event_button_down = false;
                    released_game_event = true;
                }
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_game_event_deadline_missed"
                    " tick=%llu control=A expected_phase=0x%08" PRIX32
                    " last_phase=0x%08" PRIX32 " deadline_tick=%llu"
                    " button_was_down=%d schema=3\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    event.observed_phase,
                    bumble::native_checkpoint::last_frontend_phase(),
                    static_cast<unsigned long long>(event.deadline_tick),
                    button_was_down ? 1 : 0
                );
                std::fflush(stderr);
            }
        } else if (g_replay.game_event_button_down) {
            const uint32_t confirm_count =
                bumble::native_checkpoint::frontend_confirm_count();
            const uint32_t accepted_phase =
                bumble::native_checkpoint::last_accepted_frontend_phase();
            if (confirm_count > g_replay.game_event_confirm_baseline) {
                apply_replay_control(g_replay.snapshot, event.control, false);
                g_replay.game_event_button_down = false;
                released_game_event = true;
                const bool phase_matches = accepted_phase == event.observed_phase;
                if (!phase_matches) {
                    g_replay.deadline_missed = true;
                }
                ++g_replay.game_event_cursor;
                g_replay_completed_game_events.store(
                    static_cast<uint32_t>(g_replay.game_event_cursor),
                    std::memory_order_release
                );
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_game_event_ack tick=%llu"
                    " control=A accepted_count=%" PRIu32
                    " expected_phase=0x%08" PRIX32
                    " accepted_phase=0x%08" PRIX32
                    " phase_matches=%d op=up schema=3\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    confirm_count,
                    event.observed_phase,
                    accepted_phase,
                    phase_matches ? 1 : 0
                );
                std::fflush(stderr);
            }
        } else {
            const uint32_t phase = bumble::native_checkpoint::last_frontend_phase();
            if (phase == event.observed_phase &&
                g_replay.tick >= event.reference_tick &&
                !released_game_event) {
                g_replay.game_event_confirm_baseline =
                    bumble::native_checkpoint::frontend_confirm_count();
                apply_replay_control(g_replay.snapshot, event.control, true);
                g_replay.game_event_button_down = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_game_event_request tick=%llu"
                    " control=A observed_phase=0x%08" PRIX32
                    " reference_tick=%llu deadline_tick=%llu op=down schema=3\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    phase,
                    static_cast<unsigned long long>(event.reference_tick),
                    static_cast<unsigned long long>(event.deadline_tick)
                );
                std::fflush(stderr);
            }
        }
    }

    const bool pre_fork_complete =
        g_replay.game_event_cursor == g_replay.game_owned_events.size();
    const bool masked_late_control =
        g_replay.masked_control == ReplayControl::DpadLeft;
    if (pre_fork_complete && !g_replay.game_event_button_down) {
        const uint32_t phase = bumble::native_checkpoint::last_frontend_phase();
        if (!later.cheat_started && phase == later.cheat_phase) {
            later.cheat_started = true;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_level_select_cheat_started"
                " tick=%llu phase=0x%08" PRIX32 " steps=%zu schema=3\n",
                static_cast<unsigned long long>(g_replay.tick),
                phase,
                later.cheat_sequence.size()
            );
            std::fflush(stderr);
        }

        if (later.cheat_started && !later.cheat_attempt_complete) {
            if (later.cheat_direction_down) {
                const ReplayControl control =
                    later.cheat_sequence[later.cheat_cursor];
                const bool masked = replay_control_is_masked(control);
                if (!masked) {
                    apply_replay_control(g_replay.snapshot, control, false);
                }
                later.cheat_direction_down = false;
                if (later.cheat_cursor == 3u && later.cheat_z_down) {
                    apply_replay_control(g_replay.snapshot, ReplayControl::Z, false);
                    later.cheat_z_down = false;
                }
                const uint32_t progress =
                    bumble::native_checkpoint::level_select_cheat_progress();
                const uint32_t expected_progress =
                    static_cast<uint32_t>(later.cheat_cursor + 1u);
                const bool acknowledged = progress >= expected_progress;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_level_select_cheat_step_release"
                    " tick=%llu step=%zu control=%s applied=%d"
                    " progress=%" PRIu32 " expected_progress=%" PRIu32
                    " acknowledged=%d z_held=%d schema=3\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    later.cheat_cursor + 1u,
                    replay_control_name(control),
                    masked ? 0 : 1,
                    progress,
                    expected_progress,
                    acknowledged ? 1 : 0,
                    later.cheat_z_down ? 1 : 0
                );
                std::fflush(stderr);
                if (masked || masked_late_control) {
                    ++later.cheat_cursor;
                } else if (acknowledged) {
                    ++later.cheat_cursor;
                } else {
                    later.cheat_waiting_ack = true;
                }
            } else if (later.cheat_waiting_ack) {
                const uint32_t expected_progress =
                    static_cast<uint32_t>(later.cheat_cursor + 1u);
                if (bumble::native_checkpoint::level_select_cheat_progress() >=
                    expected_progress) {
                    ++later.cheat_cursor;
                    later.cheat_waiting_ack = false;
                }
            } else if (later.cheat_cursor < later.cheat_sequence.size()) {
                if (later.cheat_cursor == 0u && !later.cheat_z_down) {
                    apply_replay_control(g_replay.snapshot, ReplayControl::Z, true);
                    later.cheat_z_down = true;
                }
                const ReplayControl control =
                    later.cheat_sequence[later.cheat_cursor];
                const bool masked = replay_control_is_masked(control);
                later.cheat_progress_baseline =
                    bumble::native_checkpoint::level_select_cheat_progress();
                if (!masked) {
                    apply_replay_control(g_replay.snapshot, control, true);
                }
                later.cheat_direction_down = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_level_select_cheat_step_request"
                    " tick=%llu step=%zu control=%s applied=%d"
                    " progress_baseline=%" PRIu32 " z_held=%d schema=3\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    later.cheat_cursor + 1u,
                    replay_control_name(control),
                    masked ? 0 : 1,
                    later.cheat_progress_baseline,
                    later.cheat_z_down ? 1 : 0
                );
                std::fflush(stderr);
            }

            if (later.cheat_cursor == later.cheat_sequence.size() &&
                !later.cheat_direction_down && !later.cheat_waiting_ack) {
                later.cheat_attempt_complete = true;
                const bool game_complete =
                    bumble::native_checkpoint::level_select_cheat_complete();
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_level_select_cheat_attempt_complete"
                    " tick=%llu game_complete=%d masked_late_control=%d schema=3\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    game_complete ? 1 : 0,
                    masked_late_control ? 1 : 0
                );
                std::fflush(stderr);
                if (game_complete == masked_late_control) {
                    g_replay.deadline_missed = true;
                }
            }
        }

        if (later.cheat_started && !later.cheat_attempt_complete &&
            g_replay.tick > later.cheat_deadline_tick &&
            !later.deadline_reported) {
            later.deadline_reported = true;
            g_replay.deadline_missed = true;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_later_progression_deadline_missed"
                " tick=%llu reason=cheat progress=%" PRIu32
                " cursor=%zu deadline_tick=%llu schema=3\n",
                static_cast<unsigned long long>(g_replay.tick),
                bumble::native_checkpoint::level_select_cheat_progress(),
                later.cheat_cursor,
                static_cast<unsigned long long>(later.cheat_deadline_tick)
            );
            std::fflush(stderr);
        }

        const bool game_cheat_complete =
            bumble::native_checkpoint::level_select_cheat_complete();
        if (later.cheat_attempt_complete && !later.main_menu_acked) {
            const bool route_ready = masked_late_control || game_cheat_complete;
            if (route_ready && !later.main_menu_a_down && phase == later.cheat_phase) {
                later.main_menu_confirm_baseline =
                    bumble::native_checkpoint::frontend_confirm_count();
                apply_replay_control(g_replay.snapshot, ReplayControl::A, true);
                later.main_menu_a_down = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_later_main_menu_request"
                    " tick=%llu game_cheat_complete=%d op=down schema=3\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    game_cheat_complete ? 1 : 0
                );
                std::fflush(stderr);
            } else if (later.main_menu_a_down &&
                bumble::native_checkpoint::frontend_confirm_count() >
                    later.main_menu_confirm_baseline) {
                apply_replay_control(g_replay.snapshot, ReplayControl::A, false);
                later.main_menu_a_down = false;
                later.main_menu_acked =
                    bumble::native_checkpoint::last_accepted_frontend_phase() ==
                    later.cheat_phase;
                if (!later.main_menu_acked) {
                    g_replay.deadline_missed = true;
                }
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_later_main_menu_ack"
                    " tick=%llu accepted_phase=0x%08" PRIX32
                    " phase_matches=%d op=up schema=3\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    bumble::native_checkpoint::last_accepted_frontend_phase(),
                    later.main_menu_acked ? 1 : 0
                );
                std::fflush(stderr);
            }
        }

        if (later.main_menu_acked && !later.control_phase_e_acked) {
            if (!later.control_phase_e_a_down && phase == 0x0Eu) {
                if (!later.pi_capture_started) {
                    bumble::native_pi_dma::set_capture_enabled(true);
                    later.pi_capture_started = true;
                }
                later.control_phase_e_confirm_baseline =
                    bumble::native_checkpoint::frontend_confirm_count();
                apply_replay_control(g_replay.snapshot, ReplayControl::A, true);
                later.control_phase_e_a_down = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_later_phase_e_request"
                    " tick=%llu masked_late_control=%d op=down schema=3\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    masked_late_control ? 1 : 0
                );
                std::fflush(stderr);
            } else if (later.control_phase_e_a_down &&
                bumble::native_checkpoint::frontend_confirm_count() >
                    later.control_phase_e_confirm_baseline) {
                apply_replay_control(g_replay.snapshot, ReplayControl::A, false);
                later.control_phase_e_a_down = false;
                later.control_phase_e_acked =
                    bumble::native_checkpoint::last_accepted_frontend_phase() == 0x0Eu;
                if (!later.control_phase_e_acked) {
                    g_replay.deadline_missed = true;
                }
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_later_phase_e_ack"
                    " tick=%llu masked_late_control=%d phase_matches=%d"
                    " op=up schema=3\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    masked_late_control ? 1 : 0,
                    later.control_phase_e_acked ? 1 : 0
                );
                std::fflush(stderr);
            }
        }

        if (later.main_menu_acked && later.control_phase_e_acked &&
            !masked_late_control) {
            const bool selector_initialized =
                bumble::native_checkpoint::mission2_selector_initialized();
            const uint32_t selector_index =
                bumble::native_checkpoint::campaign_selector_index();
            const bool selected =
                selector_index == later.target_level_index;
            const bool committed =
                bumble::native_checkpoint::campaign_selection_committed_index() ==
                    later.target_level_index;
            const bool player_observed =
                bumble::native_checkpoint::campaign_player_level_observed() ==
                    later.target_level_index;
            const char* selection_request_stage =
                later.target_level_index == 2u
                    ? "replay_mission2_selection_request"
                    : "replay_campaign_selection_request";
            const char* selection_release_stage =
                later.target_level_index == 2u
                    ? "replay_mission2_selection_release"
                    : "replay_campaign_selection_release";
            const char* commit_request_stage =
                later.target_level_index == 2u
                    ? "replay_mission2_commit_request"
                    : "replay_campaign_commit_request";
            const char* commit_ack_stage =
                later.target_level_index == 2u
                    ? "replay_mission2_commit_ack"
                    : "replay_campaign_commit_ack";
            if (selector_initialized && !selected &&
                !later.selector_pulse_down) {
                apply_replay_control(
                    g_replay.snapshot,
                    ReplayControl::StickUp,
                    true
                );
                later.selector_pulse_down = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=%s"
                    " tick=%llu control=STICK_U current=%" PRIu32
                    " target=%" PRIu32 " op=down schema=3\n",
                    selection_request_stage,
                    static_cast<unsigned long long>(g_replay.tick),
                    selector_index,
                    later.target_level_index
                );
                std::fflush(stderr);
            } else if (later.selector_pulse_down) {
                apply_replay_control(
                    g_replay.snapshot,
                    ReplayControl::StickUp,
                    false
                );
                later.selector_pulse_down = false;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=%s"
                    " tick=%llu current=%" PRIu32 " target=%" PRIu32
                    " selected=%d op=up schema=3\n",
                    selection_release_stage,
                    static_cast<unsigned long long>(g_replay.tick),
                    selector_index,
                    later.target_level_index,
                    selected ? 1 : 0
                );
                std::fflush(stderr);
            }

            if (selected && !committed && !later.commit_a_sent) {
                apply_replay_control(g_replay.snapshot, ReplayControl::A, true);
                later.commit_a_down = true;
                later.commit_a_sent = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=%s"
                    " tick=%llu target=%" PRIu32 " op=down schema=3\n",
                    commit_request_stage,
                    static_cast<unsigned long long>(g_replay.tick),
                    later.target_level_index
                );
                std::fflush(stderr);
            } else if (later.commit_a_down && committed) {
                apply_replay_control(g_replay.snapshot, ReplayControl::A, false);
                later.commit_a_down = false;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=%s"
                    " tick=%llu target=%" PRIu32 " op=up schema=3\n",
                    commit_ack_stage,
                    static_cast<unsigned long long>(g_replay.tick),
                    later.target_level_index
                );
                std::fflush(stderr);
            }

            const char* missed_reason = nullptr;
            uint64_t missed_deadline = 0;
            if (!selector_initialized &&
                g_replay.tick > later.selector_deadline_tick) {
                missed_reason = "selector_initialized";
                missed_deadline = later.selector_deadline_tick;
            } else if (!selected && g_replay.tick > later.commit_deadline_tick) {
                missed_reason = "campaign_level_selected";
                missed_deadline = later.commit_deadline_tick;
            } else if (!committed && g_replay.tick > later.player_deadline_tick) {
                missed_reason = "selection_committed";
                missed_deadline = later.player_deadline_tick;
            } else if (!player_observed &&
                g_replay.tick > later.player_deadline_tick) {
                missed_reason = "campaign_player";
                missed_deadline = later.player_deadline_tick;
            }
            if (missed_reason != nullptr && !later.deadline_reported) {
                later.deadline_reported = true;
                g_replay.deadline_missed = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_later_progression_deadline_missed"
                    " tick=%llu reason=%s deadline_tick=%llu schema=3\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    missed_reason,
                    static_cast<unsigned long long>(missed_deadline)
                );
                std::fflush(stderr);
            }
        }
    }

    while (g_replay.observation_cursor < g_replay.observations.size() &&
           g_replay.observations[g_replay.observation_cursor].tick == g_replay.tick) {
        const ReplayObservation& observation =
            g_replay.observations[g_replay.observation_cursor++];
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_observation tick=%llu label=%s"
            " schema=3 frontend_accepts=%" PRIu32
            " frontend_phase=0x%08" PRIX32
            " cheat_progress=%" PRIu32 " cheat_complete=%d"
            " selector_initialized=%d mission2_selected=%d"
            " selection_committed=%d mission1_player=%d mission2_player=%d\n",
            static_cast<unsigned long long>(g_replay.tick),
            observation.label.c_str(),
            bumble::native_checkpoint::frontend_confirm_count(),
            bumble::native_checkpoint::last_frontend_phase(),
            bumble::native_checkpoint::level_select_cheat_progress(),
            bumble::native_checkpoint::level_select_cheat_complete() ? 1 : 0,
            bumble::native_checkpoint::mission2_selector_initialized() ? 1 : 0,
            bumble::native_checkpoint::mission2_selected() ? 1 : 0,
            bumble::native_checkpoint::mission2_selection_committed() ? 1 : 0,
            bumble::native_checkpoint::mission1_player_observed() ? 1 : 0,
            bumble::native_checkpoint::mission2_player_observed() ? 1 : 0
        );
        std::fflush(stderr);
    }

    if (g_replay.tick >= g_replay.end_tick && !g_replay.completion_logged) {
        constexpr uint64_t kCaptureDrainGraceTicks = 120;
        if (later.pi_capture_started && !later.capture_drain_started) {
            bumble::native_rsp_task::begin_capture_drain();
            bumble::native_pi_dma::begin_capture_drain();
            later.capture_drain_started = true;
        }
        const uint64_t pending_pi_transfers = later.pi_capture_started
            ? bumble::native_pi_dma::pending_transfer_count()
            : 0;
        const bumble::native_rsp_task::CaptureStats pre_stop_rsp_stats =
            bumble::native_rsp_task::capture_stats();
        const uint64_t pending_rsp_lifecycles = later.pi_capture_started
            ? pre_stop_rsp_stats.pending_sampled_lifecycles
            : 0;
        const bool rsp_contract_ready = later.pi_capture_started &&
            pre_stop_rsp_stats.contract_passed;
        const bool capture_pending = later.pi_capture_started &&
            (pending_pi_transfers != 0 || !rsp_contract_ready);
        if (capture_pending &&
            g_replay.tick < g_replay.end_tick + kCaptureDrainGraceTicks) {
            if (!later.capture_drain_wait_logged) {
                later.capture_drain_wait_logged = true;
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=replay_later_capture_drain_wait"
                    " tick=%llu pending_pi_transfers=%llu"
                    " pending_rsp_sampled_lifecycles=%llu"
                    " rsp_contract_ready=%d"
                    " deadline_tick=%llu schema=3\n",
                    static_cast<unsigned long long>(g_replay.tick),
                    static_cast<unsigned long long>(pending_pi_transfers),
                    static_cast<unsigned long long>(pending_rsp_lifecycles),
                    rsp_contract_ready ? 1 : 0,
                    static_cast<unsigned long long>(
                        g_replay.end_tick + kCaptureDrainGraceTicks
                    )
                );
                std::fflush(stderr);
            }
            return;
        }
        if (capture_pending) {
            g_replay.deadline_missed = true;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_later_capture_drain_deadline_missed"
                " tick=%llu pending_pi_transfers=%llu"
                " pending_rsp_sampled_lifecycles=%llu"
                " rsp_contract_ready=%d"
                " deadline_tick=%llu schema=3\n",
                static_cast<unsigned long long>(g_replay.tick),
                static_cast<unsigned long long>(pending_pi_transfers),
                static_cast<unsigned long long>(pending_rsp_lifecycles),
                rsp_contract_ready ? 1 : 0,
                static_cast<unsigned long long>(
                    g_replay.end_tick + kCaptureDrainGraceTicks
                )
            );
            std::fflush(stderr);
        }

        g_replay.completion_logged = true;
        if (later.cheat_direction_down &&
            later.cheat_cursor < later.cheat_sequence.size()) {
            apply_replay_control(
                g_replay.snapshot,
                later.cheat_sequence[later.cheat_cursor],
                false
            );
        }
        if (later.cheat_z_down) {
            apply_replay_control(g_replay.snapshot, ReplayControl::Z, false);
        }
        if (later.main_menu_a_down || later.commit_a_down ||
            later.control_phase_e_a_down) {
            apply_replay_control(g_replay.snapshot, ReplayControl::A, false);
        }
        if (later.selector_pulse_down) {
            apply_replay_control(
                g_replay.snapshot,
                ReplayControl::StickUp,
                false
            );
        }

        if (later.pi_capture_started) {
            bumble::native_pi_dma::set_capture_enabled(false);
        }
        const bumble::native_rsp_task::CaptureStats rsp_stats =
            bumble::native_rsp_task::finalize_capture();
        const bool rsp_task_contract = later.pi_capture_started &&
            rsp_stats.contract_passed;
        const bumble::native_pi_dma::CaptureStats pi_stats =
            bumble::native_pi_dma::capture_stats();
        const bool pi_dma_contract = later.pi_capture_started &&
            pi_stats.transfers > 0 &&
            pi_stats.transfers == pi_stats.receipts &&
            pi_stats.successful_starts == pi_stats.transfers &&
            pi_stats.successful_receives == pi_stats.receipts &&
            pi_stats.associated_receipts == pi_stats.receipts &&
            pi_stats.verified_copies == pi_stats.transfers &&
            pi_stats.start_failures == 0 &&
            pi_stats.receive_failures == 0 &&
            pi_stats.unassociated_receipts == 0 &&
            pi_stats.receipt_call_site_mismatches == 0 &&
            pi_stats.copy_mismatches == 0 &&
            pi_stats.source_bounds_failures == 0 &&
            pi_stats.destination_bounds_failures == 0 &&
            pi_stats.bounds_failures == 0 &&
            pi_stats.alignment_failures == 0 &&
            pi_stats.unknown_call_sites == 0 &&
            pi_stats.unknown_receipt_pcs == 0 &&
            pi_stats.blocking_transfers > 0 &&
            pi_stats.chunked_transfers > 0 &&
            pi_stats.audio_page_transfers > 0 &&
            pi_stats.blocking_transfers == pi_stats.blocking_receipts &&
            pi_stats.chunked_transfers == pi_stats.chunked_receipts &&
            pi_stats.audio_page_transfers == pi_stats.audio_page_receipts &&
            pi_stats.unknown_transfers == 0 &&
            pi_stats.unknown_receipts == 0 &&
            pi_stats.pending_transfers == 0;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_pi_dma_contract schema=3"
            " transfers=%llu receipts=%llu verified_copies=%llu"
            " copy_mismatches=%llu blocking_transfers=%llu"
            " chunked_transfers=%llu audio_page_transfers=%llu"
            " excluded_unadmitted_receipts=%llu"
            " excluded_pre_admission_receipts=%llu"
            " excluded_post_boundary_receipts=%llu"
            " pending_transfers=%llu detailed_transfer_logs=%llu"
            " detailed_receipt_logs=%llu contract_passed=%d\n",
            static_cast<unsigned long long>(pi_stats.transfers),
            static_cast<unsigned long long>(pi_stats.receipts),
            static_cast<unsigned long long>(pi_stats.verified_copies),
            static_cast<unsigned long long>(pi_stats.copy_mismatches),
            static_cast<unsigned long long>(pi_stats.blocking_transfers),
            static_cast<unsigned long long>(pi_stats.chunked_transfers),
            static_cast<unsigned long long>(pi_stats.audio_page_transfers),
            static_cast<unsigned long long>(
                pi_stats.excluded_unadmitted_receipts
            ),
            static_cast<unsigned long long>(
                pi_stats.excluded_pre_admission_receipts
            ),
            static_cast<unsigned long long>(
                pi_stats.excluded_post_boundary_receipts
            ),
            static_cast<unsigned long long>(pi_stats.pending_transfers),
            static_cast<unsigned long long>(pi_stats.detailed_transfer_logs),
            static_cast<unsigned long long>(pi_stats.detailed_receipt_logs),
            pi_dma_contract ? 1 : 0
        );
        std::fflush(stderr);

        const bool all_game_events =
            g_replay.game_event_cursor == g_replay.game_owned_events.size();
        const bool cheat_complete =
            bumble::native_checkpoint::level_select_cheat_complete();
        const bool selector_initialized =
            bumble::native_checkpoint::mission2_selector_initialized();
        const bool selected = bumble::native_checkpoint::mission2_selected();
        const bool committed =
            bumble::native_checkpoint::mission2_selection_committed();
        const bool mission1 =
            bumble::native_checkpoint::mission1_player_observed();
        const bool mission2 =
            bumble::native_checkpoint::mission2_player_observed();
        const bool target_selected =
            bumble::native_checkpoint::campaign_selector_index() ==
                later.target_level_index;
        const bool target_committed =
            bumble::native_checkpoint::campaign_selection_committed_index() ==
                later.target_level_index;
        const bool target_player =
            bumble::native_checkpoint::campaign_player_level_observed() ==
                later.target_level_index;
        const bool exact_mission2_contract = later.target_level_index != 2u ||
            (selected && committed &&
             (!g_replay.require_mission2_checkpoint || mission2));
        const bool positive_success = all_game_events &&
            later.cheat_attempt_complete && cheat_complete &&
            later.main_menu_acked && later.control_phase_e_acked &&
            selector_initialized && target_selected && target_committed &&
            target_player && exact_mission2_contract && !mission1;
        const bool control_success = all_game_events &&
            later.cheat_attempt_complete && !cheat_complete &&
            later.main_menu_acked && later.control_phase_e_acked &&
            !selector_initialized && !selected && !committed && !mission2 && mission1;
        g_replay.semantic_success = !g_replay.deadline_missed && pi_dma_contract &&
            rsp_task_contract &&
            (masked_late_control ? control_success : positive_success);
        g_replay_semantic_success.store(
            g_replay.semantic_success,
            std::memory_order_release
        );
        g_replay_complete.store(true, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_complete tick=%llu schema=3 controllers=1"
            " completed_game_events=%zu expected_game_events=%zu"
            " frontend_accepts=%" PRIu32 " cheat_attempt_complete=%d"
            " cheat_complete=%d selector_initialized=%d mission2_selected=%d"
            " selection_committed=%d mission1_player=%d mission2_player=%d"
            " target_level=%" PRIu32 " target_selected=%d"
            " target_committed=%d target_player=%d"
            " control_phase_e_acked=%d deadline_missed=%d semantic_success=%d"
            " pi_dma_contract=%d pi_dma_transfers=%llu"
            " rsp_task_contract=%d rsp_audio_correlated=%llu"
            " rsp_graphics_correlated=%llu masked_control=%s\n",
            static_cast<unsigned long long>(g_replay.tick),
            g_replay.game_event_cursor,
            g_replay.game_owned_events.size(),
            bumble::native_checkpoint::frontend_confirm_count(),
            later.cheat_attempt_complete ? 1 : 0,
            cheat_complete ? 1 : 0,
            selector_initialized ? 1 : 0,
            selected ? 1 : 0,
            committed ? 1 : 0,
            mission1 ? 1 : 0,
            mission2 ? 1 : 0,
            later.target_level_index,
            target_selected ? 1 : 0,
            target_committed ? 1 : 0,
            target_player ? 1 : 0,
            later.control_phase_e_acked ? 1 : 0,
            g_replay.deadline_missed ? 1 : 0,
            g_replay.semantic_success ? 1 : 0,
            pi_dma_contract ? 1 : 0,
            static_cast<unsigned long long>(pi_stats.transfers),
            rsp_task_contract ? 1 : 0,
            static_cast<unsigned long long>(rsp_stats.fully_correlated_audio),
            static_cast<unsigned long long>(rsp_stats.fully_correlated_graphics),
            g_replay.masked_control.has_value()
                ? replay_control_name(*g_replay.masked_control)
                : "NONE"
        );
        std::fflush(stderr);
    }
}

void advance_replay_locked() {
    if (g_replay.schema_version == 2) {
        advance_schema2_replay_locked();
    } else if (g_replay.schema_version == 3) {
        advance_schema3_replay_locked();
    } else if (g_replay.schema_version == 4) {
        advance_schema4_replay_locked();
    } else if (g_replay.schema_version == 5) {
        advance_schema5_replay_locked();
    } else {
        advance_schema1_replay_locked();
    }
}

float normalize_axis(Sint16 value) {
    if (std::abs(static_cast<int>(value)) <= kMovementStickDeadzone) {
        return 0.0f;
    }
    const float denominator = value < 0 ? 32768.0f : 32767.0f;
    return std::clamp(static_cast<float>(value) / denominator, -1.0f, 1.0f);
}

float normalize_look_axis(Sint16 value, Sint16 deadzone) {
    if (std::abs(static_cast<int>(value)) <= deadzone) {
        return 0.0f;
    }
    const float denominator = value < 0 ? 32768.0f : 32767.0f;
    return std::clamp(static_cast<float>(value) / denominator, -1.0f, 1.0f);
}

void clamp_physical_stick(ControllerSnapshot& snapshot) {
    const float magnitude = std::hypot(snapshot.x, snapshot.y);
    if (magnitude > kN64StickMagnitude) {
        const float scale = kN64StickMagnitude / magnitude;
        snapshot.x *= scale;
        snapshot.y *= scale;
    }
}

bool controller_input_down(
    SDL_GameController* controller,
    bumble::input_bindings::ControllerInput input
) {
    using bumble::input_bindings::ControllerInput;
    if (controller == nullptr ||
        SDL_GameControllerGetAttached(controller) == SDL_FALSE) {
        return false;
    }
    switch (input) {
    case ControllerInput::None: return false;
    case ControllerInput::A:
        return SDL_GameControllerGetButton(
            controller, SDL_CONTROLLER_BUTTON_A) != 0;
    case ControllerInput::B:
        return SDL_GameControllerGetButton(
            controller, SDL_CONTROLLER_BUTTON_B) != 0;
    case ControllerInput::X:
        return SDL_GameControllerGetButton(
            controller, SDL_CONTROLLER_BUTTON_X) != 0;
    case ControllerInput::Y:
        return SDL_GameControllerGetButton(
            controller, SDL_CONTROLLER_BUTTON_Y) != 0;
    case ControllerInput::Back:
        return SDL_GameControllerGetButton(
            controller, SDL_CONTROLLER_BUTTON_BACK) != 0;
    case ControllerInput::Guide:
        return SDL_GameControllerGetButton(
            controller, SDL_CONTROLLER_BUTTON_GUIDE) != 0;
    case ControllerInput::Start:
        return SDL_GameControllerGetButton(
            controller, SDL_CONTROLLER_BUTTON_START) != 0;
    case ControllerInput::LeftStick:
        return SDL_GameControllerGetButton(
            controller, SDL_CONTROLLER_BUTTON_LEFTSTICK) != 0;
    case ControllerInput::RightStick:
        return SDL_GameControllerGetButton(
            controller, SDL_CONTROLLER_BUTTON_RIGHTSTICK) != 0;
    case ControllerInput::LeftShoulder:
        return SDL_GameControllerGetButton(
            controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
    case ControllerInput::RightShoulder:
        return SDL_GameControllerGetButton(
            controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;
    case ControllerInput::DpadUp:
        return SDL_GameControllerGetButton(
            controller, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
    case ControllerInput::DpadDown:
        return SDL_GameControllerGetButton(
            controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;
    case ControllerInput::DpadLeft:
        return SDL_GameControllerGetButton(
            controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0;
    case ControllerInput::DpadRight:
        return SDL_GameControllerGetButton(
            controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0;
    case ControllerInput::LeftTrigger:
        return SDL_GameControllerGetAxis(
            controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > kCButtonThreshold;
    case ControllerInput::RightTrigger:
        return SDL_GameControllerGetAxis(
            controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > kCButtonThreshold;
    case ControllerInput::LeftStickUp:
        return SDL_GameControllerGetAxis(
            controller, SDL_CONTROLLER_AXIS_LEFTY) < -kCButtonThreshold;
    case ControllerInput::LeftStickDown:
        return SDL_GameControllerGetAxis(
            controller, SDL_CONTROLLER_AXIS_LEFTY) > kCButtonThreshold;
    case ControllerInput::LeftStickLeft:
        return SDL_GameControllerGetAxis(
            controller, SDL_CONTROLLER_AXIS_LEFTX) < -kCButtonThreshold;
    case ControllerInput::LeftStickRight:
        return SDL_GameControllerGetAxis(
            controller, SDL_CONTROLLER_AXIS_LEFTX) > kCButtonThreshold;
    case ControllerInput::RightStickUp:
        return SDL_GameControllerGetAxis(
            controller, SDL_CONTROLLER_AXIS_RIGHTY) < -kCButtonThreshold;
    case ControllerInput::RightStickDown:
        return SDL_GameControllerGetAxis(
            controller, SDL_CONTROLLER_AXIS_RIGHTY) > kCButtonThreshold;
    case ControllerInput::RightStickLeft:
        return SDL_GameControllerGetAxis(
            controller, SDL_CONTROLLER_AXIS_RIGHTX) < -kCButtonThreshold;
    case ControllerInput::RightStickRight:
        return SDL_GameControllerGetAxis(
            controller, SDL_CONTROLLER_AXIS_RIGHTX) > kCButtonThreshold;
    }
    return false;
}

bool controller_action_down(
    SDL_GameController* controller,
    const bumble::input_bindings::Settings& settings,
    bumble::input_bindings::InputAction action
) {
    const auto& bindings =
        settings.controller[static_cast<size_t>(action)];
    return std::any_of(
        bindings.begin(),
        bindings.end(),
        [controller](uint16_t binding) {
            return controller_input_down(
                controller,
                static_cast<bumble::input_bindings::ControllerInput>(binding)
            );
        }
    );
}

bumble::input_bindings::ControllerInput first_controller_input(
    SDL_GameController* controller
) {
    using bumble::input_bindings::ControllerInput;
    constexpr std::array<ControllerInput, 25> inputs{
        ControllerInput::A,
        ControllerInput::B,
        ControllerInput::X,
        ControllerInput::Y,
        ControllerInput::Back,
        ControllerInput::Guide,
        ControllerInput::Start,
        ControllerInput::LeftStick,
        ControllerInput::RightStick,
        ControllerInput::LeftShoulder,
        ControllerInput::RightShoulder,
        ControllerInput::DpadUp,
        ControllerInput::DpadDown,
        ControllerInput::DpadLeft,
        ControllerInput::DpadRight,
        ControllerInput::LeftTrigger,
        ControllerInput::RightTrigger,
        ControllerInput::LeftStickUp,
        ControllerInput::LeftStickDown,
        ControllerInput::LeftStickLeft,
        ControllerInput::LeftStickRight,
        ControllerInput::RightStickUp,
        ControllerInput::RightStickDown,
        ControllerInput::RightStickLeft,
        ControllerInput::RightStickRight,
    };
    const auto found = std::find_if(
        inputs.begin(),
        inputs.end(),
        [controller](ControllerInput input) {
            return controller_input_down(controller, input);
        }
    );
    return found == inputs.end() ? ControllerInput::None : *found;
}

bool controller_is_neutral(SDL_GameController* controller) {
    return first_controller_input(controller) ==
        bumble::input_bindings::ControllerInput::None;
}

bool controller_preferred_for_gameplay(
    const bumble::input_bindings::Settings& settings
) {
    const bool available =
        g_connected_controller_count.load(std::memory_order_acquire) != 0u;
    switch (settings.device_mode) {
    case bumble::input_bindings::InputDeviceMode::Auto:
        return available;
    case bumble::input_bindings::InputDeviceMode::KeyboardMouse:
        return false;
    case bumble::input_bindings::InputDeviceMode::Controller:
        return available;
    }
    return false;
}

bool pause_input_blocked(
    std::atomic_bool& context_active,
    std::atomic_bool& requires_release,
    std::atomic_uint8_t& neutral_polls,
    bool pause_menu,
    bool pause_down
) {
    const bool was_active = context_active.exchange(
        pause_menu,
        std::memory_order_acq_rel
    );
    if (pause_menu && !was_active) {
        requires_release.store(true, std::memory_order_release);
        neutral_polls.store(0u, std::memory_order_release);
    } else if (!pause_menu) {
        requires_release.store(false, std::memory_order_release);
        neutral_polls.store(0u, std::memory_order_release);
    } else if (requires_release.load(std::memory_order_acquire)) {
        if (pause_down) {
            neutral_polls.store(0u, std::memory_order_release);
        } else if (neutral_polls.fetch_add(1u, std::memory_order_acq_rel) + 1u >= 12u) {
            requires_release.store(false, std::memory_order_release);
            neutral_polls.store(0u, std::memory_order_release);
        }
    }
    return pause_menu && pause_down &&
        requires_release.load(std::memory_order_acquire);
}

void close_controllers_locked() {
    for (SDL_GameController*& controller : g_controllers) {
        if (controller != nullptr) {
            SDL_GameControllerClose(controller);
            controller = nullptr;
        }
    }
}

void scan_controllers_locked() {
    close_controllers_locked();

    size_t slot = 0;
    const int joystick_count = SDL_NumJoysticks();
    for (int joystick = 0; joystick < joystick_count && slot < g_controllers.size(); ++joystick) {
        if (SDL_IsGameController(joystick) == SDL_FALSE) {
            continue;
        }
        SDL_GameController* controller = SDL_GameControllerOpen(joystick);
        if (controller != nullptr) {
            g_controllers[slot++] = controller;
        }
    }

    g_connected_controller_count.store(slot, std::memory_order_release);
    const auto bindings = bumble::input_bindings::current();
    const bool controller_active =
        controller_preferred_for_gameplay(bindings);
    const char* preferred_name =
        slot != 0u && g_controllers[0] != nullptr
        ? SDL_GameControllerName(g_controllers[0])
        : nullptr;
    g_modern_visuals_toggle_down = controller_input_down(
        g_controllers[0],
        bumble::input_bindings::ControllerInput::Back
    );
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=controllers_scanned count=%zu"
        " requested=%s effective=%s preferred=\"%s\""
        " hotplug_fallback=keyboard_mouse\n",
        slot,
        bumble::input_bindings::device_mode_name(bindings.device_mode),
        controller_active ? "controller" : "keyboard_mouse",
        preferred_name != nullptr ? preferred_name : "none"
    );
    std::fflush(stderr);
    g_rescan_requested = false;
}

void apply_keyboard(
    ControllerSnapshot& snapshot,
    const bumble::input_bindings::Settings& bindings,
    float& modern_forward,
    float& modern_strafe,
    bool& modern_takeoff_land,
    bool& modern_frontend_confirm,
    bool& modern_loop_de_loop,
    bool& modern_quick_flip,
    bool& modern_forward_dash,
    bool& modern_barrel_roll
) {
    snapshot.connected = true;

    const bool modern_gameplay =
        bumble::modern_controls::gameplay_input_active();
    const bool pause_menu =
        bumble::modern_controls::pause_menu_active();
    const bool modern_profile = bumble::modern_controls::enabled();
    const bool frontend_confirm_pending = modern_gameplay &&
        bumble::modern_controls::frontend_confirm_pending();
    const auto action_down = [&bindings](
        bumble::input_bindings::InputAction action
    ) {
        return keyboard_action_down(bindings, action);
    };
    const bool pause_entry_key_down =
        action_down(bumble::input_bindings::InputAction::Pause);
    const bool pause_entry_blocked = pause_input_blocked(
        g_keyboard_pause_context_active,
        g_keyboard_pause_confirm_requires_release,
        g_keyboard_pause_neutral_polls,
        pause_menu,
        pause_entry_key_down
    );
    const bool pause_owns_keyboard_action =
        pause_menu && pause_entry_key_down;

    const bool pointer_menu_confirm_owned =
        !modern_gameplay &&
        bumble::graphics_options::mouse_menu_navigation_active();
    const bool keyboard_confirm =
        keyboard_action_down(
            bindings,
            bumble::input_bindings::InputAction::MenuConfirm,
            !pointer_menu_confirm_owned
        ) && !pause_entry_blocked && !pause_owns_keyboard_action;
    const bool keyboard_back = !modern_gameplay &&
        (action_down(bumble::input_bindings::InputAction::MenuBack) ||
            key_down(bumble::key::Escape)) && !pause_entry_blocked &&
        !pause_owns_keyboard_action;
    if (keyboard_confirm) {
        modern_frontend_confirm = true;
    }
    if (!modern_gameplay && keyboard_confirm) {
        snapshot.buttons |= kButtonA;
    }
    if (keyboard_back) {
        snapshot.buttons |= kButtonB;
    }
    const bool primary_fire =
        action_down(bumble::input_bindings::InputAction::PrimaryFire);
    bumble::modern_controls::set_primary_fire(primary_fire);
    if ((!modern_profile &&
            action_down(bumble::input_bindings::InputAction::TakeOffLand)) ||
        (modern_gameplay && primary_fire)) {
        snapshot.buttons |= kButtonZ;
    }
    if (!pause_entry_blocked &&
        ((modern_gameplay && !frontend_confirm_pending &&
            pause_entry_key_down) ||
        (!modern_gameplay && pause_entry_key_down && !keyboard_confirm &&
            !keyboard_back))) {
        snapshot.buttons |= kButtonStart;
    }
    if (action_down(bumble::input_bindings::InputAction::TakeOffLand)) {
        modern_takeoff_land = true;
    }
    modern_quick_flip =
        action_down(bumble::input_bindings::InputAction::QuickFlip);
    modern_forward_dash =
        action_down(bumble::input_bindings::InputAction::ForwardDash);
    modern_barrel_roll =
        action_down(bumble::input_bindings::InputAction::BarrelRoll);
    modern_loop_de_loop =
        action_down(bumble::input_bindings::InputAction::LoopDeLoop);
    if (modern_gameplay) {
        if (action_down(
                bumble::input_bindings::InputAction::PreviousWeapon)) {
            snapshot.buttons |= kCLeft;
        }
        if (action_down(bumble::input_bindings::InputAction::NextWeapon)) {
            snapshot.buttons |= kCDown;
        }
        if (action_down(bumble::input_bindings::InputAction::LoopDeLoop)) {
            snapshot.buttons |= kCRight;
        }
    } else {
        if (action_down(
                bumble::input_bindings::InputAction::PreviousWeapon)) {
            snapshot.buttons |= kButtonL;
        }
        if (action_down(bumble::input_bindings::InputAction::NextWeapon)) {
            snapshot.buttons |= kButtonR;
        }
    }

    float keyboard_x = 0.0f;
    float keyboard_y = 0.0f;
    const bool left =
        action_down(bumble::input_bindings::InputAction::StrafeLeft);
    const bool right =
        action_down(bumble::input_bindings::InputAction::StrafeRight);
    const bool backward =
        action_down(bumble::input_bindings::InputAction::MoveBackward);
    const bool forward =
        action_down(bumble::input_bindings::InputAction::MoveForward);
    if (left != right) keyboard_x = left ? -kN64StickMagnitude : kN64StickMagnitude;
    if (modern_gameplay) {
        if (forward != backward) modern_forward = forward ? 1.0f : -1.0f;
        if (right != left &&
            bumble::graphics_options::a_d_strafing_enabled()) {
            modern_strafe = right ? 1.0f : -1.0f;
        }
        keyboard_x = 0.0f;
    } else if (forward != backward) {
        keyboard_y = forward ? kN64StickMagnitude : -kN64StickMagnitude;
    }
    if (!modern_gameplay) {
        if (forward != backward) {
            snapshot.buttons |= forward ? kDpadUp : kDpadDown;
        }
        if (left != right) {
            snapshot.buttons |= left ? kDpadLeft : kDpadRight;
        }
    }
    if (keyboard_x != 0.0f || keyboard_y != 0.0f) {
        snapshot.x = keyboard_x;
        snapshot.y = keyboard_y;
    }
}

void apply_game_controller(
    SDL_GameController* controller,
    ControllerSnapshot& snapshot,
    const bumble::input_bindings::Settings& bindings,
    bool modern_gameplay,
    float& modern_look_x,
    float& modern_look_y,
    bool& modern_loop_de_loop,
    bool& modern_quick_flip,
    bool& modern_forward_dash,
    bool& modern_barrel_roll
) {
    if (controller == nullptr || SDL_GameControllerGetAttached(controller) == SDL_FALSE) {
        return;
    }

    snapshot.connected = true;
    snapshot.physical_rumble = SDL_GameControllerHasRumble(controller) == SDL_TRUE;

    const auto action_down = [controller, &bindings](
        bumble::input_bindings::InputAction action
    ) {
        return controller_action_down(controller, bindings, action);
    };
    const bool pause_down =
        action_down(bumble::input_bindings::InputAction::Pause);
    const bool pause_entry_blocked = controller == g_controllers[0] &&
        pause_input_blocked(
            g_controller_pause_context_active,
            g_controller_pause_confirm_requires_release,
            g_controller_pause_neutral_polls,
            bumble::modern_controls::pause_menu_active(),
            pause_down
        );
    if (modern_gameplay) {
        if (action_down(bumble::input_bindings::InputAction::TakeOffLand)) {
            snapshot.buttons |= kButtonA;
        }
    } else if (!pause_entry_blocked) {
        if (action_down(bumble::input_bindings::InputAction::MenuConfirm)) {
            snapshot.buttons |= kButtonA;
        }
        if (action_down(bumble::input_bindings::InputAction::MenuBack)) {
            snapshot.buttons |= kButtonB;
        }
    }
    if (action_down(bumble::input_bindings::InputAction::PrimaryFire)) {
        snapshot.buttons |= kButtonZ;
    }
    if (pause_down && !pause_entry_blocked) {
        snapshot.buttons |= kButtonStart;
    }
    modern_quick_flip =
        action_down(bumble::input_bindings::InputAction::QuickFlip);
    modern_forward_dash =
        action_down(bumble::input_bindings::InputAction::ForwardDash);
    modern_barrel_roll =
        action_down(bumble::input_bindings::InputAction::BarrelRoll);
    modern_loop_de_loop =
        action_down(bumble::input_bindings::InputAction::LoopDeLoop);
    if (action_down(bumble::input_bindings::InputAction::PreviousWeapon)) {
        snapshot.buttons |= modern_gameplay ? kCLeft : kButtonL;
    }
    if (action_down(bumble::input_bindings::InputAction::NextWeapon)) {
        snapshot.buttons |= modern_gameplay ? kCDown : kButtonR;
    }

    const SDL_GameControllerAxis movement_x_axis =
        bindings.stick_layout ==
            bumble::input_bindings::StickLayout::RightMoveLeftLook
        ? SDL_CONTROLLER_AXIS_RIGHTX
        : SDL_CONTROLLER_AXIS_LEFTX;
    const SDL_GameControllerAxis movement_y_axis =
        bindings.stick_layout ==
            bumble::input_bindings::StickLayout::RightMoveLeftLook
        ? SDL_CONTROLLER_AXIS_RIGHTY
        : SDL_CONTROLLER_AXIS_LEFTY;
    const SDL_GameControllerAxis look_x_axis =
        bindings.stick_layout ==
            bumble::input_bindings::StickLayout::RightMoveLeftLook
        ? SDL_CONTROLLER_AXIS_LEFTX
        : SDL_CONTROLLER_AXIS_RIGHTX;
    const SDL_GameControllerAxis look_y_axis =
        bindings.stick_layout ==
            bumble::input_bindings::StickLayout::RightMoveLeftLook
        ? SDL_CONTROLLER_AXIS_LEFTY
        : SDL_CONTROLLER_AXIS_RIGHTY;
    const Sint16 look_x = SDL_GameControllerGetAxis(controller, look_x_axis);
    const Sint16 look_y = SDL_GameControllerGetAxis(controller, look_y_axis);
    if (modern_gameplay) {
        const Sint16 look_deadzone = static_cast<Sint16>(
            bindings.joystick_look_deadzone
        );
        const float overall =
            static_cast<float>(bindings.joystick_look_sensitivity) / 100.0f;
        modern_look_x = normalize_look_axis(look_x, look_deadzone) * overall *
            (static_cast<float>(bindings.joystick_look_sensitivity_x) / 100.0f) *
            (bindings.invert_joystick_look_x ? -1.0f : 1.0f);
        modern_look_y = normalize_look_axis(look_y, look_deadzone) * overall *
            (static_cast<float>(bindings.joystick_look_sensitivity_y) / 100.0f) *
            (bindings.invert_joystick_look_y ? -1.0f : 1.0f);
        if (action_down(bumble::input_bindings::InputAction::LoopDeLoop)) {
            snapshot.buttons |= kCRight;
        }
    }

    snapshot.x = normalize_axis(
        SDL_GameControllerGetAxis(controller, movement_x_axis)
    );
    snapshot.y = -normalize_axis(
        SDL_GameControllerGetAxis(controller, movement_y_axis)
    );
    if (bumble::modern_controls::menu_navigation_active()) {
        bumble::input_bindings::apply_menu_dpad(snapshot.x, snapshot.y,
            SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0,
            SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0,
            SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0,
            SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0,
            true);
    }
    if (!modern_gameplay) {
        constexpr float kLegacyDirectionalThreshold = 0.5f;
        if (snapshot.y > kLegacyDirectionalThreshold) {
            snapshot.buttons |= kDpadUp;
        } else if (snapshot.y < -kLegacyDirectionalThreshold) {
            snapshot.buttons |= kDpadDown;
        }
        if (snapshot.x < -kLegacyDirectionalThreshold) {
            snapshot.buttons |= kDpadLeft;
        } else if (snapshot.x > kLegacyDirectionalThreshold) {
            snapshot.buttons |= kDpadRight;
        }
    }
}

bool open_audio_locked(uint32_t frequency) {
    if (g_audio_device == 0) {
        SDL_AudioSpec desired{};
        desired.freq = static_cast<int>(kAudioOutputFrequency);
        desired.format = AUDIO_S16SYS;
        desired.channels = 2;
        desired.samples = 512;
        desired.callback = nullptr;

        SDL_AudioSpec obtained{};
        g_audio_device = SDL_OpenAudioDevice(
            nullptr,
            0,
            &desired,
            &obtained,
            0
        );
        if (g_audio_device == 0) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=audio_open_failed"
                " input_frequency=%" PRIu32 " output_frequency=%" PRIu32
                " error=%s\n",
                frequency,
                kAudioOutputFrequency,
                SDL_GetError()
            );
            std::fflush(stderr);
            return false;
        }
        g_audio_output_frequency = static_cast<uint32_t>(obtained.freq);
        g_audio_device_buffer_frames = obtained.samples;
        SDL_PauseAudioDevice(g_audio_device, 1);
    }

    if (g_audio_stream != nullptr) {
        SDL_FreeAudioStream(g_audio_stream);
        g_audio_stream = nullptr;
    }
    SDL_ClearQueuedAudio(g_audio_device);
    SDL_PauseAudioDevice(g_audio_device, 1);
    g_audio_stream = SDL_NewAudioStream(
        AUDIO_S16SYS,
        2,
        static_cast<int>(frequency),
        AUDIO_S16SYS,
        2,
        static_cast<int>(g_audio_output_frequency)
    );
    if (g_audio_stream == nullptr) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=audio_stream_failed"
            " input_frequency=%" PRIu32 " output_frequency=%" PRIu32
            " error=%s\n",
            frequency,
            g_audio_output_frequency,
            SDL_GetError()
        );
        std::fflush(stderr);
        SDL_CloseAudioDevice(g_audio_device);
        g_audio_device = 0;
        g_audio_output_frequency = 0;
        return false;
    }

    g_audio_frequency = frequency;
    g_audio_prebuffer_target_frames = std::max<uint64_t>(
        static_cast<uint64_t>(g_audio_device_buffer_frames) * 2u,
        (
            static_cast<uint64_t>(g_audio_output_frequency) *
                kAudioGameplayPrebufferViIntervals +
            (kAudioViRate - 1u)
        ) / kAudioViRate
    );
    g_audio_transition_prebuffer_target_frames = std::max<uint64_t>(
        static_cast<uint64_t>(g_audio_device_buffer_frames) * 2u,
        (
            static_cast<uint64_t>(g_audio_output_frequency) *
                kAudioTransitionPrebufferViIntervals +
            (kAudioViRate - 1u)
        ) / kAudioViRate
    );
    g_audio_playback_started = false;
    g_audio_transition_reservoir_armed = false;
    g_audio_queue_calls_current = 0;
    g_audio_playback_starts_current = 0;
    g_audio_underruns_current = 0;
    g_audio_reprimes_current = 0;
    g_audio_transition_rebuffers_current = 0;
    g_audio_min_started_queue_frames = std::numeric_limits<uint64_t>::max();
    g_audio_input_frames_current = 0;
    g_audio_output_frames_current = 0;
    g_audio_conversion_log_emitted = false;
    g_audio_input_buffer.clear();
    g_audio_resample_buffer.clear();
    g_audio_submitted_bytes_current = 0;
    g_audio_observed_consumed_bytes_current = 0;
    g_audio_consumption_logged = false;
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=audio_opened input_frequency=%" PRIu32
        " output_frequency=%" PRIu32 " channels=2 samples=%u"
        " converter=sdl_audio_stream channel_order=deinterleaved_swapped"
        " gain=0.5"
        " paused=1 gameplay_prebuffer_target_frames=%llu"
        " gameplay_prebuffer_vi_intervals=%llu"
        " transition_prebuffer_target_frames=%llu"
        " transition_prebuffer_vi_intervals=%llu\n",
        g_audio_frequency,
        g_audio_output_frequency,
        static_cast<unsigned>(g_audio_device_buffer_frames),
        static_cast<unsigned long long>(g_audio_prebuffer_target_frames),
        static_cast<unsigned long long>(kAudioGameplayPrebufferViIntervals),
        static_cast<unsigned long long>(g_audio_transition_prebuffer_target_frames),
        static_cast<unsigned long long>(kAudioTransitionPrebufferViIntervals)
    );
    std::fflush(stderr);
    return true;
}

} // namespace

bool bumble::native_io::configure_replay(
    const char* manifest_path,
    const char* masked_control
) {
    if (manifest_path == nullptr || manifest_path[0] == '\0') {
        return false;
    }
    std::lock_guard lock(g_controller_mutex);
    if (g_initialized || g_replay.configured) {
        std::fprintf(stderr, "BUMBLE_RT64_PROBE stage=replay_config_failed error=already_initialized\n");
        std::fflush(stderr);
        return false;
    }
    return load_replay_manifest_locked(manifest_path, masked_control);
}

bool bumble::native_io::configure_connected_pak(
    int controller_num,
    ultramodern::input::Pak connected_pak
) {
    if (controller_num < 0 || controller_num >= static_cast<int>(g_connected_paks.size())) {
        return false;
    }

    std::lock_guard lock(g_controller_mutex);
    if (g_initialized ||
        (g_replay.configured && replay_owns_controller(static_cast<size_t>(controller_num)))) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=connected_pak_config_failed"
            " controller=%d error=already_initialized_or_replay_owned\n",
            controller_num
        );
        std::fflush(stderr);
        return false;
    }

    g_connected_paks[static_cast<size_t>(controller_num)] = connected_pak;
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=connected_pak_configured controller=%d pak=%s\n",
        controller_num,
        connected_pak_name(connected_pak)
    );
    std::fflush(stderr);
    return true;
}

void bumble::native_io::arm_replay() {
    std::lock_guard lock(g_controller_mutex);
    if (!g_replay.configured || g_replay.armed) {
        return;
    }
    g_replay.armed = true;
    g_replay.tick = 0;
    if (g_replay.schema_version == 1 || g_replay.schema_version == 3 ||
        g_replay.schema_version == 4 || g_replay.schema_version == 5) {
        g_replay.snapshot = ControllerSnapshot{
            .connected = true,
            .connected_pak = g_replay.replay_connected_pak,
        };
    } else {
        for (size_t controller = 0;
             controller < g_replay.replay_controller_count;
             ++controller) {
            g_replay.snapshots[controller] = ControllerSnapshot{
                .connected = true,
                .connected_pak = g_replay.replay_connected_paks[controller],
            };
        }
    }
    g_replay_tick.store(0, std::memory_order_release);
    if (g_replay.schema_version == 1) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_clock_armed origin=first_osContStartReadData_after_game_init\n"
        );
    } else if (g_replay.schema_version == 2) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_clock_armed"
            " origin=first_osContStartReadData_after_game_init schema=2 controllers=2\n"
        );
    } else if (g_replay.schema_version == 3) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_clock_armed"
            " origin=first_osContStartReadData_after_game_init schema=3 controllers=1\n"
        );
    } else if (g_replay.schema_version == 4) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_clock_armed"
            " origin=first_osContStartReadData_after_game_init schema=4 controllers=1"
            " normal_mission1_completion=1\n"
        );
    } else {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_clock_armed"
            " origin=first_osContStartReadData_after_game_init schema=5"
            " controllers=1 saved_profile_resume=1\n"
        );
    }
    std::fflush(stderr);
}

bool bumble::native_io::replay_configured() {
    return g_replay_configured.load(std::memory_order_acquire);
}

bool bumble::native_io::replay_uses_modern_controls() {
    std::lock_guard lock(g_controller_mutex);
    return g_replay.configured && g_replay.schema_version == 4;
}

bool bumble::native_io::replay_complete() {
    return g_replay_complete.load(std::memory_order_acquire);
}

bool bumble::native_io::replay_semantic_success() {
    return g_replay_semantic_success.load(std::memory_order_acquire);
}

uint64_t bumble::native_io::replay_tick() {
    return g_replay_tick.load(std::memory_order_acquire);
}

uint32_t bumble::native_io::replay_completed_game_event_count() {
    return g_replay_completed_game_events.load(std::memory_order_acquire);
}

uint32_t bumble::native_io::replay_expected_game_event_count() {
    std::lock_guard lock(g_controller_mutex);
    return static_cast<uint32_t>(g_replay.game_owned_events.size());
}

void bumble::native_io::set_keyboard_key_state(
    uint32_t virtual_key,
    bool pressed
) {
    if (virtual_key >= kKeyboardVirtualKeyCount) {
        return;
    }
    if (bumble::level_editor::handle_key(virtual_key, pressed)) {
        return;
    }
    const uint64_t mask = UINT64_C(1) <<
        (virtual_key % kKeyboardWordBits);
    std::atomic_uint64_t& word =
        g_keyboard_down[virtual_key / kKeyboardWordBits];
    uint64_t previous = 0;
    if (pressed) {
        previous = word.fetch_or(mask, std::memory_order_acq_rel);
    } else {
        previous = word.fetch_and(~mask, std::memory_order_acq_rel);
    }
    const bool was_pressed = (previous & mask) != 0;
    if (pressed && !was_pressed) {
        g_keyboard_pressed_since_poll[
            virtual_key / kKeyboardWordBits
        ].fetch_or(mask, std::memory_order_acq_rel);
    }
    bumble::input_bindings::process_keyboard_capture(
        static_cast<uint16_t>(virtual_key),
        pressed && !was_pressed,
        physical_keyboard_mouse_neutral()
    );
    if (was_pressed != pressed &&
        (virtual_key == 'W' || virtual_key == 'S' ||
         virtual_key == 'A' || virtual_key == 'D' ||
         virtual_key == 'X')) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=host_keyboard_transition"
            " key=%c virtual_key=0x%02" PRIX32
            " pressed=%d focus_owned=1\n",
            static_cast<int>(virtual_key),
            virtual_key,
            pressed ? 1 : 0
        );
        std::fflush(stderr);
    }
}

void bumble::native_io::set_mouse_button_state(
    input_bindings::MouseButton button,
    bool pressed
) {
    if (button < input_bindings::MouseButton::Left ||
        button > input_bindings::MouseButton::X2) {
        return; // Wheel events are handled separately.
    }
    if (button == input_bindings::MouseButton::Left &&
        bumble::level_editor::handle_primary_mouse(pressed)) {
        return;
    }
    const uint32_t mask = mouse_button_mask(button);
    uint32_t previous = 0u;
    if (pressed) {
        previous = g_mouse_buttons_down.fetch_or(
            mask,
            std::memory_order_acq_rel
        );
    } else {
        previous = g_mouse_buttons_down.fetch_and(
            ~mask,
            std::memory_order_acq_rel
        );
    }
    const bool was_pressed = (previous & mask) != 0u;
    if (pressed && !was_pressed) {
        g_mouse_buttons_pressed_since_poll.fetch_or(
            mask,
            std::memory_order_acq_rel
        );
    }
    input_bindings::process_keyboard_capture(
        input_bindings::mouse_binding(button),
        pressed && !was_pressed,
        physical_keyboard_mouse_neutral()
    );
}

void bumble::native_io::add_mouse_wheel_delta(int delta) {
    if (delta == 0 || !modern_controls::window_focused() ||
        g_replay_configured.load(std::memory_order_acquire)) return;
    const auto binding = input_bindings::mouse_binding(delta > 0
        ? input_bindings::MouseButton::WheelUp
        : input_bindings::MouseButton::WheelDown);
    if (input_bindings::process_keyboard_capture(binding, true,
            physical_keyboard_mouse_neutral())) return;
    if (!modern_controls::gameplay_input_active() ||
        modern_controls::pause_menu_active() ||
        !keyboard_mouse_gameplay_active()) return;
    const uint64_t revision = input_bindings::revision();
    std::lock_guard lock(g_mouse_wheel_mutex);
    if (g_mouse_wheel_binding_revision != revision) g_mouse_wheel.clear();
    g_mouse_wheel_binding_revision = revision;
    g_mouse_wheel.add(delta);
}

void bumble::native_io::clear_keyboard_key_state() {
    bumble::level_editor::release_input_state();
    for (size_t index = 0; index < kKeyboardWordCount; ++index) {
        g_keyboard_down[index].store(0, std::memory_order_release);
        g_keyboard_pressed_since_poll[index].store(
            0,
            std::memory_order_release
        );
    }
    g_keyboard_pause_context_active.store(false, std::memory_order_release);
    g_keyboard_pause_confirm_requires_release.store(
        false,
        std::memory_order_release
    );
    g_keyboard_pause_neutral_polls.store(0u, std::memory_order_release);
    g_mouse_buttons_down.store(0u, std::memory_order_release);
    g_mouse_buttons_pressed_since_poll.store(0u, std::memory_order_release);
    g_mouse_buttons_pressed_for_poll = 0u;
    {
        std::lock_guard lock(g_mouse_wheel_mutex);
        g_mouse_wheel.clear();
    }
    input_bindings::process_keyboard_capture(0u, false, true);
}

bool bumble::native_io::keyboard_mouse_gameplay_active() {
    return !controller_preferred_for_gameplay(input_bindings::current());
}

size_t bumble::native_io::connected_controller_count() {
    return g_connected_controller_count.load(std::memory_order_acquire);
}

std::string bumble::native_io::preferred_controller_name() {
    std::lock_guard lock(g_controller_mutex);
    if (g_controllers[0] == nullptr ||
        SDL_GameControllerGetAttached(g_controllers[0]) == SDL_FALSE) {
        return {};
    }
    const char* name = SDL_GameControllerName(g_controllers[0]);
    return name != nullptr ? name : "Controller";
}

bool bumble::native_io::initialize() {
    clear_keyboard_key_state();
    g_quit_requested.store(false, std::memory_order_release);
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    Uint32 flags = SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC | SDL_INIT_EVENTS;
#if !defined(_WIN32)
    flags |= SDL_INIT_VIDEO;
#endif
    if (SDL_Init(flags) != 0) {
        std::fprintf(stderr, "BUMBLE_RT64_PROBE stage=sdl_init_failed error=%s\n", SDL_GetError());
        std::fflush(stderr);
        return false;
    }
#if !defined(_WIN32)
    SDL_StopTextInput();
#endif
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "BUMBLE_RT64_PROBE stage=sdl_audio_init_failed error=%s\n", SDL_GetError());
        std::fflush(stderr);
        SDL_ClearError();
    }

    {
        std::lock_guard lock(g_controller_mutex);
        scan_controllers_locked();
    }
    g_initialized = true;
    std::fprintf(stderr, "BUMBLE_RT64_PROBE stage=sdl_io_ready\n");
    std::fflush(stderr);
    return true;
}

void bumble::native_io::shutdown() {
    clear_keyboard_key_state();
    if (!g_initialized) {
        return;
    }

    {
        std::lock_guard lock(g_audio_mutex);
        if (g_audio_device != 0) {
            const uint64_t queued_frames =
                SDL_GetQueuedAudioSize(g_audio_device) / (sizeof(int16_t) * 2u);
            const uint64_t minimum_frames =
                g_audio_min_started_queue_frames == std::numeric_limits<uint64_t>::max()
                    ? 0u
                    : g_audio_min_started_queue_frames;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=audio_health input_frequency=%u"
                " output_frequency=%u device_buffer_frames=%u"
                " converter=sdl_audio_stream channel_swap=1 gain=0.5"
                " gameplay_prebuffer_target_frames=%llu"
                " gameplay_prebuffer_vi_intervals=%llu"
                " transition_prebuffer_target_frames=%llu"
                " transition_prebuffer_vi_intervals=%llu"
                " pitch_stretch_frames=0 queue_calls=%llu playback_starts=%llu"
                " underruns=%llu reprimes=%llu transition_rebuffers=%llu"
                " min_started_queue_frames=%llu"
                " input_frames=%llu output_frames=%llu"
                " queued_frames=%llu started=%d\n",
                g_audio_frequency,
                g_audio_output_frequency,
                static_cast<unsigned>(g_audio_device_buffer_frames),
                static_cast<unsigned long long>(g_audio_prebuffer_target_frames),
                static_cast<unsigned long long>(kAudioGameplayPrebufferViIntervals),
                static_cast<unsigned long long>(g_audio_transition_prebuffer_target_frames),
                static_cast<unsigned long long>(kAudioTransitionPrebufferViIntervals),
                static_cast<unsigned long long>(g_audio_queue_calls_current),
                static_cast<unsigned long long>(g_audio_playback_starts_current),
                static_cast<unsigned long long>(g_audio_underruns_current),
                static_cast<unsigned long long>(g_audio_reprimes_current),
                static_cast<unsigned long long>(g_audio_transition_rebuffers_current),
                static_cast<unsigned long long>(minimum_frames),
                static_cast<unsigned long long>(g_audio_input_frames_current),
                static_cast<unsigned long long>(g_audio_output_frames_current),
                static_cast<unsigned long long>(queued_frames),
                g_audio_playback_started ? 1 : 0
            );
            std::fflush(stderr);
            SDL_ClearQueuedAudio(g_audio_device);
            if (g_audio_stream != nullptr) {
                SDL_FreeAudioStream(g_audio_stream);
                g_audio_stream = nullptr;
            }
            SDL_CloseAudioDevice(g_audio_device);
            g_audio_device = 0;
            g_audio_output_frequency = 0;
        }
    }
    {
        std::lock_guard lock(g_controller_mutex);
        close_controllers_locked();
        g_connected_controller_count.store(0u, std::memory_order_release);
        g_controller_pause_context_active.store(false, std::memory_order_release);
        g_controller_pause_confirm_requires_release.store(
            false,
            std::memory_order_release
        );
        g_controller_pause_neutral_polls.store(0u, std::memory_order_release);
    }

    SDL_Quit();
    g_initialized = false;
    std::fprintf(stderr, "BUMBLE_RT64_PROBE stage=sdl_io_shutdown\n");
    std::fflush(stderr);
}

void bumble::native_io::pump_events() {
    std::lock_guard lock(g_controller_mutex);
#if !defined(_WIN32)
    bumble::graphics_options::apply_pending_windowed_resolution();
    const bool replay_configured =
        g_replay_configured.load(std::memory_order_acquire);
#endif
    SDL_Event event{};
    while (SDL_PollEvent(&event) != 0) {
        if (event.type == SDL_CONTROLLERDEVICEADDED ||
            event.type == SDL_CONTROLLERDEVICEREMOVED ||
            event.type == SDL_CONTROLLERDEVICEREMAPPED) {
            g_rescan_requested = true;
        }
#if !defined(_WIN32)
        if (event.type == SDL_QUIT ||
            (event.type == SDL_WINDOWEVENT &&
             event.window.event == SDL_WINDOWEVENT_CLOSE)) {
            g_quit_requested.store(true, std::memory_order_release);
        } else if (!replay_configured &&
                   (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)) {
            const uint32_t key = portable_key_code(event.key.keysym.sym);
            if (key != 0u) {
                set_keyboard_key_state(key, event.type == SDL_KEYDOWN);
            }
        } else if (!replay_configured && event.type == SDL_MOUSEWHEEL) {
            const int notches = std::clamp(event.wheel.y, -64, 64);
            add_mouse_wheel_delta(notches * 120 *
                (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1));
        } else if (!replay_configured && event.type == SDL_MOUSEMOTION) {
            int width = 0;
            int height = 0;
            SDL_Window* window = SDL_GetWindowFromID(event.motion.windowID);
            if (window != nullptr) {
                SDL_GetWindowSize(window, &width, &height);
                bumble::graphics_options::update_menu_pointer(
                    event.motion.x,
                    event.motion.y,
                    static_cast<uint32_t>(std::max(0, width)),
                    static_cast<uint32_t>(std::max(0, height)),
                    false,
                    true
                );
            }
            bumble::modern_controls::add_raw_mouse_delta(
                event.motion.xrel,
                event.motion.yrel
            );
        } else if (!replay_configured &&
                   (event.type == SDL_MOUSEBUTTONDOWN ||
                    event.type == SDL_MOUSEBUTTONUP)) {
            const bool pressed = event.type == SDL_MOUSEBUTTONDOWN;
            std::optional<bumble::input_bindings::MouseButton> button;
            switch (event.button.button) {
            case SDL_BUTTON_LEFT:
                button = bumble::input_bindings::MouseButton::Left;
                break;
            case SDL_BUTTON_RIGHT:
                button = bumble::input_bindings::MouseButton::Right;
                break;
            case SDL_BUTTON_MIDDLE:
                button = bumble::input_bindings::MouseButton::Middle;
                break;
            case SDL_BUTTON_X1:
                button = bumble::input_bindings::MouseButton::X1;
                break;
            case SDL_BUTTON_X2:
                button = bumble::input_bindings::MouseButton::X2;
                break;
            default:
                break;
            }
            if (button.has_value()) {
                set_mouse_button_state(*button, pressed);
            }
            if (pressed && event.button.button == SDL_BUTTON_LEFT) {
                int width = 0;
                int height = 0;
                SDL_Window* window =
                    SDL_GetWindowFromID(event.button.windowID);
                if (window != nullptr) {
                    SDL_GetWindowSize(window, &width, &height);
                    bumble::graphics_options::update_menu_pointer(
                        event.button.x,
                        event.button.y,
                        static_cast<uint32_t>(std::max(0, width)),
                        static_cast<uint32_t>(std::max(0, height)),
                        true,
                        true
                    );
                }
            } else if (event.button.button == SDL_BUTTON_LEFT) {
                bumble::graphics_options::release_menu_pointer_click();
            }
        } else if (event.type == SDL_WINDOWEVENT) {
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                clear_keyboard_key_state();
                bumble::modern_controls::set_window_focused(true);
            } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                clear_keyboard_key_state();
                bumble::modern_controls::set_primary_fire(false);
                bumble::modern_controls::set_window_focused(false);
                bumble::graphics_options::clear_menu_pointer();
            } else if (event.window.event == SDL_WINDOWEVENT_LEAVE) {
                bumble::graphics_options::clear_menu_pointer();
            } else if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                bumble::graphics_options::resize_menu_pointer(
                    static_cast<uint32_t>(std::max(0, event.window.data1)),
                    static_cast<uint32_t>(std::max(0, event.window.data2))
                );
            }
        }
#endif
    }
    if (g_rescan_requested) {
        scan_controllers_locked();
    }
}

bool bumble::native_io::quit_requested() {
    return g_quit_requested.load(std::memory_order_acquire);
}

void bumble::native_io::poll_input() {
    std::lock_guard lock(g_controller_mutex);
    for (size_t index = 0; index < kKeyboardWordCount; ++index) {
        g_keyboard_pressed_for_poll[index] =
            g_keyboard_pressed_since_poll[index].exchange(
                0,
                std::memory_order_acq_rel
            );
    }
    g_mouse_buttons_pressed_for_poll =
        g_mouse_buttons_pressed_since_poll.exchange(
            0u,
            std::memory_order_acq_rel
        );
    input_bindings::process_keyboard_capture(
        0u,
        false,
        physical_keyboard_mouse_neutral()
    );
    SDL_GameControllerUpdate();
    const input_bindings::ControllerInput capture_candidate =
        first_controller_input(g_controllers[0]);
    input_bindings::process_controller_capture(
        capture_candidate,
        controller_is_neutral(g_controllers[0])
    );
    const input_bindings::Settings bindings = input_bindings::current();
    const bool controller_gameplay_selected =
        controller_preferred_for_gameplay(bindings);
    const bool controller_back_down = controller_input_down(
        g_controllers[0],
        input_bindings::ControllerInput::Back
    );
    const bool physical_input_suppressed =
        input_bindings::input_suppressed();
    {
        const uint64_t revision = input_bindings::revision();
        std::lock_guard wheel_lock(g_mouse_wheel_mutex);
        if (g_replay.configured || physical_input_suppressed ||
            controller_gameplay_selected ||
            !modern_controls::window_focused() ||
            !modern_controls::gameplay_input_active() ||
            modern_controls::pause_menu_active() ||
            g_mouse_wheel_binding_revision != revision) {
            g_mouse_wheel.clear();
        } else {
            const int direction = g_mouse_wheel.poll();
            if (direction != 0) {
                g_mouse_buttons_pressed_for_poll |= mouse_button_mask(direction > 0
                    ? input_bindings::MouseButton::WheelUp
                    : input_bindings::MouseButton::WheelDown);
            }
        }
    }
    if (!controller_back_down && g_modern_visuals_toggle_down &&
        !g_replay.configured && !physical_input_suppressed &&
        controller_gameplay_selected &&
        bumble::modern_controls::gameplay_input_active() &&
        bumble::modern_controls::window_focused()) {
        bumble::graphics_options::toggle_modern_visuals();
    }
    g_modern_visuals_toggle_down = controller_back_down;

    const bool keyboard_visuals_down = keyboard_action_down(
        bindings, input_bindings::InputAction::ToggleModernVisuals);
    if (!keyboard_visuals_down && g_keyboard_visuals_toggle_down &&
        !g_replay.configured && !physical_input_suppressed &&
        !controller_gameplay_selected &&
        bumble::modern_controls::gameplay_input_active() &&
        bumble::modern_controls::window_focused()) {
        bumble::graphics_options::toggle_modern_visuals();
    }
    g_keyboard_visuals_toggle_down = keyboard_visuals_down &&
        !physical_input_suppressed && !controller_gameplay_selected &&
        bumble::modern_controls::window_focused();

    if (g_replay.configured) {
        advance_replay_locked();
        bumble::native_pi_dma::set_replay_observer_tick(g_replay.tick);
    }

    for (size_t index = 0; index < g_snapshots.size(); ++index) {
        const ControllerSnapshot previous = g_snapshots[index];
        ControllerSnapshot snapshot{};
        if (g_replay.configured) {
            if ((g_replay.schema_version == 1 || g_replay.schema_version == 3 ||
                 g_replay.schema_version == 4 ||
                 g_replay.schema_version == 5) &&
                index == 0) {
                snapshot = g_replay.snapshot;
                if (g_replay.schema_version == 4) {
                    constexpr uint16_t kMissionOneDriverButtonMask =
                        kButtonZ | kCDown;
                    snapshot.buttons &= static_cast<uint16_t>(
                        ~kMissionOneDriverButtonMask
                    );
                    snapshot.buttons |= static_cast<uint16_t>(
                        bumble::native_checkpoint::
                            mission1_completion_buttons_requested() &
                        kMissionOneDriverButtonMask
                    );
                }
            } else if (g_replay.schema_version == 2 &&
                       index < g_replay.replay_controller_count) {
                snapshot = g_replay.snapshots[index];
            }
        } else {
            float modern_forward = 0.0f;
            float modern_strafe = 0.0f;
            float modern_look_x = 0.0f;
            float modern_look_y = 0.0f;
            bool modern_takeoff_land = false;
            bool modern_frontend_confirm = false;
            bool modern_loop_de_loop = false;
            bool modern_quick_flip = false;
            bool modern_forward_dash = false;
            bool modern_barrel_roll = false;
            const bool modern_primary = index == 0 &&
                bumble::modern_controls::enabled();
            const bool modern_gameplay = modern_primary &&
                bumble::modern_controls::gameplay_input_active();
            const bool exclusive_gameplay = modern_gameplay &&
                !bumble::modern_controls::pause_menu_active();
            const bool allow_controller =
                !physical_input_suppressed &&
                (index != 0u || !exclusive_gameplay ||
                 controller_gameplay_selected);
            const bool allow_keyboard =
                !physical_input_suppressed && index == 0u &&
                (!exclusive_gameplay || !controller_gameplay_selected);
            if (allow_controller) {
                apply_game_controller(
                    g_controllers[index],
                    snapshot,
                    bindings,
                    modern_gameplay,
                    modern_look_x,
                    modern_look_y,
                    modern_loop_de_loop,
                    modern_quick_flip,
                    modern_forward_dash,
                    modern_barrel_roll
                );
            }
            if (index == 0) {
                snapshot.connected = true;
                if (modern_primary) {
                    modern_takeoff_land =
                        (snapshot.buttons & kButtonA) != 0;
                    modern_frontend_confirm = modern_takeoff_land;
                }
                if (modern_gameplay) {
                    modern_forward = snapshot.y;
                    modern_strafe = snapshot.x;
                    snapshot.x = 0.0f;
                    snapshot.y = 0.0f;
                    snapshot.buttons &= static_cast<uint16_t>(
                        ~(kButtonA | kButtonB)
                    );
                }
                if (modern_primary) {
                    if (!bumble::modern_controls::window_focused()) {
                        snapshot.buttons = 0;
                        modern_forward = 0.0f;
                        modern_strafe = 0.0f;
                        modern_look_x = 0.0f;
                        modern_look_y = 0.0f;
                        modern_takeoff_land = false;
                        modern_frontend_confirm = false;
                        modern_loop_de_loop = false;
                        modern_quick_flip = false;
                        modern_forward_dash = false;
                        modern_barrel_roll = false;
                    }
                }
                if (allow_keyboard &&
                    (!modern_primary ||
                     bumble::modern_controls::window_focused())) {
                    apply_keyboard(
                        snapshot,
                        bindings,
                        modern_forward,
                        modern_strafe,
                        modern_takeoff_land,
                        modern_frontend_confirm,
                        modern_loop_de_loop,
                        modern_quick_flip,
                        modern_forward_dash,
                        modern_barrel_roll
                    );
                } else {
                    bumble::modern_controls::set_primary_fire(false);
                }
                if (modern_gameplay &&
                    (snapshot.buttons & kButtonStart) != 0u &&
                    (previous.buttons & kButtonStart) == 0u) {
                    bumble::graphics_options::stage_pause_menu_transition();
                }
                if (modern_primary) {
                    if (!modern_gameplay && modern_frontend_confirm) {
                        arm_audio_transition_reservoir();
                    }
                    bumble::modern_controls::set_frontend_confirm_pressed(
                        modern_frontend_confirm
                    );
                    bumble::modern_controls::set_takeoff_land_pressed(
                        modern_takeoff_land
                    );
                    bumble::modern_controls::set_loop_de_loop_pressed(
                        modern_loop_de_loop
                    );
                    bumble::modern_controls::set_quick_flip_pressed(
                        modern_quick_flip
                    );
                    bumble::modern_controls::set_sprint_pressed(
                        modern_forward_dash
                    );
                    bumble::modern_controls::set_barrel_roll_pressed(
                        modern_barrel_roll
                    );
                    bumble::modern_controls::set_movement_input(
                        modern_forward,
                        modern_strafe
                    );
                    bumble::modern_controls::add_controller_look(
                        modern_look_x,
                        modern_look_y
                    );
                }
            }
            clamp_physical_stick(snapshot);
            if (snapshot.connected) {
                snapshot.connected_pak = g_connected_paks[index];
            }
        }
        if (index == 0u) {
            bumble::modern_controls::set_cutscene_skip_pressed(
                (snapshot.buttons & kButtonB) != 0u
            );
        }
        g_snapshots[index] = snapshot;

        const bool controls_changed =
            snapshot.buttons != previous.buttons ||
            std::abs(snapshot.x - previous.x) >= 0.05f ||
            std::abs(snapshot.y - previous.y) >= 0.05f;
        if (controls_changed) {
            const uint64_t transition =
                g_input_transitions.fetch_add(1, std::memory_order_relaxed) + 1;
            if (transition <= 16) {
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=input_transition count=%llu controller=%zu "
                    "buttons=0x%04X x=%.3f y=%.3f\n",
                    static_cast<unsigned long long>(transition),
                    index,
                    static_cast<unsigned>(snapshot.buttons),
                    static_cast<double>(snapshot.x),
                    static_cast<double>(snapshot.y)
                );
                std::fflush(stderr);
            }
        }
    }

    const uint64_t count = g_input_polls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count == 60 || count == 600) {
        std::fprintf(stderr, "BUMBLE_RT64_PROBE stage=input_polled count=%llu\n", static_cast<unsigned long long>(count));
        std::fflush(stderr);
    }
    g_keyboard_pressed_for_poll.fill(0);
    g_mouse_buttons_pressed_for_poll = 0u;
}

bool bumble::native_io::get_input(
    int controller_num,
    uint16_t* buttons,
    float* x,
    float* y
) {
    if (controller_num < 0 || controller_num >= static_cast<int>(g_snapshots.size()) ||
        buttons == nullptr || x == nullptr || y == nullptr) {
        return false;
    }

    std::lock_guard lock(g_controller_mutex);
    const ControllerSnapshot& snapshot = g_snapshots[static_cast<size_t>(controller_num)];
    if (!snapshot.connected) {
        return false;
    }

    *buttons = snapshot.buttons;
    *x = snapshot.x;
    *y = snapshot.y;
    return true;
}

void bumble::native_io::set_rumble(int controller_num, bool rumble) {
    if (controller_num < 0 || controller_num >= static_cast<int>(g_controllers.size())) {
        return;
    }

    std::lock_guard lock(g_controller_mutex);
    if (g_replay.configured && controller_num == 0 &&
        g_replay.replay_connected_pak == ultramodern::input::Pak::RumblePak) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=replay_rumble_set controller=0 enabled=%d"
            " emulated_accessory=1 physical_motor=0\n",
            rumble ? 1 : 0
        );
        std::fflush(stderr);
        return;
    }
    if (g_replay.configured && g_replay.schema_version == 2 &&
        replay_owns_controller(static_cast<size_t>(controller_num))) {
        const ultramodern::input::Pak connected_pak =
            g_replay.replay_connected_paks[static_cast<size_t>(controller_num)];
        if (connected_pak == ultramodern::input::Pak::RumblePak) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=replay_rumble_set controller=%d enabled=%d"
                " emulated_accessory=1 physical_motor=0 schema=2\n",
                controller_num,
                rumble ? 1 : 0
            );
            std::fflush(stderr);
        }
        return;
    }
    const ControllerSnapshot& snapshot =
        g_snapshots[static_cast<size_t>(controller_num)];
    if (snapshot.connected_pak != ultramodern::input::Pak::RumblePak) {
        return;
    }
    SDL_GameController* controller = g_controllers[static_cast<size_t>(controller_num)];
    if (controller != nullptr && snapshot.physical_rumble) {
        const Uint16 strength = rumble ? 0xFFFFu : 0u;
        SDL_GameControllerRumble(controller, strength, strength, rumble ? 1000u : 0u);
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=rumble_set controller=%d enabled=%d\n",
            controller_num,
            rumble ? 1 : 0
        );
        std::fflush(stderr);
    }
}

ultramodern::input::connected_device_info_t
bumble::native_io::get_connected_device_info(int controller_num) {
    using Device = ultramodern::input::Device;
    using Pak = ultramodern::input::Pak;

    if (controller_num < 0 || controller_num >= static_cast<int>(g_snapshots.size())) {
        return {Device::None, Pak::None};
    }

    std::lock_guard lock(g_controller_mutex);
    const ControllerSnapshot& snapshot = g_snapshots[static_cast<size_t>(controller_num)];
    const uint32_t query = g_device_info_queries.fetch_add(1, std::memory_order_relaxed) + 1;
    if (query <= 8) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=device_info_query count=%" PRIu32
            " controller=%d connected=%d connected_pak=%s physical_rumble=%d"
            " replay=%d armed=%d\n",
            query,
            controller_num,
            snapshot.connected ? 1 : 0,
            connected_pak_name(snapshot.connected_pak),
            snapshot.physical_rumble ? 1 : 0,
            g_replay.configured ? 1 : 0,
            g_replay.armed ? 1 : 0
        );
        std::fflush(stderr);
    }
    return {
        snapshot.connected ? Device::Controller : Device::None,
        snapshot.connected ? snapshot.connected_pak : Pak::None,
    };
}

void bumble::native_io::queue_samples(int16_t* samples, size_t sample_count) {
    if (samples == nullptr || sample_count == 0) {
        return;
    }

    std::lock_guard lock(g_audio_mutex);
    if (g_audio_device == 0) {
        return;
    }

    release_audio_transition_reservoir_locked();
    ++g_audio_queue_calls_current;
    const uint64_t target_frames = active_audio_prebuffer_target_frames();
    const uint64_t queued_frames_before =
        SDL_GetQueuedAudioSize(g_audio_device) / (sizeof(int16_t) * 2u);
    if (g_audio_playback_started) {
        if (queued_frames_before == 0u) {
            SDL_PauseAudioDevice(g_audio_device, 1);
            g_audio_playback_started = false;
            ++g_audio_underruns_current;
            ++g_audio_reprimes_current;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=audio_underrun source=queue"
                " queue_call=%llu queued_frames=0 target_frames=%llu reprime=1\n",
                static_cast<unsigned long long>(g_audio_queue_calls_current),
                static_cast<unsigned long long>(target_frames)
            );
            std::fflush(stderr);
        }
        else {
            g_audio_min_started_queue_frames = std::min(
                g_audio_min_started_queue_frames,
                queued_frames_before
            );
        }
    }

    if ((sample_count & 1u) != 0u || g_audio_stream == nullptr) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=audio_queue_rejected"
            " sample_count=%zu stream_ready=%d\n",
            sample_count,
            g_audio_stream != nullptr ? 1 : 0
        );
        std::fflush(stderr);
        return;
    }

    const size_t input_frames = sample_count / 2u;
    g_audio_input_frames_current += input_frames;
    g_audio_input_buffer.resize(sample_count);
    for (size_t frame = 0; frame < input_frames; ++frame) {
        g_audio_input_buffer[frame * 2u] = static_cast<int16_t>(
            static_cast<int32_t>(samples[frame * 2u + 1u]) /
                kAudioGainDivisor
        );
        g_audio_input_buffer[frame * 2u + 1u] = static_cast<int16_t>(
            static_cast<int32_t>(samples[frame * 2u]) /
                kAudioGainDivisor
        );
    }

    const size_t input_bytes = sample_count * sizeof(int16_t);
    if (input_bytes > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        SDL_AudioStreamPut(
            g_audio_stream,
            g_audio_input_buffer.data(),
            static_cast<int>(input_bytes)
        ) != 0) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=audio_stream_put_failed error=%s\n",
            SDL_GetError()
        );
        std::fflush(stderr);
        return;
    }

    int available_bytes = SDL_AudioStreamAvailable(g_audio_stream);
    if (available_bytes < 0) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=audio_stream_available_failed error=%s\n",
            SDL_GetError()
        );
        std::fflush(stderr);
        return;
    }
    available_bytes -= available_bytes % static_cast<int>(sizeof(int16_t) * 2u);
    if (available_bytes == 0) {
        return;
    }
    g_audio_resample_buffer.resize(
        static_cast<size_t>(available_bytes) / sizeof(int16_t)
    );
    const int converted_bytes = SDL_AudioStreamGet(
        g_audio_stream,
        g_audio_resample_buffer.data(),
        available_bytes
    );
    if (converted_bytes <= 0 ||
        (converted_bytes % static_cast<int>(sizeof(int16_t) * 2u)) != 0) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=audio_stream_get_failed"
            " converted_bytes=%d error=%s\n",
            converted_bytes,
            SDL_GetError()
        );
        std::fflush(stderr);
        return;
    }
    const int16_t* queued_samples = g_audio_resample_buffer.data();
    const size_t queued_sample_count =
        static_cast<size_t>(converted_bytes) / sizeof(int16_t);
    const size_t output_frames = queued_sample_count / 2u;
    g_audio_output_frames_current += output_frames;
    if (!g_audio_conversion_log_emitted) {
        g_audio_conversion_log_emitted = true;
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=audio_rate_conversion"
            " input_frequency=%" PRIu32 " output_frequency=%" PRIu32
            " input_frames=%zu output_frames=%zu"
            " converter=sdl_audio_stream channel_swap=1 gain=0.5"
            " pitch_stretch=0\n",
            g_audio_frequency,
            g_audio_output_frequency,
            input_frames,
            output_frames
        );
        std::fflush(stderr);
    }

    const Uint32 byte_count =
        static_cast<Uint32>(queued_sample_count * sizeof(int16_t));
    if (SDL_QueueAudio(g_audio_device, queued_samples, byte_count) == 0) {
        g_audio_submitted_bytes_current += byte_count;
        const uint64_t queued_frames_after =
            SDL_GetQueuedAudioSize(g_audio_device) / (sizeof(int16_t) * 2u);
        if (!g_audio_playback_started &&
            queued_frames_after >= target_frames) {
            g_audio_playback_started = true;
            ++g_audio_playback_starts_current;
            g_audio_min_started_queue_frames = std::min(
                g_audio_min_started_queue_frames,
                queued_frames_after
            );
            SDL_PauseAudioDevice(g_audio_device, 0);
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=audio_playback_started queued_frames=%llu"
                " target_frames=%llu start_count=%llu\n",
                static_cast<unsigned long long>(queued_frames_after),
                static_cast<unsigned long long>(target_frames),
                static_cast<unsigned long long>(g_audio_playback_starts_current)
            );
            std::fflush(stderr);
        }
        const uint64_t total =
            g_audio_samples.fetch_add(queued_sample_count, std::memory_order_relaxed) +
            queued_sample_count;
        if (total == queued_sample_count ||
            total >= 48000 && total - queued_sample_count < 48000) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=audio_samples_queued total=%llu\n",
                static_cast<unsigned long long>(total)
            );
            std::fflush(stderr);
        }
    }
}

size_t bumble::native_io::get_frames_remaining() {
    std::lock_guard lock(g_audio_mutex);
    if (g_audio_device == 0) {
        return 0;
    }
    release_audio_transition_reservoir_locked();
    const uint64_t queued_bytes = SDL_GetQueuedAudioSize(g_audio_device);
    const uint64_t queued_frames = queued_bytes / (sizeof(int16_t) * 2u);
    const uint64_t target_frames = active_audio_prebuffer_target_frames();
    if (g_audio_playback_started) {
        if (queued_frames == 0u) {
            SDL_PauseAudioDevice(g_audio_device, 1);
            g_audio_playback_started = false;
            ++g_audio_underruns_current;
            ++g_audio_reprimes_current;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=audio_underrun source=remaining"
                " queue_call=%llu queued_frames=0 target_frames=%llu reprime=1\n",
                static_cast<unsigned long long>(g_audio_queue_calls_current),
                static_cast<unsigned long long>(target_frames)
            );
            std::fflush(stderr);
        }
        else {
            g_audio_min_started_queue_frames = std::min(
                g_audio_min_started_queue_frames,
                queued_frames
            );
        }
    }
    const uint64_t consumed_bytes =
        g_audio_submitted_bytes_current >= queued_bytes
            ? g_audio_submitted_bytes_current - queued_bytes
            : 0;
    if (consumed_bytes > g_audio_observed_consumed_bytes_current) {
        const uint64_t delta_bytes = consumed_bytes - g_audio_observed_consumed_bytes_current;
        g_audio_observed_consumed_bytes_current = consumed_bytes;
        const uint64_t delta_samples = delta_bytes / sizeof(int16_t);
        const uint64_t consumed_samples =
            g_audio_consumed_samples.fetch_add(delta_samples, std::memory_order_relaxed) + delta_samples;
        if (!g_audio_consumption_logged && consumed_samples >= 4096) {
            g_audio_consumption_logged = true;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=audio_playback_consumed samples=%llu queued_frames=%llu\n",
                static_cast<unsigned long long>(consumed_samples),
                static_cast<unsigned long long>(queued_frames)
            );
            std::fflush(stderr);
        }
    }
    const uint64_t reportable_output_frames = queued_frames > target_frames
        ? queued_frames - target_frames
        : 0u;
    const uint64_t input_equivalent_frames = g_audio_output_frequency > 0u
        ? reportable_output_frames * static_cast<uint64_t>(g_audio_frequency) /
            static_cast<uint64_t>(g_audio_output_frequency)
        : 0u;
    const uint64_t sdl_report_bias_frames =
        (static_cast<uint64_t>(g_audio_frequency) + 59u) / 60u;
    return input_equivalent_frames > sdl_report_bias_frames
        ? static_cast<size_t>(
            input_equivalent_frames - sdl_report_bias_frames
        )
        : 0u;
}

void bumble::native_io::set_frequency(uint32_t frequency) {
    if (frequency == 0) {
        return;
    }

    std::lock_guard lock(g_audio_mutex);
    if (g_audio_device != 0 && g_audio_frequency == frequency) {
        return;
    }
    open_audio_locked(frequency);
}

uint64_t bumble::native_io::input_poll_count() {
    return g_input_polls.load(std::memory_order_acquire);
}

uint64_t bumble::native_io::input_transition_count() {
    return g_input_transitions.load(std::memory_order_acquire);
}

uint64_t bumble::native_io::queued_audio_sample_count() {
    return g_audio_samples.load(std::memory_order_acquire);
}

uint64_t bumble::native_io::consumed_audio_sample_count() {
    return g_audio_consumed_samples.load(std::memory_order_acquire);
}
