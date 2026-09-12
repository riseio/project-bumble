#pragma once

#include <cstdint>

namespace bumble::native_campaign_level {

enum class ObservationSite : uint32_t {
    PlayerUpdate = 1u,
    GraphicsTaskSubmit = 2u,
};

void observe(uint8_t* rdram, ObservationSite site);

} // namespace bumble::native_campaign_level
