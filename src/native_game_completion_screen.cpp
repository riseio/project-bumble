#include "native_game_completion_screen.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <json/json.hpp>

#include "native_checkpoint_bridge.hpp"
#include "librecomp/addresses.hpp"
#include "native_input_bindings.hpp"
#include "native_sdl_io.hpp"
#include "native_widescreen.hpp"
#include "native_text_overlay_state.hpp"
#include "native_text_overlay.hpp"
#include "render/rt64_descriptor_sets.h"
#include "render/rt64_shader_library.h"
#include "render/rt64_texture_cache.h"
#include "shared/rt64_video_interface.h"

namespace {

constexpr uint32_t kGameCompletionPhase = 0x00000022u;
constexpr uint32_t kGameCompletionDescriptor = 0x800FDC80u;
constexpr uint32_t kFrontendObject = 0x800FFF80u;
constexpr uint32_t kFrontendPhaseOffset = 0x84u;
constexpr uint32_t kFrontendTransitionOffset = 0x40u;
constexpr uint32_t kCurrentPad = 0x80035F00u;
constexpr uint16_t kButtonA = 0x8000u;
constexpr uint16_t kButtonB = 0x4000u;
constexpr uint16_t kButtonStart = 0x1000u;
constexpr uint32_t kMusicTrackOffset = 0x34u;
constexpr uint32_t kOriginalCompletionMusicTrack = 0x0000000Bu;
constexpr uint32_t kMainMenuMusicTrack = 0x00000000u;
constexpr uint32_t kExpectedWidth = 1672u;
constexpr uint32_t kExpectedHeight = 941u;
constexpr char kExpectedHash[] =
    "b92e87c19e86c4fe5afd57d470bcd7f0da73ce8636a990d060ddeae3848c949b";

struct GameCompletionState {
    std::mutex mutex;
    std::vector<uint8_t> image_bytes;
    std::unique_ptr<RT64::Texture> texture;
    std::unique_ptr<RenderBuffer> upload_buffer;
    std::unique_ptr<RT64::VideoInterfaceDescriptorSet> descriptor_set;
    plume::RenderDevice* device = nullptr;
    const RT64::ShaderLibrary* shader_library = nullptr;
    plume::RenderSwapChain* swap_chain = nullptr;
    std::atomic_bool configured{false};
    std::atomic_bool prepare_observed{false};
    std::atomic_bool prepared_active{false};
    std::atomic_uint32_t prepared_phase{0xFFFFFFFFu};
    std::atomic_uint32_t prepared_descriptor{0xFFFFFFFFu};
    std::atomic_bool active_logged{false};
    std::atomic_bool presented_logged{false};
    std::atomic_bool upload_failure_logged{false};
    std::atomic_bool music_override_logged{false};
    std::atomic_bool input_route_logged{false};
};

GameCompletionState g_state;

struct ScoreDrawScope {
    uint32_t cursor = 0u;
    uint32_t arena_end = 0u;
    bool accepted = false;
};
thread_local ScoreDrawScope g_score_draw_scope{};
thread_local bool g_score_options_return = false;

gpr guest_address(uint32_t address) {
    return static_cast<gpr>(static_cast<int32_t>(address));
}

bool load_contract_and_image(const std::filesystem::path& directory) {
    const std::filesystem::path manifest_path = directory / "manifest.json";
    const std::filesystem::path image_path =
        directory / "BuckBumble_GameCompletion_Background.png";
    std::ifstream manifest_stream(manifest_path);
    if (!manifest_stream) {
        return false;
    }

    nlohmann::json manifest;
    try {
        manifest_stream >> manifest;
        if (manifest.at("schema_version").get<int>() != 1 ||
            manifest.at("origin").get<std::string>() != "user_authored" ||
            !manifest.at("redistribution_authorized").get<bool>() ||
            manifest.at("contains_rom_bytes").get<bool>() ||
            manifest.at("contains_rom_derived_assets").get<bool>()) {
            return false;
        }
        const auto& assets = manifest.at("assets");
        const auto matching = std::find_if(
            assets.begin(),
            assets.end(),
            [](const nlohmann::json& asset) {
                return asset.at("id").get<std::string>() ==
                    "game_completion_background";
            }
        );
        if (matching == assets.end() ||
            matching->at("path").get<std::string>() !=
                "NewImages/BuckBumble_GameCompletion_Background.png" ||
            matching->at("width").get<uint32_t>() != kExpectedWidth ||
            matching->at("height").get<uint32_t>() != kExpectedHeight ||
            matching->at("sha256").get<std::string>() != kExpectedHash) {
            return false;
        }
    }
    catch (const std::exception&) {
        return false;
    }

    std::ifstream image_stream(image_path, std::ios::binary);
    if (!image_stream) {
        return false;
    }
    image_stream.seekg(0, std::ios::end);
    const std::streamoff size = image_stream.tellg();
    image_stream.seekg(0, std::ios::beg);
    if (size < 8) {
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    image_stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    if (!image_stream ||
        bytes[0] != 0x89u || bytes[1] != 0x50u || bytes[2] != 0x4Eu ||
        bytes[3] != 0x47u || bytes[4] != 0x0Du || bytes[5] != 0x0Au ||
        bytes[6] != 0x1Au || bytes[7] != 0x0Au) {
        return false;
    }
    g_state.image_bytes = std::move(bytes);
    return true;
}

} // namespace

bool bumble::game_completion_screen::configure(
    const std::filesystem::path& asset_directory
) {
    std::scoped_lock lock(g_state.mutex);
    g_state.image_bytes.clear();
    g_state.texture.reset();
    g_state.upload_buffer.reset();
    g_state.descriptor_set.reset();
    g_state.configured.store(false, std::memory_order_release);
    g_state.prepare_observed.store(false, std::memory_order_release);
    g_state.prepared_active.store(false, std::memory_order_release);
    g_state.prepared_phase.store(0xFFFFFFFFu, std::memory_order_release);
    g_state.prepared_descriptor.store(
        0xFFFFFFFFu,
        std::memory_order_release
    );
    g_state.active_logged.store(false, std::memory_order_release);
    g_state.presented_logged.store(false, std::memory_order_release);
    g_state.upload_failure_logged.store(false, std::memory_order_release);
    g_state.music_override_logged.store(false, std::memory_order_release);
    g_state.input_route_logged.store(false, std::memory_order_release);
    if (!load_contract_and_image(asset_directory)) {
        std::fprintf(
            stderr,
            "BUMBLE_GAME_COMPLETION_SCREEN stage=asset_rejected path=%s\n",
            asset_directory.string().c_str()
        );
        std::fflush(stderr);
        return false;
    }

    g_state.configured.store(true, std::memory_order_release);
    std::fprintf(
        stderr,
        "BUMBLE_GAME_COMPLETION_SCREEN stage=asset_ready path=%s bytes=%zu"
        " source=user_authored redistribution_authorized=1"
        " rom_derived=0 width=%" PRIu32 " height=%" PRIu32 "\n",
        asset_directory.string().c_str(),
        g_state.image_bytes.size(),
        kExpectedWidth,
        kExpectedHeight
    );
    std::fflush(stderr);
    return true;
}

void bumble::game_completion_screen::prepare_frontend_phase(
    uint8_t* rdram,
    uint32_t phase,
    uint32_t descriptor
) {
    if (rdram != nullptr && g_score_options_return && phase != 0x17u) {
        const gpr viewer = guest_address(0x800FD190u);
        if (MEM_W(0, viewer) == 0x0F && MEM_W(4, viewer) == 0x0F) {
            MEM_W(0, viewer) = 0x12;
            MEM_W(4, viewer) = 0x12;
        }
        g_score_options_return = false;
    }
    const bool exact_completion = phase == kGameCompletionPhase &&
        descriptor == kGameCompletionDescriptor;
    g_state.prepared_phase.store(phase, std::memory_order_release);
    g_state.prepared_descriptor.store(descriptor, std::memory_order_release);
    g_state.prepare_observed.store(true, std::memory_order_release);
    const bool was_active = g_state.prepared_active.exchange(
        exact_completion,
        std::memory_order_acq_rel
    );

    if (rdram != nullptr) {
        if (exact_completion) {
            MEM_W(
                kMusicTrackOffset,
                guest_address(kGameCompletionDescriptor)
            ) = kMainMenuMusicTrack;
            if (!g_state.music_override_logged.exchange(
                    true,
                    std::memory_order_acq_rel
                )) {
                std::fprintf(
                    stderr,
                    "BUMBLE_GAME_COMPLETION_SCREEN"
                    " stage=main_menu_music_routed"
                    " phase=0x%08" PRIX32
                    " descriptor=0x%08" PRIX32
                    " original_track=%" PRIu32
                    " requested_track=%" PRIu32
                    " owner=func_800AAC0C\n",
                    phase,
                    descriptor,
                    kOriginalCompletionMusicTrack,
                    kMainMenuMusicTrack
                );
                std::fflush(stderr);
            }
        }
        else if (was_active) {
            MEM_W(
                kMusicTrackOffset,
                guest_address(kGameCompletionDescriptor)
            ) = kOriginalCompletionMusicTrack;
            g_state.music_override_logged.store(
                false,
                std::memory_order_release
            );
        }
    }

    if (exact_completion && !was_active) {
        bumble::text_overlay::clear_kind(
            bumble::text_overlay::TextKind::Script
        );
        g_state.input_route_logged.store(false, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_GAME_COMPLETION_SCREEN stage=prepared_before_guest_init"
            " phase=0x%08" PRIX32 " descriptor=0x%08" PRIX32
            " composition_owner=committed_frame stale_script_cleared=1\n",
            phase,
            descriptor
        );
        std::fflush(stderr);
    }
}

bool bumble::game_completion_screen::active() {
    if (g_state.prepare_observed.load(std::memory_order_acquire)) {
        return g_state.prepared_active.load(std::memory_order_acquire);
    }
    return bumble::native_checkpoint::last_frontend_phase() ==
            kGameCompletionPhase &&
        bumble::native_checkpoint::last_frontend_descriptor() ==
            kGameCompletionDescriptor;
}

void bumble::game_completion_screen::observe_frame() {
    if (!configured() || !active() ||
        !bumble::text_overlay::observe_completion_backdrop()) {
        return;
    }
    // Publish on the guest thread.
    (void)bumble::text_overlay::observe(
        bumble::text_overlay::TextKind::Completion, 202u, 22u, "GAME COMPLETE!",
        bumble::text_overlay::HorizontalAnchor::Center,
        bumble::text_overlay::VerticalAnchor::Top, 1.18f, 0xFFD64AFFu,
        0x000000FFu, UINT64_C(0x434F4D505449544C));
}

void bumble::game_completion_screen::bind_renderer(
    plume::RenderDevice* device,
    const RT64::ShaderLibrary* shader_library,
    plume::RenderSwapChain* swap_chain
) {
    std::scoped_lock lock(g_state.mutex);
    g_state.device = device;
    g_state.shader_library = shader_library;
    g_state.swap_chain = swap_chain;
}

void bumble::game_completion_screen::draw(
    plume::RenderCommandList* command_list,
    plume::RenderFramebuffer* framebuffer
) {
    if (!g_state.configured.load(std::memory_order_acquire)) {
        return;
    }

    if (!g_state.active_logged.exchange(true, std::memory_order_acq_rel)) {
        std::fprintf(
            stderr,
            "BUMBLE_GAME_COMPLETION_SCREEN stage=completion_phase_observed"
            " phase=0x%08" PRIX32 " descriptor=0x%08" PRIX32
            " owner=committed_overlay_composition\n",
            kGameCompletionPhase,
            kGameCompletionDescriptor
        );
        std::fflush(stderr);
    }

    std::scoped_lock lock(g_state.mutex);
    if (command_list == nullptr || framebuffer == nullptr ||
        g_state.device == nullptr || g_state.shader_library == nullptr ||
        g_state.swap_chain == nullptr || g_state.image_bytes.empty()) {
        return;
    }
    if (g_state.texture == nullptr) {
        g_state.texture.reset(RT64::TextureCache::loadTextureFromBytes(
            g_state.device,
            command_list,
            g_state.image_bytes,
            g_state.upload_buffer
        ));
        if (g_state.texture == nullptr ||
            g_state.texture->width != kExpectedWidth ||
            g_state.texture->height != kExpectedHeight) {
            g_state.texture.reset();
            if (!g_state.upload_failure_logged.exchange(
                    true,
                    std::memory_order_acq_rel
                )) {
                std::fprintf(
                    stderr,
                    "BUMBLE_GAME_COMPLETION_SCREEN stage=gpu_upload_failed\n"
                );
                std::fflush(stderr);
            }
            return;
        }
        command_list->barriers(
            RenderBarrierStage::GRAPHICS,
            RenderTextureBarrier(
                g_state.texture->texture.get(),
                RenderTextureLayout::SHADER_READ
            )
        );
    }

    const RT64::ShaderRecord& shader =
        g_state.shader_library->videoInterfaceLinear;
    const RenderSampler* sampler =
        g_state.shader_library->samplerLibrary.linear.borderBorder.get();
    if (g_state.descriptor_set == nullptr) {
        g_state.descriptor_set =
            std::make_unique<RT64::VideoInterfaceDescriptorSet>(
                sampler,
                g_state.device
            );
        g_state.descriptor_set->setTexture(
            g_state.descriptor_set->gInput,
            g_state.texture->texture.get(),
            RenderTextureLayout::SHADER_READ
        );
    }

    const uint32_t width = g_state.swap_chain->getWidth();
    const uint32_t height = g_state.swap_chain->getHeight();
    if (width == 0u || height == 0u) {
        return;
    }
    const float target_width = static_cast<float>(width);
    const float target_height = static_cast<float>(height);
    const float source_aspect =
        static_cast<float>(g_state.texture->width) /
        static_cast<float>(g_state.texture->height);
    const float target_aspect = target_width / target_height;
    float draw_width = target_width;
    float draw_height = target_height;
    float draw_x = 0.0f;
    float draw_y = 0.0f;
    if (target_aspect > source_aspect) {
        draw_height = target_width / source_aspect;
        draw_y = (target_height - draw_height) * 0.5f;
    }
    else if (target_aspect < source_aspect) {
        draw_width = target_height * source_aspect;
        draw_x = (target_width - draw_width) * 0.5f;
    }
    const RenderViewport viewport(draw_x, draw_y, draw_width, draw_height);
    const RenderRect scissor(0, 0, width, height);
    interop::VideoInterfaceCB constants{};
    constants.videoResolution = {
        static_cast<float>(g_state.texture->width),
        static_cast<float>(g_state.texture->height)
    };
    constants.textureResolution = constants.videoResolution;
    constants.gamma = 1.0f;

    command_list->setFramebuffer(framebuffer);
    command_list->setViewports(viewport);
    command_list->setScissors(scissor);
    command_list->setPipeline(shader.pipeline.get());
    command_list->setGraphicsPipelineLayout(shader.pipelineLayout.get());
    command_list->setGraphicsDescriptorSet(g_state.descriptor_set->get(), 0);
    command_list->setGraphicsPushConstants(0, &constants);
    command_list->setVertexBuffers(0, nullptr, 0, nullptr);
    command_list->drawInstanced(3, 1, 0, 0);

    if (!g_state.presented_logged.exchange(
            true,
            std::memory_order_acq_rel
        )) {
        std::fprintf(
            stderr,
            "BUMBLE_GAME_COMPLETION_SCREEN stage=overlay_presented"
            " source=user_authored full_framebuffer=1 width=%" PRIu32
            " height=%" PRIu32
            " scale=cover_preserve_aspect title_owner=committed_composition\n",
            width,
            height
        );
        std::fflush(stderr);
    }
}

void bumble::game_completion_screen::release_renderer() {
    std::scoped_lock lock(g_state.mutex);
    g_state.descriptor_set.reset();
    g_state.texture.reset();
    g_state.upload_buffer.reset();
    g_state.device = nullptr;
    g_state.shader_library = nullptr;
    g_state.swap_chain = nullptr;
}

bool bumble::game_completion_screen::configured() {
    return g_state.configured.load(std::memory_order_acquire);
}

bool bumble::game_completion_screen::open_high_scores_from_options(uint8_t* rdram) {
    if (rdram == nullptr || recomp::mem_size < 0x00200000u) { return false; }
    const gpr viewer = guest_address(0x800FD190u);
    if (MEM_W(0, guest_address(0x800FE5CCu)) != static_cast<int32_t>(0x800FD190u) ||
        MEM_W(0, viewer) != 0x12 || MEM_W(4, viewer) != 0x12 ||
        MEM_W(0x1C, viewer) != 0x23 || MEM_W(0x14, viewer) != 0 ||
        MEM_W(0x4C, viewer) != 0 || MEM_W(0x54, viewer) != 0 ||
        MEM_W(0x64, viewer) != 0 ||
        MEM_W(0x5C, viewer) != static_cast<int32_t>(0x800B11B8u)) {
        return false;
    }
    MEM_W(0, viewer) = 0x0F;
    MEM_W(4, viewer) = 0x0F;
    g_score_options_return = true;
    return true;
}

extern "C" void bumble_handle_native_game_completion_input(
    uint8_t* rdram,
    recomp_context* context
) {
    if (rdram == nullptr || context == nullptr) {
        return;
    }

    const uint32_t live_object =
        static_cast<uint32_t>(context->r19) | 0x80000000u;
    const uint32_t descriptor =
        static_cast<uint32_t>(context->r18) | 0x80000000u;
    if (live_object != kFrontendObject ||
        descriptor != kGameCompletionDescriptor ||
        static_cast<uint32_t>(
            MEM_W(kFrontendPhaseOffset, guest_address(kFrontendObject))
        ) != kGameCompletionPhase ||
        static_cast<uint32_t>(
            MEM_W(kFrontendTransitionOffset, guest_address(kFrontendObject))
        ) != 0u) {
        return;
    }

    const gpr current_pad = guest_address(kCurrentPad);
    const uint16_t held = MEM_HU(0, current_pad);
    const uint16_t pressed = MEM_HU(2, current_pad);
    if ((static_cast<uint32_t>(context->r2) & kButtonA) != 0u) {
        if (!g_state.input_route_logged.exchange(
                true,
                std::memory_order_acq_rel
            )) {
            std::fprintf(
                stderr,
                "BUMBLE_GAME_COMPLETION_SCREEN stage=input_confirm_observed"
                " phase=0x%08" PRIX32
                " descriptor=0x%08" PRIX32
                " pressed=0x%04" PRIX16
                " successor=0x%08" PRIX32
                " owner=frontend_menu_update_original_a\n",
                kGameCompletionPhase,
                kGameCompletionDescriptor,
                pressed,
                static_cast<uint32_t>(MEM_W(4, guest_address(kGameCompletionDescriptor)))
            );
            std::fflush(stderr);
        }
        return;
    }
    const uint16_t alternate_exit =
        static_cast<uint16_t>(pressed & (kButtonB | kButtonStart));
    if (alternate_exit == 0u) {
        return;
    }

    const uint16_t normalized_pressed = static_cast<uint16_t>(
        (pressed & ~(kButtonB | kButtonStart)) | kButtonA
    );
    const uint16_t normalized_held = static_cast<uint16_t>(
        held & ~(kButtonB | kButtonStart)
    );
    MEM_H(0, current_pad) = static_cast<int16_t>(normalized_held);
    MEM_H(2, current_pad) = static_cast<int16_t>(normalized_pressed);
    context->r2 = static_cast<gpr>(
        (static_cast<uint32_t>(context->r2) & ~kButtonB) | kButtonA
    );

    if (!g_state.input_route_logged.exchange(
            true,
            std::memory_order_acq_rel
        )) {
        std::fprintf(
            stderr,
            "BUMBLE_GAME_COMPLETION_SCREEN stage=input_normalized"
            " phase=0x%08" PRIX32
            " descriptor=0x%08" PRIX32
            " pressed_before=0x%04" PRIX16
            " pressed_after=0x%04" PRIX16
            " successor=0x%08" PRIX32
            " dynamic_score_successor_preserved=1\n",
            kGameCompletionPhase,
            kGameCompletionDescriptor,
            pressed,
            normalized_pressed,
            static_cast<uint32_t>(MEM_W(4, guest_address(kGameCompletionDescriptor)))
        );
        std::fflush(stderr);
    }
}

extern "C" void bumble_begin_native_high_scores(uint8_t* rdram, recomp_context* context) {
    g_score_draw_scope = {};
    if (rdram == nullptr || context == nullptr ||
        !bumble::text_overlay::renderer_ready() ||
        !bumble::text_overlay::menu_backdrop_available(false) ||
        static_cast<uint32_t>(context->r4) != kFrontendObject) {
        return;
    }
    const uint32_t phase = static_cast<uint32_t>(context->r5);
    const uint32_t expected_descriptor = phase == 0x17u ? 0x800FD190u :
        phase == 0x0Du ? 0x800FD260u : phase == 0x1Eu ? 0x800FD920u : 0u;
    if (expected_descriptor == 0u ||
        static_cast<uint32_t>(MEM_W(0, guest_address(0x800FE570u + phase * 4u))) !=
            expected_descriptor ||
        static_cast<uint32_t>(MEM_W(0, guest_address(kFrontendObject))) != phase) {
        return;
    }
    if (phase == 0x0Du && MEM_W(0, guest_address(0x800FE768u)) == 0) {
        return; // Attract animation, not scores.
    }
    constexpr uint32_t table = 0x800E96CCu;
    constexpr uint32_t stride = 0x14u;
    static_assert(table + stride * 8u < 0x80200000u);
    if (recomp::mem_size < 0x00200000u) {
        return;
    }
    const bool entry = phase == 0x1Eu;
    const uint32_t selected = entry
        ? static_cast<uint32_t>(MEM_W(0, guest_address(0x80107BECu))) : UINT32_MAX;
    if (entry && selected >= 8u) {
        return;
    }
    struct Row { std::string name; uint32_t score; uint32_t mission; };
    std::array<Row, 8> rows{};
    for (uint32_t index = 0u; index < rows.size(); ++index) {
        const uint32_t record = table + stride * index;
        Row& row = rows[index];
        row.score = static_cast<uint32_t>(MEM_W(0, guest_address(record)));
        row.mission = static_cast<uint32_t>(MEM_W(4, guest_address(record)));
        bool terminated = false;
        for (uint32_t character = 0u; character < stride - 8u; ++character) {
            const uint8_t value = MEM_BU(8u + character, guest_address(record));
            if (value == 0u) { terminated = true; break; }
            if (value == 0x7Fu && entry && index == selected) {
                row.name += "[END]";
            } else if (value >= 0x20u && value <= 0x7Eu) {
                row.name += static_cast<char>(value);
            } else {
                return;
            }
        }
        if (!terminated) { return; }
        if (row.name.empty()) { row.name = "-"; }
        if (entry && index == selected && row.name.find("[END]") == std::string::npos) {
            row.name.insert(row.name.size() - 1u, "[");
            row.name += "]";
        }
    }
    constexpr uint32_t cursor_address = 0x80035F30u;
    const uint32_t cursor = static_cast<uint32_t>(MEM_W(0, guest_address(cursor_address)));
    const uint32_t arena_end = bumble::widescreen::display_list_arena_end();
    if (cursor < bumble::widescreen::display_list_arena_base() ||
        cursor >= arena_end || (cursor & 7u) != 0u) {
        return;
    }
    using namespace bumble::text_overlay;
    constexpr uint32_t gold = 0xFFD64AFFu;
    constexpr uint32_t white = 0xFFF4D8FFu;
    const auto label = [](uint32_t x, uint32_t y, const std::string& text,
                         uint32_t color, float scale = 0.70f) {
        return observe(TextKind::HighScores, x, y, text.c_str(),
            HorizontalAnchor::Center, VerticalAnchor::Authored, scale,
            color, 0x000000FFu);
    };
    bool accepted = observe_menu_backdrop(0x162A3DFFu, TextKind::HighScores);
    accepted &= observe_panel(TextKind::HighScores, 32u, 38u, 256u, 159u,
        0x081421E8u, HorizontalAnchor::Center);
    accepted &= observe(TextKind::HighScores, 160u, 20u,
        entry ? "NEW HIGH SCORE" : "HIGH SCORES", HorizontalAnchor::CenterText,
        VerticalAnchor::Authored, 0.95f, gold);
    accepted &= label(40u, 47u, "#", gold);
    accepted &= label(70u, 47u, "NAME", gold);
    accepted &= label(157u, 47u, "SCORE", gold);
    accepted &= label(237u, 47u, "MISSION", gold);
    const bool show_rows = phase != 0x0Du || MEM_W(0, guest_address(0x800FE768u)) != 0;
    if (show_rows) {
        for (uint32_t index = 0u; index < rows.size(); ++index) {
            const Row& row = rows[index];
            const uint32_t y = 65u + index * 17u;
            const uint32_t color = index == selected ? gold : white;
            accepted &= label(40u, y, std::to_string(index + 1u), color);
            accepted &= label(70u, y, row.name, color);
            accepted &= label(157u, y, std::to_string(row.score), color);
            accepted &= label(237u, y, row.mission == 99u ? "CLEAR" :
                std::to_string(row.mission), color);
        }
    }
    const auto profile = bumble::native_io::keyboard_mouse_gameplay_active()
        ? bumble::input_bindings::BindingProfile::KeyboardMouse
        : bumble::input_bindings::BindingProfile::Controller;
    const std::string confirm = bumble::input_bindings::binding_text(profile,
        bumble::input_bindings::InputAction::MenuConfirm);
    const std::string back = bumble::input_bindings::binding_text(profile,
        bumble::input_bindings::InputAction::MenuBack);
    const std::string prompt = entry
        ? "Choose letter  |  " + confirm + ": accept  |  " + back + ": erase"
        : confirm + " / " + back + ": return";
    accepted &= observe(TextKind::HighScores, 160u, 214u, prompt.c_str(),
        HorizontalAnchor::CenterText, VerticalAnchor::Authored, 0.60f, white);
    if (!accepted) {
        clear_kind(TextKind::HighScores);
        return;
    }
    g_score_draw_scope = {cursor, arena_end, true};
}

extern "C" void bumble_end_native_high_scores(uint8_t* rdram, recomp_context*) {
    const ScoreDrawScope scope = g_score_draw_scope;
    g_score_draw_scope = {};
    if (rdram == nullptr || !scope.accepted) { return; }
    constexpr uint32_t cursor_address = 0x80035F30u;
    const uint32_t cursor = static_cast<uint32_t>(MEM_W(0, guest_address(cursor_address)));
    if (cursor >= scope.cursor && cursor <= scope.arena_end) {
        MEM_W(0, guest_address(cursor_address)) = scope.cursor;
    } else {
        bumble::text_overlay::clear_kind(bumble::text_overlay::TextKind::HighScores);
    }
}
