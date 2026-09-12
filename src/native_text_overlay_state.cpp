#include "native_text_overlay_state.hpp"
#include "native_text_overlay.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <utility>

namespace {

constexpr auto kObservationLifetime = std::chrono::milliseconds(300);
constexpr size_t kMaximumObservations = 128;

struct TimedObservation {
    bumble::text_overlay::Observation value;
    std::chrono::steady_clock::time_point observed_at{};
    uint64_t frame_generation = 0;
};

std::mutex g_mutex;
std::vector<TimedObservation> g_observations;
std::atomic_bool g_renderer_ready{false};
uint64_t g_next_revision = 1;
uint64_t g_next_frame_generation = 1;
uint64_t g_build_frame_generation = 0;
uint64_t g_committed_frame_generation = 0;
uint64_t g_build_menu_generation = 0;
uint64_t g_committed_menu_generation = 0;

bool frame_scoped_kind(bumble::text_overlay::TextKind kind) {
    using bumble::text_overlay::TextKind;
    return kind == TextKind::Script || kind == TextKind::Briefing ||
        kind == TextKind::Credits || kind == TextKind::Menu ||
        kind == TextKind::HighScores || kind == TextKind::Completion ||
        kind == TextKind::CombatHud;
}

bool same_slot(
    const TimedObservation& existing,
    const bumble::text_overlay::Observation& candidate
) {
    if (existing.value.kind != candidate.kind ||
        existing.value.primitive != candidate.primitive) {
        return false;
    }
    if (existing.value.stable_slot != 0u ||
        candidate.stable_slot != 0u) {
        return existing.value.stable_slot != 0u &&
            existing.value.stable_slot == candidate.stable_slot;
    }
    return existing.value.x == candidate.x &&
        existing.value.y == candidate.y;
}

void remove_expired_locked(std::chrono::steady_clock::time_point now) {
    std::erase_if(
        g_observations,
        [now](const TimedObservation& observation) {
            if (observation.frame_generation != 0u ||
                observation.value.kind ==
                    bumble::text_overlay::TextKind::Menu ||
                observation.value.kind ==
                    bumble::text_overlay::TextKind::Editor) {
                return false;
            }
            return now - observation.observed_at > kObservationLifetime;
        }
    );
}

bool same_raster(
    const bumble::text_overlay::Observation& left,
    const bumble::text_overlay::Observation& right
) {
    if (left.primitive == bumble::text_overlay::OverlayPrimitive::Panel &&
        right.primitive == bumble::text_overlay::OverlayPrimitive::Panel) {
        return left.face_rgba == right.face_rgba;
    }
    return left.text == right.text &&
        left.panel_reveal == right.panel_reveal &&
        left.width == right.width &&
        left.height == right.height &&
        left.horizontal_anchor == right.horizontal_anchor &&
        left.vertical_anchor == right.vertical_anchor &&
        left.screen_scale == right.screen_scale &&
        left.face_rgba == right.face_rgba &&
        left.outline_rgba == right.outline_rgba &&
        left.fill_fraction == right.fill_fraction &&
        left.cell_count == right.cell_count;
}

bool store_observation(bumble::text_overlay::Observation candidate) {
    using bumble::text_overlay::OverlayPrimitive;
    if (!g_renderer_ready.load(std::memory_order_acquire) ||
        candidate.screen_scale <= 0.0f ||
        (candidate.primitive == OverlayPrimitive::Text &&
            candidate.text.empty()) ||
        (candidate.primitive == OverlayPrimitive::Panel &&
            (candidate.width == 0u || candidate.height == 0u)) ||
        (candidate.primitive == OverlayPrimitive::HealthHoneycomb &&
            (candidate.cell_count == 0u || candidate.cell_count > 10u ||
                candidate.fill_fraction < 0.0f ||
                candidate.fill_fraction > 1.0f)) ||
        ((candidate.primitive == OverlayPrimitive::MenuBackdrop ||
            candidate.primitive == OverlayPrimitive::MainMenuBackdrop) &&
            candidate.kind != bumble::text_overlay::TextKind::Menu &&
            candidate.kind != bumble::text_overlay::TextKind::HighScores &&
            candidate.kind != bumble::text_overlay::TextKind::Editor)) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(g_mutex);
    remove_expired_locked(now);
    const bool frame_scoped = frame_scoped_kind(candidate.kind);
    const uint64_t candidate_frame_generation = frame_scoped
        ? (candidate.kind == bumble::text_overlay::TextKind::Menu &&
                g_build_menu_generation != 0u
            ? g_build_menu_generation
            : g_build_frame_generation != 0u
                ? g_build_frame_generation
                : candidate.kind == bumble::text_overlay::TextKind::Menu
                    ? g_committed_menu_generation
                    : g_committed_frame_generation)
        : 0u;
    const auto matching = std::find_if(
        g_observations.begin(),
        g_observations.end(),
        [&candidate, candidate_frame_generation](
            const TimedObservation& observation
        ) {
            return observation.frame_generation ==
                    candidate_frame_generation &&
                same_slot(observation, candidate);
        }
    );
    if (matching != g_observations.end()) {
        const bool raster_changed =
            !same_raster(matching->value, candidate);
        if (raster_changed) {
            candidate.revision = g_next_revision++;
        }
        else {
            candidate.revision = matching->value.revision;
        }
        matching->value = std::move(candidate);
        matching->observed_at = now;
        return true;
    }

    const auto prior_slot = std::find_if(
        g_observations.begin(),
        g_observations.end(),
        [&candidate](const TimedObservation& observation) {
            return same_slot(observation, candidate);
        }
    );
    if (prior_slot != g_observations.end() &&
        same_raster(prior_slot->value, candidate)) {
        candidate.revision = prior_slot->value.revision;
    }
    else {
        candidate.revision = g_next_revision++;
    }

    if (g_observations.size() >= kMaximumObservations) {
        const auto oldest = std::min_element(
            g_observations.begin(),
            g_observations.end(),
            [](const TimedObservation& left, const TimedObservation& right) {
                return left.observed_at < right.observed_at;
            }
        );
        g_observations.erase(oldest);
    }
    g_observations.push_back({
        std::move(candidate),
        now,
        candidate_frame_generation
    });
    return true;
}

} // namespace

