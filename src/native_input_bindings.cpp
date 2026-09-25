#include "native_input_bindings.hpp"

#if defined(_WIN32)
#include <Windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <string>

#include "native_key_codes.hpp"

namespace {

namespace key = bumble::key;

using bumble::input_bindings::ActionBindings;
using bumble::input_bindings::BindingProfile;
using bumble::input_bindings::CaptureState;
using bumble::input_bindings::ControllerInput;
using bumble::input_bindings::InputAction;
using bumble::input_bindings::InputDeviceMode;
using bumble::input_bindings::MouseButton;
using bumble::input_bindings::Settings;
using bumble::input_bindings::StickLayout;

std::mutex g_binding_mutex;
Settings g_settings = bumble::input_bindings::default_settings();
CaptureState g_capture{};
bool g_release_guard = false;
BindingProfile g_release_profile = BindingProfile::KeyboardMouse;
std::atomic_uint64_t g_revision{1u};

constexpr size_t action_index(InputAction action) {
    return static_cast<size_t>(action);
}

constexpr uint16_t controller_value(ControllerInput input) {
    return static_cast<uint16_t>(input);
}

void assign(
    bumble::input_bindings::BindingTable& table,
    InputAction action,
    std::initializer_list<uint16_t> bindings
) {
    ActionBindings& target = table[action_index(action)];
    target.fill(0u);
    size_t index = 0u;
    for (const uint16_t binding : bindings) {
        if (index >= target.size()) {
            break;
        }
        target[index++] = binding;
    }
}

void publish_change_locked(
    const char* operation,
    BindingProfile profile,
    InputAction action
) {
    const uint64_t revision =
        g_revision.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    std::fprintf(
        stderr,
        "BUMBLE_INPUT_BINDINGS stage=changed operation=%s profile=%s"
        " action=%s revision=%llu\n",
        operation,
        profile == BindingProfile::Controller ? "controller" : "keyboard_mouse",
        bumble::input_bindings::action_config_name(action),
        static_cast<unsigned long long>(revision)
    );
    std::fflush(stderr);
}

void replace_binding_locked(
    BindingProfile profile,
    InputAction action,
    uint16_t binding
) {
    auto& table = profile == BindingProfile::Controller
        ? g_settings.controller
        : g_settings.keyboard_mouse;
    ActionBindings& target = table[action_index(action)];
    target.fill(0u);
    target[0] = binding;
}

bool capture_matches_locked(BindingProfile profile) {
    return g_capture.active && g_capture.profile == profile;
}

void cancel_capture_locked(const char* source) {
    if (!g_capture.active) {
        return;
    }
    std::fprintf(
        stderr,
        "BUMBLE_INPUT_BINDINGS stage=capture_cancelled source=%s"
        " profile=%s action=%s\n",
        source,
        g_capture.profile == BindingProfile::Controller
            ? "controller"
            : "keyboard_mouse",
        bumble::input_bindings::action_config_name(g_capture.action)
    );
    std::fflush(stderr);
    g_release_guard = true;
    g_release_profile = g_capture.profile;
    g_capture = CaptureState{};
    g_revision.fetch_add(1u, std::memory_order_acq_rel);
}

void complete_capture_locked(uint16_t binding) {
    const BindingProfile profile = g_capture.profile;
    const InputAction action = g_capture.action;
    replace_binding_locked(profile, action, binding);
    g_release_guard = true;
    g_release_profile = profile;
    g_capture = CaptureState{};
    publish_change_locked("capture", profile, action);
}

} // namespace

