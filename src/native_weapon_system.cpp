#include "native_weapon_system.hpp"

#include "funcs.h"
#include "native_electric_effect.hpp"
#include "native_player_collision.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {

constexpr uint32_t kObjectRegistry = 0x800E2408u;
constexpr float kMaximumSafeProjectileCoordinate = 200000.0f;
constexpr float kLightningArcRadius = 240.0f;
constexpr float kLightningImpactRadius = 4.0f;
constexpr float kLightningLineOfSightRadius = 1.0f;
constexpr float kStaticTopologyProbeRadius = 1.0f;
constexpr float kLightningMinimumEndpointAllowance = 6.0f;
constexpr float kLightningMaximumEndpointAllowance = 64.0f;

enum class FlightPolicy : uint32_t {
    CollisionOwnedTimer,
    GrenadeCollisionOwnedTimer,
    ElectroTerrainProbe,
    CrossbowPreRicochetTimer,
    AuthoredHomingFuelAndFall,
    AuthoredGuidedFuelAndFall,
    HybridLightningTerrain,
    AuthoredClusterPhases,
    StingerPreImpactTimer,
};

struct WeaponContract {
    const char* name;
    const char* hud_label;
    const char* editor_label;
    bumble::weapon_system::ActorFamily family;
    const char* profiler_name;
    uint32_t descriptor;
    uint32_t timer_offset;
    FlightPolicy policy;
};

constexpr std::array<WeaponContract, 11> kWeaponContracts{{
    {"blaster",             "BLASTER", "Blaster", bumble::weapon_system::ActorFamily::Blaster, "Host.Game.WeaponActors.Blaster",    0x80044F08u, 0x98u, FlightPolicy::CollisionOwnedTimer},
    {"plasma_blaster",      "PLASMA", "Plasma blaster", bumble::weapon_system::ActorFamily::PlasmaBlaster, "Host.Game.WeaponActors.PlasmaBlaster",     0x80044ED0u, 0x98u, FlightPolicy::CollisionOwnedTimer},
    {"grenade",             "GRENADE", "Grenade launcher", bumble::weapon_system::ActorFamily::Grenade, "Host.Game.WeaponActors.Grenade",    0x80044F78u, 0x98u, FlightPolicy::GrenadeCollisionOwnedTimer},
    {"electro",             "ELECTRO STUN", "Electro stun", bumble::weapon_system::ActorFamily::Electro, "Host.Game.WeaponActors.Electro",   0x80044D80u, 0x8Cu, FlightPolicy::ElectroTerrainProbe},
    {"auto_crossbow",       "CROSSBOW", "Auto crossbow", bumble::weapon_system::ActorFamily::AutoCrossbow, "Host.Game.WeaponActors.AutoCrossbow",   0x80044E60u, 0x98u, FlightPolicy::CrossbowPreRicochetTimer},
    {"homing_missile",      "HOMING", "Homing missile", bumble::weapon_system::ActorFamily::Missile, "Host.Game.WeaponActors.Missile",     0x80044DB8u, 0x90u, FlightPolicy::AuthoredHomingFuelAndFall},
    {"laser",               "LASER", "Laser", bumble::weapon_system::ActorFamily::Laser, "Host.Game.WeaponActors.Laser",      0x80044F40u, 0x98u, FlightPolicy::CollisionOwnedTimer},
    {"fly_by_wire_missile", "FLY-WIRE", "Fly-by-wire missile", bumble::weapon_system::ActorFamily::Missile, "Host.Game.WeaponActors.Missile",   0x80044DB8u, 0x90u, FlightPolicy::AuthoredGuidedFuelAndFall},
    {"lightning",            "LIGHTNING", "Lightning", bumble::weapon_system::ActorFamily::Lightning, "Host.Game.WeaponActors.Lightning",  0x80044E28u, 0x98u, FlightPolicy::HybridLightningTerrain},
    {"homing_cluster",      "HOMING CLUSTER", "Homing cluster", bumble::weapon_system::ActorFamily::HomingCluster, "Host.Game.WeaponActors.HomingCluster", 0x80044FE8u, 0x00u, FlightPolicy::AuthoredClusterPhases},
    {"stinger",             "STINGER", "Stinger", bumble::weapon_system::ActorFamily::Stinger, "Host.Game.WeaponActors.Stinger",       0x80044E98u, 0x9Cu, FlightPolicy::StingerPreImpactTimer},
}};


