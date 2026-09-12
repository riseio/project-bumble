#include "native_player_collision.hpp"

#include "funcs.h"
#include "native_campaign_levels.hpp"
#include "native_gameplay_options.hpp"
#include "native_modern_controls.hpp"

#if defined(BUMBLE_COLLISION_DEBUG_RT64) && BUMBLE_COLLISION_DEBUG_RT64
#   include "render/rt64_bumble_collision_debug.h"
#endif

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Geometry/ClosestPoint.h>
#include <Jolt/Geometry/Triangle.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "librecomp/addresses.hpp"

namespace {

bool collision_runtime_telemetry_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv(
            "BUMBLE_COLLISION_RUNTIME_TELEMETRY"
        );
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

namespace Layers {
constexpr JPH::ObjectLayer kStatic = 0;
constexpr JPH::ObjectLayer kMoving = 1;
constexpr JPH::ObjectLayer kCount = 2;
}

namespace BroadPhaseLayers {
constexpr JPH::BroadPhaseLayer kStatic{0};
constexpr JPH::BroadPhaseLayer kMoving{1};
constexpr JPH::uint kCount = 2;
}

constexpr uint32_t kPlayerVtable = 0x800445E8u;
constexpr uint32_t kPlayerOneOwnerSlot = 0x800E91D4u;
constexpr uint32_t kPlayerOneCamera = 0x800E9254u;
constexpr uint32_t kPlayerOneHealth = 0x800E92D8u;
constexpr uint32_t kCurrentLevelIndex = 0x800E9640u;
constexpr uint32_t kTerrainCellTable = 0x803CE000u;
constexpr uint32_t kTerrainCellCount = 64u;
constexpr uint32_t kTerrainCellStride = 8u;
constexpr uint32_t kTerrainDescriptorBase = 0x800EA4D0u;
constexpr uint32_t kTerrainDescriptorEnd = 0x80100000u;
constexpr uint32_t kTerrainDescriptorStride = 0x1Cu;
constexpr uint32_t kTerrainRotationMatrices = 0x801043E0u;
constexpr uint32_t kFlyingState = 2u;
constexpr uint32_t kGroundedEntryState = 5u;
constexpr uint32_t kGroundedFirstState = 6u;
constexpr uint32_t kGroundedLastState = 7u;

constexpr float kGameOwnedPlayerRadius = 8.0f;
constexpr float kCapsuleRadius = kGameOwnedPlayerRadius;
constexpr float kCapsuleHalfHeight = 8.0f;
constexpr float kCharacterPadding = 0.5f;
constexpr float kAirborneActorClearance = 15.0f;
constexpr float kGroundedActorClearance = 9.0f;
constexpr float kAirborneCapsuleCenterAboveActor =
    kCapsuleRadius + kCapsuleHalfHeight - kAirborneActorClearance;
constexpr float kGroundedCapsuleCenterAboveActor =
    kCapsuleRadius + kCapsuleHalfHeight - kGroundedActorClearance;
static_assert(kAirborneCapsuleCenterAboveActor == 1.0f);
static_assert(kGroundedCapsuleCenterAboveActor == 7.0f);
constexpr float kPlayerCeilingY = 800.0f;
constexpr float kLandingProbeDistance = 6.0f;
constexpr float kGroundStickDistance = 8.0f;
constexpr float kGroundStepHeight = 12.0f;
constexpr float kExternalPositionEpsilon = 0.25f;
constexpr float kMaximumWorldCoordinate = 200000.0f;
// Terrain collision uses cell-local signed 12.4 coordinates; reject vertices outside that range.
constexpr int32_t kMinimumAuthoredLocalCoordinate = -2048;
constexpr int32_t kMaximumAuthoredLocalCoordinate = 2047;
constexpr size_t kMaximumDisplayListCommands = 4096u;
constexpr uint32_t kMaximumDisplayListDepth = 8u;
// Segment bases from 0x80054474..0x800544F8; segment 1 is loaded per level.
constexpr uint32_t kLevelGeometrySegmentBaseSlot = 0x800CC8C0u;
constexpr std::array<uint32_t, 16> kF3dexSegmentGuestBases{
    0x80000000u, // segment 0: physical RDRAM
    0u,          // segment 1: live pointer in kLevelGeometrySegmentBaseSlot
    0u,
    0x801DCAD0u, // segment 3
    0x80164DB0u, // segment 4: level terrain geometry
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct TrackedHollowPassage {
    const char* name = nullptr;
    Vec3 pickup{};
    float actor_vertical_offset = 0.0f;
    float yaw_degrees = 0.0f;
};

constexpr std::array<TrackedHollowPassage, 4>
kMissionOneTrackedHollowPassages{{
    {"passage_0", {-2155.2f, 265.6f, -784.0f}, -8.0f, 90.0f},
    {"passage_1", {-1750.4f, 273.6f, 88.0f}, -16.0f, 0.0f},
    {"passage_2", {-2708.8f, 265.6f, 403.2f}, -8.0f, 5.0f},
    {"passage_3", {-2801.6f, 268.8f, -3838.4f}, -12.0f, 90.0f},
}};

struct LocalTriangle {
    Vec3 a{};
    Vec3 b{};
    Vec3 c{};
};

struct DynamicGateRegistration {
    int32_t column = 0;
    int32_t row = 0;
};

std::vector<DynamicGateRegistration> g_dynamic_gate_registrations;
uint32_t g_dynamic_gate_registration_level =
    std::numeric_limits<uint32_t>::max();

struct Matrix3 {
    float value[3][3]{};
};

struct GeometryStats {
    uint32_t solid_cells = 0;
    uint32_t descriptor_instances = 0;
    uint32_t decoded_descriptors = 0;
    uint32_t empty_descriptors = 0;
    uint32_t unresolved_addresses = 0;
    uint32_t malformed_commands = 0;
    uint32_t rejected_local_vertices = 0;
    uint32_t maximum_abs_local_vertex = 0;
    uint32_t rejected_visual_triangles = 0;
};

JPH::RVec3 to_jolt(const Vec3& value) {
    return JPH::RVec3(value.x, value.y, value.z);
}

Vec3 from_jolt(JPH::RVec3Arg value) {
    return {
        static_cast<float>(value.GetX()),
        static_cast<float>(value.GetY()),
        static_cast<float>(value.GetZ()),
    };
}

float capsule_center_above_actor(bool grounded_pose) {
    return grounded_pose
        ? kGroundedCapsuleCenterAboveActor
        : kAirborneCapsuleCenterAboveActor;
}

Vec3 actor_to_character_center(
    const Vec3& actor_position,
    bool grounded_pose
) {
    return {
        actor_position.x,
        actor_position.y + capsule_center_above_actor(grounded_pose),
        actor_position.z,
    };
}

Vec3 character_center_to_actor(
    const Vec3& character_position,
    bool grounded_pose
) {
    return {
        character_position.x,
        character_position.y - capsule_center_above_actor(grounded_pose),
        character_position.z,
    };
}

bool state_uses_grounded_pose(uint32_t state) {
    return state >= kGroundedFirstState && state <= kGroundedLastState;
}

bool finite_vector(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.z) &&
        std::abs(value.x) <= kMaximumWorldCoordinate &&
        std::abs(value.y) <= kMaximumWorldCoordinate &&
        std::abs(value.z) <= kMaximumWorldCoordinate;
}

float distance_squared(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

float vector_length(const Vec3& value) {
    return std::sqrt(
        value.x * value.x + value.y * value.y + value.z * value.z
    );
}

Vec3 subtract(const Vec3& first, const Vec3& second) {
    return {
        first.x - second.x,
        first.y - second.y,
        first.z - second.z,
    };
}

Vec3 add_scaled(const Vec3& origin, const Vec3& direction, float scale) {
    return {
        origin.x + direction.x * scale,
        origin.y + direction.y * scale,
        origin.z + direction.z * scale,
    };
}

Vec3 normalized(const Vec3& value) {
    const float length = std::sqrt(
        value.x * value.x + value.y * value.y + value.z * value.z
    );
    if (!std::isfinite(length) || length < 0.0001f) {
        return {};
    }
    return {value.x / length, value.y / length, value.z / length};
}

gpr guest_address(uint32_t address) {
    return static_cast<gpr>(static_cast<int32_t>(address));
}

bool valid_guest_address(uint32_t address, size_t bytes) {
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

int16_t read_s16(uint8_t* rdram, uint32_t address) {
    (void)rdram;
    return static_cast<int16_t>(MEM_H(0, guest_address(address)));
}

uint8_t read_u8(uint8_t* rdram, uint32_t address) {
    (void)rdram;
    return static_cast<uint8_t>(MEM_BU(0, guest_address(address)));
}

float read_f32(uint8_t* rdram, uint32_t address) {
    return std::bit_cast<float>(read_u32(rdram, address));
}

bool player_one_alive(uint8_t* rdram) {
    const float health = read_f32(rdram, kPlayerOneHealth);
    return std::isfinite(health) && health > 0.0f;
}

void write_u32(uint8_t* rdram, uint32_t address, uint32_t value) {
    (void)rdram;
    MEM_W(0, guest_address(address)) = static_cast<int32_t>(value);
}

void write_f32(uint8_t* rdram, uint32_t address, float value) {
    write_u32(rdram, address, std::bit_cast<uint32_t>(value));
}

void write_s16(uint8_t* rdram, uint32_t address, int16_t value) {
    (void)rdram;
    MEM_H(0, guest_address(address)) = value;
}

Vec3 read_actor_position(uint8_t* rdram, uint32_t actor) {
    return {
        read_f32(rdram, actor + 0x40u),
        read_f32(rdram, actor + 0x44u),
        read_f32(rdram, actor + 0x48u),
    };
}

void write_actor_position(uint8_t* rdram, uint32_t actor, const Vec3& position) {
    write_f32(rdram, actor + 0x40u, position.x);
    write_f32(rdram, actor + 0x44u, position.y);
    write_f32(rdram, actor + 0x48u, position.z);
}

bool exact_modern_player_one(uint8_t* rdram, uint32_t actor, uint32_t state) {
    return rdram != nullptr && actor != 0u &&
        valid_guest_address(actor, 0xFCu) &&
        read_u32(rdram, kPlayerOneOwnerSlot) == actor &&
        read_u32(rdram, actor + 0x88u) == kPlayerVtable &&
        read_u32(rdram, actor + 0xF8u) == kPlayerOneCamera &&
        read_u32(rdram, actor + 0x8Cu) == state;
}

uint32_t resolve_game_address(
    uint8_t* rdram,
    uint32_t address,
    size_t bytes
) {
    uint32_t guest = 0u;
    if ((address & 0x80000000u) != 0u) {
        guest = address;
    } else {
        const uint32_t segment = address >> 24u;
        if (segment < kF3dexSegmentGuestBases.size()) {
            const uint32_t base = segment == 1u
                ? read_u32(rdram, kLevelGeometrySegmentBaseSlot)
                : kF3dexSegmentGuestBases[segment];
            const uint64_t resolved = static_cast<uint64_t>(base) +
                static_cast<uint64_t>(address & 0x00FFFFFFu);
            if (base != 0u && resolved <= std::numeric_limits<uint32_t>::max()) {
                guest = static_cast<uint32_t>(resolved);
            }
        }
    }
    return guest != 0u && valid_guest_address(guest, bytes) ? guest : 0u;
}

uint64_t fnv_mix(uint64_t hash, uint32_t value) {
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        hash ^= static_cast<uint8_t>(value >> shift);
        hash *= 1099511628211ull;
    }
    return hash;
}

class BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    BroadPhaseLayerInterface() {
        layers_[Layers::kStatic] = BroadPhaseLayers::kStatic;
        layers_[Layers::kMoving] = BroadPhaseLayers::kMoving;
    }

    JPH::uint GetNumBroadPhaseLayers() const override {
        return BroadPhaseLayers::kCount;
    }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(
        JPH::ObjectLayer layer
    ) const override {
        return layers_[layer];
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == BroadPhaseLayers::kStatic ? "STATIC" : "MOVING";
    }
#endif

private:
    JPH::BroadPhaseLayer layers_[Layers::kCount]{};
};

class ObjectVsBroadPhaseFilter final
    : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(
        JPH::ObjectLayer object,
        JPH::BroadPhaseLayer broad_phase
    ) const override {
        if (object == Layers::kStatic) {
            return broad_phase == BroadPhaseLayers::kMoving;
        }
        return object == Layers::kMoving;
    }
};

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(
        JPH::ObjectLayer first,
        JPH::ObjectLayer second
    ) const override {
        if (first == Layers::kStatic) {
            return second == Layers::kMoving;
        }
        return first == Layers::kMoving;
    }
};

class DisplayListDecoder {
public:
    DisplayListDecoder(uint8_t* rdram, GeometryStats& stats)
        : rdram_(rdram), stats_(stats) {}

    std::vector<LocalTriangle> decode(uint32_t display_list) {
        triangles_.clear();
        vertices_ = {};
        active_lists_.clear();
        command_count_ = 0u;
        decode_list(resolve_game_address(rdram_, display_list, 8u), 0u);
        return triangles_;
    }

private:
    struct CachedVertex {
        Vec3 value{};
        bool valid = false;
    };