bumble::input_bindings::Settings bumble::input_bindings::default_settings(uint32_t version) {
    Settings settings{};

    assign(settings.keyboard_mouse, InputAction::MoveForward, {'W'});
    assign(settings.keyboard_mouse, InputAction::MoveBackward, {'S'});
    assign(settings.keyboard_mouse, InputAction::StrafeLeft, {'A'});
    assign(settings.keyboard_mouse, InputAction::StrafeRight, {'D'});
    assign(
        settings.keyboard_mouse,
        InputAction::PrimaryFire,
        {mouse_binding(MouseButton::Left)}
    );
    assign(settings.keyboard_mouse, InputAction::TakeOffLand, {key::Space});
    assign(settings.keyboard_mouse, InputAction::PreviousWeapon, {'Q'});
    assign(settings.keyboard_mouse, InputAction::NextWeapon, {'E'});
    assign(settings.keyboard_mouse, InputAction::LoopDeLoop, {key::Shift});
    assign(settings.keyboard_mouse, InputAction::QuickFlip, {'R'});
    assign(settings.keyboard_mouse, InputAction::ForwardDash, {key::Control});
    assign(
        settings.keyboard_mouse,
        InputAction::BarrelRoll,
        {mouse_binding(MouseButton::X1)}
    );
    assign(
        settings.keyboard_mouse,
        InputAction::Pause,
        {key::Enter, key::Escape, 'P'}
    );
    assign(
        settings.keyboard_mouse,
        InputAction::MenuConfirm,
        {'X', key::Enter, mouse_binding(MouseButton::Left)}
    );
    assign(settings.keyboard_mouse, InputAction::MenuBack, {'C'});
    assign(settings.keyboard_mouse, InputAction::ToggleModernVisuals, {key::Tab});

    if (version >= 7u) {
        assign(settings.keyboard_mouse, InputAction::ForwardDash, {key::Shift});
        assign(settings.keyboard_mouse, InputAction::BarrelRoll, {key::Control});
        assign(settings.keyboard_mouse, InputAction::LoopDeLoop,
            {mouse_binding(MouseButton::X1)});
        assign(settings.keyboard_mouse, InputAction::PreviousWeapon,
            {'Q', mouse_binding(MouseButton::WheelUp)});
        assign(settings.keyboard_mouse, InputAction::NextWeapon,
            {'E', mouse_binding(MouseButton::WheelDown)});
    }

    assign(
        settings.controller,
        InputAction::PrimaryFire,
        {controller_value(ControllerInput::RightTrigger)}
    );
    assign(
        settings.controller,
        InputAction::TakeOffLand,
        {controller_value(ControllerInput::A)}
    );
    assign(
        settings.controller,
        InputAction::PreviousWeapon,
        {controller_value(ControllerInput::LeftShoulder)}
    );
    assign(
        settings.controller,
        InputAction::NextWeapon,
        {controller_value(ControllerInput::RightShoulder)}
    );
    assign(
        settings.controller,
        InputAction::LoopDeLoop,
        {controller_value(ControllerInput::B)}
    );
    assign(
        settings.controller,
        InputAction::QuickFlip,
        {controller_value(ControllerInput::Y)}
    );
    assign(
        settings.controller,
        InputAction::ForwardDash,
        {
            controller_value(ControllerInput::X),
            controller_value(ControllerInput::LeftStick),
        }
    );
    assign(
        settings.controller,
        InputAction::BarrelRoll,
        {
            controller_value(ControllerInput::LeftTrigger),
            controller_value(ControllerInput::RightStick),
        }
    );
    assign(
        settings.controller,
        InputAction::Pause,
        {controller_value(ControllerInput::Start)}
    );
    assign(
        settings.controller,
        InputAction::MenuConfirm,
        {controller_value(ControllerInput::A)}
    );
    assign(
        settings.controller,
        InputAction::MenuBack,
        {
            controller_value(ControllerInput::B),
            controller_value(ControllerInput::X),
        }
    );
    if (version >= 8u) {
        assign(settings.keyboard_mouse, InputAction::FlyUp, {'F'});
        assign(settings.keyboard_mouse, InputAction::FlyDown, {'V'});
        assign(settings.controller, InputAction::ForwardDash, {controller_value(ControllerInput::X)});
        assign(settings.controller, InputAction::BarrelRoll, {controller_value(ControllerInput::LeftTrigger)});
        assign(settings.controller, InputAction::FlyUp, {controller_value(ControllerInput::LeftStick)});
        assign(settings.controller, InputAction::FlyDown, {controller_value(ControllerInput::RightStick)});
    }
    return settings;
}

