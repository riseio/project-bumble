#include "native_electric_effect.hpp"
#include "native_weapon_system.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "librecomp/addresses.hpp"
#include "native_widescreen.hpp"

#if defined(BUMBLE_ELECTRIC_RT64) && BUMBLE_ELECTRIC_RT64
#   include "render/rt64_bumble_electric.h"
#endif

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kDisplayListCursor = 0x80035F30u;
constexpr uint32_t kFrameMatrixBase = 0x80105224u;
constexpr uint32_t kFrameBaseFromMatrixBase = 0x68u;
constexpr uint32_t kDisplayListArenaOffset = 0xB068u;
constexpr uint32_t kDisplayListArenaEndOffset = 0x14CA8u;
constexpr uint32_t kCommandBytes = 8u;
constexpr uint32_t kExtendedOpcode = 0x64u;
constexpr uint32_t kBumbleElectricMarker = 0x38u;
constexpr size_t kActiveArcCapacity = 32u;
constexpr size_t kActiveEffectCapacity = 32u;
constexpr auto kArcRetention = std::chrono::milliseconds(170);
constexpr auto kImpactRetention = std::chrono::milliseconds(220);
constexpr auto kEffectRetention = std::chrono::milliseconds(1800);
constexpr auto kExplosionMergeWindow = std::chrono::milliseconds(100);
constexpr uint32_t kEffectModeRender = 1u;
constexpr uint32_t kEffectModeSuppress = 2u;
constexpr uint32_t kParticleRenderer = 1u;
constexpr uint32_t kLegacyExplosionRenderer = 2u;
constexpr uint32_t kClusterExplosionType = 0x434C5553u;
constexpr uint32_t kDebrisExplosionType = 0x44454252u;
constexpr uint32_t kLaserImpactType = 0x4C415352u;
#if defined(BUMBLE_ELECTRIC_RT64) && BUMBLE_ELECTRIC_RT64
static_assert(kLaserImpactType == RT64::BumbleParticleLaserImpact);
#endif