bool bumble::text_overlay::observe(
    TextKind kind,
    uint32_t x,
    uint32_t y,
    const char* text,
    HorizontalAnchor horizontal_anchor,
    VerticalAnchor vertical_anchor,
    float screen_scale,
    uint32_t face_rgba,
    uint32_t outline_rgba,
    uint64_t stable_slot
) {
    if (text == nullptr) {
        return false;
    }
    Observation observation{};
    observation.kind = kind;
    observation.primitive = OverlayPrimitive::Text;
    observation.x = x;
    observation.y = y;
    observation.horizontal_anchor = horizontal_anchor;
    observation.vertical_anchor = vertical_anchor;
    observation.screen_scale = screen_scale;
    observation.face_rgba = face_rgba;
    observation.outline_rgba = outline_rgba;
    observation.text = text;
    observation.stable_slot = stable_slot;
    return store_observation(std::move(observation));
}

bool bumble::text_overlay::observe_briefing(
    uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    const std::string& text, uint32_t visible_characters
) {
    Observation observation{};
    observation.kind = TextKind::Briefing;
    observation.x = x;
    observation.y = y;
    observation.width = width;
    observation.height = height;
    observation.horizontal_anchor = HorizontalAnchor::Panel;
    observation.text = text;
    observation.panel_reveal = true;
    observation.visible_characters = visible_characters;
    observation.stable_slot = 0x4252494546494E47ull;
    if (!supports_briefing_layout(observation)) {
        return false;
    }
    return store_observation(std::move(observation));
}

bool bumble::text_overlay::observe_menu_backdrop(
    uint32_t rgba,
    TextKind kind,
    bool main_menu
) {
    Observation observation{};
    observation.kind = kind;
    observation.primitive = main_menu
        ? OverlayPrimitive::MainMenuBackdrop
        : OverlayPrimitive::MenuBackdrop;
    observation.face_rgba = rgba;
    observation.outline_rgba = 0u;
    return store_observation(std::move(observation));
}