    void append_triangle(uint32_t a, uint32_t b, uint32_t c) {
        if (a >= vertices_.size() || b >= vertices_.size() ||
            c >= vertices_.size() || !vertices_[a].valid ||
            !vertices_[b].valid || !vertices_[c].valid) {
            ++stats_.malformed_commands;
            return;
        }
        const Vec3& va = vertices_[a].value;
        const Vec3& vb = vertices_[b].value;
        const Vec3& vc = vertices_[c].value;
        const float abx = vb.x - va.x;
        const float aby = vb.y - va.y;
        const float abz = vb.z - va.z;
        const float acx = vc.x - va.x;
        const float acy = vc.y - va.y;
        const float acz = vc.z - va.z;
        const float nx = aby * acz - abz * acy;
        const float ny = abz * acx - abx * acz;
        const float nz = abx * acy - aby * acx;
        if (nx * nx + ny * ny + nz * nz > 0.0001f) {
            triangles_.push_back({va, vb, vc});
        }
    }

    void append_triangle_word(uint32_t word) {
        append_triangle(
            (word >> 17u) & 0x7Fu,
            (word >> 9u) & 0x7Fu,
            (word >> 1u) & 0x7Fu
        );
    }

    void load_vertices(uint32_t word0, uint32_t word1) {
        const uint32_t count = (word0 >> 12u) & 0xFFu;
        const uint32_t end = (word0 >> 1u) & 0x7Fu;
        if (count == 0u || end < count || end > vertices_.size()) {
            ++stats_.malformed_commands;
            return;
        }
        const uint32_t first = end - count;
        const uint32_t address = resolve_game_address(
            rdram_,
            word1,
            static_cast<size_t>(count) * 16u
        );
        if (address == 0u) {
            ++stats_.unresolved_addresses;
            return;
        }
        for (uint32_t index = 0; index < count; ++index) {
            const uint32_t vertex = address + index * 16u;
            const int32_t x = read_s16(rdram_, vertex + 0u);
            const int32_t y = read_s16(rdram_, vertex + 2u);
            const int32_t z = read_s16(rdram_, vertex + 4u);
            stats_.maximum_abs_local_vertex = std::max({
                stats_.maximum_abs_local_vertex,
                static_cast<uint32_t>(std::abs(x)),
                static_cast<uint32_t>(std::abs(y)),
                static_cast<uint32_t>(std::abs(z)),
            });
            const bool authored_coordinate =
                x >= kMinimumAuthoredLocalCoordinate &&
                x <= kMaximumAuthoredLocalCoordinate &&
                y >= kMinimumAuthoredLocalCoordinate &&
                y <= kMaximumAuthoredLocalCoordinate &&
                z >= kMinimumAuthoredLocalCoordinate &&
                z <= kMaximumAuthoredLocalCoordinate;
            vertices_[first + index] = {
                {
                    static_cast<float>(x),
                    static_cast<float>(y),
                    static_cast<float>(z),
                },
                authored_coordinate,
            };
            if (!authored_coordinate) {
                ++stats_.rejected_local_vertices;
            }
        }
    }

    void decode_list(uint32_t display_list, uint32_t depth) {
        if (display_list == 0u || depth > kMaximumDisplayListDepth ||
            active_lists_.contains(display_list)) {
            ++stats_.unresolved_addresses;
            return;
        }
        active_lists_.insert(display_list);
        for (size_t index = 0u;
             command_count_ < kMaximumDisplayListCommands;
             ++index, ++command_count_) {
            const uint32_t address = display_list +
                static_cast<uint32_t>(index * 8u);
            if (!valid_guest_address(address, 8u)) {
                ++stats_.unresolved_addresses;
                break;
            }
            const uint32_t word0 = read_u32(rdram_, address);
            const uint32_t word1 = read_u32(rdram_, address + 4u);
            const uint8_t opcode = static_cast<uint8_t>(word0 >> 24u);
            switch (opcode) {
            case 0x01u:
                load_vertices(word0, word1);
                break;
            case 0x05u:
                append_triangle_word(word0);
                break;
            case 0x06u:
            case 0x07u:
                append_triangle_word(word0);
                append_triangle_word(word1);
                break;
            case 0xDEu: {
                const uint32_t nested = resolve_game_address(
                    rdram_,
                    word1,
                    8u
                );
                if (nested == 0u) {
                    ++stats_.unresolved_addresses;
                } else {
                    decode_list(nested, depth + 1u);
                }
                if ((word0 & 0x00010000u) != 0u) {
                    active_lists_.erase(display_list);
                    return;
                }
                break;
            }
            case 0xDFu:
                active_lists_.erase(display_list);
                return;
            default:
                break;
            }
        }
        if (command_count_ >= kMaximumDisplayListCommands) {
            ++stats_.malformed_commands;
        }
        active_lists_.erase(display_list);
    }

    uint8_t* rdram_ = nullptr;
    GeometryStats& stats_;
    std::array<CachedVertex, 80u> vertices_{};
    std::vector<LocalTriangle> triangles_{};
    std::unordered_set<uint32_t> active_lists_{};
    size_t command_count_ = 0u;
};

Matrix3 decode_rotation_matrix(uint8_t* rdram, uint32_t rotation) {
    Matrix3 matrix{};
    const uint32_t base = kTerrainRotationMatrices + (rotation & 3u) * 64u;
    bool plausible = valid_guest_address(base, 64u);
    for (uint32_t row = 0u; row < 3u; ++row) {
        for (uint32_t column = 0u; column < 3u; ++column) {
            const uint32_t element = row * 4u + column;
            const uint32_t plane_offset = (element / 2u) * 4u +
                (element & 1u) * 2u;
            const int16_t integer = read_s16(rdram, base + plane_offset);
            const uint16_t fraction = read_u16(
                rdram,
                base + 32u + plane_offset
            );
            const float value = static_cast<float>(integer) +
                static_cast<float>(fraction) / 65536.0f;
            matrix.value[row][column] = value;
            plausible = plausible && std::isfinite(value) &&
                std::abs(value) <= 1.01f;
        }
    }
    if (plausible) {
        return matrix;
    }

    matrix = {};
    const float angle = static_cast<float>(rotation & 3u) *
        (0.5f * JPH::JPH_PI);
    matrix.value[0][0] = std::cos(angle);
    matrix.value[0][2] = -std::sin(angle);
    matrix.value[1][1] = 1.0f;
    matrix.value[2][0] = std::sin(angle);
    matrix.value[2][2] = std::cos(angle);
    return matrix;
}

Vec3 transform_vertex(
    const Vec3& local,
    const Matrix3& rotation,
    float translation_x,
    float translation_y,
    float translation_z
) {
    return {
        local.x * rotation.value[0][0] +
            local.y * rotation.value[1][0] +
            local.z * rotation.value[2][0] + translation_x,
        local.x * rotation.value[0][1] +
            local.y * rotation.value[1][1] +
            local.z * rotation.value[2][1] + translation_y,
        local.x * rotation.value[0][2] +
            local.y * rotation.value[1][2] +
            local.z * rotation.value[2][2] + translation_z,
    };
}


bool authored_world_is_solid(
    uint8_t* rdram,
    recomp_context* context,
    const Vec3& point,
    bool unavailable_is_solid = true
) {
    if (rdram == nullptr || context == nullptr) {
        return unavailable_is_solid;
    }
    const uint32_t scratch = static_cast<uint32_t>(context->r29) + 0x10u;
    if (!valid_guest_address(scratch, 12u)) {
        return unavailable_is_solid;
    }
    const std::array<uint32_t, 3> saved{
        read_u32(rdram, scratch + 0u),
        read_u32(rdram, scratch + 4u),
        read_u32(rdram, scratch + 8u),
    };
    write_f32(rdram, scratch + 0u, point.x);
    write_f32(rdram, scratch + 4u, point.y);
    write_f32(rdram, scratch + 8u, point.z);
    recomp_context query = *context;
    query.r4 = guest_address(scratch);
    func_80086AF8(rdram, &query);
    write_u32(rdram, scratch + 0u, saved[0]);
    write_u32(rdram, scratch + 4u, saved[1]);
    write_u32(rdram, scratch + 8u, saved[2]);
    return static_cast<uint32_t>(query.r2) != 0u;
}

class AuthoredTerrainContactListener final
    : public JPH::CharacterContactListener {
public:
    void bind(
        uint8_t* rdram,
        recomp_context* context,
        JPH::BodyID terrain_body
    ) {
        rdram_ = rdram;
        context_ = context;
        terrain_body_ = terrain_body;
    }

    bool OnContactValidate(
        const JPH::CharacterVirtual*,
        const JPH::CharacterContact& contact
    ) override {
        if (contact.mBodyB != terrain_body_ ||
            rdram_ == nullptr ||
            context_ == nullptr) {
            return true;
        }
        const Vec3 position = from_jolt(contact.mPosition);
        const JPH::Vec3 normal = contact.mContactNormal;
        constexpr float kSolidSideProbeDistance = 2.0f;
        const Vec3 solid_side{
            position.x - normal.GetX() * kSolidSideProbeDistance,
            position.y - normal.GetY() * kSolidSideProbeDistance,
            position.z - normal.GetZ() * kSolidSideProbeDistance,
        };
        const bool solid =
            authored_world_is_solid(rdram_, context_, solid_side);
        if (!solid) {
            const uint64_t count = ++discarded_contacts_;
            if (collision_runtime_telemetry_enabled() &&
                (count <= 8u || (count % 600u) == 0u)) {
                std::fprintf(
                    stderr,
                    "BUMBLE_COLLISION stage=authored_contact_discarded"
                    " count=%" PRIu64
                    " level=%" PRIu32
                    " point=(%.3f,%.3f,%.3f)\n",
                    count,
                    read_u32(rdram_, kCurrentLevelIndex),
                    static_cast<double>(solid_side.x),
                    static_cast<double>(solid_side.y),
                    static_cast<double>(solid_side.z)
                );
                std::fflush(stderr);
            }
        }
        return solid;
    }

private:
    uint8_t* rdram_ = nullptr;
    recomp_context* context_ = nullptr;
    JPH::BodyID terrain_body_{};
    uint64_t discarded_contacts_ = 0u;
};


std::vector<LocalTriangle> decode_collision_descriptor(
    uint8_t* rdram,
    recomp_context* context,
    uint16_t descriptor_id,
    GeometryStats& stats
) {
    if (descriptor_id == 0u) {
        return {};
    }
    const uint64_t descriptor64 =
        static_cast<uint64_t>(kTerrainDescriptorBase) +
        static_cast<uint64_t>(descriptor_id) * kTerrainDescriptorStride;
    if (descriptor64 + kTerrainDescriptorStride > kTerrainDescriptorEnd) {
        return {};
    }
    const uint32_t descriptor = static_cast<uint32_t>(descriptor64);
    const uint32_t collision_data = read_u32(rdram, descriptor + 4u);
    if (collision_data == 0u) {
        return {};
    }

    DisplayListDecoder decoder(rdram, stats);
    const uint32_t full = read_u32(rdram, descriptor);
    std::vector<LocalTriangle> triangles = decoder.decode(full);
    if (triangles.empty()) {
        const uint32_t continuation = read_u32(rdram, descriptor + 0x10u);
        if (continuation != full) {
            triangles = decoder.decode(continuation);
        }
    }
    if (triangles.empty()) {
        ++stats.empty_descriptors;
    } else {
        ++stats.decoded_descriptors;
    }
    return triangles;
}

struct PickupPassageCast {
    float fraction = 1.0f;
    Vec3 contact{};
    Vec3 normal{};
    uint32_t sub_shape = 0u;
};

PickupPassageCast cast_pickup_passage_capsule(
    JPH::PhysicsSystem& physics,
    const JPH::CapsuleShape& capsule,
    const Vec3& center,
    const JPH::Vec3& motion
) {
    JPH::ShapeCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
    settings.mReturnDeepestPoint = true;
    const JPH::RShapeCast shape_cast = JPH::RShapeCast::sFromWorldTransform(
        &capsule,
        JPH::Vec3::sOne(),
        JPH::RMat44::sTranslation(to_jolt(center)),
        motion
    );
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    physics.GetNarrowPhaseQuery().CastShape(
        shape_cast,
        settings,
        JPH::RVec3::sZero(),
        collector,
        JPH::SpecifiedBroadPhaseLayerFilter(BroadPhaseLayers::kStatic),
        JPH::SpecifiedObjectLayerFilter(Layers::kStatic)
    );
    PickupPassageCast result{};
    if (!collector.HadHit()) {
        return result;
    }
    result.fraction = collector.mHit.mFraction;
    result.contact = {
        collector.mHit.mContactPointOn2.GetX(),
        collector.mHit.mContactPointOn2.GetY(),
        collector.mHit.mContactPointOn2.GetZ(),
    };
    const JPH::Vec3 normal =
        -collector.mHit.mPenetrationAxis.NormalizedOr(JPH::Vec3::sZero());
    result.normal = {normal.GetX(), normal.GetY(), normal.GetZ()};
    result.sub_shape = collector.mHit.mSubShapeID2.GetValue();
    return result;
}

bool pickup_passage_capsule_overlaps(
    JPH::PhysicsSystem& physics,
    const JPH::CapsuleShape& capsule,
    const Vec3& center,
    PickupPassageCast& hit
) {
    JPH::CollideShapeSettings settings;
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::AnyHitCollisionCollector<JPH::CollideShapeCollector> collector;
    physics.GetNarrowPhaseQuery().CollideShape(
        &capsule,
        JPH::Vec3::sOne(),
        JPH::RMat44::sTranslation(to_jolt(center)),
        settings,
        JPH::RVec3::sZero(),
        collector,
        JPH::SpecifiedBroadPhaseLayerFilter(BroadPhaseLayers::kStatic),
        JPH::SpecifiedObjectLayerFilter(Layers::kStatic)
    );
    if (!collector.HadHit()) {
        return false;
    }
    hit.fraction = 0.0f;
    hit.contact = {
        collector.mHit.mContactPointOn2.GetX(),
        collector.mHit.mContactPointOn2.GetY(),
        collector.mHit.mContactPointOn2.GetZ(),
    };
    const JPH::Vec3 normal = collector.mHit.mPenetrationAxis.NormalizedOr(
        JPH::Vec3::sZero()
    );
    hit.normal = {normal.GetX(), normal.GetY(), normal.GetZ()};
    hit.sub_shape = collector.mHit.mSubShapeID2.GetValue();
    return true;
}