struct Position {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct ActiveArc {
    bool active = false;
    uint32_t sourceActor = 0u;
    uint32_t targetKey = 0u;
    uint32_t flags = 0u;
    uint32_t seed = 0u;
    Position source{};
    Position target{};
    Position normal{};
    float targetRadius = 0.0f;
    Clock::time_point born{};
    Clock::time_point lastSeen{};
    Clock::time_point impactUntil{};
};

struct ActiveEffect {
    bool active = false;
    bool renderModern = false;
    uint32_t actor = 0u;
    uint32_t renderer = 0u;
    Position position{};
    float scale = 1.0f;
    uint32_t sourceScaleBits = 0u;
    uint32_t type = 0u;
    uint32_t flags = 0u;
    uint32_t seed = 0u;
    Clock::time_point born{};
};

std::array<ActiveArc, kActiveArcCapacity> g_active_arcs{};
std::array<ActiveEffect, kActiveEffectCapacity> g_active_effects{};
std::mutex g_arc_mutex;
std::mutex g_effect_mutex;
std::atomic_bool g_extended_gbi_required{false};
std::atomic_uint64_t g_sequence{0u};
std::atomic_uint64_t g_recorded_arcs{0u};
std::atomic_uint64_t g_recorded_effects{0u};
std::atomic_uint64_t g_markers{0u};
std::atomic_bool g_diagnostics_enabled{false};
thread_local uint32_t g_pending_particle_mode = 0u;
thread_local uint32_t g_pending_legacy_effect_mode = 0u;

gpr guest_address(uint32_t address) {
    return static_cast<gpr>(static_cast<int32_t>(address));
}

bool rdram_address(uint32_t address, size_t bytes) {
    if (address < 0x80000000u) {
        return false;
    }
    const uint64_t offset = static_cast<uint64_t>(address - 0x80000000u);
    const uint64_t limit = static_cast<uint64_t>(recomp::mem_size);
    return offset <= limit && static_cast<uint64_t>(bytes) <= limit - offset;
}

uint32_t read_u32(uint8_t* rdram, uint32_t address) {
    (void)rdram;
    return static_cast<uint32_t>(MEM_W(0, guest_address(address)));
}

void write_u32(uint8_t* rdram, uint32_t address, uint32_t value) {
    (void)rdram;
    MEM_W(0, guest_address(address)) = static_cast<int32_t>(value);
}

void write_command(
    uint8_t* rdram,
    uint32_t address,
    uint32_t word0,
    uint32_t word1
) {
    write_u32(rdram, address, word0);
    write_u32(rdram, address + 4u, word1);
}

uint32_t mix32(uint32_t value) {
    value ^= value >> 16u;
    value *= 0x7FEB352Du;
    value ^= value >> 15u;
    value *= 0x846CA68Bu;
    value ^= value >> 16u;
    return value;
}

bool finite_position(const Position& position) {
    return std::isfinite(position.x) && std::isfinite(position.y) &&
        std::isfinite(position.z);
}

bool normalize(Position& value) {
    const float length_squared =
        value.x * value.x + value.y * value.y + value.z * value.z;
    if (!std::isfinite(length_squared) || length_squared < 1.0e-8f) {
        return false;
    }
    const float reciprocal = 1.0f / std::sqrt(length_squared);
    value.x *= reciprocal;
    value.y *= reciprocal;
    value.z *= reciprocal;
    return true;
}

bool should_emit_sample(uint64_t count) {
    return g_diagnostics_enabled.load(std::memory_order_relaxed) &&
        (count <= 4u || (count & (count - 1u)) == 0u);
}

void record_effect(
    const Position& position,
    float scale,
    uint32_t source_scale_bits,
    uint32_t type,
    uint32_t flags,
    uint32_t actor,
    uint32_t renderer,
    bool render_modern
) {
    if (!finite_position(position) || !std::isfinite(scale) || scale <= 0.0f) {
        return;
    }
    scale = std::clamp(scale, 0.08f, 2.5f);

    const Clock::time_point now = Clock::now();
    const std::lock_guard<std::mutex> lock(g_effect_mutex);
    if (render_modern && type != kLaserImpactType) {
        for (ActiveEffect& effect : g_active_effects) {
            if (!effect.active || !effect.renderModern ||
                effect.type == kLaserImpactType ||
                now - effect.born > kExplosionMergeWindow) {
                continue;
            }
            const float dx = effect.position.x - position.x;
            const float dy = effect.position.y - position.y;
            const float dz = effect.position.z - position.z;
            if (dx * dx + dy * dy + dz * dz > 16.0f) {
                continue;
            }
            if (type == kDebrisExplosionType &&
                effect.type != kDebrisExplosionType) {
                return;
            }
            if (type != kDebrisExplosionType &&
                effect.type == kDebrisExplosionType) {
                effect.active = false;
                break;
            }
            if (type == kDebrisExplosionType &&
                effect.type == kDebrisExplosionType &&
                actor == effect.actor) {
                effect.scale = std::max(effect.scale, scale);
                return;
            }
        }
    }

    const uint64_t count = g_recorded_effects.fetch_add(
        1u,
        std::memory_order_relaxed
    ) + 1u;
    ActiveEffect* selected = nullptr;
    ActiveEffect* oldest = &g_active_effects[0];
    for (ActiveEffect& effect : g_active_effects) {
        if (!effect.active) {
            selected = &effect;
            break;
        }
        if (effect.born < oldest->born) {
            oldest = &effect;
        }
    }
    if (selected == nullptr) {
        selected = oldest;
    }
    *selected = {
        true,
        render_modern,
        actor,
        renderer,
        position,
        scale,
        source_scale_bits,
        type,
        flags,
        mix32(
            static_cast<uint32_t>(count) ^
            type * 0x9E3779B9u ^
            std::bit_cast<uint32_t>(position.x) ^
            std::bit_cast<uint32_t>(position.z)
        ),
        now,
    };

    if (should_emit_sample(count)) {
        std::fprintf(
            stderr,
            "BUMBLE_EFFECT stage=spawn_recorded"
            " count=%" PRIu64 " actor=0x%08" PRIX32
            " renderer=%" PRIu32 " modern=%d"
            " type=0x%08" PRIX32 " flags=0x%08" PRIX32
            " scale=%.3f position=(%.3f,%.3f,%.3f)\n",
            count,
            actor,
            renderer,
            render_modern ? 1 : 0,
            type,
            flags,
            static_cast<double>(scale),
            static_cast<double>(position.x),
            static_cast<double>(position.y),
            static_cast<double>(position.z)
        );
        std::fflush(stderr);
    }
}

void record_spawn(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t mode,
    uint32_t renderer
) {
    if ((mode != kEffectModeRender && mode != kEffectModeSuppress) ||
        rdram == nullptr || context == nullptr) {
        return;
    }

    const uint32_t position_address = static_cast<uint32_t>(context->r6);
    const uint32_t stack_address = static_cast<uint32_t>(context->r29);
    if (!rdram_address(position_address, 12u) ||
        !rdram_address(stack_address + 0x10u, 4u)) {
        return;
    }

    const uint32_t scale_bits = static_cast<uint32_t>(context->r5);
    record_effect(
        {
            std::bit_cast<float>(read_u32(rdram, position_address)),
            std::bit_cast<float>(read_u32(rdram, position_address + 4u)),
            std::bit_cast<float>(read_u32(rdram, position_address + 8u)),
        },
        std::bit_cast<float>(scale_bits),
        scale_bits,
        read_u32(rdram, stack_address + 0x10u),
        static_cast<uint32_t>(context->r7),
        static_cast<uint32_t>(context->r4),
        renderer,
        mode == kEffectModeRender
    );
}

uint32_t replace_effect_render(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t renderer
) {
    if (rdram == nullptr || context == nullptr) {
        return 0u;
    }
    const uint32_t actor = static_cast<uint32_t>(context->r4);
    if (!rdram_address(actor, 0x9Cu)) {
        return 0u;
    }

    const uint32_t scale_bits = read_u32(rdram, actor + 0x94u);
    const uint32_t type = read_u32(rdram, actor + 0x98u);
    const Clock::time_point now = Clock::now();
    const std::lock_guard<std::mutex> lock(g_effect_mutex);
    for (ActiveEffect& effect : g_active_effects) {
        if (!effect.active) {
            continue;
        }
        if (now - effect.born > kEffectRetention) {
            effect.active = false;
            continue;
        }
        if (effect.actor == actor && effect.renderer == renderer &&
            effect.sourceScaleBits == scale_bits && effect.type == type) {
            return 1u;
        }
    }
    return 0u;
}

bool append_marker(uint8_t* rdram, uint32_t ticket, uint32_t& new_cursor) {
    if (ticket == 0u || !rdram_address(kDisplayListCursor, 4u) ||
        !rdram_address(kFrameMatrixBase, 4u)) {
        return false;
    }

    uint32_t arena_start = bumble::widescreen::display_list_arena_base();
    uint32_t frame_end = bumble::widescreen::display_list_arena_end();
    if (arena_start == 0u || frame_end <= arena_start) {
        const uint32_t matrix_base = read_u32(rdram, kFrameMatrixBase);
        if (matrix_base < 0x80000000u + kFrameBaseFromMatrixBase) {
            return false;
        }
        const uint32_t frame_base = matrix_base - kFrameBaseFromMatrixBase;
        arena_start = frame_base + kDisplayListArenaOffset;
        frame_end = frame_base + kDisplayListArenaEndOffset;
    }

    const uint32_t cursor = read_u32(rdram, kDisplayListCursor);
    if (!rdram_address(arena_start, frame_end - arena_start) ||
        cursor < arena_start || cursor > frame_end ||
        frame_end - cursor < kCommandBytes) {
        return false;
    }

    write_command(
        rdram,
        cursor,
        (kExtendedOpcode << 24u) | kBumbleElectricMarker,
        ticket
    );
    new_cursor = cursor + kCommandBytes;
    write_u32(rdram, kDisplayListCursor, new_cursor);
    return true;
}

} // namespace

