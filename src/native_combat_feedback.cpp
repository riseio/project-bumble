#include "native_combat_feedback.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <mutex>

#include "native_text_overlay_state.hpp"

namespace {
using Clock = std::chrono::steady_clock;
constexpr uint32_t kRegistry = 0x800E2408u;
constexpr uint32_t kFrontend = 0x800FFF80u;
constexpr uint32_t kLevel = 0x800E9640u;
constexpr uint32_t kCursor = 0x80035F30u;
constexpr uint32_t kBodyDescriptor = 0x8004C360u;
constexpr uint32_t kCoreDescriptor = 0x8004C3D0u;

bool valid(uint32_t address, uint32_t size) {
    return address >= 0x80000000u && address < 0x80800000u &&
        size <= 0x80800000u - address && (address & 3u) == 0u;
}
uint32_t word(uint8_t* rdram, uint32_t address) {
    return uint32_t(MEM_W(0, int32_t(address)));
}
uint16_t half(uint8_t* rdram, uint32_t address) {
    return uint16_t(MEM_HU(0, int32_t(address)));
}
int health(uint8_t* rdram, uint32_t actor) {
    return int(int16_t(half(rdram, actor + 0x7Cu)));
}
struct Actor {
    uint32_t address = 0u;
    uint32_t descriptor = 0u;
    uint16_t id = 0u;
};
uint32_t resolve(uint8_t* rdram, uint16_t id) {
    const uint32_t entry = kRegistry + uint32_t(id & 0x1FFu) * 8u;
    return half(rdram, entry) == id ? word(rdram, entry + 4u) : 0u;
}
bool live(uint8_t* rdram, const Actor& actor) {
    return rdram && valid(actor.address, 0x94u) &&
        resolve(rdram, actor.id) == actor.address &&
        half(rdram, actor.address + 0x7Eu) == actor.id &&
        word(rdram, actor.address + 0x88u) == actor.descriptor;
}
Actor identity(uint8_t* rdram, uint32_t address) {
    if (!rdram || !valid(address, 0x94u)) return {};
    Actor result{address, word(rdram, address + 0x88u), half(rdram, address + 0x7Eu)};
    return live(rdram, result) ? result : Actor{};
}
bool gameplay(uint8_t* rdram) {
    return rdram && word(rdram, kFrontend) == 0x19u &&
        word(rdram, kFrontend + 0x40u) == 0u &&
        word(rdram, 0x80100124u) == 0u && // Pause/menu visibility.
        word(rdram, 0x800E91E0u) != 0u; // Camera active, not input mode.
}
constexpr int lost_health(int before, int after) { return std::max(0, before - after); }
constexpr int remaining(int body, int core) { return std::max(0, body) + std::max(0, core); }
static_assert(lost_health(10, 10) == 0 && lost_health(10, 12) == 0);
static_assert(lost_health(0, -1) == 1 && lost_health(10, 6) == 4);
static_assert(remaining(70, 50) == 120 && remaining(0, 50) == 50 && remaining(-1, 0) == 0);

struct DamageScope {
    uint8_t* rdram = nullptr;
    Actor target{};
    uint32_t stack = 0u;
    uint32_t level = 0u;
    int before = 0;
};
thread_local std::array<DamageScope, 16> g_damage_scopes{};
thread_local uint32_t g_damage_depth = 0u;
thread_local uint32_t g_damage_overflow = 0u;

struct Encounter {
    uint8_t* rdram = nullptr;
    uint32_t level = 0u;
    Actor body{}, core{};
    int maximum = 0;
    int body_health = 0;
    int core_health = 0;
    Clock::time_point damage_at{};
    bool admitted = false;
};
std::mutex g_mutex;
Encounter g_encounter{};
// Rewind only commands replaced by the native bar.
thread_local uint32_t g_saved_bar_cursor = 0u;
thread_local bool g_bar_replacement_published = false;
}

extern "C" void bumble_begin_damage_feedback(uint8_t* rdram, recomp_context* ctx) {
    if (g_damage_overflow != 0u || g_damage_depth == g_damage_scopes.size()) {
        ++g_damage_overflow;
        return;
    }
    DamageScope scope{};
    scope.rdram = rdram;
    scope.stack = uint32_t(ctx->r29);
    if (gameplay(rdram)) {
        scope.level = word(rdram, kLevel);
        scope.target = identity(rdram, uint32_t(ctx->r4));
        if (scope.target.descriptor == kBodyDescriptor ||
            scope.target.descriptor == kCoreDescriptor) {
            scope.before = health(rdram, scope.target.address);
        } else {
            scope.target = {};
        }
    }
    g_damage_scopes[g_damage_depth++] = scope;
}