#if 0
struct PickupRouteTraversal {
    bool passed = false;
    bool overlapped = false;
    float minimum_fraction = 1.0f;
    size_t blocking_segment = 0u;
    PickupPassageCast blocking_hit{};
};

PickupRouteTraversal traverse_tracked_pickup_route(
    JPH::PhysicsSystem& physics,
    const TrackedPickupRoute& route,
    float radius,
    float vertical_offset,
    float exterior_extension
) {
    constexpr float kMinimumClearFraction = 0.995f;
    PickupRouteTraversal result{};
    if (route.count < 2u || route.count > route.points.size()) {
        return result;
    }

    const Vec3 start_direction = normalized(subtract(
        route.points[1],
        route.points[0]
    ));
    const Vec3 end_direction = normalized(subtract(
        route.points[route.count - 1u],
        route.points[route.count - 2u]
    ));
    if (distance_squared(start_direction, {}) < 0.5f ||
        distance_squared(end_direction, {}) < 0.5f) {
        return result;
    }

    std::array<Vec3, 7> actor_path{};
    size_t path_count = 0u;
    actor_path[path_count++] = add_scaled(
        route.points[0],
        start_direction,
        -exterior_extension
    );
    for (size_t index = 0u; index < route.count; ++index) {
        actor_path[path_count++] = route.points[index];
    }
    actor_path[path_count++] = add_scaled(
        route.points[route.count - 1u],
        end_direction,
        exterior_extension
    );
    for (size_t index = 0u; index < path_count; ++index) {
        actor_path[index].y += vertical_offset;
    }

    JPH::CapsuleShape capsule(kCapsuleHalfHeight, radius);
    for (size_t point = 0u; point < path_count; ++point) {
        PickupPassageCast overlap{};
        const Vec3 center = actor_to_character_center(
            actor_path[point],
            false
        );
        if (pickup_passage_capsule_overlaps(
                physics,
                capsule,
                center,
                overlap)) {
            result.overlapped = true;
            result.minimum_fraction = 0.0f;
            result.blocking_segment = point;
            result.blocking_hit = overlap;
            return result;
        }
    }

    for (size_t segment = 0u; segment + 1u < path_count; ++segment) {
        const Vec3 start = actor_to_character_center(
            actor_path[segment],
            false
        );
        const Vec3 delta = subtract(
            actor_path[segment + 1u],
            actor_path[segment]
        );
        const PickupPassageCast cast = cast_pickup_passage_capsule(
            physics,
            capsule,
            start,
            JPH::Vec3(delta.x, delta.y, delta.z)
        );
        if (cast.fraction < result.minimum_fraction) {
            result.minimum_fraction = cast.fraction;
            result.blocking_segment = segment;
            result.blocking_hit = cast;
        }
    }
    result.passed = result.minimum_fraction >= kMinimumClearFraction;
    return result;
}

struct PickupRouteBest {
    PickupRouteTraversal traversal{};
    float vertical_offset = 0.0f;
    float exterior_extension = 0.0f;
};

void probe_all_mission_one_pickup_enclosures(JPH::PhysicsSystem& physics) {
    constexpr float kProbeDistance = 768.0f;
    constexpr uint32_t kAxisCount = 36u;
    constexpr std::array<float, 9> kPitchDegrees{
        -60.0f, -45.0f, -30.0f, -15.0f, 0.0f,
        15.0f, 30.0f, 45.0f, 60.0f,
    };
    constexpr std::array<float, 3> kVerticalOffsets{
        -32.0f, 0.0f, 32.0f,
    };
    JPH::CapsuleShape point_capsule(1.0f, 1.0f);
    JPH::CapsuleShape player_capsule(kCapsuleHalfHeight, kCapsuleRadius);

    for (size_t pickup_index = 0u;
         pickup_index < kMissionOneOrdinaryPickupPositions.size();
         ++pickup_index) {
        const Vec3& pickup =
            kMissionOneOrdinaryPickupPositions[pickup_index];
        for (const float vertical_offset : kVerticalOffsets) {
            Vec3 actor_position = pickup;
            actor_position.y += vertical_offset;
            const Vec3 center = actor_to_character_center(
                actor_position,
                false
            );
            PickupPassageCast point_overlap_hit{};
            PickupPassageCast player_overlap_hit{};
            const bool point_overlap = pickup_passage_capsule_overlaps(
                physics,
                point_capsule,
                center,
                point_overlap_hit
            );
            const bool player_overlap = pickup_passage_capsule_overlaps(
                physics,
                player_capsule,
                center,
                player_overlap_hit
            );

            uint32_t clear_directions = 0u;
            float best_paired_fraction = -1.0f;
            float best_yaw = 0.0f;
            float best_pitch = 0.0f;
            PickupPassageCast best_forward{};
            PickupPassageCast best_reverse{};
            for (const float pitch_degrees : kPitchDegrees) {
                const float pitch = JPH::DegreesToRadians(pitch_degrees);
                const float horizontal = std::cos(pitch);
                const float vertical = std::sin(pitch);
                for (uint32_t axis = 0u; axis < kAxisCount; ++axis) {
                    const float yaw_degrees =
                        static_cast<float>(axis) * (180.0f / kAxisCount);
                    const float yaw = JPH::DegreesToRadians(yaw_degrees);
                    const JPH::Vec3 motion(
                        std::sin(yaw) * horizontal * kProbeDistance,
                        vertical * kProbeDistance,
                        std::cos(yaw) * horizontal * kProbeDistance
                    );
                    const PickupPassageCast forward =
                        cast_pickup_passage_capsule(
                            physics,
                            point_capsule,
                            center,
                            motion
                        );
                    const PickupPassageCast reverse =
                        cast_pickup_passage_capsule(
                            physics,
                            point_capsule,
                            center,
                            -motion
                        );
                    clear_directions +=
                        forward.fraction >= 0.999f ? 1u : 0u;
                    clear_directions +=
                        reverse.fraction >= 0.999f ? 1u : 0u;
                    const float paired_fraction = std::min(
                        forward.fraction,
                        reverse.fraction
                    );
                    if (paired_fraction > best_paired_fraction) {
                        best_paired_fraction = paired_fraction;
                        best_yaw = yaw_degrees;
                        best_pitch = pitch_degrees;
                        best_forward = forward;
                        best_reverse = reverse;
                    }
                }
            }
            std::fprintf(
                stderr,
                "BUMBLE_COLLISION stage=ordinary_pickup_enclosure_probe"
                " index=%zu pickup=(%.3f,%.3f,%.3f)"
                " vertical_offset=%.3f point_overlap=%d player_overlap=%d"
                " clear_directions=%" PRIu32 "/%zu"
                " best_paired_fraction=%.6f best_yaw=%.3f best_pitch=%.3f"
                " forward_fraction=%.6f reverse_fraction=%.6f"
                " forward_contact=(%.3f,%.3f,%.3f)"
                " reverse_contact=(%.3f,%.3f,%.3f)"
                " forward_normal=(%.3f,%.3f,%.3f)"
                " reverse_normal=(%.3f,%.3f,%.3f)"
                " forward_sub_shape=%" PRIu32
                " reverse_sub_shape=%" PRIu32 "\n",
                pickup_index,
                static_cast<double>(pickup.x),
                static_cast<double>(pickup.y),
                static_cast<double>(pickup.z),
                static_cast<double>(vertical_offset),
                point_overlap ? 1 : 0,
                player_overlap ? 1 : 0,
                clear_directions,
                static_cast<size_t>(kAxisCount) * 2u * kPitchDegrees.size(),
                static_cast<double>(best_paired_fraction),
                static_cast<double>(best_yaw),
                static_cast<double>(best_pitch),
                static_cast<double>(best_forward.fraction),
                static_cast<double>(best_reverse.fraction),
                static_cast<double>(best_forward.contact.x),
                static_cast<double>(best_forward.contact.y),
                static_cast<double>(best_forward.contact.z),
                static_cast<double>(best_reverse.contact.x),
                static_cast<double>(best_reverse.contact.y),
                static_cast<double>(best_reverse.contact.z),
                static_cast<double>(best_forward.normal.x),
                static_cast<double>(best_forward.normal.y),
                static_cast<double>(best_forward.normal.z),
                static_cast<double>(best_reverse.normal.x),
                static_cast<double>(best_reverse.normal.y),
                static_cast<double>(best_reverse.normal.z),
                best_forward.sub_shape,
                best_reverse.sub_shape
            );
        }
    }
}

void probe_candidate_hollow_clearance(JPH::PhysicsSystem& physics) {
    constexpr std::array<size_t, 5> kCandidatePickupIndices{
        10u, 25u, 27u, 32u, 26u,
    };
    constexpr std::array<float, 5> kRadii{
        24.0f, 22.0f, 20.0f, 18.0f, 16.0f,
    };
    constexpr std::array<float, 4> kHalfHeights{
        12.0f, 8.0f, 4.0f, 1.0f,
    };
    constexpr float kProbeDistance = 384.0f;
    constexpr uint32_t kYawSteps = 36u;

    for (const size_t pickup_index : kCandidatePickupIndices) {
        const Vec3& pickup =
            kMissionOneOrdinaryPickupPositions[pickup_index];
        for (const float radius : kRadii) {
            for (const float half_height : kHalfHeights) {
                JPH::CapsuleShape capsule(half_height, radius);
                bool found_nonoverlap = false;
                float best_pair = -1.0f;
                float best_offset = 0.0f;
                float best_yaw = 0.0f;
                PickupPassageCast best_forward{};
                PickupPassageCast best_reverse{};
                for (int32_t offset_step = -12; offset_step <= 12;
                     ++offset_step) {
                    const float vertical_offset =
                        static_cast<float>(offset_step) * 4.0f;
                    Vec3 actor_position = pickup;
                    actor_position.y += vertical_offset;
                    const Vec3 center = actor_to_character_center(
                        actor_position,
                        false
                    );
                    PickupPassageCast overlap{};
                    if (pickup_passage_capsule_overlaps(
                            physics,
                            capsule,
                            center,
                            overlap)) {
                        continue;
                    }
                    found_nonoverlap = true;
                    for (uint32_t yaw_step = 0u; yaw_step < kYawSteps;
                         ++yaw_step) {
                        const float yaw_degrees =
                            static_cast<float>(yaw_step) *
                            (180.0f / kYawSteps);
                        const float yaw = JPH::DegreesToRadians(yaw_degrees);
                        const JPH::Vec3 motion(
                            std::sin(yaw) * kProbeDistance,
                            0.0f,
                            std::cos(yaw) * kProbeDistance
                        );
                        const PickupPassageCast forward =
                            cast_pickup_passage_capsule(
                                physics,
                                capsule,
                                center,
                                motion
                            );
                        const PickupPassageCast reverse =
                            cast_pickup_passage_capsule(
                                physics,
                                capsule,
                                center,
                                -motion
                            );
                        const float pair = std::min(
                            forward.fraction,
                            reverse.fraction
                        );
                        if (pair > best_pair) {
                            best_pair = pair;
                            best_offset = vertical_offset;
                            best_yaw = yaw_degrees;
                            best_forward = forward;
                            best_reverse = reverse;
                        }
                    }
                }
                std::fprintf(
                    stderr,
                    "BUMBLE_COLLISION stage=candidate_hollow_clearance_probe"
                    " pickup_index=%zu pickup=(%.3f,%.3f,%.3f)"
                    " radius=%.3f half_height=%.3f"
                    " nonoverlap=%d pass=%d best_pair=%.6f"
                    " vertical_offset=%.3f yaw=%.3f"
                    " forward_fraction=%.6f reverse_fraction=%.6f"
                    " forward_contact=(%.3f,%.3f,%.3f)"
                    " reverse_contact=(%.3f,%.3f,%.3f)"
                    " forward_sub_shape=%" PRIu32
                    " reverse_sub_shape=%" PRIu32 "\n",
                    pickup_index,
                    static_cast<double>(pickup.x),
                    static_cast<double>(pickup.y),
                    static_cast<double>(pickup.z),
                    static_cast<double>(radius),
                    static_cast<double>(half_height),
                    found_nonoverlap ? 1 : 0,
                    best_pair >= 0.995f ? 1 : 0,
                    static_cast<double>(best_pair),
                    static_cast<double>(best_offset),
                    static_cast<double>(best_yaw),
                    static_cast<double>(best_forward.fraction),
                    static_cast<double>(best_reverse.fraction),
                    static_cast<double>(best_forward.contact.x),
                    static_cast<double>(best_forward.contact.y),
                    static_cast<double>(best_forward.contact.z),
                    static_cast<double>(best_reverse.contact.x),
                    static_cast<double>(best_reverse.contact.y),
                    static_cast<double>(best_reverse.contact.z),
                    best_forward.sub_shape,
                    best_reverse.sub_shape
                );
            }
        }
    }
}