void bumble::electric_effect::record_arc(
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
) {
    Position source{source_x, source_y, source_z};
    Position target{target_x, target_y, target_z};
    Position normal{normal_x, normal_y, normal_z};
    const float dx = target.x - source.x;
    const float dy = target.y - source.y;
    const float dz = target.z - source.z;
    const float distance_squared = dx * dx + dy * dy + dz * dz;
    if (source_actor == 0u || !finite_position(source) ||
        !finite_position(target) || !std::isfinite(distance_squared) ||
        distance_squared < 1.0f || distance_squared > 400.0f * 400.0f ||
        !std::isfinite(target_radius)) {
        return;
    }

    if (!normalize(normal)) {
        normal = {-dx, -dy, -dz};
        if (!normalize(normal)) {
            normal = {0.0f, 1.0f, 0.0f};
        }
    }
    target_radius = std::clamp(target_radius, 0.0f, 96.0f);
    flags &= ArcGeometry | ArcEnemy | ArcImpact;
    if ((flags & (ArcGeometry | ArcEnemy)) == 0u) {
        return;
    }

    const Clock::time_point now = Clock::now();
    const std::lock_guard<std::mutex> lock(g_arc_mutex);
    ActiveArc* selected = nullptr;
    ActiveArc* oldest = &g_active_arcs[0];
    for (ActiveArc& arc : g_active_arcs) {
        if (arc.active && arc.sourceActor == source_actor &&
            arc.targetKey == target_key &&
            (arc.flags & (ArcGeometry | ArcEnemy)) ==
                (flags & (ArcGeometry | ArcEnemy))) {
            selected = &arc;
            break;
        }
        if (!arc.active && selected == nullptr) {
            selected = &arc;
        }
        if (arc.lastSeen < oldest->lastSeen) {
            oldest = &arc;
        }
    }
    if (selected == nullptr) {
        selected = oldest;
    }

    const bool new_arc = !selected->active ||
        selected->sourceActor != source_actor ||
        selected->targetKey != target_key ||
        (selected->flags & (ArcGeometry | ArcEnemy)) !=
            (flags & (ArcGeometry | ArcEnemy));
    if (new_arc) {
        selected->born = now;
        selected->seed = mix32(
            source_actor ^ (target_key * 0x9E3779B9u) ^
            static_cast<uint32_t>(
                g_sequence.load(std::memory_order_relaxed)
            )
        );
        selected->impactUntil = {};
    }
    selected->active = true;
    selected->sourceActor = source_actor;
    selected->targetKey = target_key;
    selected->flags = flags & (ArcGeometry | ArcEnemy);
    selected->source = source;
    selected->target = target;
    selected->normal = normal;
    selected->targetRadius = target_radius;
    selected->lastSeen = now;
    if ((flags & ArcImpact) != 0u) {
        selected->impactUntil = now + kImpactRetention;
    }

    const uint64_t count = g_recorded_arcs.fetch_add(
        1u,
        std::memory_order_relaxed
    ) + 1u;
    if (should_emit_sample(count)) {
        std::fprintf(
            stderr,
            "BUMBLE_ELECTRIC stage=arc_endpoint_recorded"
            " count=%" PRIu64 " source=0x%08" PRIX32
            " target=0x%08" PRIX32
            " kind=%s impact=%d"
            " from=(%.3f,%.3f,%.3f) to=(%.3f,%.3f,%.3f)\n",
            count,
            source_actor,
            target_key,
            (flags & ArcEnemy) != 0u ? "enemy" : "geometry",
            (flags & ArcImpact) != 0u ? 1 : 0,
            static_cast<double>(source.x),
            static_cast<double>(source.y),
            static_cast<double>(source.z),
            static_cast<double>(target.x),
            static_cast<double>(target.y),
            static_cast<double>(target.z)
        );
        std::fflush(stderr);
    }
}