extern "C" void bumble_end_damage_feedback(uint8_t* rdram, recomp_context* ctx, uint32_t stack_size) {
    if (g_damage_overflow != 0u) { --g_damage_overflow; return; }
    if (g_damage_depth == 0u) return;
    const DamageScope scope = g_damage_scopes[--g_damage_depth];
    if (scope.rdram != rdram || scope.stack != uint32_t(ctx->r29) + stack_size ||
        !gameplay(rdram) || scope.level != word(rdram, kLevel) ||
        !live(rdram, scope.target)) return;
    const int loss = lost_health(scope.before, health(rdram, scope.target.address));
    if (loss == 0) return;
    const auto now = Clock::now();
    std::scoped_lock lock(g_mutex);
    if (g_encounter.rdram == rdram && g_encounter.level == scope.level) {
        if (g_encounter.body.address == scope.target.address &&
            g_encounter.body.id == scope.target.id) {
            g_encounter.body_health = health(rdram, scope.target.address);
            g_encounter.damage_at = now;
        } else if (g_encounter.core.address == scope.target.address &&
            g_encounter.core.id == scope.target.id) {
            g_encounter.core_health = health(rdram, scope.target.address);
            g_encounter.damage_at = now;
        }
    }
}

extern "C" void bumble_bind_boss_encounter(uint8_t* rdram, recomp_context* ctx) {
    // CA2E0 has constructed both actors.
    const Actor core = identity(rdram, uint32_t(ctx->r16));
    const Actor body = identity(rdram, uint32_t(ctx->r17));
    if (body.descriptor != kBodyDescriptor || core.descriptor != kCoreDescriptor ||
        half(rdram, core.address + 0x90u) != body.id) return;
    const int body_hp = health(rdram, body.address);
    const int core_hp = health(rdram, core.address);
    const int maximum = remaining(body_hp, core_hp);
    if (maximum <= 0) return;
    std::scoped_lock lock(g_mutex);
    g_encounter = {rdram, word(rdram, kLevel), body, core, maximum, body_hp, core_hp, {}};
}

void bumble::combat_feedback::observe_frame(uint8_t* rdram) {
    using namespace bumble::text_overlay;
    g_bar_replacement_published = false;
    g_saved_bar_cursor = 0u;
    const auto now = Clock::now();
    std::scoped_lock lock(g_mutex);
    if (!rdram || g_encounter.rdram != rdram ||
        g_encounter.level != word(rdram, kLevel) ||
        (g_encounter.admitted && word(rdram, kFrontend) != 0x18u &&
            word(rdram, kFrontend) != 0x19u)) {
        g_encounter = {};
    }
    if (!renderer_ready() || !gameplay(rdram)) return;
    if (g_encounter.rdram == rdram && g_encounter.level == word(rdram, kLevel) &&
        g_encounter.maximum > 0) {
        g_encounter.admitted = true;
        const bool body_live = live(rdram, g_encounter.body);
        const bool core_live = live(rdram, g_encounter.core);
        if ((!body_live && g_encounter.body_health > 0) ||
            (!core_live && g_encounter.core_health > 0)) {
            g_encounter = {}; // Unexpected actor retirement.
        } else if (body_live || core_live) {
            const int body_hp = body_live ? health(rdram, g_encounter.body.address) : 0;
            const int core_hp = core_live ? health(rdram, g_encounter.core.address) : 0;
            const int current = remaining(body_hp, core_hp);
            if ((body_live && body_hp < g_encounter.body_health) ||
                (core_live && core_hp < g_encounter.core_health)) g_encounter.damage_at = now;
            g_encounter.body_health = body_hp;
            g_encounter.core_health = core_hp;
            const bool flash = now - g_encounter.damage_at < std::chrono::milliseconds(250);
            const char* stage = body_hp > 0 ? "HEAD" :
                core_hp <= 0 ? "DEFEATED" :
                core_live && word(rdram, g_encounter.core.address + 0x8Cu) == 1u ? "CORE" : "TRANSITION";
            char label[80];
            std::snprintf(label, sizeof(label), "BOSS %s   %d / %d", stage, current, g_encounter.maximum);
            bool accepted = observe_panel(TextKind::CombatHud, 68, 6, 184, 17,
                0x09111BE8u, HorizontalAnchor::Center, VerticalAnchor::Top, 0xCB000001u);
            accepted &= observe(TextKind::CombatHud, 160, 7, label,
                HorizontalAnchor::CenterText, VerticalAnchor::Authored, 0.65f,
                0xFFF4D8FFu, 0x000000FFu, 0xCB000002u);
            accepted &= observe_panel(TextKind::CombatHud, 69, 17, 182, 5,
                0x413B46FFu, HorizontalAnchor::Center, VerticalAnchor::Top, 0xCB000003u);
            if (current > 0) {
                const uint32_t fill = std::max(1u, uint32_t(180.0 * std::clamp(
                    double(current) / g_encounter.maximum, 0.0, 1.0)));
                accepted &= observe_panel(TextKind::CombatHud, 70, 18, fill, 3,
                    flash ? 0xFFF4D8FFu : 0xE86191FFu,
                    HorizontalAnchor::Center, VerticalAnchor::Top, 0xCB000004u);
            }
            g_bar_replacement_published = accepted;
        } else {
            g_encounter = {}; // Both actors are gone, not just off-screen.
        }
    }
}

extern "C" void bumble_begin_boss_health_bar(uint8_t* rdram, recomp_context*) {
    g_saved_bar_cursor = g_bar_replacement_published && gameplay(rdram)
        ? word(rdram, kCursor) : 0u;
}
extern "C" void bumble_end_boss_health_bar(uint8_t* rdram, recomp_context*) {
    if (g_saved_bar_cursor != 0u) {
        MEM_W(0, int32_t(kCursor)) = g_saved_bar_cursor;
        g_saved_bar_cursor = 0u;
    }
}