bool bumble::text_overlay::observe_panel(
    TextKind kind,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t rgba,
    HorizontalAnchor horizontal_anchor,
    VerticalAnchor vertical_anchor,
    uint64_t stable_slot
) {
    Observation observation{};
    observation.kind = kind;
    observation.primitive = OverlayPrimitive::Panel;
    observation.x = x;
    observation.y = y;
    observation.width = width;
    observation.height = height;
    observation.horizontal_anchor = horizontal_anchor;
    observation.vertical_anchor = vertical_anchor;
    observation.face_rgba = rgba;
    observation.outline_rgba = 0u;
    observation.stable_slot = stable_slot;
    return store_observation(std::move(observation));
}

bool bumble::text_overlay::observe_health_honeycomb(
    uint32_t x,
    uint32_t y,
    float fill_fraction,
    uint32_t cell_count
) {
    Observation observation{};
    observation.kind = TextKind::GameplayHud;
    observation.primitive = OverlayPrimitive::HealthHoneycomb;
    observation.x = x;
    observation.y = y;
    observation.horizontal_anchor = HorizontalAnchor::Left;
    observation.vertical_anchor = VerticalAnchor::Top;
    observation.fill_fraction = fill_fraction;
    observation.cell_count = cell_count;
    observation.stable_slot = UINT64_C(0x4845414C54484855);
    return store_observation(std::move(observation));
}

bool bumble::text_overlay::observe_completion_backdrop() {
    Observation observation{};
    observation.kind = TextKind::Completion;
    observation.primitive = OverlayPrimitive::CompletionBackdrop;
    observation.stable_slot = UINT64_C(0x434F4D504C455445);
    return store_observation(std::move(observation));
}

std::vector<bumble::text_overlay::Observation>
bumble::text_overlay::active_observations() {
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(g_mutex);
    remove_expired_locked(now);
    std::vector<Observation> result;
    result.reserve(g_observations.size());
    for (const TimedObservation& observation : g_observations) {
        const uint64_t committed_generation =
            observation.value.kind == TextKind::Menu
                ? g_committed_menu_generation
                : g_committed_frame_generation;
        if (observation.frame_generation != 0u &&
            observation.frame_generation != committed_generation) {
            continue;
        }
        result.push_back(observation.value);
    }
    std::sort(
        result.begin(),
        result.end(),
        [](const Observation& left, const Observation& right) {
            const auto primitive_order = [](OverlayPrimitive primitive) {
                switch (primitive) {
                case OverlayPrimitive::MenuBackdrop:
                case OverlayPrimitive::CompletionBackdrop:
                case OverlayPrimitive::MainMenuBackdrop: return 0u;
                case OverlayPrimitive::Panel: return 1u;
                case OverlayPrimitive::HealthHoneycomb: return 2u;
                case OverlayPrimitive::Text: return 3u;
                }
                return 3u;
            };
            if (left.kind != right.kind) {
                return left.kind < right.kind;
            }
            if (left.primitive != right.primitive) {
                return primitive_order(left.primitive) <
                    primitive_order(right.primitive);
            }
            if (left.y != right.y) {
                return left.y < right.y;
            }
            return left.x < right.x;
        }
    );
    return result;
}

void bumble::text_overlay::begin_frame_observations() {
    std::scoped_lock lock(g_mutex);
    if (g_build_frame_generation != 0u) {
        const uint64_t abandoned_generation = g_build_frame_generation;
        std::erase_if(
            g_observations,
            [abandoned_generation](const TimedObservation& observation) {
                return observation.frame_generation ==
                    abandoned_generation;
            }
        );
    }
    g_build_frame_generation = g_next_frame_generation++;
    if (g_next_frame_generation == 0u) {
        g_next_frame_generation = 1u;
    }
}

