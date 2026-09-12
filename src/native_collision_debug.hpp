#pragma once

#include <cstdint>

#include "recomp.h"

namespace bumble::collision_debug {

bool extended_gbi_required();

void set_editor_preview_active(bool active);

void emit_frame(uint8_t* rdram, recomp_context* context);

} // namespace bumble::collision_debug
