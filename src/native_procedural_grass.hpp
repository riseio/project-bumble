#pragma once

#include <cstdint>

#include "recomp.h"

namespace bumble::procedural_grass {

enum class Mode : uint32_t {
    Off = 0,
    Low = 1,
    High = 2,
};

void set_mode(Mode mode);
Mode mode();
bool enabled();

// Keep opcode 0x64 enabled after first use; queued display lists may still contain it.
bool extended_gbi_required();

} // namespace bumble::procedural_grass

extern "C" void bumble_collect_procedural_grass_cell(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_emit_procedural_grass_pass(
    uint8_t* rdram,
    recomp_context* context
);