void bumble::input_bindings::upgrade_keyboard_defaults(Settings& settings, uint32_t version) {
    if (version < 7u && settings.keyboard_mouse == default_settings(6u).keyboard_mouse) {
        settings.keyboard_mouse = default_settings().keyboard_mouse;
    }
}

void bumble::input_bindings::configure(const Settings& requested) {
    Settings sanitized = requested;
    sanitized.mouse_sensitivity = std::clamp(sanitized.mouse_sensitivity, 10u, 400u);
    sanitized.mouse_sensitivity_x = std::clamp(sanitized.mouse_sensitivity_x, 10u, 400u);
    sanitized.mouse_sensitivity_y = std::clamp(sanitized.mouse_sensitivity_y, 10u, 400u);
    if (static_cast<uint32_t>(sanitized.device_mode) >
        static_cast<uint32_t>(InputDeviceMode::Controller)) {
        sanitized.device_mode = InputDeviceMode::Auto;
    }
    if (static_cast<uint32_t>(sanitized.stick_layout) >
        static_cast<uint32_t>(StickLayout::RightMoveLeftLook)) {
        sanitized.stick_layout = StickLayout::LeftMoveRightLook;
    }
    sanitized.joystick_look_sensitivity = std::clamp(
        sanitized.joystick_look_sensitivity, 25u, 200u
    );
    sanitized.joystick_look_sensitivity_x = std::clamp(
        sanitized.joystick_look_sensitivity_x, 50u, 150u
    );
    sanitized.joystick_look_sensitivity_y = std::clamp(
        sanitized.joystick_look_sensitivity_y, 50u, 150u
    );
    sanitized.joystick_look_deadzone = std::min(
        sanitized.joystick_look_deadzone, 16384u
    );
    for (ActionBindings& bindings : sanitized.keyboard_mouse) {
        for (uint16_t& binding : bindings) {
            if (!valid_keyboard_binding(binding)) {
                binding = 0u;
            }
        }
    }
    for (ActionBindings& bindings : sanitized.controller) {
        for (uint16_t& binding : bindings) {
            if (!valid_controller_binding(binding)) {
                binding = 0u;
            }
        }
    }

    std::lock_guard lock(g_binding_mutex);
    g_settings = sanitized;
    g_capture = CaptureState{};
    g_release_guard = false;
    g_revision.fetch_add(1u, std::memory_order_acq_rel);
}

std::pair<float, float> bumble::input_bindings::scale_mouse_delta(
    const Settings& settings, float x, float y, float seconds) {
    const float acceleration = settings.mouse_acceleration
        ? 1.0f + std::clamp((std::hypot(x, y) / std::clamp(seconds, .001f, .1f) - 300.0f) / 1500.0f, 0.0f, 2.0f)
        : 1.0f;
    const float gain = float(settings.mouse_sensitivity) * acceleration / 10000.0f;
    return {x * gain * settings.mouse_sensitivity_x,
        y * gain * settings.mouse_sensitivity_y};
}