void bumble::text_overlay::commit_frame_observations() {
    std::scoped_lock lock(g_mutex);
    if (g_build_frame_generation == 0u) {
        return;
    }
    g_committed_frame_generation = g_build_frame_generation;
    if (std::ranges::any_of(
            g_observations,
            [](const TimedObservation& observation) {
                return observation.frame_generation ==
                        g_build_frame_generation &&
                    observation.value.kind == TextKind::Menu;
            })) {
        g_committed_menu_generation = g_build_frame_generation;
    }
    g_build_frame_generation = 0u;
    std::erase_if(
        g_observations,
        [](const TimedObservation& observation) {
            if (observation.frame_generation == 0u) {
                return false;
            }
            const uint64_t committed_generation =
                observation.value.kind == TextKind::Menu
                    ? g_committed_menu_generation
                    : g_committed_frame_generation;
            return observation.frame_generation != committed_generation;
        }
    );
}

void bumble::text_overlay::set_renderer_ready(bool ready) {
    g_renderer_ready.store(ready, std::memory_order_release);
    if (!ready) {
        clear();
    }
}

bool bumble::text_overlay::renderer_ready() {
    return g_renderer_ready.load(std::memory_order_acquire);
}

void bumble::text_overlay::begin_menu_observations() {
    std::scoped_lock lock(g_mutex);
    if (g_build_menu_generation != 0u) {
        const uint64_t abandoned_generation = g_build_menu_generation;
        std::erase_if(
            g_observations,
            [abandoned_generation](const TimedObservation& observation) {
                return observation.frame_generation == abandoned_generation &&
                    observation.value.kind == TextKind::Menu;
            }
        );
    }
    g_build_menu_generation = g_next_frame_generation++;
    if (g_next_frame_generation == 0u) {
        g_next_frame_generation = 1u;
    }
}

bool bumble::text_overlay::commit_menu_observations() {
    std::scoped_lock lock(g_mutex);
    if (g_build_menu_generation == 0u) {
        return false;
    }
    const uint64_t committed_generation = g_build_menu_generation;
    const bool complete = std::ranges::any_of(
        g_observations,
        [committed_generation](const TimedObservation& observation) {
            return observation.frame_generation == committed_generation &&
                observation.value.kind == TextKind::Menu;
        }
    );
    if (!complete) {
        g_build_menu_generation = 0u;
        std::erase_if(
            g_observations,
            [committed_generation](const TimedObservation& observation) {
                return observation.frame_generation == committed_generation &&
                    observation.value.kind == TextKind::Menu;
            }
        );
        return false;
    }
    g_committed_menu_generation = committed_generation;
    g_build_menu_generation = 0u;
    std::erase_if(
        g_observations,
        [committed_generation](const TimedObservation& observation) {
            return observation.value.kind == TextKind::Menu &&
                observation.frame_generation != 0u &&
                observation.frame_generation != committed_generation;
        }
    );
    return true;
}

void bumble::text_overlay::cancel_menu_observations() {
    std::scoped_lock lock(g_mutex);
    if (g_build_menu_generation == 0u) {
        return;
    }
    const uint64_t abandoned_generation = g_build_menu_generation;
    g_build_menu_generation = 0u;
    std::erase_if(
        g_observations,
        [abandoned_generation](const TimedObservation& observation) {
            return observation.frame_generation == abandoned_generation &&
                observation.value.kind == TextKind::Menu;
        }
    );
}

void bumble::text_overlay::clear_kind(TextKind kind) {
    std::scoped_lock lock(g_mutex);
    if (kind == TextKind::Menu) {
        g_build_menu_generation = 0u;
        g_committed_menu_generation = 0u;
    }
    std::erase_if(
        g_observations,
        [kind](const TimedObservation& observation) {
            return observation.value.kind == kind;
        }
    );
}

void bumble::text_overlay::clear() {
    std::scoped_lock lock(g_mutex);
    g_observations.clear();
    g_build_frame_generation = 0u;
    g_committed_frame_generation = 0u;
    g_build_menu_generation = 0u;
    g_committed_menu_generation = 0u;
}