constexpr bool valid_weapon_catalog() {
    using bumble::weapon_system::ActorFamily;
    uint32_t families = 0;
    for (size_t i = 0; i < kWeaponContracts.size(); ++i) {
        const auto& entry = kWeaponContracts[i];
        if (!entry.name[0] || !entry.hud_label[0] || !entry.editor_label[0] ||
            entry.family >= ActorFamily::Count || !entry.descriptor) return false;
        families |= 1u << static_cast<uint32_t>(entry.family);
        for (size_t j = 0; j < i; ++j) {
            if (entry.descriptor == kWeaponContracts[j].descriptor &&
                !(j == 5u && i == 7u && entry.family == kWeaponContracts[j].family)) return false;
        }
    }
    return families == (1u << bumble::weapon_system::kActorFamilyCount) - 1u;
}
static_assert(kWeaponContracts.size() == 11u && valid_weapon_catalog());

std::atomic_uint64_t g_terrain_termination_count{0u};
std::atomic_uint64_t g_safety_termination_count{0u};
std::atomic_uint64_t g_lightning_geometry_arc_count{0u};
std::atomic_uint64_t g_lightning_target_rejection_count{0u};
std::atomic_uint64_t g_lightning_volume_only_rejection_count{0u};
std::atomic_bool g_diagnostics_enabled{false};

using PerWeaponCounters =
    std::array<std::atomic_uint64_t, kWeaponContracts.size()>;
PerWeaponCounters g_static_step_counts{};
PerWeaponCounters g_static_matched_contact_counts{};
PerWeaponCounters g_static_volume_only_contact_counts{};
PerWeaponCounters g_static_snapshot_miss_counts{};
std::atomic_uint64_t g_scene_range_extension_count{0u};