bool bumble::input_bindings::validate_default_contracts() {
    bool menu_pass = true;
    for (unsigned directions = 0; directions < 16; ++directions) {
        for (bool menu : {false, true}) {
            float x = 0.25f, y = -0.75f;
            const bool up = directions & 1u, down = directions & 2u;
            const bool left = directions & 4u, right = directions & 8u;
            apply_menu_dpad(x, y, up, down, left, right, menu);
            menu_pass &= x == (menu && (left || right) ? float(right) - float(left) : 0.25f);
            menu_pass &= y == (menu && (up || down) ? float(up) - float(down) : -0.75f);
        }
    }
    const auto defaults = default_settings();
    auto mouse = defaults;
    mouse.mouse_sensitivity = 200;
    mouse.mouse_sensitivity_x = 50;
    mouse.mouse_sensitivity_y = 150;
    const auto scaled = scale_mouse_delta(mouse, 3, -2, .01f);
    const auto slow = scale_mouse_delta(mouse, 3, -2, .1f);
    bool mouse_pass = scaled == std::pair<float, float>{3, -6} && scaled == slow;
    mouse.mouse_acceleration = true;
    const auto accelerated = scale_mouse_delta(mouse, 30, -20, .01f);
    const auto half = scale_mouse_delta(mouse, 15, -10, .005f);
    mouse_pass &= accelerated.first > 30 && accelerated.second < -60 &&
        std::abs(accelerated.first - 2 * half.first) < .0001f &&
        std::abs(accelerated.second - 2 * half.second) < .0001f;
    auto legacy = default_settings(6u);
    const auto controller = legacy.controller;
    legacy.controller[action_index(InputAction::BarrelRoll)] =
        {controller_value(ControllerInput::DpadUp), 0u, 0u};
    const auto custom_controller = legacy.controller;
    upgrade_keyboard_defaults(legacy, 6u);
    auto custom = default_settings(6u);
    custom.keyboard_mouse[action_index(InputAction::MoveForward)] = {'T', 0u, 0u};
    const auto custom_keyboard = custom.keyboard_mouse;
    upgrade_keyboard_defaults(custom, 6u);
    const bool pass = mouse_pass && menu_pass && default_settings(7u).controller == controller &&
        legacy.controller == custom_controller &&
        legacy.keyboard_mouse == defaults.keyboard_mouse &&
        custom.keyboard_mouse == custom_keyboard &&
        defaults.keyboard_mouse[action_index(InputAction::ForwardDash)][0] == key::Shift &&
        defaults.keyboard_mouse[action_index(InputAction::BarrelRoll)][0] == key::Control &&
        defaults.keyboard_mouse[action_index(InputAction::ToggleModernVisuals)][0] == key::Tab &&
        defaults.controller[action_index(InputAction::ToggleModernVisuals)] == ActionBindings{} &&
        defaults.keyboard_mouse[action_index(InputAction::PreviousWeapon)][1] == mouse_binding(MouseButton::WheelUp) &&
        defaults.keyboard_mouse[action_index(InputAction::NextWeapon)][1] == mouse_binding(MouseButton::WheelDown) &&
        valid_keyboard_binding(mouse_binding(MouseButton::WheelUp)) &&
        valid_keyboard_binding(mouse_binding(MouseButton::WheelDown)) &&
        defaults.controller[action_index(InputAction::FlyUp)][0] == controller_value(ControllerInput::LeftStick) &&
        defaults.controller[action_index(InputAction::FlyDown)][0] == controller_value(ControllerInput::RightStick) &&
        defaults.controller[action_index(InputAction::ForwardDash)][1] == 0u &&
        defaults.controller[action_index(InputAction::BarrelRoll)][1] == 0u;
    std::fprintf(stderr, "BUMBLE_PC_BINDING_CONTRACT result=%s defaults_migrated=1 custom_preserved=1 vertical_flight=1 wheel_bindable=1\n",
        pass ? "pass" : "fail");
    return pass;
}

bumble::input_bindings::Settings bumble::input_bindings::current() {
    std::lock_guard lock(g_binding_mutex);
    return g_settings;
}

void bumble::input_bindings::set_device_mode(InputDeviceMode mode) {
    if (static_cast<uint32_t>(mode) >
        static_cast<uint32_t>(InputDeviceMode::Controller)) {
        mode = InputDeviceMode::Auto;
    }
    std::lock_guard lock(g_binding_mutex);
    if (g_settings.device_mode == mode) {
        return;
    }
    g_settings.device_mode = mode;
    publish_change_locked(
        "device_mode",
        BindingProfile::KeyboardMouse,
        InputAction::MoveForward
    );
}

void bumble::input_bindings::cycle_device_mode(int direction) {
    std::lock_guard lock(g_binding_mutex);
    constexpr int count = 3;
    const int step = direction < 0 ? -1 : 1;
    const int current_mode = static_cast<int>(g_settings.device_mode);
    g_settings.device_mode = static_cast<InputDeviceMode>(
        (current_mode + step + count) % count
    );
    publish_change_locked(
        "device_mode",
        BindingProfile::KeyboardMouse,
        InputAction::MoveForward
    );
}