bool bumble::electric_effect::extended_gbi_required() {
    return g_extended_gbi_required.load(std::memory_order_acquire);
}

void bumble::electric_effect::set_diagnostics_enabled(bool enabled) {
    g_diagnostics_enabled.store(enabled, std::memory_order_relaxed);
}

extern "C" void bumble_mark_next_modern_particle(uint32_t mode) {
    g_pending_particle_mode = mode;
}

extern "C" void bumble_mark_next_modern_legacy_effect(uint32_t mode) {
    g_pending_legacy_effect_mode = mode;
}

extern "C" void bumble_record_modern_particle_spawn(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t mode = g_pending_particle_mode;
    g_pending_particle_mode = 0u;
    record_spawn(rdram, context, mode, kParticleRenderer);
}

extern "C" void bumble_record_modern_legacy_effect_spawn(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t mode = g_pending_legacy_effect_mode;
    g_pending_legacy_effect_mode = 0u;
    record_spawn(rdram, context, mode, kLegacyExplosionRenderer);
}

extern "C" void bumble_record_actor_modern_explosion(
    uint8_t* rdram,
    uint32_t actor,
    float scale
) {
    if (rdram == nullptr || !rdram_address(actor + 0x40u, 12u)) {
        return;
    }
    record_effect(
        {
            std::bit_cast<float>(read_u32(rdram, actor + 0x40u)),
            std::bit_cast<float>(read_u32(rdram, actor + 0x44u)),
            std::bit_cast<float>(read_u32(rdram, actor + 0x48u)),
        },
        scale,
        0u,
        kClusterExplosionType,
        0u,
        0u,
        0u,
        true
    );
}

