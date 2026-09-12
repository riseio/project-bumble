#pragma once

#include <cstdint>
#include "recomp.h"

namespace bumble::combat_feedback {
void observe_frame(uint8_t* rdram);
}

extern "C" void bumble_begin_damage_feedback(uint8_t*, recomp_context*);
extern "C" void bumble_end_damage_feedback(uint8_t*, recomp_context*, uint32_t stack_size);
extern "C" void bumble_bind_boss_encounter(uint8_t*, recomp_context*);
extern "C" void bumble_begin_boss_health_bar(uint8_t*, recomp_context*);
extern "C" void bumble_end_boss_health_bar(uint8_t*, recomp_context*);