void bumble::input_bindings::set_stick_layout(StickLayout layout) {
    if (static_cast<uint32_t>(layout) >
        static_cast<uint32_t>(StickLayout::RightMoveLeftLook)) {
        layout = StickLayout::LeftMoveRightLook;
    }
    std::lock_guard lock(g_binding_mutex);
    if (g_settings.stick_layout == layout) {
        return;
    }
    g_settings.stick_layout = layout;
    publish_change_locked(
        "stick_layout",
        BindingProfile::Controller,
        InputAction::MoveForward
    );
}

void bumble::input_bindings::cycle_stick_layout(int) {
    std::lock_guard lock(g_binding_mutex);
    g_settings.stick_layout =
        g_settings.stick_layout == StickLayout::LeftMoveRightLook
        ? StickLayout::RightMoveLeftLook
        : StickLayout::LeftMoveRightLook;
    publish_change_locked(
        "stick_layout",
        BindingProfile::Controller,
        InputAction::MoveForward
    );
}

void bumble::input_bindings::reset_profile(BindingProfile profile) {
    const Settings defaults = default_settings();
    std::lock_guard lock(g_binding_mutex);
    if (profile == BindingProfile::Controller) {
        g_settings.controller = defaults.controller;
        g_settings.stick_layout = defaults.stick_layout;
        g_settings.joystick_look_sensitivity =
            defaults.joystick_look_sensitivity;
        g_settings.joystick_look_sensitivity_x =
            defaults.joystick_look_sensitivity_x;
        g_settings.joystick_look_sensitivity_y =
            defaults.joystick_look_sensitivity_y;
        g_settings.joystick_look_deadzone = defaults.joystick_look_deadzone;
        g_settings.invert_joystick_look_x =
            defaults.invert_joystick_look_x;
        g_settings.invert_joystick_look_y =
            defaults.invert_joystick_look_y;
    } else {
        g_settings.keyboard_mouse = defaults.keyboard_mouse;
        g_settings.mouse_sensitivity = defaults.mouse_sensitivity;
        g_settings.mouse_sensitivity_x = defaults.mouse_sensitivity_x;
        g_settings.mouse_sensitivity_y = defaults.mouse_sensitivity_y;
        g_settings.mouse_acceleration = defaults.mouse_acceleration;
    }
    publish_change_locked("reset_profile", profile, InputAction::MoveForward);
}

void bumble::input_bindings::reset_all() {
    std::lock_guard lock(g_binding_mutex);
    g_settings = default_settings();
    g_capture = CaptureState{};
    g_release_guard = false;
    publish_change_locked(
        "reset_all",
        BindingProfile::KeyboardMouse,
        InputAction::MoveForward
    );
}

bool bumble::input_bindings::begin_capture(
    BindingProfile profile,
    InputAction action
) {
    if (action_index(action) >= kInputActionCount) {
        return false;
    }
    std::lock_guard lock(g_binding_mutex);
    g_capture = CaptureState{
        .active = true,
        .waiting_for_neutral = true,
        .profile = profile,
        .action = action,
    };
    g_release_guard = false;
    std::fprintf(
        stderr,
        "BUMBLE_INPUT_BINDINGS stage=capture_started profile=%s action=%s"
        " neutral_required=1\n",
        profile == BindingProfile::Controller ? "controller" : "keyboard_mouse",
        action_config_name(action)
    );
    std::fflush(stderr);
    return true;
}

void bumble::input_bindings::cancel_capture() {
    std::lock_guard lock(g_binding_mutex);
    cancel_capture_locked("api");
}

bumble::input_bindings::CaptureState
bumble::input_bindings::capture_state() {
    std::lock_guard lock(g_binding_mutex);
    return g_capture;
}

