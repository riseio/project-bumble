#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace bumble::input_bindings {

enum class InputDeviceMode : uint32_t {
    Auto = 0,
    KeyboardMouse = 1,
    Controller = 2,
};

enum class BindingProfile : uint32_t {
    KeyboardMouse = 0,
    Controller = 1,
};

enum class StickLayout : uint32_t {
    LeftMoveRightLook = 0,
    RightMoveLeftLook = 1,
};

enum class InputAction : uint32_t {
    MoveForward = 0,
    MoveBackward,
    StrafeLeft,
    StrafeRight,
    PrimaryFire,
    TakeOffLand,
    PreviousWeapon,
    NextWeapon,
    LoopDeLoop,
    QuickFlip,
    ForwardDash,
    BarrelRoll,
    Pause,
    MenuConfirm,
    MenuBack,
    ToggleModernVisuals,
    FlyUp,
    FlyDown,
    Count,
};

enum class MouseButton : uint16_t {
    Left = 1,
    Right = 2,
    Middle = 3,
    X1 = 4,
    X2 = 5,
    WheelUp = 6,
    WheelDown = 7,
};

// Persist stable IDs, not SDL enum values.
enum class ControllerInput : uint16_t {
    None = 0,
    A,
    B,
    X,
    Y,
    Back,
    Guide,
    Start,
    LeftStick,
    RightStick,
    LeftShoulder,
    RightShoulder,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    LeftTrigger,
    RightTrigger,
    LeftStickUp,
    LeftStickDown,
    LeftStickLeft,
    LeftStickRight,
    RightStickUp,
    RightStickDown,
    RightStickLeft,
    RightStickRight,
};

constexpr size_t kInputActionCount =
    static_cast<size_t>(InputAction::Count);
constexpr size_t kBindingSlots = 3u;
constexpr uint16_t kMouseBindingBase = 0x0100u;

using ActionBindings = std::array<uint16_t, kBindingSlots>;
using BindingTable = std::array<ActionBindings, kInputActionCount>;

struct Settings {
    InputDeviceMode device_mode = InputDeviceMode::Auto;
    StickLayout stick_layout = StickLayout::LeftMoveRightLook;
    uint32_t joystick_look_sensitivity = 100u;
    uint32_t joystick_look_sensitivity_x = 100u;
    uint32_t joystick_look_sensitivity_y = 100u;
    uint32_t joystick_look_deadzone = 7000u;
    bool invert_joystick_look_x = false;
    bool invert_joystick_look_y = false;
    uint32_t mouse_sensitivity = 100u;
    uint32_t mouse_sensitivity_x = 100u;
    uint32_t mouse_sensitivity_y = 100u;
    bool mouse_acceleration = false;
    BindingTable keyboard_mouse{};
    BindingTable controller{};
};

struct CaptureState {
    bool active = false;
    bool waiting_for_neutral = false;
    BindingProfile profile = BindingProfile::KeyboardMouse;
    InputAction action = InputAction::MoveForward;
};

Settings default_settings(uint32_t version = 8u);
std::pair<float, float> scale_mouse_delta(const Settings& settings, float x, float y, float seconds);
inline void apply_menu_dpad(float& x, float& y, bool up, bool down,
                            bool left, bool right, bool menu_active) {
    if (!menu_active) return;
    if (left || right) x = float(right) - float(left);
    if (up || down) y = float(up) - float(down);
}

void upgrade_keyboard_defaults(Settings& settings, uint32_t version);
bool validate_default_contracts();
void configure(const Settings& settings);
Settings current();

void set_device_mode(InputDeviceMode mode);
void cycle_device_mode(int direction);
void set_stick_layout(StickLayout layout);
void cycle_stick_layout(int direction);
void reset_profile(BindingProfile profile);
void reset_all();

bool begin_capture(BindingProfile profile, InputAction action);
void cancel_capture();
CaptureState capture_state();

bool process_keyboard_capture(
    uint16_t binding,
    bool pressed,
    bool keyboard_mouse_neutral
);

bool process_controller_capture(
    ControllerInput candidate,
    bool controller_neutral
);

bool input_suppressed();
uint64_t revision();

bool valid_keyboard_binding(uint16_t binding);
bool valid_controller_binding(uint16_t binding);
uint16_t mouse_binding(MouseButton button);
bool is_mouse_binding(uint16_t binding);
MouseButton mouse_button_from_binding(uint16_t binding);

const char* action_config_name(InputAction action);
const char* action_label(InputAction action);
const char* device_mode_name(InputDeviceMode mode);
const char* stick_layout_name(StickLayout layout);
std::string keyboard_input_name(uint16_t binding);
const char* controller_input_name(ControllerInput input);
std::string binding_text(BindingProfile profile, InputAction action);

} // namespace bumble::input_bindings
