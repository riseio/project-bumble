#pragma once

#include <cstdint>
#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace bumble::rt64_renderer {

std::unique_ptr<ultramodern::renderer::RendererContext> create(
    uint8_t* rdram,
    ultramodern::renderer::WindowHandle window_handle,
    bool developer_mode
);

// Wake render queues before joining. Idempotent; false means initialization is pending.
// Retry on false rather than exiting the host.
bool request_shutdown();

void release_diagnostic_capture_resources();

uint32_t display_list_count();
uint32_t screen_update_count();
bool mission2_screen_presented();

void set_fog_scale(float scale);
float fog_scale();

bool raytracing_supported();

void set_contract_validation_enabled(bool enabled);

} // namespace bumble::rt64_renderer
