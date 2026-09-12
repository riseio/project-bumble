#pragma once

#include <cstdint>

#include "recomp.h"

namespace bumble::startup_flow {

void configure(bool skip_intro, bool suppress_attract);
bool skip_intro_enabled();
bool attract_suppression_enabled();
uint64_t intro_skip_count();
uint64_t attract_suppression_count();

void configure_rumble_prompt_bypass(bool bypass_prompt);
bool rumble_prompt_bypass_enabled();
uint64_t rumble_prompt_bypass_count();

} // namespace bumble::startup_flow

extern "C" void bumble_apply_startup_menu_skip(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_bypass_new_game_rumble_prompt(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_disable_main_menu_attract(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_preempt_main_menu_attract_transition(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_retain_main_menu_after_attract_transition(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_preempt_new_game_rumble_prompt(
    uint8_t* rdram,
    recomp_context* context
);
