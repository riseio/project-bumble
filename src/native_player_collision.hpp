#pragma once

#include <cstdint>

#include "recomp.h"

struct BumbleStaticCollisionHit {
    float fraction;
    float x;
    float y;
    float z;
    float normal_x;
    float normal_y;
    float normal_z;
};

extern "C" uint32_t bumble_modern_static_world_ready(uint8_t* rdram);

extern "C" void bumble_prepare_modern_collision_world(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" uint32_t bumble_step_modern_player_airborne(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" uint32_t bumble_step_modern_player_grounded(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" uint32_t bumble_continue_modern_player_collision_update(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" void bumble_finish_modern_player_collision_update(
    uint8_t* rdram,
    recomp_context* context
);

extern "C" uint32_t bumble_query_modern_static_sphere(
    uint8_t* rdram,
    recomp_context* context,
    float x,
    float y,
    float z,
    float radius,
    BumbleStaticCollisionHit* hit
);

extern "C" uint32_t bumble_sweep_modern_static_sphere(
    uint8_t* rdram,
    recomp_context* context,
    float start_x,
    float start_y,
    float start_z,
    float end_x,
    float end_y,
    float end_z,
    float radius,
    BumbleStaticCollisionHit* hit
);
