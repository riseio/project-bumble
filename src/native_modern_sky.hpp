#pragma once

#include <cstdint>

#include "recomp.h"

namespace bumble::modern_sky {

void set_enabled(bool enabled);
bool enabled();

bool extended_gbi_required();

} // namespace bumble::modern_sky

extern "C" void bumble_emit_modern_sky_pass(
    uint8_t* rdram,
    recomp_context* context
);
