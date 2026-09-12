#pragma once

#include <cstdint>
#include <filesystem>

namespace plume {
    struct RenderCommandList;
    struct RenderDevice;
    struct RenderFramebuffer;
    struct RenderInterface;
    struct RenderSwapChain;
}

namespace RT64 {
    struct ShaderLibrary;
}

namespace bumble::text_overlay {

struct Observation;
bool supports_briefing_layout(const Observation& observation);

bool configure_menu_background(
    const std::filesystem::path& asset_directory
);
bool menu_backdrop_available(bool main_menu);
bool bind_renderer(
    plume::RenderInterface* render_interface,
    plume::RenderDevice* device,
    const RT64::ShaderLibrary* shader_library,
    plume::RenderSwapChain* swap_chain
);
void draw(
    plume::RenderCommandList* command_list,
    plume::RenderFramebuffer* framebuffer,
    uint64_t submission_id
);
void submission_complete(uint64_t submission_id);
void release_renderer();

uint32_t last_draw_selected_weapon();

} // namespace bumble::text_overlay