extern "C" void bumble_record_laser_impact(uint8_t* rdram, uint32_t actor) {
    if (rdram == nullptr || !rdram_address(actor, 0x8Cu) ||
        bumble::weapon_system::actor_family_for_descriptor(
            read_u32(rdram, actor + 0x88u)) !=
            bumble::weapon_system::ActorFamily::Laser) {
        return;
    }
    record_effect({
        std::bit_cast<float>(read_u32(rdram, actor + 0x40u)),
        std::bit_cast<float>(read_u32(rdram, actor + 0x44u)),
        std::bit_cast<float>(read_u32(rdram, actor + 0x48u)),
    }, 1.0f, 0u, kLaserImpactType, 0u, actor, 0u, true);
}

extern "C" void bumble_record_debris_modern_explosion(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr) {
        return;
    }
    const uint32_t actor = static_cast<uint32_t>(context->r4);
    const uint32_t count = static_cast<uint32_t>(context->r5);
    const uint32_t stack = static_cast<uint32_t>(context->r29);
    if (count == 0u || count > 256u ||
        !rdram_address(actor + 0x40u, 12u) ||
        !rdram_address(stack + 0x10u, 0x10u) ||
        (read_u32(rdram, stack + 0x1Cu) & 0xFFu) == 0u) {
        return;
    }

    const float debris_size = std::bit_cast<float>(
        read_u32(rdram, stack + 0x10u)
    );
    if (!std::isfinite(debris_size) || debris_size <= 0.0f) {
        return;
    }
    const float scale = std::clamp(
        static_cast<float>(count) * 0.065f + debris_size * 0.0875f,
        0.25f,
        2.5f
    );
    record_effect(
        {
            std::bit_cast<float>(read_u32(rdram, actor + 0x40u)),
            std::bit_cast<float>(read_u32(rdram, actor + 0x44u)),
            std::bit_cast<float>(read_u32(rdram, actor + 0x48u)),
        },
        scale,
        0u,
        kDebrisExplosionType,
        count,
        actor,
        0u,
        true
    );
}

extern "C" uint32_t bumble_replace_modern_particle_render(
    uint8_t* rdram,
    recomp_context* context
) {
    return replace_effect_render(rdram, context, kParticleRenderer);
}

extern "C" uint32_t bumble_replace_modern_legacy_effect_render(
    uint8_t* rdram,
    recomp_context* context
) {
    return replace_effect_render(
        rdram,
        context,
        kLegacyExplosionRenderer
    );
}

extern "C" void bumble_emit_modern_electric_pass(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr) {
        return;
    }

