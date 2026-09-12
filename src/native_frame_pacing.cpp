#include "native_frame_pacing.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>

#include "common/rt64_timer.h"
#include "native_death_screen.hpp"
#include "native_game_completion_screen.hpp"
#include "native_graphics_options.hpp"
#include "native_rt64_renderer.hpp"
#include "native_text_overlay.hpp"
#include "rhi/rt64_render_hooks.h"

namespace {

constexpr double kHistogramBinMilliseconds = 0.25;
constexpr size_t kHistogramBinCount = 256;

struct TimingState {
    std::mutex mutex;
    bool installed = false;
    uint32_t target_hz = 0;
    RT64::Timestamp previous_present{};
    uint64_t interval_count = 0;
    uint64_t total_interval_nanoseconds = 0;
    uint64_t maximum_interval_nanoseconds = 0;
    uint64_t intervals_over_150_percent = 0;
    uint64_t intervals_over_200_percent = 0;
    uint64_t intervals_over_16_67_ms = 0;
    uint64_t intervals_over_33_33_ms = 0;
    std::array<uint64_t, kHistogramBinCount> histogram{};
    RT64::RenderHookInit* previous_init = nullptr;
    RT64::RenderHookDraw* previous_draw = nullptr;
    RT64::RenderHookDrawComplete* previous_draw_complete = nullptr;
    RT64::RenderHookPresent* previous_present_hook = nullptr;
    RT64::RenderHookDeinit* previous_deinit = nullptr;
};

TimingState g_timing;

bool performance_reporting_enabled() {
    static const bool enabled = []() {
        const char* report_path =
            std::getenv("BUMBLE_RT64_FRAME_PACING_REPORT");
        return report_path != nullptr && report_path[0] != '\0';
    }();
    return enabled;
}

uint64_t target_interval_nanoseconds(uint32_t target_hz) {
    return target_hz == 0u ? 0u : 1'000'000'000ull / target_hz;
}

void clear_window_locked(TimingState& state) {
    state.interval_count = 0;
    state.total_interval_nanoseconds = 0;
    state.maximum_interval_nanoseconds = 0;
    state.intervals_over_150_percent = 0;
    state.intervals_over_200_percent = 0;
    state.intervals_over_16_67_ms = 0;
    state.intervals_over_33_33_ms = 0;
    state.histogram.fill(0);
}

void record_interval_locked(TimingState& state, uint64_t nanoseconds) {
    if (nanoseconds == 0u) {
        return;
    }

    ++state.interval_count;
    state.total_interval_nanoseconds += nanoseconds;
    state.maximum_interval_nanoseconds = std::max(
        state.maximum_interval_nanoseconds,
        nanoseconds
    );

    const uint64_t target_ns = target_interval_nanoseconds(state.target_hz);
    if (target_ns > 0u) {
        if (nanoseconds * 2u > target_ns * 3u) {
            ++state.intervals_over_150_percent;
        }
        if (nanoseconds > target_ns * 2u) {
            ++state.intervals_over_200_percent;
        }
    }
    if (nanoseconds > 16'666'667ull) {
        ++state.intervals_over_16_67_ms;
    }
    if (nanoseconds > 33'333'333ull) {
        ++state.intervals_over_33_33_ms;
    }

    const double milliseconds = static_cast<double>(nanoseconds) / 1'000'000.0;
    const size_t bin = std::min(
        static_cast<size_t>(milliseconds / kHistogramBinMilliseconds),
        kHistogramBinCount - 1u
    );
    ++state.histogram[bin];
}

double percentile_milliseconds(
    const std::array<uint64_t, kHistogramBinCount>& histogram,
    uint64_t count,
    double percentile
) {
    if (count == 0u) {
        return 0.0;
    }

    const uint64_t wanted = std::max<uint64_t>(
        1u,
        static_cast<uint64_t>(std::ceil(percentile * static_cast<double>(count)))
    );
    uint64_t accumulated = 0;
    for (size_t index = 0; index < histogram.size(); ++index) {
        accumulated += histogram[index];
        if (accumulated >= wanted) {
            return (static_cast<double>(index) + 0.5) *
                kHistogramBinMilliseconds;
        }
    }
    return static_cast<double>(kHistogramBinCount) *
        kHistogramBinMilliseconds;
}

void presentation_submission_hook(
    RenderCommandList* command_list,
    RenderFramebuffer* framebuffer,
    uint64_t submission_id
) {
    bumble::graphics_options::complete_main_menu_handoff();
    bumble::text_overlay::draw(
        command_list,
        framebuffer,
        submission_id
    );
    bumble::death_screen::draw(command_list, framebuffer);
    if (g_timing.previous_draw != nullptr) {
        g_timing.previous_draw(
            command_list,
            framebuffer,
            submission_id
        );
    }
}

void presentation_submission_complete(uint64_t submission_id) {
    bumble::text_overlay::submission_complete(submission_id);
    if (g_timing.previous_draw_complete != nullptr) {
        g_timing.previous_draw_complete(submission_id);
    }
}

void presentation_observed_hook() {
    if (performance_reporting_enabled()) {
        std::scoped_lock lock(g_timing.mutex);
        const RT64::Timestamp now = RT64::Timer::current();
        if (g_timing.previous_present != RT64::Timestamp{}) {
            const uint64_t interval_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - g_timing.previous_present
                ).count()
            );
            record_interval_locked(g_timing, interval_ns);
        }
        g_timing.previous_present = now;
    }

    // Call the previous hook outside the statistics lock to avoid reentrant locking.
    if (g_timing.previous_present_hook != nullptr) {
        g_timing.previous_present_hook();
    }
}

