#pragma once

#include <cstdint>

#include "recomp.h"

namespace bumble::object_cull_telemetry {

struct PerformanceSample {
    uint64_t sample_count = 0;
    uint64_t sample_nanoseconds = 0;
};

void set_diagnostics_enabled(bool enabled);
PerformanceSample consume_performance_sample();

}

extern "C" void bumble_object_cull_pvs_reject_probe(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_expand_static_geometry_pvs(
    uint8_t* rdram,
    recomp_context* context
);
extern "C" void bumble_object_cull_distance_reject_probe(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_scale_object_distance_threshold(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_guard_object_matrix_capacity(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_guard_post_object_matrix_capacity(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_matrix_arena_world_pass_probe(
    uint8_t* rdram,
    recomp_context* context
);