struct PendingWeaponStaticStep {
    uint32_t actor = 0u;
    uint32_t descriptor = 0u;
    uint32_t slot = 0u;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

thread_local PendingWeaponStaticStep g_pending_static_step{};

gpr guest_address(uint32_t address) {
    return static_cast<gpr>(static_cast<int32_t>(address));
}

bool valid_guest_pointer(uint32_t address, uint32_t size) {
    return size != 0u && address >= 0x80000000u &&
        address <= 0x84000000u - size;
}

uint32_t read_u32(uint8_t* rdram, uint32_t address) {
    return static_cast<uint32_t>(MEM_W(0, guest_address(address)));
}

int32_t read_s32(uint8_t* rdram, uint32_t address) {
    return static_cast<int32_t>(MEM_W(0, guest_address(address)));
}

uint16_t read_u16(uint8_t* rdram, uint32_t address) {
    return static_cast<uint16_t>(MEM_HU(0, guest_address(address)));
}

float read_f32(uint8_t* rdram, uint32_t address) {
    return std::bit_cast<float>(read_u32(rdram, address));
}

void write_s32(uint8_t* rdram, uint32_t address, int32_t value) {
    (void)rdram;
    MEM_W(0, guest_address(address)) = value;
}

void write_u32(uint8_t* rdram, uint32_t address, uint32_t value) {
    (void)rdram;
    MEM_W(0, guest_address(address)) = value;
}

void write_f32(uint8_t* rdram, uint32_t address, float value) {
    write_u32(rdram, address, std::bit_cast<uint32_t>(value));
}

bool should_emit_sample(uint64_t count) {
    return g_diagnostics_enabled.load(std::memory_order_relaxed) &&
        (count <= 4u || (count & (count - 1u)) == 0u);
}

bool should_emit_weapon_sample(uint64_t count) {
    return g_diagnostics_enabled.load(std::memory_order_relaxed) &&
        (count <= 8u || (count & (count - 1u)) == 0u);
}

bool projectile_position_is_safe(uint8_t* rdram, uint32_t actor) {
    const float x = read_f32(rdram, actor + 0x40u);
    const float y = read_f32(rdram, actor + 0x44u);
    const float z = read_f32(rdram, actor + 0x48u);
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
        std::abs(x) <= kMaximumSafeProjectileCoordinate &&
        std::abs(y) <= kMaximumSafeProjectileCoordinate &&
        std::abs(z) <= kMaximumSafeProjectileCoordinate;
}

uint32_t resolve_runtime_weapon_slot(
    uint8_t* rdram,
    uint32_t actor,
    uint32_t requested_slot
) {
    if (rdram == nullptr || requested_slot >= kWeaponContracts.size() ||
        !valid_guest_pointer(actor, 0xA0u)) {
        return static_cast<uint32_t>(kWeaponContracts.size());
    }
    const uint32_t descriptor = read_u32(rdram, actor + 0x88u);
    if (descriptor != kWeaponContracts[requested_slot].descriptor) {
        return static_cast<uint32_t>(kWeaponContracts.size());
    }
    if ((requested_slot == 5u || requested_slot == 7u) &&
        descriptor == kWeaponContracts[5u].descriptor) {
        return read_s32(rdram, actor + 0x9Cu) == 1 ? 7u : 5u;
    }
    return requested_slot;
}

void emit_static_contact_validation(
    const WeaponContract& contract,
    uint32_t slot,
    uint32_t actor,
    bool authored_contact,
    bool surface_contact,
    const PendingWeaponStaticStep& step,
    float end_x,
    float end_y,
    float end_z,
    const BumbleStaticCollisionHit& hit,
    const char* owner,
    PerWeaponCounters& counters
) {
    if (!g_diagnostics_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    const uint64_t count = counters[slot].fetch_add(
        1u,
        std::memory_order_relaxed
    ) + 1u;
    if (!should_emit_weapon_sample(count)) {
        return;
    }
    std::fprintf(
        stderr,
        "BUMBLE_WEAPON stage=projectile_static_contact_validated"
        " count=%" PRIu64 " slot=%" PRIu32 " weapon=%s"
        " actor=0x%08" PRIX32 " owner=%s"
        " authored_contact=%d surface_contact=%d"
        " start=(%.3f,%.3f,%.3f) end=(%.3f,%.3f,%.3f)"
        " radius=%.3f hit_fraction=%.6f hit=(%.3f,%.3f,%.3f)\n",
        count,
        slot,
        contract.name,
        actor,
        owner,
        authored_contact ? 1 : 0,
        surface_contact ? 1 : 0,
        static_cast<double>(step.x),
        static_cast<double>(step.y),
        static_cast<double>(step.z),
        static_cast<double>(end_x),
        static_cast<double>(end_y),
        static_cast<double>(end_z),
        static_cast<double>(kStaticTopologyProbeRadius),
        static_cast<double>(hit.fraction),
        static_cast<double>(hit.x),
        static_cast<double>(hit.y),
        static_cast<double>(hit.z)
    );
    std::fflush(stderr);
}

bool query_projectile_terrain(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t actor
) {
    if (context == nullptr || !projectile_position_is_safe(rdram, actor)) {
        return true;
    }

    // Use a private register context and copy SP for the callee's stack frame.
    recomp_context query = *context;
    query.r4 = guest_address(actor + 0x40u);
    func_80086AF8(rdram, &query);
    return static_cast<uint32_t>(query.r2) != 0u;
}

enum class GeometryArcOwner : uint32_t {
    None,
    AuthoredGround,
    DecodedStaticSurface,
};

float contact_distance_squared(
    float x,
    float y,
    float z,
    const BumbleStaticCollisionHit& hit
) {
    const float dx = hit.x - x;
    const float dy = hit.y - y;
    const float dz = hit.z - z;
    return dx * dx + dy * dy + dz * dz;
}

GeometryArcOwner query_projectile_geometry_within_radius(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t actor,
    float radius,
    BumbleStaticCollisionHit& hit
) {
    if (context == nullptr || !projectile_position_is_safe(rdram, actor)) {
        return GeometryArcOwner::None;
    }
    const float x = read_f32(rdram, actor + 0x40u);
    const float y = read_f32(rdram, actor + 0x44u);
    const float z = read_f32(rdram, actor + 0x48u);

    BumbleStaticCollisionHit ground_hit{};
    bool ground_in_radius = false;
    recomp_context query = *context;
    query.r4 = guest_address(actor + 0x40u);
    func_80085DD0(rdram, &query);
    const float ground_y = query.f0.fl;
    if (std::isfinite(ground_y) && std::abs(y - ground_y) <= radius) {
        ground_hit = {
            0.0f,
            x,
            ground_y,
            z,
            0.0f,
            1.0f,
            0.0f,
        };
        ground_in_radius = true;
    }

    BumbleStaticCollisionHit surface_hit{};
    const bool surface_in_radius =
        bumble_query_modern_static_sphere(
            rdram,
            context,
            x,
            y,
            z,
            radius,
            &surface_hit
        ) != 0u;
    if (!surface_in_radius && !ground_in_radius) {
        return GeometryArcOwner::None;
    }
    if (!surface_in_radius) {
        hit = ground_hit;
        return GeometryArcOwner::AuthoredGround;
    }
    if (!ground_in_radius ||
        contact_distance_squared(x, y, z, surface_hit) <=
            contact_distance_squared(x, y, z, ground_hit)) {
        hit = surface_hit;
        return GeometryArcOwner::DecodedStaticSurface;
    }
    hit = ground_hit;
    return GeometryArcOwner::AuthoredGround;
}

uint32_t resolve_collision_target(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t collision_record
) {
    if (context == nullptr ||
        !valid_guest_pointer(collision_record, 0x14u)) {
        return 0u;
    }
    recomp_context query = *context;
    query.r4 = guest_address(kObjectRegistry);
    query.r5 = read_u16(rdram, collision_record + 0x0Cu);
    func_8009F9EC(rdram, &query);
    const uint32_t target = static_cast<uint32_t>(query.r2);
    return valid_guest_pointer(target, 0xA0u) ? target : 0u;
}

void emit_counter_change(
    const WeaponContract& contract,
    uint32_t slot,
    uint32_t actor,
    int32_t previous,
    int32_t replacement,
    const char* reason,
    std::atomic_uint64_t& counter
) {
    const uint64_t count = counter.fetch_add(
        1u,
        std::memory_order_relaxed
    ) + 1u;
    if (!should_emit_sample(count)) {
        return;
    }
    std::fprintf(
        stderr,
        "BUMBLE_WEAPON stage=projectile_flight_counter_adjusted"
        " count=%" PRIu64 " slot=%" PRIu32 " weapon=%s"
        " actor=0x%08" PRIX32 " descriptor=0x%08" PRIX32
        " counter_offset=0x%02" PRIX32
        " previous=%" PRId32 " replacement=%" PRId32
        " reason=%s post_impact_timers_preserved=1\n",
        count,
        slot,
        contract.name,
        actor,
        contract.descriptor,
        contract.timer_offset,
        previous,
        replacement,
        reason
    );
    std::fflush(stderr);
}

} // namespace

void bumble::weapon_system::set_diagnostics_enabled(bool enabled) {
    g_diagnostics_enabled.store(enabled, std::memory_order_relaxed);
}

uint32_t bumble::weapon_system::weapon_count() {
    return static_cast<uint32_t>(kWeaponContracts.size());
}

const char* bumble::weapon_system::hud_label(uint32_t weapon_slot) {
    return weapon_slot < kWeaponContracts.size()
        ? kWeaponContracts[weapon_slot].hud_label
        : "";
}

const char* bumble::weapon_system::editor_label(uint32_t weapon_slot) {
    return weapon_slot < kWeaponContracts.size() ? kWeaponContracts[weapon_slot].editor_label : "";
}

bumble::weapon_system::ActorFamily
bumble::weapon_system::actor_family_for_descriptor(uint32_t descriptor) {
    for (const auto& entry : kWeaponContracts)
        if (entry.descriptor == descriptor) return entry.family;
    return ActorFamily::Count;
}

const char* bumble::weapon_system::actor_family_profiler_name(ActorFamily family) {
    for (const auto& entry : kWeaponContracts)
        if (entry.family == family) return entry.profiler_name;
    return "Host.Game.WeaponActors.Invalid";
}

extern "C" void bumble_prepare_projectile_scene_range(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t actor,
    uint32_t weapon_slot
) {
    if (weapon_slot != 0u && weapon_slot != 1u &&
        weapon_slot != 4u && weapon_slot != 6u && weapon_slot != 8u) {
        return;
    }
    if (context == nullptr ||
        resolve_runtime_weapon_slot(rdram, actor, weapon_slot) != weapon_slot ||
        !projectile_position_is_safe(rdram, actor) ||
        bumble_modern_static_world_ready(rdram) == 0u) {
        return;
    }
    const uint32_t player = read_u32(rdram, 0x800E91D4u);
    if (!valid_guest_pointer(player, 0x90u) ||
        read_u32(rdram, player + 0x88u) != 0x800445E8u ||
        read_u16(rdram, actor + 0x80u) != read_u16(rdram, player + 0x7Eu)) {
        return;
    }
    const std::array<float, 3> position{
        read_f32(rdram, actor + 0x40u), read_f32(rdram, actor + 0x44u),
        read_f32(rdram, actor + 0x48u)};
    const std::array<float, 3> velocity{
        read_f32(rdram, actor + 0x8Cu), read_f32(rdram, actor + 0x90u),
        read_f32(rdram, actor + 0x94u)};
    double travel_updates = std::numeric_limits<double>::infinity();
    for (size_t axis = 0u; axis < velocity.size(); ++axis) {
        if (!std::isfinite(velocity[axis])) {
            return;
        }
        if (velocity[axis] != 0.0f) {
            const double edge = std::copysign(
                static_cast<double>(kMaximumSafeProjectileCoordinate), velocity[axis]);
            travel_updates = std::min(travel_updates,
                (edge - position[axis]) / velocity[axis]);
        }
    }
    if (!std::isfinite(travel_updates) || travel_updates <= 0.0) {
        return;
    }
    std::array<float, 3> endpoint{};
    for (size_t axis = 0u; axis < endpoint.size(); ++axis) {
        endpoint[axis] = std::clamp(static_cast<float>(position[axis] +
            velocity[axis] * travel_updates), -kMaximumSafeProjectileCoordinate,
            kMaximumSafeProjectileCoordinate);
    }
    BumbleStaticCollisionHit hit{};
    const float impact_radius = weapon_slot == 8u
        ? kLightningImpactRadius : kStaticTopologyProbeRadius;
    if (bumble_sweep_modern_static_sphere(rdram, context,
            position[0], position[1], position[2],
            endpoint[0], endpoint[1], endpoint[2], impact_radius,
            &hit) == 0u || !std::isfinite(hit.fraction) ||
        hit.fraction <= 0.0f || hit.fraction > 1.0f) {
        return;
    }
    const double required = std::ceil(hit.fraction * travel_updates) + 2.0;
    const WeaponContract& contract = kWeaponContracts[weapon_slot];
    const int32_t original = read_s32(rdram, actor + contract.timer_offset);
    if (original <= 1 || !std::isfinite(required) || required <= original ||
        required >= std::numeric_limits<int32_t>::max()) {
        return;
    }
    const int32_t extended = static_cast<int32_t>(required);
    write_s32(rdram, actor + contract.timer_offset, extended);
    emit_counter_change(contract, weapon_slot, actor, original, extended,
        "finite_verified_scenery_range", g_scene_range_extension_count);
}

extern "C" void bumble_begin_weapon_static_step(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t actor,
    uint32_t weapon_slot
) {
    (void)context;
    const uint32_t slot = resolve_runtime_weapon_slot(
        rdram,
        actor,
        weapon_slot
    );
    if (rdram == nullptr || slot >= kWeaponContracts.size() ||
        !projectile_position_is_safe(rdram, actor)) {
        g_pending_static_step = {};
        return;
    }
    g_pending_static_step = {
        actor,
        kWeaponContracts[slot].descriptor,
        slot,
        read_f32(rdram, actor + 0x40u),
        read_f32(rdram, actor + 0x44u),
        read_f32(rdram, actor + 0x48u),
    };
    if (g_diagnostics_enabled.load(std::memory_order_relaxed)) {
        const uint64_t count = g_static_step_counts[slot].fetch_add(
            1u,
            std::memory_order_relaxed
        ) + 1u;
        if (!should_emit_weapon_sample(count)) {
            return;
        }
        std::fprintf(
            stderr,
            "BUMBLE_WEAPON stage=projectile_static_path"
            " count=%" PRIu64 " slot=%" PRIu32 " weapon=%s"
            " actor=0x%08" PRIX32 " start=(%.3f,%.3f,%.3f)\n",
            count,
            slot,
            kWeaponContracts[slot].name,
            actor,
            static_cast<double>(g_pending_static_step.x),
            static_cast<double>(g_pending_static_step.y),
            static_cast<double>(g_pending_static_step.z)
        );
        std::fflush(stderr);
    }
}

extern "C" uint32_t bumble_resolve_weapon_static_contact(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t actor,
    uint32_t weapon_slot,
    uint32_t authored_contact
) {
    const uint32_t slot = resolve_runtime_weapon_slot(
        rdram,
        actor,
        weapon_slot
    );
    if (rdram == nullptr || context == nullptr ||
        slot >= kWeaponContracts.size() ||
        !projectile_position_is_safe(rdram, actor)) {
        return authored_contact != 0u ? 1u : 0u;
    }

    if (authored_contact == 0u) {
        return 0u;
    }

    const WeaponContract& contract = kWeaponContracts[slot];
    PendingWeaponStaticStep step = g_pending_static_step;
    if (step.actor != actor || step.descriptor != contract.descriptor ||
        step.slot != slot) {
        if (g_diagnostics_enabled.load(std::memory_order_relaxed)) {
            const uint64_t count =
                g_static_snapshot_miss_counts[slot].fetch_add(
                    1u,
                    std::memory_order_relaxed
                ) + 1u;
            if (should_emit_weapon_sample(count)) {
                std::fprintf(
                    stderr,
                    "BUMBLE_WEAPON stage=projectile_static_snapshot_missed"
                    " count=%" PRIu64 " slot=%" PRIu32 " weapon=%s"
                    " actor=0x%08" PRIX32
                    " pending_actor=0x%08" PRIX32
                    " pending_slot=%" PRIu32
                    " action=preserve_authored_contact\n",
                    count,
                    slot,
                    contract.name,
                    actor,
                    step.actor,
                    step.slot
                );
                std::fflush(stderr);
            }
        }
        return authored_contact != 0u ? 1u : 0u;
    }
    const float end_x = read_f32(rdram, actor + 0x40u);
    const float end_y = read_f32(rdram, actor + 0x44u);
    const float end_z = read_f32(rdram, actor + 0x48u);
    BumbleStaticCollisionHit hit{};
    const bool surface_contact = bumble_sweep_modern_static_sphere(
        rdram,
        context,
        step.x,
        step.y,
        step.z,
        end_x,
        end_y,
        end_z,
        kStaticTopologyProbeRadius,
        &hit
    ) != 0u;
    PerWeaponCounters& counters = surface_contact
        ? g_static_matched_contact_counts
        : g_static_volume_only_contact_counts;
    emit_static_contact_validation(
        contract,
        slot,
        actor,
        true,
        surface_contact,
        step,
        end_x,
        end_y,
        end_z,
        hit,
        "authored_point_volume",
        counters
    );

    if (surface_contact) {
        write_f32(rdram, actor + 0x40u, hit.x);
        write_f32(rdram, actor + 0x44u, hit.y);
        write_f32(rdram, actor + 0x48u, hit.z);
        if (slot == 6u) {
            bumble_record_laser_impact(rdram, actor);
        }
    }
    return surface_contact ? 1u : 0u;
}

extern "C" void bumble_update_multi_bomb_static_contact(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t actor
) {
    constexpr uint32_t kMultiBombSlot = 3u;
    const WeaponContract& contract = kWeaponContracts[kMultiBombSlot];
    if (rdram == nullptr || context == nullptr ||
        !valid_guest_pointer(actor, 0xA0u)) {
        return;
    }
    if (read_u32(rdram, actor + 0x88u) != contract.descriptor) {
        return;
    }

    const uint32_t timer_address = actor + contract.timer_offset;
    const int32_t timer = read_s32(rdram, timer_address);
    const bool position_safe = projectile_position_is_safe(rdram, actor);
    const bool authored_contact = query_projectile_terrain(
        rdram,
        context,
        actor
    );
    if (bumble_resolve_weapon_static_contact(
            rdram,
            context,
            actor,
            kMultiBombSlot,
            authored_contact ? 1u : 0u) == 0u) {
        return;
    }

    // func_80065894 destroys the source at counter <= 0; set zero on contact.
    if (timer != 0) {
        write_s32(rdram, timer_address, 0);
    }
    emit_counter_change(
        contract,
        kMultiBombSlot,
        actor,
        timer,
        0,
        position_safe
            ? "terrain_collision"
            : "invalid_or_escaped_world_safety_termination",
        position_safe
            ? g_terrain_termination_count
            : g_safety_termination_count
    );
}

extern "C" uint32_t bumble_update_modern_lightning_projectile(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t actor
) {
    constexpr uint32_t kLightningSlot = 8u;
    const WeaponContract& contract = kWeaponContracts[kLightningSlot];
    if (rdram == nullptr || context == nullptr ||
        !valid_guest_pointer(actor, 0xA0u) ||
        read_u32(rdram, actor + 0x88u) != contract.descriptor) {
        return 0u;
    }

    const uint32_t timer_address = actor + contract.timer_offset;
    const int32_t timer = read_s32(rdram, timer_address);
    const float authored_acquisition_radius = read_f32(
        rdram,
        actor + 0x6Cu
    );
    if (!std::isfinite(authored_acquisition_radius) ||
        authored_acquisition_radius < 0.0f) {
        return 0u;
    }
    const float acquisition_radius = std::min(
        authored_acquisition_radius,
        kLightningArcRadius
    );
    write_f32(rdram, actor + 0x6Cu, acquisition_radius);

    if (!projectile_position_is_safe(rdram, actor)) {
        write_s32(rdram, timer_address, 0);
        emit_counter_change(
            contract,
            kLightningSlot,
            actor,
            timer,
            0,
            "invalid_or_escaped_world_safety_termination",
            g_safety_termination_count
        );
        return 1u;
    }

    const float end_x = read_f32(rdram, actor + 0x40u);
    const float end_y = read_f32(rdram, actor + 0x44u);
    const float end_z = read_f32(rdram, actor + 0x48u);
    const float velocity_x = read_f32(rdram, actor + 0x8Cu);
    const float velocity_y = read_f32(rdram, actor + 0x90u);
    const float velocity_z = read_f32(rdram, actor + 0x94u);
    const float start_x = end_x - velocity_x;
    const float start_y = end_y - velocity_y;
    const float start_z = end_z - velocity_z;

    BumbleStaticCollisionHit impact{};
    const bool scenery_impact = bumble_sweep_modern_static_sphere(
        rdram,
        context,
        start_x,
        start_y,
        start_z,
        end_x,
        end_y,
        end_z,
        kLightningImpactRadius,
        &impact
    ) != 0u;
    if (!scenery_impact &&
        g_diagnostics_enabled.load(std::memory_order_relaxed) &&
        query_projectile_terrain(rdram, context, actor)) {
        const uint64_t count =
            g_lightning_volume_only_rejection_count.fetch_add(
                1u,
                std::memory_order_relaxed
            ) + 1u;
        if (should_emit_sample(count)) {
            std::fprintf(
                stderr,
                "BUMBLE_WEAPON stage=lightning_volume_only_contact_rejected"
                " count=%" PRIu64 " actor=0x%08" PRIX32
                " start=(%.3f,%.3f,%.3f) end=(%.3f,%.3f,%.3f)"
                " impact_radius=%.3f"
                " rejected_owner=authored_point_volume"
                " accepted_owner=surface_and_authored_bsp_sweep\n",
                count,
                actor,
                static_cast<double>(start_x),
                static_cast<double>(start_y),
                static_cast<double>(start_z),
                static_cast<double>(end_x),
                static_cast<double>(end_y),
                static_cast<double>(end_z),
                static_cast<double>(kLightningImpactRadius)
            );
            std::fflush(stderr);
        }
    }

    BumbleStaticCollisionHit geometry_contact{};
    const GeometryArcOwner geometry_owner =
        query_projectile_geometry_within_radius(
        rdram,
        context,
        actor,
        acquisition_radius,
        geometry_contact
    );
    const bool geometry_in_radius =
        geometry_owner != GeometryArcOwner::None;

    if (scenery_impact || geometry_in_radius) {
        const BumbleStaticCollisionHit& endpoint =
            scenery_impact ? impact : geometry_contact;
        const float visual_source_x = scenery_impact ? start_x : end_x;
        const float visual_source_y = scenery_impact ? start_y : end_y;
        const float visual_source_z = scenery_impact ? start_z : end_z;
        bumble::electric_effect::record_arc(
            actor,
            0u,
            visual_source_x,
            visual_source_y,
            visual_source_z,
            endpoint.x,
            endpoint.y,
            endpoint.z,
            endpoint.normal_x,
            endpoint.normal_y,
            endpoint.normal_z,
            0.0f,
            bumble::electric_effect::ArcGeometry |
                (scenery_impact
                    ? bumble::electric_effect::ArcImpact
                    : 0u)
        );
        const uint64_t count = g_lightning_geometry_arc_count.fetch_add(
            1u,
            std::memory_order_relaxed
        ) + 1u;
        if (should_emit_sample(count)) {
            std::fprintf(
                stderr,
                "BUMBLE_WEAPON stage=lightning_geometry_arc"
                " count=%" PRIu64 " actor=0x%08" PRIX32
                " endpoint=(%.3f,%.3f,%.3f)"
                " acquisition_radius=%.3f impact=%d"
                " contact_owner=%s"
                " renderer=native_source_to_contact"
                " authored_child=0\n",
                count,
                actor,
                static_cast<double>(endpoint.x),
                static_cast<double>(endpoint.y),
                static_cast<double>(endpoint.z),
                static_cast<double>(acquisition_radius),
                scenery_impact ? 1 : 0,
                scenery_impact
                    ? "validated_swept_impact"
                    : geometry_owner == GeometryArcOwner::DecodedStaticSurface
                        ? "nearest_decoded_surface"
                        : "authored_ground"
            );
            std::fflush(stderr);
        }
    }

    if (scenery_impact) {
        write_f32(rdram, actor + 0x40u, impact.x);
        write_f32(rdram, actor + 0x44u, impact.y);
        write_f32(rdram, actor + 0x48u, impact.z);
        write_s32(rdram, timer_address, 0);
        emit_counter_change(
            contract,
            kLightningSlot,
            actor,
            timer,
            0,
            "surface_and_authored_bsp_swept_collision",
            g_terrain_termination_count
        );
        return 1u;
    }

    return 1u;
}

extern "C" uint32_t bumble_lightning_collision_target_visible(
    uint8_t* rdram,
    recomp_context* context,
    uint32_t source,
    uint32_t collision_record
) {
    constexpr uint32_t kLightningSlot = 8u;
    const WeaponContract& contract = kWeaponContracts[kLightningSlot];
    if (rdram == nullptr || context == nullptr ||
        !valid_guest_pointer(source, 0xA0u) ||
        !valid_guest_pointer(collision_record, 0x14u) ||
        read_u32(rdram, source + 0x88u) != contract.descriptor ||
        read_u32(rdram, collision_record + 4u) != 0x100u) {
        return 0u;
    }

    const uint32_t target = resolve_collision_target(
        rdram,
        context,
        collision_record
    );
    if (target == 0u || !projectile_position_is_safe(rdram, source) ||
        !projectile_position_is_safe(rdram, target)) {
        return 0u;
    }

    const float source_x = read_f32(rdram, source + 0x40u);
    const float source_y = read_f32(rdram, source + 0x44u);
    const float source_z = read_f32(rdram, source + 0x48u);
    const float target_x = read_f32(rdram, target + 0x40u);
    const float target_y = read_f32(rdram, target + 0x44u);
    const float target_z = read_f32(rdram, target + 0x48u);
    const float target_radius = read_f32(rdram, target + 0x6Cu);
    if (!std::isfinite(target_radius) || target_radius < 0.0f ||
        target_radius > kMaximumSafeProjectileCoordinate) {
        return 0u;
    }

    const float delta_x = target_x - source_x;
    const float delta_y = target_y - source_y;
    const float delta_z = target_z - source_z;
    const float distance_squared =
        delta_x * delta_x + delta_y * delta_y + delta_z * delta_z;
    const float acquisition_radius = read_f32(rdram, source + 0x6Cu);
    if (!std::isfinite(acquisition_radius) || acquisition_radius < 0.0f ||
        acquisition_radius > kLightningArcRadius) {
        return 0u;
    }
    const float maximum_distance = acquisition_radius + target_radius;
    const float distance = std::sqrt(distance_squared);
    bool visible = std::isfinite(distance) &&
        distance <= maximum_distance;

    BumbleStaticCollisionHit blocker{};
    if (visible && distance > 0.001f) {
        const float endpoint_allowance = std::clamp(
            target_radius,
            kLightningMinimumEndpointAllowance,
            kLightningMaximumEndpointAllowance
        );
        if (bumble_sweep_modern_static_sphere(
            rdram,
            context,
            source_x,
            source_y,
            source_z,
            target_x,
            target_y,
            target_z,
            kLightningLineOfSightRadius,
            &blocker) != 0u) {
            visible = blocker.fraction * distance + endpoint_allowance >=
                distance;
        }
    }
    if (visible) {
        float normal_x = -delta_x;
        float normal_y = -delta_y;
        float normal_z = -delta_z;
        if (distance > 0.001f) {
            normal_x /= distance;
            normal_y /= distance;
            normal_z /= distance;
        }
        else {
            normal_x = 0.0f;
            normal_y = 1.0f;
            normal_z = 0.0f;
        }
        const float surface_offset = std::min(
            std::clamp(target_radius * 0.45f, 0.0f, 36.0f),
            distance * 0.35f
        );
        bumble::electric_effect::record_arc(
            source,
            target,
            source_x,
            source_y,
            source_z,
            target_x + normal_x * surface_offset,
            target_y + normal_y * surface_offset,
            target_z + normal_z * surface_offset,
            normal_x,
            normal_y,
            normal_z,
            target_radius,
            bumble::electric_effect::ArcEnemy |
                bumble::electric_effect::ArcImpact
        );
        return 1u;
    }

    const uint64_t count = g_lightning_target_rejection_count.fetch_add(
        1u,
        std::memory_order_relaxed
    ) + 1u;
    if (should_emit_sample(count)) {
        std::fprintf(
            stderr,
            "BUMBLE_WEAPON stage=lightning_target_rejected"
            " count=%" PRIu64 " source=0x%08" PRIX32
            " target=0x%08" PRIX32 " distance=%.3f"
            " acquisition_radius=%.3f reason=%s\n",
            count,
            source,
            target,
            static_cast<double>(distance),
            static_cast<double>(acquisition_radius),
            distance > maximum_distance
                ? "outside_collision_sphere"
                : "authored_geometry_occluded"
        );
        std::fflush(stderr);
    }
    return 0u;
}