#if defined(BUMBLE_ELECTRIC_RT64) && BUMBLE_ELECTRIC_RT64
    static_assert(
        kActiveArcCapacity == RT64::BumbleElectricMaxArcs &&
        kActiveEffectCapacity == RT64::BumbleParticleMaxEffects,
        "The RT64 snapshot must carry every retained modern effect"
    );
    static_assert(
        bumble::electric_effect::ArcGeometry ==
        RT64::BumbleElectricArcGeometry
    );
    static_assert(
        bumble::electric_effect::ArcEnemy ==
        RT64::BumbleElectricArcEnemy
    );
    static_assert(
        bumble::electric_effect::ArcImpact ==
        RT64::BumbleElectricArcImpact
    );

    const Clock::time_point now = Clock::now();
    static const Clock::time_point epoch = now;
    RT64::BumbleElectricSnapshot snapshot{};
    snapshot.sequence = g_sequence.fetch_add(
        1u,
        std::memory_order_relaxed
    ) + 1u;
    snapshot.phaseSeconds = std::fmod(
        std::chrono::duration<float>(now - epoch).count(),
        4096.0f
    );

    {
        const std::lock_guard<std::mutex> lock(g_arc_mutex);
        for (ActiveArc& active : g_active_arcs) {
            if (!active.active) {
                continue;
            }
            const bool impact_alive = now <= active.impactUntil;
            if (now - active.lastSeen > kArcRetention && !impact_alive) {
                active.active = false;
                continue;
            }
            if (snapshot.arcCount >= RT64::BumbleElectricMaxArcs) {
                break;
            }

            RT64::BumbleElectricArc& arc =
                snapshot.arcs[snapshot.arcCount++];
            arc.source = {
                active.source.x,
                active.source.y,
                active.source.z,
            };
            arc.target = {
                active.target.x,
                active.target.y,
                active.target.z,
            };
            arc.normal = {
                active.normal.x,
                active.normal.y,
                active.normal.z,
            };
            arc.ageSeconds = std::chrono::duration<float>(
                now - active.born
            ).count();
            arc.targetRadius = active.targetRadius;
            arc.seed = active.seed;
            arc.flags = active.flags |
                (impact_alive ? RT64::BumbleElectricArcImpact : 0u);
            arc.intensity =
                (active.flags & bumble::electric_effect::ArcEnemy) != 0u
                    ? 1.0f
                    : 0.88f;
            if (impact_alive) {
                arc.intensity *= 1.28f;
            }
        }
    }

    {
        const std::lock_guard<std::mutex> lock(g_effect_mutex);
        for (ActiveEffect& active : g_active_effects) {
            if (!active.active) {
                continue;
            }
            const auto age = now - active.born;
            if (age > (active.type == kLaserImpactType
                    ? kImpactRetention : kEffectRetention)) {
                active.active = false;
                continue;
            }
            if (!active.renderModern) {
                continue;
            }
            if (snapshot.effectCount >= RT64::BumbleParticleMaxEffects) {
                break;
            }

            RT64::BumbleParticleEffect& effect =
                snapshot.effects[snapshot.effectCount++];
            effect.position = {
                active.position.x,
                active.position.y,
                active.position.z,
            };
            effect.scale = active.scale;
            effect.ageSeconds =
                std::chrono::duration<float>(age).count();
            effect.seed = active.seed;
            effect.type = active.type;
            effect.flags = active.flags;
        }
    }

    if (snapshot.arcCount == 0u && snapshot.effectCount == 0u) {
        return;
    }
    const uint32_t ticket = RT64::publishBumbleElectricSnapshot(snapshot);
    uint32_t cursor = 0u;
    if (!append_marker(rdram, ticket, cursor)) {
        return;
    }

    g_extended_gbi_required.store(true, std::memory_order_release);
    const uint64_t marker = g_markers.fetch_add(
        1u,
        std::memory_order_relaxed
    ) + 1u;
    if (should_emit_sample(marker)) {
        std::fprintf(
            stderr,
            "BUMBLE_ELECTRIC stage=gpu_marker_published"
            " marker=%" PRIu64 " ticket=%" PRIu32
            " arcs=%" PRIu32 " effects=%" PRIu32
            " sequence=%" PRIu64
            " opcode=0x64 operation=0x38"
            " hook=0x80085D9C cursor=0x%08" PRIX32
            " ordering=after_terrain_before_hud"
            " ownership=value_copy\n",
            marker,
            ticket,
            snapshot.arcCount,
            snapshot.effectCount,
            snapshot.sequence,
            cursor
        );
        std::fflush(stderr);
    }
#else
    (void)rdram;
    (void)context;
#endif
}