bool bumble::input_bindings::process_keyboard_capture(
    uint16_t binding,
    bool pressed,
    bool keyboard_mouse_neutral
) {
    std::lock_guard lock(g_binding_mutex);

    if (g_release_guard &&
        g_release_profile == BindingProfile::KeyboardMouse &&
        keyboard_mouse_neutral) {
        g_release_guard = false;
    }
    if (!g_capture.active) {
        return g_release_guard;
    }
    if (pressed && binding == key::Backspace) {
        cancel_capture_locked("keyboard_backspace");
        return true;
    }
    if (!capture_matches_locked(BindingProfile::KeyboardMouse)) {
        return true;
    }
    if (g_capture.waiting_for_neutral) {
        if (keyboard_mouse_neutral) {
            g_capture.waiting_for_neutral = false;
            g_revision.fetch_add(1u, std::memory_order_acq_rel);
            std::fprintf(
                stderr,
                "BUMBLE_INPUT_BINDINGS stage=capture_armed"
                " profile=keyboard_mouse action=%s\n",
                action_config_name(g_capture.action)
            );
            std::fflush(stderr);
        }
        return true;
    }
    if (pressed && valid_keyboard_binding(binding) && binding != 0u) {
        complete_capture_locked(binding);
    }
    return true;
}

bool bumble::input_bindings::process_controller_capture(
    ControllerInput candidate,
    bool controller_neutral
) {
    std::lock_guard lock(g_binding_mutex);

    if (g_release_guard &&
        g_release_profile == BindingProfile::Controller &&
        controller_neutral) {
        g_release_guard = false;
    }
    if (!g_capture.active) {
        return g_release_guard;
    }
    if (!capture_matches_locked(BindingProfile::Controller)) {
        if (candidate == ControllerInput::Back &&
            !g_capture.waiting_for_neutral) {
            cancel_capture_locked("controller_back");
        }
        return true;
    }
    if (g_capture.waiting_for_neutral) {
        if (controller_neutral) {
            g_capture.waiting_for_neutral = false;
            g_revision.fetch_add(1u, std::memory_order_acq_rel);
            std::fprintf(
                stderr,
                "BUMBLE_INPUT_BINDINGS stage=capture_armed"
                " profile=controller action=%s\n",
                action_config_name(g_capture.action)
            );
            std::fflush(stderr);
        }
        return true;
    }
    if (candidate == ControllerInput::Back) {
        cancel_capture_locked("controller_back");
    } else if (candidate != ControllerInput::None) {
        complete_capture_locked(static_cast<uint16_t>(candidate));
    }
    return true;
}

bool bumble::input_bindings::input_suppressed() {
    std::lock_guard lock(g_binding_mutex);
    return g_capture.active || g_release_guard;
}

uint64_t bumble::input_bindings::revision() {
    return g_revision.load(std::memory_order_acquire);
}

bool bumble::input_bindings::valid_keyboard_binding(uint16_t binding) {
    return binding == 0u ||
        binding < kMouseBindingBase ||
        (binding >= kMouseBindingBase + static_cast<uint16_t>(MouseButton::Left) &&
         binding <= kMouseBindingBase + static_cast<uint16_t>(MouseButton::WheelDown));
}

bool bumble::input_bindings::valid_controller_binding(uint16_t binding) {
    return binding <= static_cast<uint16_t>(ControllerInput::RightStickRight);
}

uint16_t bumble::input_bindings::mouse_binding(MouseButton button) {
    return static_cast<uint16_t>(
        kMouseBindingBase + static_cast<uint16_t>(button)
    );
}

bool bumble::input_bindings::is_mouse_binding(uint16_t binding) {
    return binding >=
            kMouseBindingBase + static_cast<uint16_t>(MouseButton::Left) &&
        binding <=
            kMouseBindingBase + static_cast<uint16_t>(MouseButton::WheelDown);
}

bumble::input_bindings::MouseButton
bumble::input_bindings::mouse_button_from_binding(uint16_t binding) {
    if (!is_mouse_binding(binding)) {
        return MouseButton::Left;
    }
    return static_cast<MouseButton>(binding - kMouseBindingBase);
}