bool pickup_passage_validation_requested() {
    const char* value = std::getenv(
        "BUMBLE_COLLISION_VALIDATE_PICKUP_PASSAGES"
    );
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

void validate_tracked_pickup_passages(JPH::PhysicsSystem& physics) {
    probe_all_mission_one_pickup_enclosures(physics);
    probe_candidate_hollow_clearance(physics);
    constexpr std::array<float, 4> kDiagnosticRadii{
        kCapsuleRadius, 20.0f, 16.0f, 12.0f,
    };
    constexpr std::array<float, 13> kDiagnosticVerticalOffsets{
        -48.0f, -40.0f, -32.0f, -24.0f, -16.0f, -8.0f, 0.0f,
        8.0f, 16.0f, 24.0f, 32.0f, 40.0f, 48.0f,
    };
    constexpr std::array<float, 6> kDiagnosticExteriorExtensions{
        96.0f, 128.0f, 160.0f, 192.0f, 224.0f, 256.0f,
    };

    for (size_t route_index = 0u;
         route_index < kMissionOneTrackedPickupRoutes.size();
         ++route_index) {
        const TrackedPickupRoute& route =
            kMissionOneTrackedPickupRoutes[route_index];
        for (const float radius : kDiagnosticRadii) {
            PickupRouteBest best{};
            best.traversal.minimum_fraction = -1.0f;
            for (const float vertical_offset : kDiagnosticVerticalOffsets) {
                for (const float exterior_extension :
                     kDiagnosticExteriorExtensions) {
                    const PickupRouteTraversal traversal =
                        traverse_tracked_pickup_route(
                            physics,
                            route,
                            radius,
                            vertical_offset,
                            exterior_extension
                        );
                    if (traversal.minimum_fraction >
                        best.traversal.minimum_fraction) {
                        best = {
                            traversal,
                            vertical_offset,
                            exterior_extension,
                        };
                    }
                    if (radius == kCapsuleRadius &&
                        vertical_offset == 0.0f) {
                        std::fprintf(
                            stderr,
                            "BUMBLE_COLLISION stage=tracked_pickup_route_probe"
                            " route=%zu name=%s radius=%.3f"
                            " vertical_offset=%.3f extension=%.3f"
                            " pass=%d overlap=%d minimum_fraction=%.6f"
                            " blocking_segment=%zu"
                            " contact=(%.3f,%.3f,%.3f)"
                            " normal=(%.3f,%.3f,%.3f)"
                            " sub_shape=%" PRIu32 "\n",
                            route_index,
                            route.name,
                            static_cast<double>(radius),
                            static_cast<double>(vertical_offset),
                            static_cast<double>(exterior_extension),
                            traversal.passed ? 1 : 0,
                            traversal.overlapped ? 1 : 0,
                            static_cast<double>(
                                traversal.minimum_fraction
                            ),
                            traversal.blocking_segment,
                            static_cast<double>(
                                traversal.blocking_hit.contact.x
                            ),
                            static_cast<double>(
                                traversal.blocking_hit.contact.y
                            ),
                            static_cast<double>(
                                traversal.blocking_hit.contact.z
                            ),
                            static_cast<double>(
                                traversal.blocking_hit.normal.x
                            ),
                            static_cast<double>(
                                traversal.blocking_hit.normal.y
                            ),
                            static_cast<double>(
                                traversal.blocking_hit.normal.z
                            ),
                            traversal.blocking_hit.sub_shape
                        );
                    }
                }
            }
            std::fprintf(
                stderr,
                "BUMBLE_COLLISION stage=tracked_pickup_route_best"
                " route=%zu name=%s radius=%.3f pass=%d overlap=%d"
                " minimum_fraction=%.6f vertical_offset=%.3f"
                " extension=%.3f blocking_segment=%zu"
                " contact=(%.3f,%.3f,%.3f) normal=(%.3f,%.3f,%.3f)"
                " sub_shape=%" PRIu32 "\n",
                route_index,
                route.name,
                static_cast<double>(radius),
                best.traversal.passed ? 1 : 0,
                best.traversal.overlapped ? 1 : 0,
                static_cast<double>(best.traversal.minimum_fraction),
                static_cast<double>(best.vertical_offset),
                static_cast<double>(best.exterior_extension),
                best.traversal.blocking_segment,
                static_cast<double>(best.traversal.blocking_hit.contact.x),
                static_cast<double>(best.traversal.blocking_hit.contact.y),
                static_cast<double>(best.traversal.blocking_hit.contact.z),
                static_cast<double>(best.traversal.blocking_hit.normal.x),
                static_cast<double>(best.traversal.blocking_hit.normal.y),
                static_cast<double>(best.traversal.blocking_hit.normal.z),
                best.traversal.blocking_hit.sub_shape
            );
        }
    }
    std::fprintf(
        stderr,
        "BUMBLE_COLLISION stage=tracked_pickup_route_summary"
        " routes=%zu source=game_owned_mission1_ordinary_pickup_chains\n",
        kMissionOneTrackedPickupRoutes.size()
    );
    std::fflush(stderr);
}
#endif

bool pickup_passage_validation_requested() {
    const char* value = std::getenv(
        "BUMBLE_COLLISION_VALIDATE_PICKUP_PASSAGES"
    );
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

struct HollowPassageTraversal {
    bool passed = false;
    bool overlapped = false;
    float minimum_fraction = 1.0f;
    std::array<float, 6> fractions{};
    PickupPassageCast blocking_hit{};
};

HollowPassageTraversal traverse_tracked_hollow_passage(
    JPH::PhysicsSystem& physics,
    const TrackedHollowPassage& passage
) {
    constexpr float kHalfRouteDistance = 384.0f;
    constexpr float kMinimumClearFraction = 0.995f;
    const float yaw = JPH::DegreesToRadians(passage.yaw_degrees);
    const Vec3 direction{std::sin(yaw), 0.0f, std::cos(yaw)};
    Vec3 middle_actor = passage.pickup;
    middle_actor.y += passage.actor_vertical_offset;
    const Vec3 start_actor = add_scaled(
        middle_actor,
        direction,
        -kHalfRouteDistance
    );
    const Vec3 end_actor = add_scaled(
        middle_actor,
        direction,
        kHalfRouteDistance
    );
    const Vec3 start = actor_to_character_center(start_actor, false);
    const Vec3 middle = actor_to_character_center(middle_actor, false);
    const Vec3 end = actor_to_character_center(end_actor, false);
    JPH::CapsuleShape capsule(kCapsuleHalfHeight, kCapsuleRadius);

    HollowPassageTraversal result{};
    for (const Vec3& point : {start, middle, end}) {
        PickupPassageCast overlap{};
        if (pickup_passage_capsule_overlaps(
                physics,
                capsule,
                point,
                overlap)) {
            result.overlapped = true;
            result.minimum_fraction = 0.0f;
            result.blocking_hit = overlap;
            return result;
        }
    }

    const JPH::Vec3 half_motion(
        direction.x * kHalfRouteDistance,
        0.0f,
        direction.z * kHalfRouteDistance
    );
    const JPH::Vec3 full_motion = half_motion * 2.0f;
    const std::array<PickupPassageCast, 6> casts{{
        cast_pickup_passage_capsule(physics, capsule, start, half_motion),
        cast_pickup_passage_capsule(physics, capsule, middle, half_motion),
        cast_pickup_passage_capsule(physics, capsule, end, -half_motion),
        cast_pickup_passage_capsule(physics, capsule, middle, -half_motion),
        cast_pickup_passage_capsule(physics, capsule, start, full_motion),
        cast_pickup_passage_capsule(physics, capsule, end, -full_motion),
    }};
    for (size_t index = 0u; index < casts.size(); ++index) {
        result.fractions[index] = casts[index].fraction;
        if (casts[index].fraction < result.minimum_fraction) {
            result.minimum_fraction = casts[index].fraction;
            result.blocking_hit = casts[index];
        }
    }
    result.passed = result.minimum_fraction >= kMinimumClearFraction;
    return result;
}

void validate_tracked_hollow_passages(JPH::PhysicsSystem& physics) {
    uint32_t passed = 0u;
    for (size_t index = 0u;
         index < kMissionOneTrackedHollowPassages.size();
         ++index) {
        const TrackedHollowPassage& passage =
            kMissionOneTrackedHollowPassages[index];
        const HollowPassageTraversal traversal =
            traverse_tracked_hollow_passage(physics, passage);
        passed += traversal.passed ? 1u : 0u;
        std::fprintf(
            stderr,
            "BUMBLE_COLLISION stage=tracked_hollow_pickup_passage"
            " index=%zu name=%s pickup=(%.3f,%.3f,%.3f)"
            " actor_vertical_offset=%.3f yaw=%.3f pass=%d overlap=%d"
            " minimum_fraction=%.6f"
            " start_to_middle=%.6f middle_to_end=%.6f"
            " end_to_middle=%.6f middle_to_start=%.6f"
            " start_to_end=%.6f end_to_start=%.6f"
            " outside_to_outside=1 route_distance=768.000"
            " capsule_radius=%.3f capsule_half_height=%.3f"
            " capsule_diameter=%.3f capsule_total_height=%.3f"
            " blocking_contact=(%.3f,%.3f,%.3f)"
            " blocking_normal=(%.3f,%.3f,%.3f)\n",
            index,
            passage.name,
            static_cast<double>(passage.pickup.x),
            static_cast<double>(passage.pickup.y),
            static_cast<double>(passage.pickup.z),
            static_cast<double>(passage.actor_vertical_offset),
            static_cast<double>(passage.yaw_degrees),
            traversal.passed ? 1 : 0,
            traversal.overlapped ? 1 : 0,
            static_cast<double>(traversal.minimum_fraction),
            static_cast<double>(traversal.fractions[0]),
            static_cast<double>(traversal.fractions[1]),
            static_cast<double>(traversal.fractions[2]),
            static_cast<double>(traversal.fractions[3]),
            static_cast<double>(traversal.fractions[4]),
            static_cast<double>(traversal.fractions[5]),
            static_cast<double>(kCapsuleRadius),
            static_cast<double>(kCapsuleHalfHeight),
            static_cast<double>(kCapsuleRadius * 2.0f),
            static_cast<double>(
                (kCapsuleRadius + kCapsuleHalfHeight) * 2.0f
            ),
            static_cast<double>(traversal.blocking_hit.contact.x),
            static_cast<double>(traversal.blocking_hit.contact.y),
            static_cast<double>(traversal.blocking_hit.contact.z),
            static_cast<double>(traversal.blocking_hit.normal.x),
            static_cast<double>(traversal.blocking_hit.normal.y),
            static_cast<double>(traversal.blocking_hit.normal.z)
        );
    }
    std::fprintf(
        stderr,
        "BUMBLE_COLLISION stage=tracked_hollow_pickup_passage_summary"
        " passed=%" PRIu32 " expected=%zu pass=%d"
        " source=game_owned_mission1_ordinary_pickup_transforms"
        " bidirectional_outside_to_outside=1\n",
        passed,
        kMissionOneTrackedHollowPassages.size(),
        passed == kMissionOneTrackedHollowPassages.size() ? 1 : 0
    );
    std::fflush(stderr);
}

class CollisionWorld final {
public:
    CollisionWorld() = default;

    ~CollisionWorld() {
        clear_scene();
        physics_.reset();
        allocator_.reset();
        if (jolt_registered_) {
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }
    }

    bool step_airborne(
        uint8_t* rdram,
        recomp_context* context,
        uint32_t actor,
        const bumble::modern_controls::SpatialIntent& intent
    ) {
        if (!ensure_world(rdram, context)) {
            return false;
        }
        contact_listener_.bind(rdram, context, terrain_body_);
        if (!prepare_character(rdram, actor, false)) {
            return false;
        }
        bool ceiling_contact = enforce_player_ceiling(false);
        const Vec3 position_before = character_center_to_actor(
            from_jolt(character_->GetPosition()),
            false
        );
        const Vec3 requested_delta{
            intent.delta_x,
            intent.delta_y,
            intent.delta_z,
        };
        Vec3 solver_delta = requested_delta;
        ceiling_contact = limit_upward_delta_at_ceiling(solver_delta) ||
            ceiling_contact;

        JPH::CharacterVirtual::ExtendedUpdateSettings settings;
        settings.mStickToFloorStepDown = JPH::Vec3::sZero();
        settings.mWalkStairsStepUp = JPH::Vec3::sZero();
        character_->SetLinearVelocity(JPH::Vec3(
            solver_delta.x,
            solver_delta.y,
            solver_delta.z
        ));
        character_->ExtendedUpdate(
            1.0f,
            JPH::Vec3::sZero(),
            settings,
            physics_->GetDefaultBroadPhaseLayerFilter(Layers::kMoving),
            physics_->GetDefaultLayerFilter(Layers::kMoving),
            {},
            {},
            *allocator_
        );
        ceiling_contact = enforce_player_ceiling(false) || ceiling_contact;
        bool landed = false;
        if (intent.landing_requested) {
            landed = character_->IsSupported();
            if (!landed) {
                landed = character_->StickToFloor(
                    JPH::Vec3(0.0f, -kLandingProbeDistance, 0.0f),
                    physics_->GetDefaultBroadPhaseLayerFilter(Layers::kMoving),
                    physics_->GetDefaultLayerFilter(Layers::kMoving),
                    {},
                    {},
                    *allocator_
                );
            }
            bumble::modern_controls::finish_spatial_landing_attempt(
                actor,
                landed
            );
        }

        publish_character(
            rdram,
            actor,
            intent.speed,
            false,
            intent.landing_requested ? kLandingProbeDistance : 0.0f
        );
        if (intent.barrel_roll_active || intent.loop_de_loop_active) {
            const Vec3 maneuver_position_after = character_center_to_actor(
                from_jolt(character_->GetPosition()),
                false
            );
            const Vec3 maneuver_resolved_delta = subtract(
                maneuver_position_after,
                position_before
            );
            const float maneuver_requested_distance = vector_length(
                requested_delta
            );
            const float maneuver_resolved_distance = vector_length(
                maneuver_resolved_delta
            );
            std::fprintf(
                stderr,
                "BUMBLE_COLLISION stage=modern_maneuver_step"
                " kind=%s actor=0x%08" PRIX32
                " requested_delta=(%.3f,%.3f,%.3f)"
                " resolved_delta=(%.3f,%.3f,%.3f)"
                " requested_distance=%.3f resolved_distance=%.3f"
                " profile_step=%" PRIu32 "/%" PRIu32
                " profile_progress=%.6f curve=%s"
                " collision_limited=%d active_contacts=%zu\n",
                intent.barrel_roll_active
                    ? "barrel_roll"
                    : "loop_de_loop",
                actor,
                static_cast<double>(requested_delta.x),
                static_cast<double>(requested_delta.y),
                static_cast<double>(requested_delta.z),
                static_cast<double>(maneuver_resolved_delta.x),
                static_cast<double>(maneuver_resolved_delta.y),
                static_cast<double>(maneuver_resolved_delta.z),
                static_cast<double>(maneuver_requested_distance),
                static_cast<double>(maneuver_resolved_distance),
                intent.maneuver_step,
                intent.maneuver_steps_total,
                static_cast<double>(intent.maneuver_progress),
                intent.loop_de_loop_active
                    ? "half_cosine_path"
                    : "half_sine_velocity",
                maneuver_resolved_distance + 0.01f <
                    maneuver_requested_distance ? 1 : 0,
                character_->GetActiveContacts().size()
            );
            std::fflush(stderr);
        }
        if (landed) {
            write_u32(rdram, actor + 0x8Cu, kGroundedEntryState);
            write_f32(rdram, actor + 0x5Cu, 0.0f);
        }
        owned_update_actor_ = actor;
        owned_update_state_ = read_u32(rdram, actor + 0x8Cu);
        log_step(
            actor,
            false,
            intent.speed > 0.001f,
            landed,
            false,
            requested_delta,
            position_before,
            ceiling_contact
        );
        (void)context;
        return true;
    }

    bool step_grounded(
        uint8_t* rdram,
        recomp_context* context,
        uint32_t actor,
        const bumble::modern_controls::SpatialIntent& intent
    ) {
        if (!ensure_world(rdram, context)) {
            return false;
        }
        contact_listener_.bind(rdram, context, terrain_body_);
        if (!prepare_character(rdram, actor, true)) {
            return false;
        }
        bool ceiling_contact = enforce_player_ceiling(true);
        const Vec3 position_before = character_center_to_actor(
            from_jolt(character_->GetPosition()),
            true
        );

        JPH::CharacterVirtual::ExtendedUpdateSettings settings;
        settings.mStickToFloorStepDown = JPH::Vec3(
            0.0f,
            -kGroundStickDistance,
            0.0f
        );
        settings.mWalkStairsStepUp = JPH::Vec3(0.0f, kGroundStepHeight, 0.0f);
        settings.mWalkStairsStepDownExtra = JPH::Vec3(
            0.0f,
            -kGroundStickDistance,
            0.0f
        );
        character_->SetLinearVelocity(JPH::Vec3(
            intent.delta_x,
            0.0f,
            intent.delta_z
        ));
        character_->ExtendedUpdate(
            1.0f,
            JPH::Vec3(0.0f, -1.0f, 0.0f),
            settings,
            physics_->GetDefaultBroadPhaseLayerFilter(Layers::kMoving),
            physics_->GetDefaultLayerFilter(Layers::kMoving),
            {},
            {},
            *allocator_
        );
        ceiling_contact = enforce_player_ceiling(true) || ceiling_contact;
        bool supported = character_->IsSupported();
        publish_character(
            rdram,
            actor,
            intent.speed,
            true,
            kGroundStickDistance
        );
        const bool automatic_takeoff = !supported && !intent.takeoff_requested;
        if (automatic_takeoff) {
            write_u32(rdram, actor + 0x8Cu, kFlyingState);
        }
        owned_update_actor_ = actor;
        owned_update_state_ = read_u32(rdram, actor + 0x8Cu);
        log_step(
            actor,
            true,
            intent.speed > 0.001f,
            false,
            automatic_takeoff,
            {intent.delta_x, 0.0f, intent.delta_z},
            position_before,
            ceiling_contact
        );
        return true;
    }

    bool owns_spatial_continuation(uint32_t actor) {
        const bool owns = actor != 0u && owned_update_actor_ == actor;
        if (owns) {
            const uint64_t count = ++legacy_spatial_bypasses_;
            if (collision_runtime_telemetry_enabled() &&
                (count <= 8u || count % 600u == 0u)) {
                std::fprintf(
                    stderr,
                    "BUMBLE_COLLISION stage=legacy_spatial_bypassed"
                    " count=%" PRIu64 " actor=0x%08" PRIX32 "\n",
                    count,
                    actor
                );
                std::fflush(stderr);
            }
        }
        return owns;
    }

    void relinquish_spatial_update(uint32_t actor) {
        if (actor != 0u && owned_update_actor_ == actor) {
            owned_update_actor_ = 0u;
            owned_update_state_ = 0u;
        }
    }

    void finish_spatial_update(uint8_t* rdram, uint32_t actor) {
        if (rdram == nullptr || actor == 0u || owned_update_actor_ != actor) {
            return;
        }

        const uint32_t current_state = read_u32(rdram, actor + 0x8Cu);
        const Vec3 actor_position = read_actor_position(rdram, actor);
        if (current_state == owned_update_state_) {
            if (finite_vector(actor_position) && published_position_valid_ &&
                distance_squared(actor_position, published_position_) >
                    kExternalPositionEpsilon * kExternalPositionEpsilon) {
                write_actor_position(rdram, actor, published_position_);
                const uint64_t count = ++legacy_tail_discards_;
                if (collision_runtime_telemetry_enabled() &&
                    (count <= 8u || count % 600u == 0u)) {
                    std::fprintf(
                        stderr,
                        "BUMBLE_COLLISION stage=legacy_tail_discarded"
                        " count=%" PRIu64 " actor=0x%08" PRIX32
                        " legacy_position=(%.3f,%.3f,%.3f)"
                        " resolved_position=(%.3f,%.3f,%.3f)\n",
                        count,
                        actor,
                        static_cast<double>(actor_position.x),
                        static_cast<double>(actor_position.y),
                        static_cast<double>(actor_position.z),
                        static_cast<double>(published_position_.x),
                        static_cast<double>(published_position_.y),
                        static_cast<double>(published_position_.z)
                    );
                    std::fflush(stderr);
                }
            }
        } else if (finite_vector(actor_position) && character_ != nullptr &&
                   character_actor_ == actor) {
            const bool grounded_pose = state_uses_grounded_pose(current_state);
            const bool authored_pose_changed = !published_position_valid_ ||
                distance_squared(actor_position, published_position_) >
                    kExternalPositionEpsilon * kExternalPositionEpsilon;
            if (authored_pose_changed) {
                character_->SetPosition(to_jolt(
                    actor_to_character_center(actor_position, grounded_pose)
                ));
            }
            character_->SetLinearVelocity(JPH::Vec3::sZero());
            published_position_ = actor_position;
            published_position_valid_ = true;
            refresh_character_contacts();
            const uint64_t count = ++state_transition_rebases_;
            if (collision_runtime_telemetry_enabled() &&
                (count <= 8u || count % 120u == 0u)) {
                std::fprintf(
                    stderr,
                    "BUMBLE_COLLISION stage=state_transition_rebased"
                    " count=%" PRIu64 " actor=0x%08" PRIX32
                    " previous_state=%" PRIu32 " state=%" PRIu32
                    " position=(%.3f,%.3f,%.3f)"
                    " grounded_pose=%d physics_rebased=%d\n",
                    count,
                    actor,
                    owned_update_state_,
                    current_state,
                    static_cast<double>(actor_position.x),
                    static_cast<double>(actor_position.y),
                    static_cast<double>(actor_position.z),
                    grounded_pose ? 1 : 0,
                    authored_pose_changed ? 1 : 0
                );
                std::fflush(stderr);
            }
        }
        owned_update_actor_ = 0u;
        owned_update_state_ = 0u;
    }

    bool query_static_sphere(
        uint8_t* rdram,
        recomp_context* context,
        const Vec3& center,
        float radius,
        BumbleStaticCollisionHit& hit
    ) {
        if (!finite_vector(center) || !std::isfinite(radius) ||
            radius <= 0.0f || radius > kMaximumWorldCoordinate ||
            !ensure_query_world(rdram, context)) {
            return false;
        }

        const JPH::SphereShape sphere(radius);
        JPH::CollideShapeSettings settings;
        settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
        // CollideShape orders by penetration, not distance; select the nearest surface point.
        class NearestSurfaceCollector final
            : public JPH::CollideShapeCollector {
        public:
            explicit NearestSurfaceCollector(const Vec3& center)
                : center_(center) {
            }

            void AddHit(
                const JPH::CollideShapeResult& candidate
            ) override {
                const float dx =
                    candidate.mContactPointOn2.GetX() - center_.x;
                const float dy =
                    candidate.mContactPointOn2.GetY() - center_.y;
                const float dz =
                    candidate.mContactPointOn2.GetZ() - center_.z;
                const float distance_squared = dx * dx + dy * dy + dz * dz;
                if (!had_hit_ || distance_squared < best_distance_squared_) {
                    hit_ = candidate;
                    best_distance_squared_ = distance_squared;
                    had_hit_ = true;
                }
            }

            bool had_hit() const {
                return had_hit_;
            }

            const JPH::CollideShapeResult& hit() const {
                return hit_;
            }

        private:
            Vec3 center_{};
            JPH::CollideShapeResult hit_{};
            float best_distance_squared_ =
                std::numeric_limits<float>::infinity();
            bool had_hit_ = false;
        } collector(center);
        physics_->GetNarrowPhaseQuery().CollideShape(
            &sphere,
            JPH::Vec3::sOne(),
            JPH::RMat44::sTranslation(to_jolt(center)),
            settings,
            JPH::RVec3::sZero(),
            collector,
            JPH::SpecifiedBroadPhaseLayerFilter(BroadPhaseLayers::kStatic),
            JPH::SpecifiedObjectLayerFilter(Layers::kStatic)
        );
        if (!collector.had_hit()) {
            return false;
        }

        const JPH::CollideShapeResult& nearest = collector.hit();
        const JPH::Vec3 normal =
            -nearest.mPenetrationAxis.NormalizedOr(
                JPH::Vec3::sZero()
            );
        hit = {
            0.0f,
            nearest.mContactPointOn2.GetX(),
            nearest.mContactPointOn2.GetY(),
            nearest.mContactPointOn2.GetZ(),
            normal.GetX(),
            normal.GetY(),
            normal.GetZ(),
        };
        return true;
    }

    bool sweep_static_sphere(
        uint8_t* rdram,
        recomp_context* context,
        const Vec3& start,
        const Vec3& end,
        float radius,
        BumbleStaticCollisionHit& hit
    ) {
        if (!finite_vector(start) || !finite_vector(end) ||
            !std::isfinite(radius) || radius <= 0.0f ||
            radius > kMaximumWorldCoordinate ||
            !ensure_query_world(rdram, context)) {
            return false;
        }

        const Vec3 delta{
            end.x - start.x,
            end.y - start.y,
            end.z - start.z,
        };
        if (distance_squared(delta, {}) <=
            std::numeric_limits<float>::epsilon()) {
            return query_static_sphere(
                rdram,
                context,
                end,
                radius,
                hit
            );
        }

        const JPH::SphereShape sphere(radius);
        JPH::ShapeCastSettings settings;
        settings.mBackFaceModeTriangles =
            JPH::EBackFaceMode::CollideWithBackFaces;
        settings.mReturnDeepestPoint = true;
        const JPH::RShapeCast shape_cast =
            JPH::RShapeCast::sFromWorldTransform(
                &sphere,
                JPH::Vec3::sOne(),
                JPH::RMat44::sTranslation(to_jolt(start)),
                JPH::Vec3(delta.x, delta.y, delta.z)
            );
        // Reject visual-only faces before tightening the early-out fraction.
        struct ValidatedCollector final : JPH::CastShapeCollector {
            uint8_t* rdram;
            recomp_context* context;
            Vec3 delta;
            BumbleStaticCollisionHit& hit;
            bool accepted = false;

            ValidatedCollector(uint8_t* memory, recomp_context* guest,
                const Vec3& direction, BumbleStaticCollisionHit& result)
                : rdram(memory), context(guest), delta(direction), hit(result) {}

            void AddHit(const JPH::ShapeCastResult& candidate) override {
                if (candidate.GetEarlyOutFraction() >= GetEarlyOutFraction()) {
                    return;
                }
                JPH::Vec3 normal =
                    -candidate.mPenetrationAxis.NormalizedOr(
                        JPH::Vec3::sZero()
                    );
                if (normal.LengthSq() < 0.5f) {
                    normal = -JPH::Vec3(delta.x, delta.y, delta.z)
                        .NormalizedOr(JPH::Vec3::sZero());
                }
                const Vec3 contact{
                    candidate.mContactPointOn2.GetX(),
                    candidate.mContactPointOn2.GetY(),
                    candidate.mContactPointOn2.GetZ(),
                };
                constexpr float kAuthoredSolidSideProbeDistance = 2.0f;
                const Vec3 solid_side{
                    contact.x - normal.GetX() *
                        kAuthoredSolidSideProbeDistance,
                    contact.y - normal.GetY() *
                        kAuthoredSolidSideProbeDistance,
                    contact.z - normal.GetZ() *
                        kAuthoredSolidSideProbeDistance,
                };
                if (!authored_world_is_solid(
                        rdram,
                        context,
                        solid_side,
                        false)) {
                    return;
                }
                hit = {
                    candidate.mFraction,
                    contact.x,
                    contact.y,
                    contact.z,
                    normal.GetX(),
                    normal.GetY(),
                    normal.GetZ(),
                };
                accepted = true;
                UpdateEarlyOutFraction(candidate.GetEarlyOutFraction());
            }
        } collector(rdram, context, delta, hit);
        physics_->GetNarrowPhaseQuery().CastShape(
            shape_cast,
            settings,
            JPH::RVec3::sZero(),
            collector,
            JPH::SpecifiedBroadPhaseLayerFilter(BroadPhaseLayers::kStatic),
            JPH::SpecifiedObjectLayerFilter(Layers::kStatic)
        );
        return collector.accepted;
    }

    bool query_world_ready(uint8_t* rdram) const {
        return rdram != nullptr && physics_ != nullptr && world_ready_ &&
            read_u32(rdram, kCurrentLevelIndex) == world_level_;
    }

    bool prepare_world(uint8_t* rdram, recomp_context* context) {
        const uint32_t level = read_u32(rdram, kCurrentLevelIndex);
        return (world_ready_ && level == world_level_) ||
            ensure_world(rdram, context);
    }

private:
    struct DynamicGateLayer {
        int32_t column = 0;
        int32_t row = 0;
        uint16_t descriptor = std::numeric_limits<uint16_t>::max();
        uint8_t transform = std::numeric_limits<uint8_t>::max();
        JPH::BodyID body{};
#if defined(BUMBLE_COLLISION_DEBUG_RT64) && BUMBLE_COLLISION_DEBUG_RT64
        std::vector<RT64::BumbleCollisionDebugVertex> debug_vertices{};
#endif
    };

    bool ensure_query_world(
        uint8_t* rdram,
        recomp_context* context
    ) {
        if (rdram != nullptr && physics_ != nullptr && world_ready_ &&
            valid_guest_address(kCurrentLevelIndex, sizeof(uint32_t)) &&
            read_u32(rdram, kCurrentLevelIndex) == world_level_) {
            return true;
        }
        return ensure_world(rdram, context);
    }

    bool initialize() {
        if (physics_ != nullptr) {
            return true;
        }
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        jolt_registered_ = true;
        allocator_ = std::make_unique<JPH::TempAllocatorImpl>(16u * 1024u * 1024u);
        physics_ = std::make_unique<JPH::PhysicsSystem>();
        physics_->Init(
            4096u,
            0u,
            8192u,
            2048u,
            broad_phase_interface_,
            object_vs_broad_phase_filter_,
            object_pair_filter_
        );
        return true;
    }

    uint64_t compute_world_hash(uint8_t* rdram) const {
        if (rdram == nullptr ||
            !valid_guest_address(kTerrainCellTable, 64u * 64u * 8u)) {
            return 0u;
        }
        uint64_t hash = 1469598103934665603ull;
        hash = fnv_mix(hash, read_u32(rdram, kCurrentLevelIndex));
        for (uint32_t index = 0u; index < 64u * 64u; ++index) {
            const uint32_t cell = kTerrainCellTable + index * 8u;
            hash = fnv_mix(hash, read_u8(rdram, cell + 6u));
            for (uint32_t layer = 0u; layer < 3u; ++layer) {
                const uint16_t id = read_u16(rdram, cell + layer * 2u);
                hash = fnv_mix(hash, id);
                const uint64_t descriptor64 =
                    static_cast<uint64_t>(kTerrainDescriptorBase) +
                    static_cast<uint64_t>(id) * kTerrainDescriptorStride;
                if (id == 0u ||
                    descriptor64 + kTerrainDescriptorStride >
                        kTerrainDescriptorEnd) {
                    hash = fnv_mix(hash, 0u);
                    continue;
                }
                const uint32_t descriptor =
                    static_cast<uint32_t>(descriptor64);
                hash = fnv_mix(
                    hash,
                    read_u32(rdram, descriptor + 4u) != 0u ? 1u : 0u
                );
            }
        }
        return hash;
    }

    void publish_collision_debug_scene() {
#if defined(BUMBLE_COLLISION_DEBUG_RT64) && BUMBLE_COLLISION_DEBUG_RT64
        RT64::publishBumbleCollisionDebugMesh(
            world_level_,
            collision_debug_static_vertices_,
            collision_debug_liquid_cells_
        );
        std::vector<RT64::BumbleCollisionDebugVertex> gates;
        for (const DynamicGateLayer& gate : dynamic_gate_layers_) {
            gates.insert(
                gates.end(),
                gate.debug_vertices.begin(),
                gate.debug_vertices.end()
            );
        }
        RT64::publishBumbleCollisionDebugGateMesh(
            world_level_,
            std::move(gates)
        );
#endif
    }

    bool is_dynamic_gate_cell(int32_t column, int32_t row) const {
        return std::any_of(
            dynamic_gate_layers_.begin(),
            dynamic_gate_layers_.end(),
            [column, row](const DynamicGateLayer& gate) {
                return gate.column == column && gate.row == row;
            }
        );
    }

    void remove_dynamic_gate_body(DynamicGateLayer& gate) {
        if (gate.body.IsInvalid()) {
            return;
        }
        JPH::BodyInterface& bodies = physics_->GetBodyInterface();
        bodies.RemoveBody(gate.body);
        bodies.DestroyBody(gate.body);
        gate.body = {};
    }

    bool refresh_dynamic_gate_layers(
        uint8_t* rdram,
        recomp_context* context,
        bool force
    ) {
        if (dynamic_gate_layers_.empty()) {
            return true;
        }
        bool changed = false;
        GeometryStats stats{};
        std::unordered_map<uint16_t, std::vector<LocalTriangle>> cache;
        const std::array<Matrix3, 4> rotations{
            decode_rotation_matrix(rdram, 0u),
            decode_rotation_matrix(rdram, 1u),
            decode_rotation_matrix(rdram, 2u),
            decode_rotation_matrix(rdram, 3u),
        };
        for (DynamicGateLayer& gate : dynamic_gate_layers_) {
            const uint32_t physical_row =
                static_cast<uint32_t>(gate.row + 32) & 63u;
            const uint32_t physical_column =
                static_cast<uint32_t>(gate.column + 32) & 63u;
            const uint32_t cell = kTerrainCellTable +
                physical_row * kTerrainCellCount * kTerrainCellStride +
                physical_column * kTerrainCellStride;
            const uint16_t descriptor = read_u16(rdram, cell + 4u);
            const uint8_t transform = read_u8(rdram, cell + 6u);
            if (!force && descriptor == gate.descriptor &&
                transform == gate.transform) {
                continue;
            }

            auto [iterator, inserted] = cache.try_emplace(descriptor);
            if (inserted) {
                iterator->second = decode_collision_descriptor(
                    rdram,
                    context,
                    descriptor,
                    stats
                );
            }
            JPH::TriangleList triangles;
            const Matrix3& rotation = rotations[transform >> 6u];
            const float tx = static_cast<float>(gate.column * 160);
            const float ty = static_cast<float>((transform & 63u) * 40u);
            const float tz = static_cast<float>(gate.row * 160);
            for (const LocalTriangle& local : iterator->second) {
                const Vec3 a = transform_vertex(local.a, rotation, tx, ty, tz);
                const Vec3 b = transform_vertex(local.b, rotation, tx, ty, tz);
                const Vec3 c = transform_vertex(local.c, rotation, tx, ty, tz);
                if (finite_vector(a) && finite_vector(b) && finite_vector(c)) {
                    triangles.emplace_back(
                        JPH::Vec3(a.x, a.y, a.z),
                        JPH::Vec3(b.x, b.y, b.z),
                        JPH::Vec3(c.x, c.y, c.z)
                    );
                }
            }

            JPH::BodyID replacement{};
            if (!triangles.empty()) {
                JPH::MeshShapeSettings mesh_settings(triangles);
                mesh_settings.mBuildQuality =
                    JPH::MeshShapeSettings::EBuildQuality::
                        FavorRuntimePerformance;
                JPH::ShapeSettings::ShapeResult shape_result =
                    mesh_settings.Create();
                if (shape_result.HasError()) {
                    std::fprintf(
                        stderr,
                        "BUMBLE_COLLISION stage=dynamic_gate_layer_failed"
                        " level=%" PRIu32
                        " cell=(%" PRId32 ",%" PRId32 ")"
                        " descriptor=%" PRIu16 " reason=mesh_shape"
                        " error=%s\n",
                        world_level_,
                        gate.column,
                        gate.row,
                        descriptor,
                        shape_result.GetError().c_str()
                    );
                    std::fflush(stderr);
                    return false;
                }
                JPH::BodyCreationSettings settings(
                    shape_result.Get(),
                    JPH::RVec3::sZero(),
                    JPH::Quat::sIdentity(),
                    JPH::EMotionType::Static,
                    Layers::kStatic
                );
                settings.mEnhancedInternalEdgeRemoval = true;
                replacement = physics_->GetBodyInterface().CreateAndAddBody(
                    settings,
                    JPH::EActivation::DontActivate
                );
                if (replacement.IsInvalid()) {
                    std::fprintf(
                        stderr,
                        "BUMBLE_COLLISION stage=dynamic_gate_layer_failed"
                        " level=%" PRIu32
                        " cell=(%" PRId32 ",%" PRId32 ")"
                        " descriptor=%" PRIu16
                        " reason=body_capacity\n",
                        world_level_,
                        gate.column,
                        gate.row,
                        descriptor
                    );
                    std::fflush(stderr);
                    return false;
                }
            }

            const uint16_t previous = gate.descriptor;
            remove_dynamic_gate_body(gate);
            gate.body = replacement;
            gate.descriptor = descriptor;
            gate.transform = transform;
#if defined(BUMBLE_COLLISION_DEBUG_RT64) && BUMBLE_COLLISION_DEBUG_RT64
            gate.debug_vertices.clear();
            gate.debug_vertices.reserve(triangles.size() * 3u);
            for (const JPH::Triangle& triangle : triangles) {
                for (const JPH::Float3& vertex : triangle.mV) {
                    gate.debug_vertices.push_back({
                        vertex.x,
                        vertex.y,
                        vertex.z,
                        0.0f,
                    });
                }
            }
#endif
            changed = true;
            std::fprintf(
                stderr,
                "BUMBLE_COLLISION stage=dynamic_gate_layer_updated"
                " level=%" PRIu32
                " cell=(%" PRId32 ",%" PRId32 ")"
                " descriptor=%" PRIu16
                " previous=%" PRIu16
                " triangles=%zu separate_body=1 world_rebuild=0\n",
                world_level_,
                gate.column,
                gate.row,
                descriptor,
                previous,
                triangles.size()
            );
        }
        if (changed) {
            publish_collision_debug_scene();
            std::fflush(stderr);
        }
        return true;
    }

    bool ensure_world(uint8_t* rdram, recomp_context* context) {
        if (!initialize()) {
            return false;
        }
        const uint32_t level = read_u32(rdram, kCurrentLevelIndex);
        if (!world_ready_ || level != world_level_) {
            const uint64_t hash = compute_world_hash(rdram);
            if (hash == 0u) {
                return false;
            }
            return rebuild_world(rdram, context, hash);
        }
        return refresh_dynamic_gate_layers(rdram, context, false);
    }

    bool rebuild_world(
        uint8_t* rdram,
        recomp_context* context,
        uint64_t hash
    ) {
        const auto started = std::chrono::steady_clock::now();
        clear_scene();
        GeometryStats stats{};
        JPH::TriangleList triangles;
        triangles.reserve(65536u);
        std::unordered_map<uint16_t, std::vector<LocalTriangle>> cache;
        const uint32_t level = read_u32(rdram, kCurrentLevelIndex);
        if (g_dynamic_gate_registration_level == level) {
            dynamic_gate_layers_.reserve(g_dynamic_gate_registrations.size());
            for (const DynamicGateRegistration& registration :
                    g_dynamic_gate_registrations) {
                dynamic_gate_layers_.push_back({
                    registration.column,
                    registration.row,
                });
            }
        }
        const std::array<Matrix3, 4> rotations{
            decode_rotation_matrix(rdram, 0u),
            decode_rotation_matrix(rdram, 1u),
            decode_rotation_matrix(rdram, 2u),
            decode_rotation_matrix(rdram, 3u),
        };

        for (int32_t row = -32; row < 32; ++row) {
            for (int32_t column = -32; column < 32; ++column) {
                const uint32_t physical_row =
                    static_cast<uint32_t>(row + 32) & 63u;
                const uint32_t physical_column =
                    static_cast<uint32_t>(column + 32) & 63u;
                const uint32_t cell = kTerrainCellTable +
                    physical_row * 64u * 8u + physical_column * 8u;
                const std::array<uint16_t, 3> ids{
                    read_u16(rdram, cell + 0u),
                    read_u16(rdram, cell + 2u),
                    read_u16(rdram, cell + 4u),
                };
                const uint8_t transform = read_u8(rdram, cell + 6u);
                bool cell_is_solid = false;
                std::array<uint16_t, 3> emitted_ids{0xFFFFu, 0xFFFFu, 0xFFFFu};
                size_t emitted_count = 0u;
                for (size_t layer = 0u; layer < ids.size(); ++layer) {
                    if (layer == 2u && is_dynamic_gate_cell(column, row)) {
                        continue;
                    }
                    const uint16_t id = ids[layer];
                    if (id == 0u) {
                        continue;
                    }
                    if (std::find(
                            emitted_ids.begin(),
                            emitted_ids.begin() + emitted_count,
                            id) != emitted_ids.begin() + emitted_count) {
                        continue;
                    }
                    emitted_ids[emitted_count++] = id;
                    const uint64_t descriptor64 =
                        static_cast<uint64_t>(kTerrainDescriptorBase) +
                        static_cast<uint64_t>(id) * kTerrainDescriptorStride;
                    if (descriptor64 + kTerrainDescriptorStride >
                        kTerrainDescriptorEnd) {
                        continue;
                    }
                    const uint32_t descriptor =
                        static_cast<uint32_t>(descriptor64);
                    if (read_u32(rdram, descriptor + 4u) == 0u) {
                        continue;
                    }
                    cell_is_solid = true;
                    auto [iterator, inserted] = cache.try_emplace(id);
                    if (inserted) {
                        iterator->second = decode_collision_descriptor(
                            rdram,
                            context,
                            id,
                            stats
                        );
                    }
                    if (iterator->second.empty()) {
                        continue;
                    }

                    ++stats.descriptor_instances;
                    const Matrix3& rotation = rotations[transform >> 6u];
                    const float tx = static_cast<float>(column * 160);
                    const float ty = static_cast<float>((transform & 63u) * 40u);
                    const float tz = static_cast<float>(row * 160);
                    for (const LocalTriangle& local : iterator->second) {
                        const Vec3 a = transform_vertex(local.a, rotation, tx, ty, tz);
                        const Vec3 b = transform_vertex(local.b, rotation, tx, ty, tz);
                        const Vec3 c = transform_vertex(local.c, rotation, tx, ty, tz);
                        if (finite_vector(a) && finite_vector(b) && finite_vector(c)) {
                            triangles.emplace_back(
                                JPH::Vec3(a.x, a.y, a.z),
                                JPH::Vec3(b.x, b.y, b.z),
                                JPH::Vec3(c.x, c.y, c.z)
                            );
                        }
                    }
                }
                if (cell_is_solid) {
                    ++stats.solid_cells;
                }
            }
        }

        if (triangles.empty()) {
            std::fprintf(
                stderr,
                "BUMBLE_COLLISION stage=world_build_failed level=%" PRIu32
                " hash=%016" PRIX64
                " reason=no_decodable_solid_triangles solid_cells=%" PRIu32
                " empty_descriptors=%" PRIu32
                " unresolved=%" PRIu32
                " rejected_local_vertices=%" PRIu32
                " rejected_visual_triangles=%" PRIu32 "\n",
                read_u32(rdram, kCurrentLevelIndex),
                hash,
                stats.solid_cells,
                stats.empty_descriptors,
                stats.unresolved_addresses,
                stats.rejected_local_vertices,
                stats.rejected_visual_triangles
            );
            std::fflush(stderr);
            return false;
        }

        JPH::MeshShapeSettings mesh_settings(triangles);
        mesh_settings.mBuildQuality =
            JPH::MeshShapeSettings::EBuildQuality::FavorBuildSpeed;
        JPH::ShapeSettings::ShapeResult shape_result = mesh_settings.Create();
        if (shape_result.HasError()) {
            std::fprintf(
                stderr,
                "BUMBLE_COLLISION stage=world_build_failed level=%" PRIu32
                " reason=mesh_shape error=%s\n",
                read_u32(rdram, kCurrentLevelIndex),
                shape_result.GetError().c_str()
            );
            std::fflush(stderr);
            return false;
        }

        JPH::BodyCreationSettings body_settings(
            shape_result.Get(),
            JPH::RVec3::sZero(),
            JPH::Quat::sIdentity(),
            JPH::EMotionType::Static,
            Layers::kStatic
        );
        body_settings.mEnhancedInternalEdgeRemoval = true;
        terrain_body_ = physics_->GetBodyInterface().CreateAndAddBody(
            body_settings,
            JPH::EActivation::DontActivate
        );
        if (terrain_body_.IsInvalid()) {
            std::fprintf(
                stderr,
                "BUMBLE_COLLISION stage=world_build_failed level=%" PRIu32
                " reason=body_capacity\n",
                read_u32(rdram, kCurrentLevelIndex)
            );
            std::fflush(stderr);
            return false;
        }
        world_level_ = read_u32(rdram, kCurrentLevelIndex);
#if defined(BUMBLE_COLLISION_DEBUG_RT64) && BUMBLE_COLLISION_DEBUG_RT64
        std::vector<RT64::BumbleCollisionDebugVertex> debug_vertices;
        debug_vertices.reserve(triangles.size() * 3u);
        for (const JPH::Triangle &triangle : triangles) {
            for (const JPH::Float3 &vertex : triangle.mV) {
                debug_vertices.push_back({
                    vertex.x,
                    vertex.y,
                    vertex.z,
                    0.0f,
                });
            }
        }

        std::vector<RT64::BumbleCollisionDebugVertex> liquid_cells;
        liquid_cells.reserve(kTerrainCellCount * kTerrainCellCount);
        for (int32_t row = -32; row < 32; ++row) {
            for (int32_t column = -32; column < 32; ++column) {
                const uint32_t physical_row =
                    static_cast<uint32_t>(row + 32) & 63u;
                const uint32_t physical_column =
                    static_cast<uint32_t>(column + 32) & 63u;
                const uint32_t cell = kTerrainCellTable +
                    physical_row * kTerrainCellCount * kTerrainCellStride +
                    physical_column * kTerrainCellStride;
                const uint16_t first_id = read_u16(rdram, cell + 0u);
                const uint16_t second_id = read_u16(rdram, cell + 2u);
                const uint16_t third_id = read_u16(rdram, cell + 4u);
                const auto descriptor_height = [&rdram](
                    uint16_t id,
                    bool zero_when_absent
                ) {
                    // func_80085F20 reads descriptor 0 for layer 1, but zeroes absent layers 2/3.
                    if (zero_when_absent && id == 0u) {
                        return int32_t{0};
                    }
                    const uint64_t descriptor64 =
                        static_cast<uint64_t>(kTerrainDescriptorBase) +
                        static_cast<uint64_t>(id) *
                            kTerrainDescriptorStride;
                    if (descriptor64 + kTerrainDescriptorStride >
                        kTerrainDescriptorEnd) {
                        return int32_t{0};
                    }
                    return static_cast<int32_t>(read_s16(
                        rdram,
                        static_cast<uint32_t>(descriptor64) + 8u
                    ));
                };
                const int32_t authored_height = std::max({
                    descriptor_height(first_id, false),
                    descriptor_height(second_id, true),
                    descriptor_height(third_id, true),
                }) + static_cast<int32_t>(read_u8(rdram, cell + 6u) & 63u) *
                    40;
                liquid_cells.push_back({
                    static_cast<float>(column * 160),
                    static_cast<float>(authored_height),
                    static_cast<float>(row * 160),
                    0.0f,
                });
            }
        }
        collision_debug_static_vertices_ = std::move(debug_vertices);
        collision_debug_liquid_cells_ = std::move(liquid_cells);
#endif
        if (!refresh_dynamic_gate_layers(rdram, context, true)) {
            return false;
        }
        publish_collision_debug_scene();
        physics_->OptimizeBroadPhase();
        if (read_u32(rdram, kCurrentLevelIndex) == 1u &&
            pickup_passage_validation_requested()) {
            validate_tracked_hollow_passages(*physics_);
        }
        world_hash_ = hash;
        world_ready_ = true;
        ++world_generation_;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started
        ).count();
        std::fprintf(
            stderr,
            "BUMBLE_COLLISION stage=world_ready generation=%" PRIu64
            " level=%" PRIu32 " hash=%016" PRIX64
            " triangles=%zu dynamic_authority=game_gate_grid_layer"
            " mutable_gate_layers=%zu"
            " solid_cells=%" PRIu32
            " descriptor_instances=%" PRIu32
            " decoded_descriptors=%" PRIu32
            " empty_descriptors=%" PRIu32
            " unresolved=%" PRIu32 " malformed=%" PRIu32
            " rejected_local_vertices=%" PRIu32
            " maximum_abs_local_vertex=%" PRIu32
            " rejected_visual_triangles=%" PRIu32
            " contact_filter=game_owned_collision_bsp"
            " build_ms=%lld source=decoded_static_display_lists\n",
            world_generation_,
            read_u32(rdram, kCurrentLevelIndex),
            hash,
            triangles.size(),
            dynamic_gate_layers_.size(),
            stats.solid_cells,
            stats.descriptor_instances,
            stats.decoded_descriptors,
            stats.empty_descriptors,
            stats.unresolved_addresses,
            stats.malformed_commands,
            stats.rejected_local_vertices,
            stats.maximum_abs_local_vertex,
            stats.rejected_visual_triangles,
            static_cast<long long>(elapsed)
        );
        std::fflush(stderr);
        return true;
    }

    void clear_scene() {
#if defined(BUMBLE_COLLISION_DEBUG_RT64) && BUMBLE_COLLISION_DEBUG_RT64
        RT64::clearBumbleCollisionDebugScene();
#endif
        character_ = nullptr;
        character_actor_ = 0u;
        owned_update_actor_ = 0u;
        owned_update_state_ = 0u;
        published_position_valid_ = false;
        last_step_mode_valid_ = false;
        last_step_grounded_ = false;
        last_step_moving_ = false;
        last_step_blocking_ = false;
        last_step_wall_contact_ = false;
        last_step_ceiling_contact_ = false;
        if (physics_ != nullptr) {
            JPH::BodyInterface& bodies = physics_->GetBodyInterface();
            for (DynamicGateLayer& gate : dynamic_gate_layers_) {
                if (!gate.body.IsInvalid()) {
                    bodies.RemoveBody(gate.body);
                    bodies.DestroyBody(gate.body);
                }
            }
            if (!terrain_body_.IsInvalid()) {
                bodies.RemoveBody(terrain_body_);
                bodies.DestroyBody(terrain_body_);
            }
        }
        dynamic_gate_layers_.clear();
        terrain_body_ = {};
#if defined(BUMBLE_COLLISION_DEBUG_RT64) && BUMBLE_COLLISION_DEBUG_RT64
        collision_debug_static_vertices_.clear();
        collision_debug_liquid_cells_.clear();
#endif
        world_ready_ = false;
    }

    bool prepare_character(
        uint8_t* rdram,
        uint32_t actor,
        bool grounded_pose
    ) {
        const Vec3 actor_position = read_actor_position(rdram, actor);
        if (!finite_vector(actor_position)) {
            return false;
        }
        if (character_ == nullptr || character_actor_ != actor) {
            JPH::CharacterVirtualSettings settings;
            settings.mMaxSlopeAngle = JPH::DegreesToRadians(58.0f);
            settings.mShape = new JPH::CapsuleShape(
                kCapsuleHalfHeight,
                kCapsuleRadius
            );
            settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
            settings.mCharacterPadding = kCharacterPadding;
            settings.mPenetrationRecoverySpeed = 1.0f;
            settings.mPredictiveContactDistance = 2.0f;
            settings.mSupportingVolume = JPH::Plane(
                JPH::Vec3::sAxisY(),
                -kCapsuleRadius
            );
            settings.mEnhancedInternalEdgeRemoval = true;
            character_ = new JPH::CharacterVirtual(
                &settings,
                to_jolt(actor_to_character_center(
                    actor_position,
                    grounded_pose
                )),
                JPH::Quat::sIdentity(),
                static_cast<JPH::uint64>(actor),
                physics_.get()
            );
            character_->SetMaxNumHits(256u);
            character_->SetListener(&contact_listener_);
            character_actor_ = actor;
            published_position_ = actor_position;
            published_position_valid_ = true;
            refresh_character_contacts();
            return true;
        }

        if (!published_position_valid_ ||
            distance_squared(actor_position, published_position_) >
                kExternalPositionEpsilon * kExternalPositionEpsilon) {
            character_->SetPosition(to_jolt(
                actor_to_character_center(actor_position, grounded_pose)
            ));
            character_->SetLinearVelocity(JPH::Vec3::sZero());
            published_position_ = actor_position;
            published_position_valid_ = true;
            refresh_character_contacts();
            const uint64_t count = ++external_rebases_;
            if (collision_runtime_telemetry_enabled() &&
                (count <= 8u || count % 120u == 0u)) {
                std::fprintf(
                    stderr,
                    "BUMBLE_COLLISION stage=external_pose_rebased count=%" PRIu64
                    " actor=0x%08" PRIX32
                    " position=(%.3f,%.3f,%.3f)\n",
                    count,
                    actor,
                    static_cast<double>(actor_position.x),
                    static_cast<double>(actor_position.y),
                    static_cast<double>(actor_position.z)
                );
                std::fflush(stderr);
            }
        }
        return true;
    }

    void refresh_character_contacts() {
        character_->RefreshContacts(
            physics_->GetDefaultBroadPhaseLayerFilter(Layers::kMoving),
            physics_->GetDefaultLayerFilter(Layers::kMoving),
            {},
            {},
            *allocator_
        );
    }

    bool enforce_player_ceiling(bool grounded_pose) {
        Vec3 actor_position = character_center_to_actor(
            from_jolt(character_->GetPosition()),
            grounded_pose
        );
        if (actor_position.y <= kPlayerCeilingY) {
            return false;
        }
        actor_position.y = kPlayerCeilingY;
        character_->SetPosition(to_jolt(
            actor_to_character_center(actor_position, grounded_pose)
        ));
        refresh_character_contacts();
        return true;
    }

    bool limit_upward_delta_at_ceiling(Vec3& delta) const {
        if (delta.y <= 0.0f) {
            return false;
        }
        const Vec3 actor_position = character_center_to_actor(
            from_jolt(character_->GetPosition()),
            false
        );
        const float remaining = std::max(
            kPlayerCeilingY - actor_position.y,
            0.0f
        );
        if (delta.y <= remaining) {
            return false;
        }
        delta.y = remaining;
        return true;
    }

    void publish_character(
        uint8_t* rdram,
        uint32_t actor,
        float speed,
        bool grounded_pose,
        float support_probe_distance
    ) {
        const Vec3 character_center = from_jolt(character_->GetPosition());
        const Vec3 position = character_center_to_actor(
            character_center,
            grounded_pose
        );
        if (!finite_vector(position)) {
            return;
        }
        write_actor_position(rdram, actor, position);
        write_f32(rdram, actor + 0x5Cu, std::max(speed, 0.0f));
        published_position_ = position;
        published_position_valid_ = true;
#if defined(BUMBLE_COLLISION_DEBUG_RT64) && BUMBLE_COLLISION_DEBUG_RT64
        RT64::publishBumbleCollisionDebugCapsule(
            character_center.x,
            character_center.y,
            character_center.z,
            kCapsuleRadius,
            kCapsuleHalfHeight,
            support_probe_distance
        );
#endif
    }

    void log_step(
        uint32_t actor,
        bool grounded,
        bool moving,
        bool landed,
        bool automatic_takeoff,
        const Vec3& requested_delta,
        const Vec3& position_before,
        bool ceiling_contact
    ) {
        const uint64_t count = ++steps_;
        uint32_t actual_contacts = 0u;
        bool blocking_contact = false;
        bool wall_contact = false;
        Vec3 wall_normal{};
        constexpr float kMaximumSlopeNormalY = 0.5299193f; // cos(58 deg)
        for (const auto& contact : character_->GetActiveContacts()) {
            if (!contact.mHadCollision) {
                continue;
            }
            ++actual_contacts;
            const float requested_into_normal =
                requested_delta.x * contact.mContactNormal.GetX() +
                requested_delta.y * contact.mContactNormal.GetY() +
                requested_delta.z * contact.mContactNormal.GetZ();
            const bool blocks_requested_motion =
                moving && requested_into_normal < -0.01f;
            blocking_contact = blocking_contact || blocks_requested_motion;
            if (blocks_requested_motion &&
                contact.mSurfaceNormal.GetY() < kMaximumSlopeNormalY) {
                wall_contact = true;
                wall_normal = {
                    contact.mContactNormal.GetX(),
                    contact.mContactNormal.GetY(),
                    contact.mContactNormal.GetZ(),
                };
            }
        }
        const bool mode_changed = !last_step_mode_valid_ ||
            grounded != last_step_grounded_;
        const bool movement_changed = !last_step_mode_valid_ ||
            moving != last_step_moving_;
        const bool blocking_changed = !last_step_mode_valid_ ||
            blocking_contact != last_step_blocking_;
        const bool wall_changed = !last_step_mode_valid_ ||
            wall_contact != last_step_wall_contact_;
        const bool ceiling_changed = !last_step_mode_valid_ ||
            ceiling_contact != last_step_ceiling_contact_;
        last_step_mode_valid_ = true;
        last_step_grounded_ = grounded;
        last_step_moving_ = moving;
        last_step_blocking_ = blocking_contact;
        last_step_wall_contact_ = wall_contact;
        last_step_ceiling_contact_ = ceiling_contact;
        if (collision_runtime_telemetry_enabled() &&
            (count <= 8u || count % 600u == 0u || mode_changed ||
             movement_changed || blocking_changed || wall_changed ||
             ceiling_changed || landed || automatic_takeoff)) {
            const Vec3 position = character_center_to_actor(
                from_jolt(character_->GetPosition()),
                grounded
            );
            const Vec3 resolved_delta{
                position.x - position_before.x,
                position.y - position_before.y,
                position.z - position_before.z,
            };
            float ground_y = std::numeric_limits<float>::quiet_NaN();
            float actor_surface_gap = std::numeric_limits<float>::quiet_NaN();
            if (character_->IsSupported()) {
                ground_y = static_cast<float>(
                    character_->GetGroundPosition().GetY()
                );
                actor_surface_gap = position.y - ground_y;
            }
            std::fprintf(
                stderr,
                "BUMBLE_COLLISION stage=player_step count=%" PRIu64
                " actor=0x%08" PRIX32 " mode=%s"
                " position=(%.3f,%.3f,%.3f)"
                " moving=%d supported=%d landed=%d automatic_takeoff=%d"
                " blocking_contact=%d actual_contacts=%" PRIu32
                " wall_contact=%d wall_normal=(%.3f,%.3f,%.3f)"
                " ceiling_contact=%d ceiling_y=%.3f"
                " ground_y=%.3f actor_surface_gap=%.3f"
                " requested_delta=(%.3f,%.3f,%.3f)"
                " resolved_delta=(%.3f,%.3f,%.3f)"
                " active_contacts=%zu max_hits_exceeded=%d\n",
                count,
                actor,
                grounded ? "grounded" : "flight",
                static_cast<double>(position.x),
                static_cast<double>(position.y),
                static_cast<double>(position.z),
                moving ? 1 : 0,
                character_->IsSupported() ? 1 : 0,
                landed ? 1 : 0,
                automatic_takeoff ? 1 : 0,
                blocking_contact ? 1 : 0,
                actual_contacts,
                wall_contact ? 1 : 0,
                static_cast<double>(wall_normal.x),
                static_cast<double>(wall_normal.y),
                static_cast<double>(wall_normal.z),
                ceiling_contact ? 1 : 0,
                static_cast<double>(kPlayerCeilingY),
                static_cast<double>(ground_y),
                static_cast<double>(actor_surface_gap),
                static_cast<double>(requested_delta.x),
                static_cast<double>(requested_delta.y),
                static_cast<double>(requested_delta.z),
                static_cast<double>(resolved_delta.x),
                static_cast<double>(resolved_delta.y),
                static_cast<double>(resolved_delta.z),
                static_cast<size_t>(character_->GetActiveContacts().size()),
                character_->GetMaxHitsExceeded() ? 1 : 0
            );
            std::fflush(stderr);
        }
    }

    BroadPhaseLayerInterface broad_phase_interface_{};
    ObjectVsBroadPhaseFilter object_vs_broad_phase_filter_{};
    ObjectLayerPairFilter object_pair_filter_{};
    std::unique_ptr<JPH::TempAllocatorImpl> allocator_{};
    std::unique_ptr<JPH::PhysicsSystem> physics_{};
    JPH::Ref<JPH::CharacterVirtual> character_{};
    JPH::BodyID terrain_body_{};
    std::vector<DynamicGateLayer> dynamic_gate_layers_{};
    AuthoredTerrainContactListener contact_listener_{};
#if defined(BUMBLE_COLLISION_DEBUG_RT64) && BUMBLE_COLLISION_DEBUG_RT64
    std::vector<RT64::BumbleCollisionDebugVertex>
        collision_debug_static_vertices_;
    std::vector<RT64::BumbleCollisionDebugVertex>
        collision_debug_liquid_cells_;
#endif
    uint64_t world_hash_ = 0u;
    uint64_t world_generation_ = 0u;
    uint64_t steps_ = 0u;
    uint64_t external_rebases_ = 0u;
    uint64_t legacy_spatial_bypasses_ = 0u;
    uint64_t legacy_tail_discards_ = 0u;
    uint64_t state_transition_rebases_ = 0u;
    uint32_t character_actor_ = 0u;
    uint32_t owned_update_actor_ = 0u;
    uint32_t owned_update_state_ = 0u;
    uint32_t world_level_ = std::numeric_limits<uint32_t>::max();
    Vec3 published_position_{};
    bool published_position_valid_ = false;
    bool world_ready_ = false;
    bool jolt_registered_ = false;
    bool last_step_mode_valid_ = false;
    bool last_step_grounded_ = false;
    bool last_step_moving_ = false;
    bool last_step_blocking_ = false;
    bool last_step_wall_contact_ = false;
    bool last_step_ceiling_contact_ = false;
};

CollisionWorld& collision_world() {
    static CollisionWorld world;
    return world;
}

uint32_t actor_from_context(recomp_context* context) {
    return context != nullptr ? static_cast<uint32_t>(context->r17) : 0u;
}

} // namespace

