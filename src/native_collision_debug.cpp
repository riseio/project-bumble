#include "native_collision_debug.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "librecomp/addresses.hpp"
#include "native_graphics_options.hpp"
#include "native_level_editor.hpp"
#include "native_widescreen.hpp"

#if defined(BUMBLE_COLLISION_DEBUG_RT64) && BUMBLE_COLLISION_DEBUG_RT64
#   include "render/rt64_bumble_collision_debug.h"
#endif

namespace {

constexpr uint32_t kGenericObjectListBase = 0x800D7460u;
constexpr uint32_t kGenericObjectListStride = 0xC8Cu;
constexpr uint32_t kGenericObjectListCount = 14u;
constexpr uint32_t kGenericObjectCompleteListMask =
    (1u << kGenericObjectListCount) - 1u;
constexpr uint32_t kPlayerOneOwnerSlot = 0x800E91D4u;
constexpr uint32_t kPlayerTwoOwnerSlot = 0x800E91D8u;
constexpr uint32_t kPlayerVtable = 0x800445E8u;
constexpr uint32_t kCurrentLevelIndex = 0x800E9640u;
constexpr uint32_t kDisplayListCursor = 0x80035F30u;
constexpr uint32_t kFrameMatrixBase = 0x80105224u;
constexpr uint32_t kFrameBaseFromMatrixBase = 0x68u;
constexpr uint32_t kDisplayListArenaOffset = 0xB068u;
constexpr uint32_t kDisplayListArenaEndOffset = 0x14CA8u;
constexpr uint32_t kDownstreamDisplayListReserve = 0x2800u;
constexpr uint32_t kGfxCommandBytes = 8u;
constexpr uint32_t kExtendedOpcode = 0x64u;
constexpr uint32_t kBumbleCollisionMarker = 0x37u;
constexpr uint32_t kMaximumActorsPerList = 8192u;
constexpr float kMaximumWorldCoordinate = 200000.0f;
constexpr float kMaximumActorRadius = 200000.0f;

std::atomic_bool g_extended_gbi_required{false};
std::atomic_uint64_t g_snapshot_sequence{0u};
std::atomic_uint64_t g_markers{0u};
std::atomic_uint64_t g_rejected_frames{0u};

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

uint16_t read_u16(uint8_t* rdram, uint32_t address) {
    (void)rdram;
    return static_cast<uint16_t>(MEM_HU(0, guest_address(address)));
}

uint8_t read_u8(uint8_t* rdram, uint32_t address) {
    (void)rdram;
    return static_cast<uint8_t>(MEM_BU(0, guest_address(address)));
}

float read_f32(uint8_t* rdram, uint32_t address) {
    return std::bit_cast<float>(read_u32(rdram, address));
}

void write_u32(uint8_t* rdram, uint32_t address, uint32_t value) {
    (void)rdram;
    MEM_W(0, guest_address(address)) = static_cast<int32_t>(value);
}

bool finite_world_value(float value) {
    return std::isfinite(value) &&
        std::abs(value) <= kMaximumWorldCoordinate;
}

uint32_t extended_command(uint32_t operation) {
    return (kExtendedOpcode << 24u) | operation;
}

#if defined(BUMBLE_COLLISION_DEBUG_RT64) && BUMBLE_COLLISION_DEBUG_RT64
bool collect_actor_snapshot(
    uint8_t* rdram,
    RT64::BumbleCollisionDebugSnapshot &snapshot
) {
    snapshot = {};
    snapshot.sequence = g_snapshot_sequence.fetch_add(
        1u,
        std::memory_order_relaxed
    ) + 1u;
    snapshot.levelIndex = rdram_address(kCurrentLevelIndex, 4u)
        ? read_u32(rdram, kCurrentLevelIndex)
        : 0u;

    const uint32_t debug_mode = RT64::bumbleCollisionDebugMode();
    const bool preview_only = debug_mode == 1u;
    if (preview_only) {
        snapshot.editorPreviewOnly = 1u;
        snapshot.listMask = kGenericObjectCompleteListMask;
    }

    const uint32_t player_one = rdram_address(kPlayerOneOwnerSlot, 4u)
        ? read_u32(rdram, kPlayerOneOwnerSlot)
        : 0u;
    const uint32_t player_two = rdram_address(kPlayerTwoOwnerSlot, 4u)
        ? read_u32(rdram, kPlayerTwoOwnerSlot)
        : 0u;

    std::unordered_set<uint32_t> all_actors;
    all_actors.reserve(preview_only ? 0u : 1024u);
    snapshot.primitives.reserve(preview_only ? 2u : 1024u);

    for (uint32_t category = 0u;
         !preview_only && category < kGenericObjectListCount;
         ++category) {
        const uint32_t head_address = kGenericObjectListBase +
            category * kGenericObjectListStride;
        if (!rdram_address(head_address, 4u)) {
            ++snapshot.malformedListCount;
            continue;
        }
        snapshot.listMask |= 1u << category;

        uint32_t actor = read_u32(rdram, head_address);
        std::unordered_set<uint32_t> actors_in_list;
        actors_in_list.reserve(128u);
        uint32_t traversed = 0u;
        while (actor != 0u) {
            if (++traversed > kMaximumActorsPerList ||
                !rdram_address(actor, 0x90u) ||
                !actors_in_list.insert(actor).second) {
                ++snapshot.malformedListCount;
                break;
            }

            const uint32_t next = read_u32(rdram, actor + 0x04u);
            if (!all_actors.insert(actor).second) {
                ++snapshot.malformedListCount;
                break;
            }

            const float x = read_f32(rdram, actor + 0x40u);
            const float y = read_f32(rdram, actor + 0x44u);
            const float z = read_f32(rdram, actor + 0x48u);
            float radius = read_f32(rdram, actor + 0x6Cu);
            const uint32_t collision_flags = read_u32(rdram, actor + 0x64u);
            const uint32_t collision_mask = read_u32(rdram, actor + 0x68u);
            const bool collision_participant =
                collision_flags != 0u || collision_mask != 0u;
            if (!finite_world_value(x) || !finite_world_value(y) ||
                !finite_world_value(z) ||
                (collision_participant &&
                    (!std::isfinite(radius) || radius < 0.0f ||
                     radius > kMaximumActorRadius))) {
                ++snapshot.malformedListCount;
                break;
            }
            if (!collision_participant &&
                (!std::isfinite(radius) || radius < 0.0f ||
                 radius > kMaximumActorRadius)) {
                radius = 0.0f;
            }

            const uint32_t vtable = read_u32(rdram, actor + 0x88u);
            const bool player = actor == player_one || actor == player_two ||
                vtable == kPlayerVtable;
            const bool active_sphere =
                collision_participant && radius > 0.0f;

            RT64::BumbleCollisionDebugPrimitive primitive{};
            primitive.centerX = x;
            primitive.centerY = y;
            primitive.centerZ = z;
            primitive.radius = radius;
            primitive.kind = active_sphere
                ? RT64::BumbleCollisionDebugPrimitiveActorSphere
                : RT64::BumbleCollisionDebugPrimitiveActorPoint;
            primitive.style = player
                ? RT64::BumbleCollisionDebugStylePlayerInteraction
                : collision_participant
                    ? RT64::BumbleCollisionDebugStyleActorCollider
                    : RT64::BumbleCollisionDebugStyleActorPoint;
            primitive.actorAddress = actor;
            primitive.category = category;
            primitive.collisionFlags = collision_flags;
            primitive.collisionMask = collision_mask;
            primitive.objectIdAndType =
                static_cast<uint32_t>(read_u16(rdram, actor + 0x7Eu)) |
                (static_cast<uint32_t>(read_u8(rdram, actor + 0x84u)) << 16u);
            primitive.state = read_u32(rdram, actor + 0x8Cu);
            snapshot.primitives.push_back(primitive);
            ++snapshot.actorCount;
            if (active_sphere) {
                ++snapshot.activeSphereCount;
            } else {
                ++snapshot.pointOnlyCount;
            }
            if (player) {
                ++snapshot.playerCount;
            }
            actor = next;
        }
    }

    RT64::BumbleCollisionDebugCapsule capsule{};
    if (!preview_only && RT64::readBumbleCollisionDebugCapsule(capsule)) {
        RT64::BumbleCollisionDebugPrimitive capsule_primitive{};
        capsule_primitive.centerX = capsule.centerX;
        capsule_primitive.centerY = capsule.centerY;
        capsule_primitive.centerZ = capsule.centerZ;
        capsule_primitive.radius = capsule.radius;
        capsule_primitive.halfHeight = capsule.halfHeight;
        capsule_primitive.kind =
            RT64::BumbleCollisionDebugPrimitivePlayerCapsule;
        capsule_primitive.style =
            RT64::BumbleCollisionDebugStylePlayerCapsule;
        capsule_primitive.actorAddress = player_one;
        snapshot.primitives.push_back(capsule_primitive);

        if (capsule.supportProbeDistance > 0.0f) {
            RT64::BumbleCollisionDebugPrimitive support_probe{};
            support_probe.centerX = capsule.centerX;
            support_probe.centerY =
                capsule.centerY - capsule.halfHeight - capsule.radius;
            support_probe.centerZ = capsule.centerZ;
            support_probe.extentX = support_probe.centerX;
            support_probe.extentY = support_probe.centerY -
                capsule.supportProbeDistance;
            support_probe.extentZ = support_probe.centerZ;
            support_probe.kind =
                RT64::BumbleCollisionDebugPrimitiveSupportProbe;
            support_probe.style =
                RT64::BumbleCollisionDebugStyleSupportProbe;
            support_probe.actorAddress = player_one;
            snapshot.primitives.push_back(support_probe);
        }
    }

    bumble::level_editor::Preview preview{};
    if (bumble::level_editor::preview(preview)) {
        RT64::BumbleCollisionDebugPrimitive point{};
        point.centerX = preview.x;
        point.centerY = preview.y;
        point.centerZ = preview.z;
        point.radius = preview.radius;
        point.kind = preview.delete_target || preview.actor != 0u
            ? RT64::BumbleCollisionDebugPrimitiveActorSphere
            : RT64::BumbleCollisionDebugPrimitiveActorPoint;
        point.style =
            RT64::BumbleCollisionDebugStylePlayerInteraction;
        point.actorAddress = preview.actor;
        snapshot.primitives.push_back(point);

        RT64::BumbleCollisionDebugPrimitive orientation{};
        orientation.centerX = preview.x;
        orientation.centerY = preview.y + 2.0f;
        orientation.centerZ = preview.z;
        orientation.extentX = preview.x + preview.forward_x * 64.0f;
        orientation.extentY = preview.y + 2.0f;
        orientation.extentZ = preview.z + preview.forward_z * 64.0f;
        orientation.kind =
            RT64::BumbleCollisionDebugPrimitiveSupportProbe;
        orientation.style =
            RT64::BumbleCollisionDebugStylePlayerInteraction;
        orientation.actorAddress = preview.actor;
        snapshot.primitives.push_back(orientation);
    }

    return snapshot.listMask == kGenericObjectCompleteListMask &&
        snapshot.malformedListCount == 0u;
}

bool append_marker(uint8_t* rdram, uint32_t ticket, uint32_t& new_cursor) {
    if (ticket == 0u || !rdram_address(kDisplayListCursor, 4u) ||
        !rdram_address(kFrameMatrixBase, 4u)) {
        return false;
    }

    uint32_t arena_start = bumble::widescreen::display_list_arena_base();
    uint32_t arena_end = bumble::widescreen::display_list_arena_end();
    if (arena_start == 0u || arena_end <= arena_start) {
        const uint32_t matrix_base = read_u32(rdram, kFrameMatrixBase);
        if (matrix_base < 0x80000000u + kFrameBaseFromMatrixBase) {
            return false;
        }
        const uint32_t frame_base = matrix_base - kFrameBaseFromMatrixBase;
        arena_start = frame_base + kDisplayListArenaOffset;
        arena_end = frame_base + kDisplayListArenaEndOffset;
    }

    const uint32_t cursor = read_u32(rdram, kDisplayListCursor);
    if (!rdram_address(arena_start, arena_end - arena_start) ||
        cursor < arena_start || cursor > arena_end ||
        arena_end - cursor <=
            kDownstreamDisplayListReserve + kGfxCommandBytes * 2u) {
        return false;
    }

    write_u32(
        rdram,
        cursor,
        extended_command(kBumbleCollisionMarker)
    );
    write_u32(rdram, cursor + 4u, ticket);
    new_cursor = cursor + kGfxCommandBytes;
    write_u32(rdram, kDisplayListCursor, new_cursor);
    g_extended_gbi_required.store(true, std::memory_order_release);
    return true;
}
#endif

} // namespace