void initialize_render_overlays(
    plume::RenderInterface* rhi,
    plume::RenderDevice* device
) {
    if (g_timing.previous_init != nullptr) {
        g_timing.previous_init(rhi, device);
    }
}

void release_render_overlays() {
    bumble::rt64_renderer::release_diagnostic_capture_resources();
    bumble::game_completion_screen::release_renderer();
    bumble::text_overlay::release_renderer();
    bumble::death_screen::release_renderer();
    if (g_timing.previous_deinit != nullptr) {
        g_timing.previous_deinit();
    }
}

} // namespace

bool bumble::frame_pacing::install() {
    std::scoped_lock lock(g_timing.mutex);
    if (g_timing.installed) {
        return true;
    }
    g_timing.previous_init = RT64::GetRenderHookInit();
    g_timing.previous_draw = RT64::GetRenderHookDraw();
    g_timing.previous_draw_complete = RT64::GetRenderHookDrawComplete();
    g_timing.previous_present_hook = RT64::GetRenderHookPresent();
    g_timing.previous_deinit = RT64::GetRenderHookDeinit();

    RT64::SetRenderHooks(
        &initialize_render_overlays,
        &presentation_submission_hook,
        &release_render_overlays
    );
    RT64::SetRenderHookPresent(&presentation_observed_hook);
    RT64::SetRenderHookDrawComplete(
        &presentation_submission_complete
    );
    g_timing.installed = true;
    g_timing.previous_present = {};
    clear_window_locked(g_timing);
    return true;
}

void bumble::frame_pacing::uninstall() {
    std::scoped_lock lock(g_timing.mutex);
    if (g_timing.installed) {
        RT64::SetRenderHooks(
            g_timing.previous_init,
            g_timing.previous_draw,
            g_timing.previous_deinit
        );
        RT64::SetRenderHookPresent(g_timing.previous_present_hook);
        RT64::SetRenderHookDrawComplete(
            g_timing.previous_draw_complete
        );
    }
    g_timing.installed = false;
    g_timing.target_hz = 0;
    g_timing.previous_present = {};
    g_timing.previous_init = nullptr;
    g_timing.previous_draw = nullptr;
    g_timing.previous_draw_complete = nullptr;
    g_timing.previous_present_hook = nullptr;
    g_timing.previous_deinit = nullptr;
    clear_window_locked(g_timing);
}

void bumble::frame_pacing::configure(uint32_t target_hz) {
    std::scoped_lock lock(g_timing.mutex);
    g_timing.target_hz = target_hz;
    g_timing.previous_present = {};
    clear_window_locked(g_timing);
}

bumble::frame_pacing::Statistics
bumble::frame_pacing::consume_statistics() {
    std::scoped_lock lock(g_timing.mutex);
    Statistics result{};
    result.interval_count = g_timing.interval_count;
    result.target_hz = g_timing.target_hz;
    if (g_timing.interval_count > 0u) {
        result.mean_interval_ms =
            static_cast<double>(g_timing.total_interval_nanoseconds) /
            static_cast<double>(g_timing.interval_count) /
            1'000'000.0;
        result.p50_interval_ms = percentile_milliseconds(
            g_timing.histogram,
            g_timing.interval_count,
            0.50
        );
        result.p95_interval_ms = percentile_milliseconds(
            g_timing.histogram,
            g_timing.interval_count,
            0.95
        );
        result.p99_interval_ms = percentile_milliseconds(
            g_timing.histogram,
            g_timing.interval_count,
            0.99
        );
        result.maximum_interval_ms =
            static_cast<double>(g_timing.maximum_interval_nanoseconds) /
            1'000'000.0;
    }
    result.intervals_over_150_percent =
        g_timing.intervals_over_150_percent;
    result.intervals_over_200_percent =
        g_timing.intervals_over_200_percent;
    result.intervals_over_16_67_ms =
        g_timing.intervals_over_16_67_ms;
    result.intervals_over_33_33_ms =
        g_timing.intervals_over_33_33_ms;
    clear_window_locked(g_timing);
    return result;
}
