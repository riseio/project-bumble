#include "reviewed_runtime_exports.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>

#include "ultramodern/ultra64.h"

namespace {
std::atomic_uint64_t g_mirrored_registration_count{0};
}

extern "C" void buck_osSetEventMesg_bridge(
    uint8_t* rdram,
    uint32_t event_id,
    int32_t queue,
    int32_t message
) {
    switch (event_id) {
        case OS_EVENT_SP:
        case OS_EVENT_SI:
        case OS_EVENT_DP: {
            const uint64_t count =
                g_mirrored_registration_count.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count <= 16 || count == 600) {
                std::fprintf(
                    stderr,
                    "native_event_registration_mirrored count=%" PRIu64 " event=%" PRIu32
                    " queue=0x%08" PRIX32 " message=0x%08" PRIX32 "\n",
                    count,
                    event_id,
                    static_cast<uint32_t>(queue),
                    static_cast<uint32_t>(message)
                );
            }
            ::osSetEventMesg(
                rdram,
                static_cast<OSEvent>(event_id),
                queue,
                static_cast<OSMesg>(message)
            );
            break;
        }
        default:
            break;
    }
}