bool bumble::collision_debug::extended_gbi_required() {
    return g_extended_gbi_required.load(std::memory_order_acquire);
}

void bumble::collision_debug::set_editor_preview_active(bool active) {
#if defined(BUMBLE_COLLISION_DEBUG_RT64) && BUMBLE_COLLISION_DEBUG_RT64
    RT64::setBumbleCollisionDebugMode(
        bumble::graphics_options::current().collision_overlay
            ? 2u
            : active ? 1u : 0u
    );
#else
    (void)active;
#endif
}

void bumble::collision_debug::emit_frame(
    uint8_t* rdram,
    recomp_context* context
) {
    (void)context;
#if defined(BUMBLE_COLLISION_DEBUG_RT64) && BUMBLE_COLLISION_DEBUG_RT64
    if (rdram == nullptr || RT64::bumbleCollisionDebugMode() == 0u) {
        return;
    }

    RT64::BumbleCollisionDebugSnapshot snapshot{};
    if (!collect_actor_snapshot(rdram, snapshot)) {
        const uint64_t rejected = g_rejected_frames.fetch_add(
            1u,
            std::memory_order_relaxed
        ) + 1u;
        if (rejected <= 8u || (rejected % 240u) == 0u) {
            std::fprintf(
                stderr,
                "BUMBLE_COLLISION_DEBUG stage=frame_rejected"
                " rejected=%" PRIu64 " sequence=%" PRIu64
                " level=%" PRIu32 " list_mask=0x%04" PRIX32
                " malformed_lists=%" PRIu32
                " actors=%" PRIu32 " reason=incomplete_live_owner_scan\n",
                rejected,
                snapshot.sequence,
                snapshot.levelIndex,
                snapshot.listMask,
                snapshot.malformedListCount,
                snapshot.actorCount
            );
            std::fflush(stderr);
        }
        return;
    }

    const uint32_t actor_count = snapshot.actorCount;
    const uint32_t sphere_count = snapshot.activeSphereCount;
    const uint32_t point_count = snapshot.pointOnlyCount;
    const uint32_t player_count = snapshot.playerCount;
    const uint64_t sequence = snapshot.sequence;
    const uint32_t level = snapshot.levelIndex;
    const uint32_t primitive_count = static_cast<uint32_t>(
        snapshot.primitives.size()
    );
    const uint32_t ticket = RT64::publishBumbleCollisionDebugSnapshot(
        std::move(snapshot)
    );
    uint32_t cursor = 0u;
    if (!append_marker(rdram, ticket, cursor)) {
        const uint64_t rejected = g_rejected_frames.fetch_add(
            1u,
            std::memory_order_relaxed
        ) + 1u;
        if (rejected <= 8u || (rejected % 240u) == 0u) {
            std::fprintf(
                stderr,
                "BUMBLE_COLLISION_DEBUG stage=marker_rejected"
                " rejected=%" PRIu64 " ticket=%" PRIu32
                " sequence=%" PRIu64
                " reason=display_list_capacity_or_arena\n",
                rejected,
                ticket,
                sequence
            );
            std::fflush(stderr);
        }
        return;
    }

    const uint64_t marker = g_markers.fetch_add(
        1u,
        std::memory_order_relaxed
    ) + 1u;
    if (marker <= 8u || (marker % 240u) == 0u) {
        std::fprintf(
            stderr,
            "BUMBLE_COLLISION_DEBUG stage=complete_marker_published"
            " marker=%" PRIu64 " ticket=%" PRIu32
            " sequence=%" PRIu64 " level=%" PRIu32
            " lists=14 list_mask=0x%04" PRIX32
            " actors=%" PRIu32 " spheres=%" PRIu32
            " points=%" PRIu32 " players=%" PRIu32
            " primitives=%" PRIu32
            " world=exact_jolt_mesh liquid_cells=4096"
            " ceiling=manual_y800 cursor=0x%08" PRIX32 "\n",
            marker,
            ticket,
            sequence,
            level,
            kGenericObjectCompleteListMask,
            actor_count,
            sphere_count,
            point_count,
            player_count,
            primitive_count,
            cursor
        );
        std::fflush(stderr);
    }
#else
    (void)rdram;
#endif
}
