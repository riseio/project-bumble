#pragma once

#include <filesystem>

#include "recomp.h"
#include "native_visible_ui_state.hpp"

namespace plume {
    struct RenderCommandList;
    struct RenderDevice;
    struct RenderFramebuffer;
    struct RenderSwapChain;
}

namespace RT64 {
    struct ShaderLibrary;
}

namespace bumble::game_completion_screen {

bool configure(const std::filesystem::path& asset_directory);
void observe_frame();
void prepare_frontend_phase(
    uint8_t* rdram,
    uint32_t phase,
    uint32_t descriptor
);
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
bool open_high_scores_from_options(uint8_t* rdram);

} // namespace bumble::game_completion_screen

extern "C" void bumble_handle_native_game_completion_input(
    uint8_t* rdram,
    recomp_context* context
);
