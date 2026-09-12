#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "native_input_bindings.hpp"
#include "ultramodern/input.hpp"

namespace bumble::native_io {

bool initialize();
void shutdown();
void pump_events();
bool quit_requested();

void set_keyboard_key_state(uint32_t virtual_key, bool pressed);
void set_mouse_button_state(
    input_bindings::MouseButton button,
    bool pressed
);
void clear_keyboard_key_state();
void add_mouse_wheel_delta(int delta);

bool keyboard_mouse_gameplay_active();
size_t connected_controller_count();
std::string preferred_controller_name();

bool configure_replay(const char* manifest_path, const char* masked_control);
bool configure_connected_pak(
    int controller_num,
    ultramodern::input::Pak connected_pak
);
void arm_replay();
bool replay_configured();
bool replay_uses_modern_controls();
bool replay_complete();
bool replay_semantic_success();
uint64_t replay_tick();
uint32_t replay_completed_game_event_count();
uint32_t replay_expected_game_event_count();

void poll_input();
bool get_input(int controller_num, uint16_t* buttons, float* x, float* y);
void set_rumble(int controller_num, bool rumble);
ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num);

void queue_samples(int16_t* samples, size_t sample_count);
size_t get_frames_remaining();
void set_frequency(uint32_t frequency);

uint64_t input_poll_count();
uint64_t input_transition_count();
uint64_t queued_audio_sample_count();
uint64_t consumed_audio_sample_count();

} // namespace bumble::native_io
