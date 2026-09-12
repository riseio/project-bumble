#pragma once

#include <cstdint>

#include "recomp.h"

namespace bumble::electric_effect {
    enum ArcFlags : uint32_t {
        ArcGeometry = 1u << 0u,
        ArcEnemy = 1u << 1u,
        ArcImpact = 1u << 2u,
    };

    void record_arc(
        uint32_t source_actor,
        uint32_t target_key,
        float source_x,
        float source_y,
        float source_z,
        float target_x,
        float target_y,
        float target_z,
        float normal_x,
        float normal_y,
        float normal_z,
        float target_radius,
        uint32_t flags
    );

    bool extended_gbi_required();

    void set_diagnostics_enabled(bool enabled);
}

extern "C" void bumble_emit_modern_electric_pass(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_record_modern_particle_spawn(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_mark_next_modern_particle(uint32_t mode);
extern "C" void bumble_mark_next_modern_legacy_effect(uint32_t mode);

extern "C" void bumble_record_modern_legacy_effect_spawn(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_record_actor_modern_explosion(
    uint8_t* rdram,
    uint32_t actor,
    float scale
);

extern "C" void bumble_record_laser_impact(uint8_t* rdram, uint32_t actor);

extern "C" void bumble_record_debris_modern_explosion(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" uint32_t bumble_replace_modern_particle_render(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" uint32_t bumble_replace_modern_legacy_effect_render(
    uint8_t* rdram,
    recomp_context* context
);
