#pragma once

#include <cstdint>

namespace bumble::frame_pacing {

constexpr uint32_t kFallbackDisplayHz = 60u;
constexpr uint32_t kOriginalGameplayHz = 30u;
constexpr uint32_t capped_target_hz(uint32_t requested_hz, uint32_t display_hz) {
    const uint32_t available_hz = display_hz == 0u
        ? kFallbackDisplayHz
        : display_hz;
    return requested_hz < available_hz
        ? requested_hz
        : available_hz;
}

static_assert(capped_target_hz(60u, 64u) == 60u);
static_assert(capped_target_hz(120u, 90u) == 90u);
static_assert(capped_target_hz(120u, 144u) == 120u);
static_assert(capped_target_hz(120u, 64u) == 64u);

struct Statistics {
    uint64_t interval_count = 0;
    double mean_interval_ms = 0.0;
    double p50_interval_ms = 0.0;
    double p95_interval_ms = 0.0;
    double p99_interval_ms = 0.0;
    double maximum_interval_ms = 0.0;
    uint64_t intervals_over_150_percent = 0;
    uint64_t intervals_over_200_percent = 0;
    uint64_t intervals_over_16_67_ms = 0;
    uint64_t intervals_over_33_33_ms = 0;
    uint32_t target_hz = 0;
};

bool install();
void uninstall();

void configure(uint32_t target_hz);
Statistics consume_statistics();

} // namespace bumble::frame_pacing