extern "C" void bumble_register_dynamic_gate_layer(
    uint8_t* rdram,
    uint32_t actor
) {
    if (rdram == nullptr || !valid_guest_address(actor, 0x4Cu)) {
        return;
    }
    const Vec3 position = read_actor_position(rdram, actor);
    if (!finite_vector(position)) {
        return;
    }
    const int32_t column = static_cast<int32_t>(
        std::lround(position.x / 160.0f)
    );
    const int32_t row = static_cast<int32_t>(
        std::lround(position.z / 160.0f)
    );
    if (column < -32 || column >= 32 || row < -32 || row >= 32) {
        return;
    }
    const uint32_t level = read_u32(rdram, kCurrentLevelIndex);
    if (g_dynamic_gate_registration_level != level) {
        g_dynamic_gate_registration_level = level;
        g_dynamic_gate_registrations.clear();
    }
    const bool exists = std::any_of(
        g_dynamic_gate_registrations.begin(),
        g_dynamic_gate_registrations.end(),
        [column, row](const DynamicGateRegistration& registration) {
            return registration.column == column && registration.row == row;
        }
    );
    if (exists) {
        return;
    }
    g_dynamic_gate_registrations.push_back({column, row});
    const uint32_t physical_row = static_cast<uint32_t>(row + 32) & 63u;
    const uint32_t physical_column =
        static_cast<uint32_t>(column + 32) & 63u;
    const uint32_t cell = kTerrainCellTable +
        physical_row * kTerrainCellCount * kTerrainCellStride +
        physical_column * kTerrainCellStride;
    std::fprintf(
        stderr,
        "BUMBLE_COLLISION stage=dynamic_gate_layer_registered"
        " level=%" PRIu32 " actor=0x%08" PRIX32
        " cell=(%" PRId32 ",%" PRId32 ")"
        " descriptor=%" PRIu16 "\n",
        level,
        actor,
        column,
        row,
        read_u16(rdram, cell + 4u)
    );
    std::fflush(stderr);
}

