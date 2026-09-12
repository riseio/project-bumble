#pragma once

#include <cstdint>

namespace bumble::graphics_options {

bool native_menu_descriptor_owned(uint8_t* rdram, uint32_t descriptor);
bool native_menu_overlay_active(uint8_t* rdram, uint32_t descriptor);
bool publish_native_menu_overlay(uint8_t* rdram, uint32_t descriptor);

} // namespace bumble::graphics_options

namespace bumble::game_completion_screen {
    void observe_frame();

bool active();

} // namespace bumble::game_completion_screen
