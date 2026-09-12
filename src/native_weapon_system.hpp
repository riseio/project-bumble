#pragma once

#include <cstddef>
#include <cstdint>

#include "recomp.h"

namespace bumble::weapon_system {
    enum class ActorFamily : uint32_t {
        Blaster,
        PlasmaBlaster,
        Grenade,
        Electro,
        AutoCrossbow,
        Missile,
        Laser,
        Lightning,
        HomingCluster,
        Stinger,
        Count,
    };

    inline constexpr size_t kActorFamilyCount =
        static_cast<size_t>(ActorFamily::Count);

    void set_diagnostics_enabled(bool enabled);
    uint32_t weapon_count();
    const char* hud_label(uint32_t weapon_slot);
    const char* editor_label(uint32_t weapon_slot);
    ActorFamily actor_family_for_descriptor(uint32_t descriptor);
    const char* actor_family_profiler_name(ActorFamily family);
}

extern "C" void bumble_prepare_projectile_scene_range(
    uint8_t* rdram, recomp_context* context, uint32_t actor, uint32_t weapon_slot
);

extern "C" void bumble_update_multi_bomb_static_contact(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t actor
);

extern "C" void bumble_begin_weapon_static_step(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t actor,
    uint32_t weapon_slot
);

extern "C" uint32_t bumble_resolve_weapon_static_contact(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t actor,
    uint32_t weapon_slot,
    uint32_t authored_contact
);

extern "C" uint32_t bumble_update_modern_lightning_projectile(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t actor
);

extern "C" uint32_t bumble_lightning_collision_target_visible(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t source,
    uint32_t collision_record
);