const char* bumble::input_bindings::action_config_name(InputAction action) {
    switch (action) {
    case InputAction::MoveForward: return "MoveForward";
    case InputAction::MoveBackward: return "MoveBackward";
    case InputAction::StrafeLeft: return "StrafeLeft";
    case InputAction::StrafeRight: return "StrafeRight";
    case InputAction::PrimaryFire: return "PrimaryFire";
    case InputAction::TakeOffLand: return "TakeOffLand";
    case InputAction::PreviousWeapon: return "PreviousWeapon";
    case InputAction::NextWeapon: return "NextWeapon";
    case InputAction::LoopDeLoop: return "LoopDeLoop";
    case InputAction::QuickFlip: return "QuickFlip";
    case InputAction::ForwardDash: return "ForwardDash";
    case InputAction::BarrelRoll: return "BarrelRoll";
    case InputAction::Pause: return "Pause";
    case InputAction::MenuConfirm: return "MenuConfirm";
    case InputAction::MenuBack: return "MenuBack";
    case InputAction::ToggleModernVisuals: return "ToggleModernVisuals";
    case InputAction::FlyUp: return "FlyUp";
    case InputAction::FlyDown: return "FlyDown";
    case InputAction::Count: break;
    }
    return "Unknown";
}

const char* bumble::input_bindings::action_label(InputAction action) {
    switch (action) {
    case InputAction::MoveForward: return "MOVE FORWARD";
    case InputAction::MoveBackward: return "MOVE BACKWARD";
    case InputAction::StrafeLeft: return "STRAFE LEFT";
    case InputAction::StrafeRight: return "STRAFE RIGHT";
    case InputAction::PrimaryFire: return "FIRE";
    case InputAction::TakeOffLand: return "TAKE OFF / LAND";
    case InputAction::PreviousWeapon: return "PREVIOUS WEAPON";
    case InputAction::NextWeapon: return "NEXT WEAPON";
    case InputAction::LoopDeLoop: return "LOOP-DE-LOOP";
    case InputAction::QuickFlip: return "QUICK FLIP";
    case InputAction::ForwardDash: return "SPRINT";
    case InputAction::BarrelRoll: return "BARREL ROLL";
    case InputAction::Pause: return "PAUSE";
    case InputAction::MenuConfirm: return "MENU CONFIRM";
    case InputAction::MenuBack: return "MENU BACK";
    case InputAction::ToggleModernVisuals: return "CYCLE VISUAL MODES";
    case InputAction::FlyUp: return "FLY UP";
    case InputAction::FlyDown: return "FLY DOWN";
    case InputAction::Count: break;
    }
    return "UNKNOWN";
}

const char* bumble::input_bindings::device_mode_name(InputDeviceMode mode) {
    switch (mode) {
    case InputDeviceMode::Auto: return "AUTO";
    case InputDeviceMode::KeyboardMouse: return "KEYBOARD & MOUSE";
    case InputDeviceMode::Controller: return "CONTROLLER";
    }
    return "AUTO";
}

const char* bumble::input_bindings::stick_layout_name(StickLayout layout) {
    return layout == StickLayout::RightMoveLeftLook
        ? "MOVE RIGHT / LOOK LEFT"
        : "MOVE LEFT / LOOK RIGHT";
}

