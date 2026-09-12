#include <atomic>
#include <cinttypes>
#include <cstdio>

#include "recomp.h"

namespace {

std::atomic_uint32_t g_si_busy_override_count{0};

} // namespace

extern "C" void buck_si_device_busy_clear(uint8_t*, recomp_context* context) {
    context->r4 = 0;
    const uint32_t count =
        g_si_busy_override_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 8 || count == 60 || count == 600) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=si_busy_native_override count=%" PRIu32
            " load_pc=0x80028B34 hook_pc=0x80028B38 status=0\n",
            count
        );
        std::fflush(stderr);
    }
}
