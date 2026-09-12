#pragma once

#include <cstdint>
#include <filesystem>

#include "recomp.h"

namespace plume {
    struct RenderCommandList;
    struct RenderDevice;
    struct RenderFramebuffer;
    struct RenderSwapChain;
}

namespace RT64 {
    struct ShaderLibrary;
}

namespace bumble::death_screen {

bool configure(const std::filesystem::path& asset_directory);
void prepare_frontend_phase(uint32_t phase, uint32_t descriptor);
void bind_renderer(
    plume::RenderDevice* device,
    const RT64::ShaderLibrary* shader_library,
    plume::RenderSwapChain* swap_chain
);
void draw(
    plume::RenderCommandList* command_list,
    plume::RenderFramebuffer* framebuffer
);
void release_renderer();
bool configured();

} // namespace bumble::death_screen

extern "C" void bumble_death_screen_observe_text(
    uint8_t* rdram,
    recomp_context* context
);