extern "C" void bumble_prepare_modern_collision_world(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr ||
        !bumble::modern_controls::enabled()) {
        return;
    }
    const uint32_t actor = static_cast<uint32_t>(context->r4);
    const uint32_t state = actor != 0u && valid_guest_address(actor, 0x90u)
        ? read_u32(rdram, actor + 0x8Cu)
        : UINT32_MAX;
    if (exact_modern_player_one(rdram, actor, state)) {
        (void)collision_world().prepare_world(rdram, context);
    }
}

extern "C" uint32_t bumble_step_modern_player_airborne(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t actor = actor_from_context(context);
    if (!exact_modern_player_one(rdram, actor, kFlyingState) ||
        !player_one_alive(rdram)) {
        return 0u;
    }
    bumble::modern_controls::SpatialIntent intent{};
    if (!bumble::modern_controls::query_spatial_intent(
            rdram,
            actor,
            kFlyingState,
            intent)) {
        return 0u;
    }
    return collision_world().step_airborne(rdram, context, actor, intent)
        ? 1u
        : 0u;
}

extern "C" uint32_t bumble_step_modern_player_grounded(
    uint8_t* rdram,
    recomp_context* context
) {
    const uint32_t actor = actor_from_context(context);
    if (actor == 0u || !valid_guest_address(actor, 0xFCu)) {
        return 0u;
    }
    if (!player_one_alive(rdram)) {
        return 0u;
    }
    const uint32_t state = read_u32(rdram, actor + 0x8Cu);
    if (state < kGroundedFirstState || state > kGroundedLastState ||
        !exact_modern_player_one(rdram, actor, state)) {
        return 0u;
    }
    bumble::modern_controls::SpatialIntent intent{};
    if (!bumble::modern_controls::query_spatial_intent(
            rdram,
            actor,
            state,
            intent)) {
        return 0u;
    }
    return collision_world().step_grounded(rdram, context, actor, intent)
        ? 1u
        : 0u;
}