std::string bumble::input_bindings::keyboard_input_name(uint16_t binding) {
    if (binding == 0u) {
        return "UNBOUND";
    }
    if (is_mouse_binding(binding)) {
        switch (mouse_button_from_binding(binding)) {
        case MouseButton::Left: return "MOUSE 1";
        case MouseButton::Right: return "MOUSE 2";
        case MouseButton::Middle: return "MOUSE 3";
        case MouseButton::X1: return "MOUSE 4";
        case MouseButton::X2: return "MOUSE 5";
        case MouseButton::WheelUp: return "WHEEL UP";
        case MouseButton::WheelDown: return "WHEEL DOWN";
        }
    }
    if (binding >= 'A' && binding <= 'Z') {
        return std::string(1u, static_cast<char>(binding));
    }
    if (binding >= '0' && binding <= '9') {
        return std::string(1u, static_cast<char>(binding));
    }
    switch (binding) {
    case key::Space: return "SPACE";
    case key::Enter: return "ENTER";
    case key::Escape: return "ESC";
    case key::Shift: return "SHIFT";
    case key::Control: return "CTRL";
    case key::Alt: return "ALT";
    case key::Tab: return "TAB";
    case key::Backspace: return "BACKSPACE";
    case key::Delete: return "DELETE";
    case key::Home: return "HOME";
    case key::End: return "END";
    case key::PageUp: return "PAGE UP";
    case key::PageDown: return "PAGE DOWN";
    case key::Up: return "UP";
    case key::Down: return "DOWN";
    case key::Left: return "LEFT";
    case key::Right: return "RIGHT";
    case key::MouseLeft: return "MOUSE 1";
    case key::MouseRight: return "MOUSE 2";
    case key::MouseMiddle: return "MOUSE 3";
    default:
        break;
    }

#if defined(_WIN32)
    const UINT scan_code = MapVirtualKeyW(binding, MAPVK_VK_TO_VSC);
    char key_name[64]{};
    if (scan_code != 0u &&
        GetKeyNameTextA(static_cast<LONG>(scan_code << 16u), key_name,
            static_cast<int>(std::size(key_name))) > 0) {
        return key_name;
    }
#endif
    char fallback[16]{};
    std::snprintf(fallback, sizeof(fallback), "KEY %u", binding);
    return fallback;
}

const char* bumble::input_bindings::controller_input_name(
    ControllerInput input
) {
    switch (input) {
    case ControllerInput::None: return "UNBOUND";
    case ControllerInput::A: return "A";
    case ControllerInput::B: return "B";
    case ControllerInput::X: return "X";
    case ControllerInput::Y: return "Y";
    case ControllerInput::Back: return "BACK";
    case ControllerInput::Guide: return "GUIDE";
    case ControllerInput::Start: return "START";
    case ControllerInput::LeftStick: return "LEFT STICK PRESS";
    case ControllerInput::RightStick: return "RIGHT STICK PRESS";
    case ControllerInput::LeftShoulder: return "LEFT BUMPER";
    case ControllerInput::RightShoulder: return "RIGHT BUMPER";
    case ControllerInput::DpadUp: return "D-PAD UP";
    case ControllerInput::DpadDown: return "D-PAD DOWN";
    case ControllerInput::DpadLeft: return "D-PAD LEFT";
    case ControllerInput::DpadRight: return "D-PAD RIGHT";
    case ControllerInput::LeftTrigger: return "LEFT TRIGGER";
    case ControllerInput::RightTrigger: return "RIGHT TRIGGER";
    case ControllerInput::LeftStickUp: return "LEFT STICK UP";
    case ControllerInput::LeftStickDown: return "LEFT STICK DOWN";
    case ControllerInput::LeftStickLeft: return "LEFT STICK LEFT";
    case ControllerInput::LeftStickRight: return "LEFT STICK RIGHT";
    case ControllerInput::RightStickUp: return "RIGHT STICK UP";
    case ControllerInput::RightStickDown: return "RIGHT STICK DOWN";
    case ControllerInput::RightStickLeft: return "RIGHT STICK LEFT";
    case ControllerInput::RightStickRight: return "RIGHT STICK RIGHT";
    }
    return "UNBOUND";
}

std::string bumble::input_bindings::binding_text(
    BindingProfile profile,
    InputAction action
) {
    const Settings settings = current();
    const ActionBindings& bindings =
        (profile == BindingProfile::Controller
            ? settings.controller
            : settings.keyboard_mouse)[action_index(action)];
    std::string result;
    for (const uint16_t binding : bindings) {
        if (binding == 0u) {
            continue;
        }
        if (!result.empty()) {
            result += " / ";
        }
        result += profile == BindingProfile::Controller
            ? controller_input_name(static_cast<ControllerInput>(binding))
            : keyboard_input_name(binding);
    }
    return result.empty() ? "UNBOUND" : result;
}
