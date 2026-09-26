#include "native_death_screen.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <json/json.hpp>

#include "native_checkpoint_bridge.hpp"
#include "render/rt64_descriptor_sets.h"
#include "render/rt64_shader_library.h"
#include "render/rt64_texture_cache.h"
#include "shared/rt64_video_interface.h"

namespace {

constexpr uint32_t kRdramSize = 8u * 1024u * 1024u;
constexpr uint32_t kFrontendObject = 0x800FFF80u;
constexpr uint32_t kFrontendPhaseOffset = 0x84u;
constexpr uint32_t kInvalidFrontendPhase = 0xFFFFFFFFu;
constexpr uint32_t kMissionFailurePhase = 0x1Cu;
constexpr uint32_t kMissionFailureDescriptor = 0x800FD7D8u;
constexpr auto kPhaseSynchronizationGrace = std::chrono::milliseconds(750);
constexpr uint32_t kExpectedWidth = 1456u;
constexpr uint32_t kExpectedHeight = 1080u;
constexpr char kExpectedHash[] =
    "a85bc8f93bdcede04745bd48d19e7b179606493fd407cca572dad919b1926037";

struct DeathScreenState {
    std::mutex mutex;
    std::vector<uint8_t> image_bytes;
    std::unique_ptr<RT64::Texture> texture;
    std::unique_ptr<RenderBuffer> upload_buffer;
    std::unique_ptr<RT64::VideoInterfaceDescriptorSet> descriptor_set;
    plume::RenderDevice* device = nullptr;
    const RT64::ShaderLibrary* shader_library = nullptr;
    plume::RenderSwapChain* swap_chain = nullptr;
    std::atomic_int64_t activation_ns{0};
    std::atomic_uint32_t activation_phase{kInvalidFrontendPhase};
    std::atomic_uint32_t prepared_phase{kInvalidFrontendPhase};
    std::atomic_uint32_t prepared_descriptor{kInvalidFrontendPhase};
    std::atomic_bool prepare_observed{false};
    std::atomic_bool game_owned_active{false};
    std::atomic_bool configured{false};
    std::atomic_bool activation_logged{false};
    std::atomic_bool upload_failure_logged{false};
    std::atomic_bool force_diagnostic{false};
    std::atomic_bool diagnostic_activation_seeded{false};
};

DeathScreenState g_state;

int64_t steady_nanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

bool guest_rdram_address(uint32_t address) {
    return (address & 0xE0000000u) == 0x80000000u &&
        (address & 0x1FFFFFFFu) < kRdramSize;
}

bool read_guest_string(
    uint8_t* rdram,
    uint32_t address,
    std::string& output
) {
    output.clear();
    if (rdram == nullptr || !guest_rdram_address(address)) {
        return false;
    }
    const uint32_t offset = address & 0x1FFFFFFFu;
    const uint32_t maximum = std::min<uint32_t>(128u, kRdramSize - offset);
    const gpr guest = static_cast<gpr>(static_cast<int32_t>(address));
    for (uint32_t index = 0; index < maximum; ++index) {
        const uint8_t value = static_cast<uint8_t>(MEM_BU(
            static_cast<int32_t>(index),
            guest
        ));
        if (value == 0u) {
            break;
        }
        output.push_back(static_cast<char>(value));
    }
    return !output.empty();
}

bool death_text(const std::string& text) {
    return text.find("MISSION FAILED") != std::string::npos ||
        text.find("GAME OVER") != std::string::npos;
}

uint32_t frontend_phase(uint8_t* rdram) {
    constexpr uint32_t address = kFrontendObject + kFrontendPhaseOffset;
    if (rdram == nullptr || !guest_rdram_address(address)) {
        return kInvalidFrontendPhase;
    }
    return static_cast<uint32_t>(MEM_W(
        0,
        static_cast<gpr>(static_cast<int32_t>(address))
    ));
}

bool load_contract_and_image(const std::filesystem::path& directory) {
    const std::filesystem::path manifest_path = directory / "manifest.json";
    const std::filesystem::path image_path = directory / "DeathScreen.png";
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
                return asset.at("id").get<std::string>() == "death_screen";
            }
        );
        if (matching == assets.end() ||
            matching->at("path").get<std::string>() !=
                "NewImages/DeathScreen.png" ||
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

bool bumble::death_screen::configure(
    const std::filesystem::path& asset_directory
) {
    std::scoped_lock lock(g_state.mutex);
    g_state.image_bytes.clear();
    g_state.configured.store(false, std::memory_order_release);
    g_state.force_diagnostic.store(false, std::memory_order_release);
    g_state.diagnostic_activation_seeded.store(
        false,
        std::memory_order_release
    );
    g_state.activation_ns.store(0, std::memory_order_release);
    g_state.activation_phase.store(
        kInvalidFrontendPhase,
        std::memory_order_release
    );
    g_state.prepared_phase.store(
        kInvalidFrontendPhase,
        std::memory_order_release
    );
    g_state.prepared_descriptor.store(
        kInvalidFrontendPhase,
        std::memory_order_release
    );
    g_state.prepare_observed.store(false, std::memory_order_release);
    g_state.game_owned_active.store(false, std::memory_order_release);
    g_state.activation_logged.store(false, std::memory_order_release);
    g_state.upload_failure_logged.store(false, std::memory_order_release);
    if (!load_contract_and_image(asset_directory)) {
        std::fprintf(
            stderr,
            "BUMBLE_DEATH_SCREEN stage=asset_rejected path=%s\n",
            asset_directory.string().c_str()
        );
        std::fflush(stderr);
        return false;
    }
#if !defined(BUMBLE_RELEASE_HOST)
    const char* force_diagnostic = std::getenv("BUMBLE_FORCE_DEATH_SCREEN");
    g_state.force_diagnostic.store(
        force_diagnostic != nullptr &&
            std::strcmp(force_diagnostic, "1") == 0,
        std::memory_order_release
    );
#endif
    g_state.configured.store(true, std::memory_order_release);
    return true;
}

void bumble::death_screen::prepare_frontend_phase(
    uint32_t phase,
    uint32_t descriptor
) {
    const bool exact_failure = phase == kMissionFailurePhase &&
        descriptor == kMissionFailureDescriptor;
    g_state.prepared_phase.store(phase, std::memory_order_release);
    g_state.prepared_descriptor.store(descriptor, std::memory_order_release);
    g_state.prepare_observed.store(true, std::memory_order_release);
    const bool was_active = g_state.game_owned_active.exchange(
        exact_failure,
        std::memory_order_acq_rel
    );
    if (!exact_failure) {
        return;
    }
    g_state.activation_phase.store(phase, std::memory_order_release);
    g_state.activation_ns.store(
        steady_nanoseconds(),
        std::memory_order_release
    );
    if (!was_active) {
        g_state.activation_logged.store(false, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_DEATH_SCREEN stage=prepared_before_guest_init"
            " phase=0x%08" PRIX32 " descriptor=0x%08" PRIX32
            " legacy_flash_preempted=1\n",
            phase,
            descriptor
        );
        std::fflush(stderr);
    }
}

void bumble::death_screen::bind_renderer(
    plume::RenderDevice* device,
    const RT64::ShaderLibrary* shader_library,
    plume::RenderSwapChain* swap_chain
) {
    std::scoped_lock lock(g_state.mutex);
    g_state.device = device;
    g_state.shader_library = shader_library;
    g_state.swap_chain = swap_chain;
}

void bumble::death_screen::draw(
    plume::RenderCommandList* command_list,
    plume::RenderFramebuffer* framebuffer
) {
    const bool forced_diagnostic =
        g_state.force_diagnostic.load(std::memory_order_acquire);
    if (!g_state.configured.load(std::memory_order_acquire)) {
        return;
    }

    const uint32_t current_phase =
        bumble::native_checkpoint::last_frontend_phase();
    const uint32_t current_descriptor =
        bumble::native_checkpoint::last_frontend_descriptor();
    const bool current_phase_is_prepared =
        !g_state.prepare_observed.load(std::memory_order_acquire) ||
        (current_phase ==
            g_state.prepared_phase.load(std::memory_order_acquire) &&
        current_descriptor ==
            g_state.prepared_descriptor.load(std::memory_order_acquire));
    if (current_phase_is_prepared &&
        current_phase == kMissionFailurePhase &&
        current_descriptor == kMissionFailureDescriptor &&
        !g_state.game_owned_active.exchange(
            true,
            std::memory_order_acq_rel
        )) {
        g_state.activation_phase.store(current_phase, std::memory_order_release);
        g_state.activation_ns.store(
            steady_nanoseconds(),
            std::memory_order_release
        );
        g_state.activation_logged.store(false, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_DEATH_SCREEN stage=mission_failure_phase_observed"
            " phase=0x%08" PRIX32
            " descriptor=0x%08" PRIX32
            " trigger=game_owned_failure_state"
            " lifetime=game_frontend_phase\n",
            current_phase,
            current_descriptor
        );
        std::fflush(stderr);
    }
    if (forced_diagnostic && current_phase != kInvalidFrontendPhase &&
        !g_state.diagnostic_activation_seeded.exchange(
            true,
            std::memory_order_acq_rel
        )) {
        g_state.activation_phase.store(current_phase, std::memory_order_release);
        g_state.activation_ns.store(
            steady_nanoseconds(),
            std::memory_order_release
        );
        g_state.activation_logged.store(false, std::memory_order_release);
        g_state.game_owned_active.store(true, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_DEATH_SCREEN stage=diagnostic_activation_seeded"
            " phase=0x%08" PRIX32
            " lifetime=game_frontend_phase\n",
            current_phase
        );
        std::fflush(stderr);
    }

    bool game_owned_active =
        g_state.game_owned_active.load(std::memory_order_acquire);
    if (game_owned_active) {
        const uint32_t activation_phase =
            g_state.activation_phase.load(std::memory_order_acquire);
        const int64_t activation_ns =
            g_state.activation_ns.load(std::memory_order_acquire);
        const int64_t phase_grace_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                kPhaseSynchronizationGrace
            ).count();
        if (activation_phase != kInvalidFrontendPhase &&
            current_phase != kInvalidFrontendPhase &&
            current_phase != activation_phase &&
            steady_nanoseconds() - activation_ns > phase_grace_ns) {
            game_owned_active = false;
            if (g_state.game_owned_active.exchange(
                    false,
                    std::memory_order_acq_rel
                )) {
                std::fprintf(
                    stderr,
                    "BUMBLE_DEATH_SCREEN stage=overlay_deactivated"
                    " activation_phase=0x%08" PRIX32
                    " current_phase=0x%08" PRIX32
                    " owner=game_frontend_phase_transition\n",
                    activation_phase,
                    current_phase
                );
                std::fflush(stderr);
            }
        }
    }
    if (!game_owned_active) {
        return;
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
                    "BUMBLE_DEATH_SCREEN stage=gpu_upload_failed\n"
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

    if (!g_state.activation_logged.exchange(true, std::memory_order_acq_rel)) {
        std::fprintf(
            stderr,
            "BUMBLE_DEATH_SCREEN stage=overlay_presented"
            " source=user_authored full_framebuffer=1 width=%" PRIu32
            " height=%" PRIu32
            " scale=cover_preserve_aspect forced_diagnostic=%d\n",
            width,
            height,
            forced_diagnostic ? 1 : 0
        );
        std::fflush(stderr);
    }
}

void bumble::death_screen::release_renderer() {
    std::scoped_lock lock(g_state.mutex);
    g_state.descriptor_set.reset();
    g_state.texture.reset();
    g_state.upload_buffer.reset();
    g_state.device = nullptr;
    g_state.shader_library = nullptr;
    g_state.swap_chain = nullptr;
}

bool bumble::death_screen::configured() {
    return g_state.configured.load(std::memory_order_acquire);
}

extern "C" void bumble_death_screen_observe_text(
    uint8_t* rdram,
    recomp_context* context
) {
    if (context == nullptr ||
        !g_state.configured.load(std::memory_order_acquire)) {
        return;
    }
    std::string text;
    if (!read_guest_string(
            rdram,
            static_cast<uint32_t>(context->r4),
            text
        ) || !death_text(text)) {
        return;
    }
    const int64_t now = steady_nanoseconds();
    const uint32_t phase = frontend_phase(rdram);
    const uint32_t previous_phase = g_state.activation_phase.exchange(
        phase,
        std::memory_order_acq_rel
    );
    g_state.activation_ns.store(now, std::memory_order_release);
    const bool already_active = g_state.game_owned_active.exchange(
        true,
        std::memory_order_acq_rel
    );
    if (!already_active || previous_phase != phase) {
        g_state.activation_logged.store(false, std::memory_order_release);
        std::fprintf(
            stderr,
            "BUMBLE_DEATH_SCREEN stage=death_text_observed"
            " phase=0x%08" PRIX32
            " trigger=%s owner=func_800B8180"
            " lifetime=game_frontend_phase\n",
            phase,
            text.find("MISSION FAILED") != std::string::npos
                ? "MISSION_FAILED"
                : "GAME_OVER"
        );
        std::fflush(stderr);
    }
}