extern "C" uint32_t bumble_continue_modern_player_collision_update(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || !player_one_alive(rdram)) {
        collision_world().relinquish_spatial_update(actor_from_context(context));
        return 0u;
    }
    return collision_world().owns_spatial_continuation(
        actor_from_context(context)
    ) ? 1u : 0u;
}

extern "C" void bumble_finish_modern_player_collision_update(
    uint8_t* rdram,
    recomp_context* context
) {
    collision_world().finish_spatial_update(
        rdram,
        actor_from_context(context)
    );
}

extern "C" uint32_t bumble_query_modern_static_sphere(
    uint8_t* rdram,
    recomp_context* context,
    float x,
    float y,
    float z,
    float radius,
    BumbleStaticCollisionHit* hit
) {
    if (rdram == nullptr || context == nullptr || hit == nullptr) {
        return 0u;
    }
    return collision_world().query_static_sphere(
        rdram,
        context,
        {x, y, z},
        radius,
        *hit
    ) ? 1u : 0u;
}

extern "C" uint32_t bumble_modern_static_world_ready(uint8_t* rdram) {
    return collision_world().query_world_ready(rdram) ? 1u : 0u;
}

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
) {
    if (rdram == nullptr || context == nullptr || hit == nullptr) {
        return 0u;
    }
    return collision_world().sweep_static_sphere(
        rdram,
        context,
        {start_x, start_y, start_z},
        {end_x, end_y, end_z},
        radius,
        *hit
    ) ? 1u : 0u;
}
