#include "native_rt64_renderer.hpp"
#include "native_renderer_contracts.hpp"
#include "native_first_run_assets.hpp"

#if defined(_WIN32)
#include <objbase.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <json/json.hpp>

#include "bumble_version.hpp"
#include "common/rt64_performance_profiler.h"
#include "hle/rt64_application.h"
#include "hle/rt64_present_queue.h"
#include "hle/rt64_workload_queue.h"
#include "common/rt64_tmem_hasher.h"
#include "native_checkpoint_bridge.hpp"
#include "native_collision_debug.hpp"
#include "native_death_screen.hpp"
#include "native_frame_pacing.hpp"
#include "native_electric_effect.hpp"
#include "native_game_completion_screen.hpp"
#include "native_graphics_options.hpp"
#include "native_menu_background.hpp"
#include "native_modern_sky.hpp"
#include "native_object_cull_telemetry.hpp"
#include "native_procedural_grass.hpp"
#include "native_rsp_task_bridge.hpp"
#include "native_text_overlay.hpp"
#include "native_text_overlay_state.hpp"
#include "native_weapon_system.hpp"
#include "native_widescreen.hpp"
#include "render/rt64_texture_cache.h"
#include "rhi/rt64_render_hooks.h"
#include "ultramodern/ultramodern.hpp"

namespace {

std::atomic_uint32_t g_display_lists{0};
std::atomic_uint32_t g_screen_updates{0};
std::atomic_bool g_mission1_screen_logged{false};
std::atomic_uint32_t g_player_checkpoint_display_list_baseline{UINT32_MAX};
std::atomic_bool g_mission2_screen_logged{false};
std::atomic_uint32_t g_mission2_checkpoint_display_list_baseline{UINT32_MAX};
std::atomic_bool g_campaign_screen_logged{false};
std::atomic_uint32_t g_campaign_checkpoint_display_list_baseline{UINT32_MAX};
std::atomic_uint32_t g_campaign_checkpoint_level{0};
std::atomic_bool g_two_player_screen_logged{false};
std::atomic_uint32_t g_two_player_checkpoint_display_list_baseline{UINT32_MAX};
std::atomic<float> g_fog_scale{0.0f};
std::atomic_bool g_raytracing_supported{false};
std::mutex g_active_application_mutex;
RT64::Application* g_active_application = nullptr;
std::atomic_bool g_contract_validation_enabled{false};

void validate_descriptor_contracts(RenderDevice* device) {
    using namespace plume;
    uint32_t rejected = 0;
    auto must_reject = [&](auto&& operation) {
        try { operation(); }
        catch (const std::logic_error&) { ++rejected; return; }
        throw std::runtime_error("Descriptor contract accepted an invalid operation");
    };
    auto texture = device->createTexture(RenderTextureDesc::Texture2D(
        1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
    auto sampler = device->createSampler(RenderSamplerDesc{});
    constexpr uint32_t max_textures = RT64::FramebufferRendererDescriptorTextureSet::UpperRange;
    RT64::SubmissionTextureIndices indices(max_textures);
    auto require = [](bool condition) {
        if (!condition) throw std::runtime_error("Submission texture index contract failed");
    };
    indices.reset(max_textures + 1);
    require(indices.mapStatic(max_textures) == 0 && indices.mapStatic(max_textures) == 0);
    require(indices.count == 1 && indices.staticBindings.size() == 1);
    require(indices.allocate() == 1 && indices.mapStatic(3) == 2);
    indices.reset(max_textures + 1);
    require(indices.mapStatic(3) == 0 && indices.mapStatic(max_textures) == 1);
    indices.reset(max_textures + 1);
    require(indices.allocate() == 0 && indices.mapStatic(max_textures) == 1);
    indices.reset(max_textures + 1);
    for (uint32_t i = 0; i < max_textures; ++i) require(indices.mapStatic(i) == i);
    must_reject([&] { indices.mapStatic(max_textures); });
    must_reject([&] { indices.allocate(); });
    must_reject([&] { indices.mapStatic(max_textures + 1); });
    require(indices.count == max_textures && indices.staticBindings.size() == max_textures);
    require(indices.cacheIndices[max_textures] == UINT32_MAX && indices.mapStatic(0) == 0);
    indices.reset(0);
    require(indices.count == 0 && indices.staticBindings.empty());
    for (uint8_t size = 0; size < 4; ++size) {
        for (uint8_t format = 0; format < 5; ++format) {
            RT64::LoadTile tile{};
            tile.siz = size;
            tile.fmt = format;
            require(RT64::TMEMHasher::requiresRawTMEM(tile, 1, 1, 0));
            require(RT64::TMEMHasher::requiresRawTMEM(tile, 1024, 1024, 0));
            tile.line = 1;
            require(!RT64::TMEMHasher::requiresRawTMEM(tile, 1, 1, 0));
        }
    }
    RenderDescriptorRange range(RenderDescriptorRangeType::TEXTURE, 0, 1);
    auto adjacent = device->createDescriptorSet(RenderDescriptorSetDesc(&range, 1));
    adjacent->setTexture(0, texture.get(), RenderTextureLayout::SHADER_READ);
    for (const uint32_t capacity : {1U, 3U, 8192U}) {
        range.count = capacity == 1 ? 1 : 8192;
        auto set = device->createDescriptorSet(RenderDescriptorSetDesc(
            &range, 1, capacity != 1, capacity));
        for (uint32_t i = 0; i < capacity; ++i)
            set->setTexture(i, texture.get(), RenderTextureLayout::SHADER_READ);
        must_reject([&] { set->setTexture(capacity, texture.get(), RenderTextureLayout::SHADER_READ); });
        must_reject([&] { set->setTexture(UINT32_MAX, texture.get(), RenderTextureLayout::SHADER_READ); });
        must_reject([&] { set->setSampler(0, sampler.get()); });
        must_reject([&] { set->setBuffer(0, nullptr); });
        must_reject([&] { set->setAccelerationStructure(0, nullptr); });
        // Rejection must leave the descriptor set usable.
        set->setTexture(capacity - 1, texture.get(), RenderTextureLayout::SHADER_READ);
    }
    must_reject([&] { device->createDescriptorSet(RenderDescriptorSetDesc(&range, 1, true, 8193)); });
    must_reject([&] { device->createDescriptorSet(RenderDescriptorSetDesc(nullptr, 1)); });
    must_reject([&] { device->createDescriptorSet(RenderDescriptorSetDesc(nullptr, 0, true, 1)); });
    const RenderDescriptorRange overflow[] = {
        {RenderDescriptorRangeType::TEXTURE, 0, UINT32_MAX},
        {RenderDescriptorRangeType::TEXTURE, 1, 1}
    };
    must_reject([&] { device->createDescriptorSet(RenderDescriptorSetDesc(overflow, 2)); });
    auto minimum = device->createDescriptorSet(RenderDescriptorSetDesc(&range, 1, true, 0));
    minimum->setTexture(0, texture.get(), RenderTextureLayout::SHADER_READ);
    must_reject([&] { minimum->setTexture(1, texture.get(), RenderTextureLayout::SHADER_READ); });
    adjacent->setTexture(0, texture.get(), RenderTextureLayout::SHADER_READ);
    const RenderSampler* null_sampler = nullptr;
    range = RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 0, 1, &null_sampler);
    must_reject([&] { device->createDescriptorSet(RenderDescriptorSetDesc(&range, 1)); });
    const RenderSampler* valid_sampler = sampler.get();
    range = RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1, &valid_sampler);
    must_reject([&] { device->createDescriptorSet(RenderDescriptorSetDesc(&range, 1)); });
    if (rejected != 25) throw std::runtime_error("Descriptor contract coverage incomplete");
    std::fprintf(stderr, "BUMBLE_TEXTURE_INDEX_CONTRACT result=pass cache_high_water=8193 dense_slots=8192 zero_stride_size_format_pairs=20 gpu_sampling_unproved=1\n");
    std::fprintf(stderr, "BUMBLE_DESCRIPTOR_CONTRACT result=pass rejected=%u valid_texture_writes=8202 capacity=8192 backend=selected_production_device\n", rejected);
}

constexpr uint32_t kFrontendStateOffset = 0x000FFF80u;
constexpr uint32_t kFrontendWorldActiveOffset = 0x24u;
constexpr uint32_t kFrontendPlayerLayoutOffset = 0x0Cu;
constexpr uint32_t kSinglePlayerLayout = 1u;
constexpr uint32_t kPlayerOneOwnerSlotOffset = 0x000E91D4u;
constexpr uint32_t kGuestVirtualBase = 0x80000000u;
constexpr uint32_t kGuestRdramMask = 0x1FFFFFFFu;
constexpr uint32_t kGuestRdramSize = 0x00800000u;
constexpr uint32_t kPlayerPositionOffset = 0x40u;

uint32_t read_guest_u32(const uint8_t* rdram, uint32_t offset) {
    return *reinterpret_cast<const uint32_t*>(rdram + offset);
}

bool single_player_world_active(const uint8_t* rdram) {
    return rdram != nullptr &&
        read_guest_u32(
            rdram,
            kFrontendStateOffset + kFrontendWorldActiveOffset
        ) != 0u &&
        read_guest_u32(
            rdram,
            kFrontendStateOffset + kFrontendPlayerLayoutOffset
        ) == kSinglePlayerLayout;
}

constexpr uint32_t kWidescreenHudCaptureWeaponCount = 11u;
constexpr uint32_t kWidescreenHudEarlyCaptureDelay = 0u;
constexpr uint32_t kWidescreenHudLateCaptureDelay = 180u;

struct WidescreenHudCaptureState {
    std::mutex mutex;
    std::filesystem::path directory;
    std::unique_ptr<RenderBuffer> readback;
    uint64_t readback_size = 0u;
    uint32_t row_width = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t pending_selected = UINT32_MAX;
    uint32_t deferred_selected = UINT32_MAX;
    uint32_t capture_selected = UINT32_MAX;
    uint32_t delay = 0u;
    uint32_t phase = 0u;
    uint64_t present_id = 0u;
    uint64_t workload_id = 0u;
    uint64_t target_set_id = 0u;
    uint32_t source_frame_index = UINT32_MAX;
    uint32_t target_storage_index = UINT32_MAX;
    uint32_t presentation_slot_index = UINT32_MAX;
    uint32_t swap_chain_index = UINT32_MAX;
    uint32_t color_target_address = 0u;
    bool copy_queued = false;
    uint64_t rejected_presentations = 0u;
};

WidescreenHudCaptureState g_widescreen_hud_capture{};

const char* widescreen_hud_capture_phase_name(uint32_t phase) {
    return phase == 0u ? "early" : "late";
}

bool widescreen_hud_capture_prepare(
    RenderDevice* device,
    RenderCommandList* command_list,
    RenderTexture* swap_chain_texture,
    uint32_t width,
    uint32_t height,
    uint64_t present_id,
    uint64_t workload_id,
    uint64_t target_set_id,
    uint32_t source_frame_index,
    uint32_t target_storage_index,
    uint32_t presentation_slot_index,
    uint32_t swap_chain_index,
    uint32_t color_target_address
) {
    WidescreenHudCaptureState& state = g_widescreen_hud_capture;
    const std::scoped_lock capture_lock(state.mutex);
    const uint32_t requested =
        bumble::native_checkpoint::take_widescreen_hud_capture_request();
    if (requested > 0u && requested <= kWidescreenHudCaptureWeaponCount) {
        const uint32_t selected = requested - 1u;
        if (state.pending_selected == UINT32_MAX && !state.copy_queued) {
            state.pending_selected = selected;
            state.phase = 0u;
            state.delay = kWidescreenHudEarlyCaptureDelay;
        }
        else {
            state.deferred_selected = selected;
        }
    }
    if (state.copy_queued || state.pending_selected == UINT32_MAX ||
        device == nullptr || command_list == nullptr ||
        swap_chain_texture == nullptr || width == 0u || height == 0u) {
        return false;
    }
    const uint32_t rendered_selected =
        bumble::text_overlay::last_draw_selected_weapon();
    if (rendered_selected != state.pending_selected) {
        ++state.rejected_presentations;
        if (state.rejected_presentations == 1u ||
            (state.rejected_presentations &
                (state.rejected_presentations - 1u)) == 0u) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE"
                " stage=widescreen_hud_capture_presentation_rejected"
                " expected_selected=%" PRIu32
                " rendered_selected=%" PRIu32
                " present_id=%" PRIu64 " workload_id=%" PRIu64
                " reason=rendered_overlay_identity_mismatch"
                " rejected_presentations=%" PRIu64 "\n",
                state.pending_selected,
                rendered_selected,
                present_id,
                workload_id,
                state.rejected_presentations
            );
            std::fflush(stderr);
        }
        return false;
    }
    if (state.delay > 0u) {
        --state.delay;
        return false;
    }

    // D3D12 requires 256-byte row pitch: 64 BGRA pixels. Vulkan accepts this too.
    const uint32_t row_width = (width + 63u) & ~63u;
    const uint64_t readback_size =
        static_cast<uint64_t>(row_width) * height * 4u;
    if (state.readback == nullptr || state.readback_size < readback_size) {
        state.readback = device->createBuffer(
            RenderBufferDesc::ReadbackBuffer(readback_size)
        );
        state.readback_size = state.readback != nullptr ? readback_size : 0u;
    }
    if (state.readback == nullptr) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=widescreen_hud_renderer_capture_failed"
            " selected=%" PRIu32 " reason=readback_allocation\n",
            state.pending_selected
        );
        std::fflush(stderr);
        state.pending_selected = UINT32_MAX;
        return false;
    }

    command_list->barriers(
        RenderBarrierStage::COPY,
        RenderBufferBarrier(state.readback.get(), RenderBufferAccess::WRITE),
        RenderTextureBarrier(
            swap_chain_texture,
            RenderTextureLayout::COPY_SOURCE
        )
    );
    command_list->copyTextureRegion(
        RenderTextureCopyLocation::PlacedFootprint(
            state.readback.get(),
            RenderFormat::B8G8R8A8_UNORM,
            width,
            height,
            1u,
            row_width
        ),
        RenderTextureCopyLocation::Subresource(swap_chain_texture)
    );
    state.row_width = row_width;
    state.width = width;
    state.height = height;
    state.present_id = present_id;
    state.workload_id = workload_id;
    state.target_set_id = target_set_id;
    state.source_frame_index = source_frame_index;
    state.target_storage_index = target_storage_index;
    state.presentation_slot_index = presentation_slot_index;
    state.swap_chain_index = swap_chain_index;
    state.color_target_address = color_target_address;
    state.capture_selected = state.pending_selected;
    state.copy_queued = true;
    state.rejected_presentations = 0u;
    return true;
}

void widescreen_hud_capture_complete() {
    WidescreenHudCaptureState& state = g_widescreen_hud_capture;
    const std::scoped_lock capture_lock(state.mutex);
    if (!state.copy_queued || state.readback == nullptr) {
        return;
    }
    const uint64_t capture_size =
        static_cast<uint64_t>(state.row_width) * state.height * 4u;
    const RenderRange read_range{0u, capture_size};
    const uint8_t* source = static_cast<const uint8_t*>(
        state.readback->map(0u, &read_range)
    );
    const char* phase_name = widescreen_hud_capture_phase_name(state.phase);
    const std::filesystem::path output = state.directory /
        ("weapon-" +
         (state.capture_selected < 10u ? std::string("0") : std::string()) +
         std::to_string(state.capture_selected) + "-transition-" +
         phase_name + ".ppm");
    bool written = false;
    uint64_t nonblack_pixels = 0u;
    if (source != nullptr) {
        std::ofstream stream(output, std::ios::binary | std::ios::trunc);
        if (stream.is_open()) {
            stream << "P6\n" << state.width << ' ' << state.height << "\n255\n";
            std::vector<uint8_t> rgb_row(
                static_cast<size_t>(state.width) * 3u
            );
            for (uint32_t y = 0u; y < state.height; ++y) {
                const uint8_t* bgra = source +
                    static_cast<uint64_t>(y) * state.row_width * 4u;
                for (uint32_t x = 0u; x < state.width; ++x) {
                    nonblack_pixels +=
                        (bgra[x * 4u + 0u] > 8u ||
                         bgra[x * 4u + 1u] > 8u ||
                         bgra[x * 4u + 2u] > 8u)
                        ? 1u
                        : 0u;
                    rgb_row[x * 3u + 0u] = bgra[x * 4u + 2u];
                    rgb_row[x * 3u + 1u] = bgra[x * 4u + 1u];
                    rgb_row[x * 3u + 2u] = bgra[x * 4u + 0u];
                }
                stream.write(
                    reinterpret_cast<const char*>(rgb_row.data()),
                    static_cast<std::streamsize>(rgb_row.size())
                );
            }
            written = stream.good();
        }
        state.readback->unmap();
    }

    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=widescreen_hud_renderer_capture_%s"
        " selected=%" PRIu32 " phase=%s width=%" PRIu32
        " height=%" PRIu32 " row_width=%" PRIu32
        " source=swapchain_readback occlusion_independent=1"
        " present_id=%" PRIu64 " workload_id=%" PRIu64
        " target_set_id=%" PRIu64 " source_frame_index=%" PRIu32
        " target_storage_index=%" PRIu32
        " presentation_slot_index=%" PRIu32
        " swap_chain_index=%" PRIu32
        " color_target_address=0x%08" PRIX32
        " nonblack_pixels=%" PRIu64 " total_pixels=%" PRIu64
        " blank_output=%d path=%s\n",
        written ? "written" : "failed",
        state.capture_selected,
        phase_name,
        state.width,
        state.height,
        state.row_width,
        state.present_id,
        state.workload_id,
        state.target_set_id,
        state.source_frame_index,
        state.target_storage_index,
        state.presentation_slot_index,
        state.swap_chain_index,
        state.color_target_address,
        nonblack_pixels,
        static_cast<uint64_t>(state.width) * state.height,
        nonblack_pixels * 100u <
            static_cast<uint64_t>(state.width) * state.height
            ? 1
            : 0,
        output.string().c_str()
    );
    RT64::PerformanceProfiler::recordInstant(
        RT64::PerformanceCategory::Counter,
        "Host.DiagnosticCaptureContent",
        state.present_id,
        (std::min<uint64_t>(nonblack_pixels, UINT32_MAX) << 32u) |
            std::min<uint64_t>(
                static_cast<uint64_t>(state.width) * state.height,
                UINT32_MAX
            ),
        (uint64_t(state.source_frame_index & 0xFFu) << 56u) |
            (uint64_t(state.target_storage_index & 0xFFu) << 48u) |
            (uint64_t(state.presentation_slot_index & 0xFFu) << 40u) |
            (uint64_t(state.swap_chain_index & 0xFFu) << 32u) |
            state.color_target_address
    );
    std::fflush(stderr);

    state.copy_queued = false;
    state.rejected_presentations = 0u;
    if (state.phase == 0u && written) {
        state.phase = 1u;
        state.delay = kWidescreenHudLateCaptureDelay;
    }
    else {
        if (written && state.phase == 1u) {
            bumble::native_checkpoint::mark_widescreen_hud_capture_complete(
                state.capture_selected
            );
        }
        state.pending_selected = UINT32_MAX;
        state.capture_selected = UINT32_MAX;
        state.phase = 0u;
        state.delay = 0u;
        if (state.deferred_selected != UINT32_MAX) {
            state.pending_selected = state.deferred_selected;
            state.deferred_selected = UINT32_MAX;
            state.delay = kWidescreenHudEarlyCaptureDelay;
        }
    }
}

bool install_widescreen_hud_capture_hook() {
    const char* capture_directory =
        std::getenv("BUMBLE_WIDESCREEN_HUD_CAPTURE_DIR");
    if (capture_directory == nullptr || capture_directory[0] == '\0') {
        return false;
    }
    std::error_code error;
    std::filesystem::path directory(capture_directory);
    std::filesystem::create_directories(directory, error);
    if (error || RT64::GetRenderHookCapturePrepare() != nullptr ||
        RT64::GetRenderHookCaptureComplete() != nullptr) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=widescreen_hud_renderer_capture_hook_failed"
            " reason=%s path=%s\n",
            error ? "directory" : "hook_owned",
            directory.string().c_str()
        );
        std::fflush(stderr);
        return false;
    }
    WidescreenHudCaptureState& state = g_widescreen_hud_capture;
    const std::scoped_lock capture_lock(state.mutex);
    state.directory = std::move(directory);
    state.readback.reset();
    state.readback_size = 0u;
    state.pending_selected = UINT32_MAX;
    state.deferred_selected = UINT32_MAX;
    state.capture_selected = UINT32_MAX;
    state.copy_queued = false;
    RT64::SetRenderCaptureHooks(
        &widescreen_hud_capture_prepare,
        &widescreen_hud_capture_complete
    );
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=widescreen_hud_renderer_capture_hook_installed"
        " source=swapchain_readback occlusion_independent=1 path=%s\n",
        state.directory.string().c_str()
    );
    std::fflush(stderr);
    return true;
}

void uninstall_widescreen_hud_capture_hook() {
    RT64::SetRenderCaptureHooks(nullptr, nullptr);
    WidescreenHudCaptureState& state = g_widescreen_hud_capture;
    const std::scoped_lock capture_lock(state.mutex);
    state.directory.clear();
    state.readback.reset();
    state.readback_size = 0u;
    state.row_width = 0u;
    state.width = 0u;
    state.height = 0u;
    state.pending_selected = UINT32_MAX;
    state.deferred_selected = UINT32_MAX;
    state.capture_selected = UINT32_MAX;
    state.delay = 0u;
    state.phase = 0u;
    state.present_id = 0u;
    state.workload_id = 0u;
    state.target_set_id = 0u;
    state.source_frame_index = UINT32_MAX;
    state.target_storage_index = UINT32_MAX;
    state.presentation_slot_index = UINT32_MAX;
    state.swap_chain_index = UINT32_MAX;
    state.color_target_address = 0u;
    state.copy_queued = false;
    state.rejected_presentations = 0u;
}

constexpr std::array<uint64_t, 7> kSuppressedMenuLogoTextureHashes{
    0x02A23544F1C3B7F4ULL,
    0x0E9D6DB6B1A85249ULL,
    0x2739BF2484A12B8FULL,
    0x376963BFB6FA9F74ULL,
    0x5482CC9947270C6DULL,
    0xBA3765C6A84AA72AULL,
    0xF58128D5436946E7ULL,
};

constexpr std::array<uint64_t, 8> kGameplayUiTextureHashes{
    0x5B500B6598C53D16ULL,
    0x1F175A9A2915146FULL,
    0xFD0FED1E7720C49DULL,
    0xA3610D49EF236262ULL,
    0xDDC5C435290906CFULL,
    0x8E727BA47B90502FULL,
    0xE0BB7BCDA724F2B2ULL,
    0x595E04223032B6AFULL,
};

constexpr size_t kFaithfulTextureCount = 181u;

struct FaithfulTexturePackContract {
    std::filesystem::path pack_path;
    std::unordered_set<uint64_t> hashes;
    bool valid = false;
};

FaithfulTexturePackContract load_faithful_texture_pack_contract(
    const std::filesystem::path& root,
    bool enhanced = false
) {
    FaithfulTexturePackContract contract{};
    const std::filesystem::path manifest_path = root / "manifest.json";
    try {
        std::ifstream stream(manifest_path, std::ios::binary);
        if (!stream.is_open()) {
            throw std::runtime_error("manifest is missing");
        }
        const nlohmann::json manifest = nlohmann::json::parse(stream);
        const nlohmann::json& textures = manifest.at("textures");
        const std::string pack_name =
            manifest.at("pack_name").get<std::string>();
        if (manifest.at("schema_version").get<uint32_t>() != 4u ||
            manifest.at("generator").get<std::string>() !=
                bumble::first_run::kAssetGenerator ||
            manifest.at("rom_included").get<bool>() ||
            manifest.at("distributable_assets_included").get<bool>() ||
            manifest.at("source").get<std::string>() !=
                "verified_external_us_rev0_rom" ||
            manifest.at("source_rom_size").get<uint64_t>() != 0xC00000u ||
            manifest.at("source_rom_sha256").get<std::string>() !=
                "d21e3d1c2ec4d7f025cfaa119553be9a5fa87a9fd6625ef1ef44dc1d4b0aa54b" ||
            manifest.at("algorithm").get<std::string>() !=
                (enhanced ? "coordinate-v7" : "nearest_texel_exact") ||
            manifest.at("output_format").get<std::string>() !=
                (enhanced ? "R8G8B8A8_UNORM_area_mips" : bumble::first_run::kFaithfulTextureFormat) ||
            pack_name != "textures.rtz" || !textures.is_array() ||
            manifest.at("texture_count").get<size_t>() !=
                textures.size() ||
            (enhanced ? textures.size() < kFaithfulTextureCount : textures.size() != kFaithfulTextureCount)) {
            throw std::runtime_error("manifest contract changed");
        }

        contract.hashes.reserve(textures.size());
        for (const nlohmann::json& texture : textures) {
            const std::string hash = texture.at("rt64_hash").get<std::string>();
            if (hash.size() != 16u ||
                hash.find_first_not_of("0123456789abcdef") != std::string::npos) {
                throw std::runtime_error("invalid RT64 texture hash");
            }
            size_t consumed = 0;
            const uint64_t value = std::stoull(hash, &consumed, 16);
            if (consumed != hash.size() || !contract.hashes.insert(value).second) {
                throw std::runtime_error("duplicate RT64 texture hash");
            }
        }

        contract.pack_path = root / pack_name;
        std::error_code error;
        const uint64_t actual_pack_size =
            std::filesystem::file_size(contract.pack_path, error);
        if (error || actual_pack_size == 0u) {
            throw std::runtime_error("streaming pack size changed");
        }
        contract.valid = true;
    } catch (const std::exception& exception) {
        std::fprintf(
            stderr,
            "BUMBLE_FAITHFUL_TEXTURES stage=asset_invalid path=%s reason=%s\n",
            manifest_path.string().c_str(),
            exception.what()
        );
        std::fflush(stderr);
    }
    return contract;
}

void log_stage(const char* stage) {
    std::fprintf(stderr, "BUMBLE_RT64_PROBE stage=%s\n", stage);
    std::fflush(stderr);
}

std::filesystem::path texture_dump_directory_from_environment() {
#if defined(_WIN32)
    constexpr wchar_t kVariable[] = L"BUMBLE_RT64_TEXTURE_DUMP_DIR";
    size_t required = 0;
    if (_wgetenv_s(&required, nullptr, 0, kVariable) != 0 || required <= 1) {
        return {};
    }

    std::wstring value(required, L'\0');
    size_t written = 0;
    if (_wgetenv_s(&written, value.data(), value.size(), kVariable) != 0 ||
        written != required || value.back() != L'\0') {
        return {};
    }
    value.pop_back();
    return std::filesystem::path(std::move(value));
#else
    const char* value = std::getenv("BUMBLE_RT64_TEXTURE_DUMP_DIR");
    return value != nullptr ? std::filesystem::path(value)
                            : std::filesystem::path{};
#endif
}

void configure_texture_dump_directory(RT64::State& state) {
    state.dumpingTexturesDirectory.clear();
    const std::filesystem::path requested =
        texture_dump_directory_from_environment();
    if (requested.empty()) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=rt64_texture_dump enabled=0"
            " source=BUMBLE_RT64_TEXTURE_DUMP_DIR\n"
        );
        std::fflush(stderr);
        return;
    }

    std::error_code error;
    std::filesystem::path resolved = std::filesystem::absolute(requested, error);
    if (!error) {
        resolved = resolved.lexically_normal();
        std::filesystem::create_directories(resolved, error);
    }
    if (!error && !std::filesystem::is_directory(resolved, error)) {
        error = std::make_error_code(std::errc::not_a_directory);
    }
    if (error) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=rt64_texture_dump enabled=0"
            " source=BUMBLE_RT64_TEXTURE_DUMP_DIR error=%d path=%s\n",
            error.value(),
            requested.string().c_str()
        );
        std::fflush(stderr);
        return;
    }

    state.dumpingTexturesDirectory = resolved;
    std::fprintf(
        stderr,
        "BUMBLE_RT64_PROBE stage=rt64_texture_dump enabled=1"
        " source=BUMBLE_RT64_TEXTURE_DUMP_DIR path=%s\n",
        resolved.string().c_str()
    );
    std::fflush(stderr);
}

ultramodern::renderer::SetupResult map_setup_result(
    RT64::Application::SetupResult result
) {
    using RuntimeResult = ultramodern::renderer::SetupResult;
    switch (result) {
    case RT64::Application::SetupResult::Success:
        return RuntimeResult::Success;
    case RT64::Application::SetupResult::DynamicLibrariesNotFound:
        return RuntimeResult::DynamicLibrariesNotFound;
    case RT64::Application::SetupResult::InvalidGraphicsAPI:
        return RuntimeResult::InvalidGraphicsAPI;
    case RT64::Application::SetupResult::GraphicsAPINotFound:
        return RuntimeResult::GraphicsAPINotFound;
    case RT64::Application::SetupResult::GraphicsDeviceNotFound:
        return RuntimeResult::GraphicsDeviceNotFound;
    }

    return RuntimeResult::GraphicsDeviceNotFound;
}

uint32_t requested_presentation_hz(
    bumble::graphics_options::FramePacing frame_pacing
) {
    switch (frame_pacing) {
    case bumble::graphics_options::FramePacing::Interpolated120Hz:
        return 120u;
    case bumble::graphics_options::FramePacing::Interpolated60Hz:
        return 60u;
    case bumble::graphics_options::FramePacing::Original30Hz:
    default:
        return bumble::frame_pacing::kOriginalGameplayHz;
    }
}

const char* frame_pacing_name(
    bumble::graphics_options::FramePacing frame_pacing
) {
    switch (frame_pacing) {
    case bumble::graphics_options::FramePacing::Interpolated120Hz:
        return "interpolated_120hz";
    case bumble::graphics_options::FramePacing::Interpolated60Hz:
        return "interpolated_60hz";
    case bumble::graphics_options::FramePacing::Original30Hz:
    default:
        return "original_30hz";
    }
}

ultramodern::renderer::GraphicsApi map_graphics_api(
    RT64::UserConfiguration::GraphicsAPI api
) {
    using RuntimeApi = ultramodern::renderer::GraphicsApi;
    switch (api) {
    case RT64::UserConfiguration::GraphicsAPI::D3D12:
        return RuntimeApi::D3D12;
    case RT64::UserConfiguration::GraphicsAPI::Vulkan:
        return RuntimeApi::Vulkan;
    case RT64::UserConfiguration::GraphicsAPI::Metal:
        return RuntimeApi::Metal;
    case RT64::UserConfiguration::GraphicsAPI::Automatic:
    case RT64::UserConfiguration::GraphicsAPI::OptionCount:
        return RuntimeApi::Auto;
    }

    return RuntimeApi::Auto;
}

struct ProbeMsaaSelection {
    RT64::UserConfiguration::Antialiasing antialiasing;
    const char* source;
};

ProbeMsaaSelection probe_msaa_selection_from_environment() {
    const char* value = std::getenv("BUMBLE_RT64_PROBE_MSAA_SAMPLES");
    if (value != nullptr && std::string(value) == "1") {
        return {
            RT64::UserConfiguration::Antialiasing::None,
            "BUMBLE_RT64_PROBE_MSAA_SAMPLES",
        };
    }
    if (value != nullptr && std::string(value) == "2") {
        return {
            RT64::UserConfiguration::Antialiasing::MSAA2X,
            "BUMBLE_RT64_PROBE_MSAA_SAMPLES",
        };
    }
    return {
        RT64::UserConfiguration::Antialiasing::MSAA2X,
        "default",
    };
}

class BumbleRt64Context final : public ultramodern::renderer::RendererContext {
public:
    BumbleRt64Context(
        uint8_t* rdram,
        ultramodern::renderer::WindowHandle window_handle,
        bool developer_mode
    ) {
        rdram_ = rdram;
        setup_result = ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
        chosen_api = ultramodern::renderer::GraphicsApi::Auto;

        RT64::Application::Core core{};
#if defined(_WIN32)
        core.window = window_handle.window;
#else
        core.window = window_handle;
#endif
        core.HEADER = header_.data();
        core.RDRAM = rdram;
        core.DMEM = dmem_.data();
        core.IMEM = imem_.data();
        core.MI_INTR_REG = &mi_intr_reg_;
        core.DPC_START_REG = &dpc_start_reg_;
        core.DPC_END_REG = &dpc_end_reg_;
        core.DPC_CURRENT_REG = &dpc_current_reg_;
        core.DPC_STATUS_REG = &dpc_status_reg_;
        core.DPC_CLOCK_REG = &dpc_clock_reg_;
        core.DPC_BUFBUSY_REG = &dpc_bufbusy_reg_;
        core.DPC_PIPEBUSY_REG = &dpc_pipebusy_reg_;
        core.DPC_TMEM_REG = &dpc_tmem_reg_;
        core.checkInterrupts = &check_interrupts;

        ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
        core.VI_STATUS_REG = &vi->VI_STATUS_REG;
        core.VI_ORIGIN_REG = &vi->VI_ORIGIN_REG;
        core.VI_WIDTH_REG = &vi->VI_WIDTH_REG;
        core.VI_INTR_REG = &vi->VI_INTR_REG;
        core.VI_V_CURRENT_LINE_REG = &vi->VI_V_CURRENT_LINE_REG;
        core.VI_TIMING_REG = &vi->VI_TIMING_REG;
        core.VI_V_SYNC_REG = &vi->VI_V_SYNC_REG;
        core.VI_H_SYNC_REG = &vi->VI_H_SYNC_REG;
        core.VI_LEAP_REG = &vi->VI_LEAP_REG;
        core.VI_H_START_REG = &vi->VI_H_START_REG;
        core.VI_V_START_REG = &vi->VI_V_START_REG;
        core.VI_V_BURST_REG = &vi->VI_V_BURST_REG;
        core.VI_X_SCALE_REG = &vi->VI_X_SCALE_REG;
        core.VI_Y_SCALE_REG = &vi->VI_Y_SCALE_REG;

        RT64::ApplicationConfiguration application_config{};
        application_config.appId = "Bumble";
        application_config.detectDataPath = false;
        application_config.useConfigurationFile = false;

        try {
        app_ = std::make_unique<RT64::Application>(core, application_config);
        app_->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Automatic;
#if defined(_WIN32)
        const char* warp = std::getenv("BUMBLE_D3D12_VALIDATION_WARP");
        const char* owned_input = std::getenv("BUMBLE_RT64_VALIDATION_SYNTHETIC_INPUT");
        if (warp && std::string_view(warp) == "1" && owned_input && std::string_view(owned_input) == "1") {
            app_->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12;
        }
#endif
        app_->userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
        app_->userConfig.displayBuffering =
            RT64::UserConfiguration::DisplayBuffering::Triple;
        const ProbeMsaaSelection probe_msaa =
            probe_msaa_selection_from_environment();
        app_->userConfig.antialiasing = probe_msaa.antialiasing;
        const bool adaptive_aspect =
            bumble::graphics_options::widescreen_enabled();
        app_->userConfig.aspectRatio = adaptive_aspect
            ? RT64::UserConfiguration::AspectRatio::Expand
            : RT64::UserConfiguration::AspectRatio::Original;
        app_->userConfig.extAspectRatio = adaptive_aspect
            ? RT64::UserConfiguration::AspectRatio::Expand
            : RT64::UserConfiguration::AspectRatio::Original;
        const auto initial_frame_pacing =
            bumble::graphics_options::frame_pacing();
        const bool initial_interpolation = initial_frame_pacing !=
            bumble::graphics_options::FramePacing::Original30Hz;
        const uint32_t initial_requested_hz =
            requested_presentation_hz(initial_frame_pacing);
        const bumble::graphics_options::Settings initial_settings =
            bumble::graphics_options::current();
        RT64::PerformanceProfiler::setMetadata("host_project", "Bumble");
        RT64::PerformanceProfiler::setMetadata(
            "product_version",
            bumble::build::kDisplayVersion
        );
        RT64::PerformanceProfiler::setMetadata(
            "requested_presentation_hz",
            std::to_string(initial_requested_hz)
        );
        RT64::PerformanceProfiler::setMetadata(
            "resolution",
            std::to_string(initial_settings.resolution_width) + "x" +
                std::to_string(initial_settings.resolution_height)
        );
        RT64::PerformanceProfiler::setMetadata(
            "modern_lighting",
            initial_settings.modern_lighting ? "on" : "off"
        );
        RT64::PerformanceProfiler::setMetadata(
            "fog_mode",
            std::to_string(uint32_t(initial_settings.fog_mode))
        );
        const bool grass_no_interpolation_diagnostic =
            std::getenv("BUMBLE_GRASS_GPU_NO_INTERPOLATION_DIAGNOSTIC") != nullptr;
        app_->userConfig.refreshRate = grass_no_interpolation_diagnostic
            ? RT64::UserConfiguration::RefreshRate::Original
            : initial_interpolation
                ? RT64::UserConfiguration::RefreshRate::Manual
                : RT64::UserConfiguration::RefreshRate::Original;
        app_->userConfig.refreshRateTarget =
            static_cast<int>(initial_requested_hz);
        app_->userConfig.internalColorFormat = RT64::UserConfiguration::InternalColorFormat::Automatic;
        app_->emulatorConfig.framebuffer.renderToRAM = false;
        app_->userConfig.idleWorkActive = true;
        app_->userConfig.developerMode = developer_mode;
        app_->enhancementConfig.f3dex.forceBranch = true;
        app_->enhancementConfig.textureLOD.scale = true;

        const RT64::Application::SetupResult result = app_->setup(
#if defined(_WIN32)
            window_handle.thread_id
#else
            0u
#endif
            , g_contract_validation_enabled.load(std::memory_order_relaxed)
                ? +[](RenderInterface* rhi, RenderDevice* device) {
                    validate_descriptor_contracts(device);
                    bumble::rt64_renderer::validate_texture_contracts(device, rhi->getCapabilities().shaderFormat);
                    const char* stop = std::getenv("BUMBLE_VALIDATION_STOP_AFTER_DEVICE_CHECK");
                    const char* owned = std::getenv("BUMBLE_RT64_VALIDATION_SYNTHETIC_INPUT");
                    if (stop && std::string_view(stop) == "1" && owned && std::string_view(owned) == "1") {
                        throw std::runtime_error("Injected startup failure after device checks.");
                    }
                } : nullptr
        );
        setup_result = map_setup_result(result);
        chosen_api = map_graphics_api(app_->chosenGraphicsAPI);

        if (result != RT64::Application::SetupResult::Success ||
            app_->device == nullptr || app_->swapChain == nullptr ||
            app_->state == nullptr || app_->interpreter == nullptr ||
            app_->presentQueue == nullptr || app_->sharedQueueResources == nullptr ||
            app_->textureCache == nullptr) {
            if (result == RT64::Application::SetupResult::Success) {
                setup_result = ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
            }
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=rt64_setup_failed result=%d device=%d swapchain=%d state=%d interpreter=%d present_queue=%d shared_resources=%d\n",
                static_cast<int>(result),
                app_->device != nullptr,
                app_->swapChain != nullptr,
                app_->state != nullptr,
                app_->interpreter != nullptr,
                app_->presentQueue != nullptr,
                app_->sharedQueueResources != nullptr
            );
            std::fflush(stderr);
            shutdown();
            return;
        }

        configure_texture_dump_directory(*app_->state);

        app_->userConfig.aspectRatio = adaptive_aspect
            ? RT64::UserConfiguration::AspectRatio::Expand
            : RT64::UserConfiguration::AspectRatio::Original;
        app_->userConfig.extAspectRatio = adaptive_aspect
            ? RT64::UserConfiguration::AspectRatio::Expand
            : RT64::UserConfiguration::AspectRatio::Original;
        const uint32_t detected_display_hz =
            app_->sharedQueueResources->swapChainRate;
        app_->userConfig.refreshRateTarget = static_cast<int>(
            bumble::frame_pacing::capped_target_hz(
                initial_requested_hz,
                detected_display_hz
            )
        );
        const uint32_t initial_target_hz =
            initial_interpolation && !grass_no_interpolation_diagnostic
            ? static_cast<uint32_t>(app_->userConfig.refreshRateTarget)
            : bumble::frame_pacing::kOriginalGameplayHz;
        app_->updateUserConfig(false);
        const bool initial_vsync = true;
        app_->swapChain->setVsyncEnabled(initial_vsync);
        frame_pacing_active_ = initial_frame_pacing;

        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=rt64_quality_config"
            " resolution=window_output_scale msaa_samples=%" PRIu32
            " msaa_source=%s"
            " internal_color=automatic filtering=anti_aliased_pixel_scaling"
            " refresh=%s display_rate=%" PRIu32
            " requested_target_rate=%" PRIu32
            " logical_target_rate=%d pacing_owner=rt64_present_queue"
            " idle_gpu_work=1 vsync=%d"
            " interpolation_allocation_safety_ceiling=12"
            " interpolation_policy=fixed_resource_ceiling_uniform_samples"
            " interpolation_sampling=uniform_full_source_interval"
            " interpolation=rt64 fog_scale=%.6f"
            " aspect_mode=%s extended_aspect_mode=%s"
            " shared_aspect_mode=%d shared_extended_aspect_mode=%d\n",
            app_->userConfig.msaaSampleCount(),
            probe_msaa.source,
            frame_pacing_name(initial_frame_pacing),
            app_->sharedQueueResources->swapChainRate,
            initial_requested_hz,
            static_cast<int>(initial_target_hz),
            initial_vsync ? 1 : 0,
            static_cast<double>(g_fog_scale.load(std::memory_order_acquire)),
            adaptive_aspect ? "expand" : "original",
            adaptive_aspect ? "expand" : "original",
            static_cast<int>(
                app_->sharedQueueResources->userConfig.aspectRatio
            ),
            static_cast<int>(
                app_->sharedQueueResources->userConfig.extAspectRatio
            )
        );
        std::fflush(stderr);
        if (grass_no_interpolation_diagnostic) {
            std::fprintf(
                stderr,
                "BUMBLE_GRASS stage=gpu_no_interpolation_diagnostic"
                " refresh=original production_default=0\n"
            );
            std::fflush(stderr);
        }

        if (bumble::menu_background::configured() &&
            !refresh_menu_background_pack(true)) {
            app_->end();
            app_.reset();
            return;
        }

        bumble::widescreen::publish_render_size(
            app_->swapChain->getWidth(),
            app_->swapChain->getHeight(),
            adaptive_aspect
        );
        bumble::widescreen::publish_fog_scale(
            g_fog_scale.load(std::memory_order_acquire)
        );

        if (app_->appWindow != nullptr) {
            app_->appWindow->makeResizable();
        }
#if defined(RT_ENABLED) && RT_ENABLED
        g_raytracing_supported.store(
            app_->device->getCapabilities().raytracing &&
                app_->rtShaderCache != nullptr,
            std::memory_order_release
        );
#else
        g_raytracing_supported.store(false, std::memory_order_release);
#endif
        if (!bumble::frame_pacing::install()) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=frame_pacing_hook_failed"
                " reason=render_hook_already_owned\n"
            );
            std::fflush(stderr);
            app_->end();
            app_.reset();
            return;
        }
        frame_pacing_hook_installed_ = true;
        widescreen_hud_capture_hook_installed_ =
            install_widescreen_hud_capture_hook();
        bumble::death_screen::bind_renderer(
            app_->device.get(),
            app_->shaderLibrary.get(),
            app_->swapChain.get()
        );
        bumble::game_completion_screen::bind_renderer(
            app_->device.get(),
            app_->shaderLibrary.get(),
            app_->swapChain.get()
        );
        if (!bumble::text_overlay::bind_renderer(
                app_->renderInterface.get(),
                app_->device.get(),
                app_->shaderLibrary.get(),
                app_->swapChain.get()
            )) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=text_overlay_bind_failed\n"
            );
            std::fflush(stderr);
        }
        sync_optional_enhancements(true);
        const auto initial_config = ultramodern::renderer::get_graphics_config();
        if (app_->appWindow != nullptr &&
            initial_config.wm_option ==
                ultramodern::renderer::WindowMode::Fullscreen) {
            app_->setFullScreen(true);
        }
        {
            const std::lock_guard lock(g_active_application_mutex);
            g_active_application = app_.get();
        }
        log_stage("rt64_renderer_ready");
        }
        catch (const std::exception& error) {
            std::fprintf(stderr, "BUMBLE_RT64_PROBE stage=rt64_initialization_failed error=%s\n", error.what());
            std::fflush(stderr);
            setup_result = ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
            shutdown();
        }
    }

    ~BumbleRt64Context() override {
        shutdown();
    }

    bool valid() override {
        return app_ != nullptr;
    }

    bool update_config(
        const ultramodern::renderer::GraphicsConfig& old_config,
        const ultramodern::renderer::GraphicsConfig& new_config
    ) override {
        if (app_ == nullptr || old_config == new_config) {
            return false;
        }
        if (new_config.wm_option != old_config.wm_option) {
            const bool fullscreen = new_config.wm_option ==
                ultramodern::renderer::WindowMode::Fullscreen;
            app_->setFullScreen(fullscreen);
            bumble::graphics_options::notify_window_mode_applied(fullscreen);
        }

        const bool widescreen =
            new_config.ar_option == ultramodern::renderer::AspectRatio::Expand;
        app_->userConfig.aspectRatio = widescreen
            ? RT64::UserConfiguration::AspectRatio::Expand
            : RT64::UserConfiguration::AspectRatio::Original;
        app_->userConfig.extAspectRatio =
            new_config.hr_option ==
                    ultramodern::renderer::HUDRatioMode::Full
                ? RT64::UserConfiguration::AspectRatio::Expand
                : RT64::UserConfiguration::AspectRatio::Original;
        const bool aspect_changed =
            new_config.ar_option != old_config.ar_option ||
            new_config.hr_option != old_config.hr_option;
        app_->updateUserConfig(aspect_changed);
        bumble::widescreen::publish_render_size(
            app_->swapChain->getWidth(),
            app_->swapChain->getHeight(),
            widescreen
        );
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=rt64_config_updated fullscreen=%d"
            " aspect=%s discard_framebuffers=%d\n",
            new_config.wm_option ==
                    ultramodern::renderer::WindowMode::Fullscreen
                ? 1
                : 0,
            widescreen ? "expand" : "original",
            aspect_changed ? 1 : 0
        );
        std::fflush(stderr);
        return true;
    }

    void enable_instant_present() override {
        if (app_ == nullptr) {
            return;
        }
        app_->enhancementConfig.presentation.mode =
            RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
        app_->updateEnhancementConfig();
        log_stage("instant_present_enabled");
    }

    void send_dl(const OSTask* task) override {
        if (app_ == nullptr || task == nullptr) {
            return;
        }

        bumble::native_rsp_task::graphics_execution_begin(task);
        const bool main_menu =
            bumble::menu_background::observe_phase(app_->core.RDRAM);
        bumble::graphics_options::publish_lighting_environment(
            app_->core.RDRAM
        );
        app_->state->rsp->reset();
        app_->state->rsp->setFogScale(
            g_fog_scale.load(std::memory_order_acquire)
        );
        app_->state->setRefreshRate(
            single_player_world_active(app_->core.RDRAM)
                ? static_cast<uint16_t>(
                    bumble::frame_pacing::kOriginalGameplayHz
                )
                : 0u
        );
        app_->interpreter->loadUCodeGBI(
            task->t.ucode & 0x03FFFFFFu,
            task->t.ucode_data & 0x03FFFFFFu,
            true
        );
        app_->state->setTexcoordWrapPoint(1024, 1024);
        const bool widescreen_extended_gbi =
            bumble::widescreen::extended_ui_enabled();
        const bool frame_arena_extended_gbi =
            (task->t.data_ptr & 0x03FFFFFFu) >= 0x00800000u;
        const bool grass_extended_gbi =
            bumble::procedural_grass::extended_gbi_required();
        const bool sky_extended_gbi =
            bumble::modern_sky::extended_gbi_required();
        const bool collision_extended_gbi =
            bumble::collision_debug::extended_gbi_required();
        const bool electric_extended_gbi =
            bumble::electric_effect::extended_gbi_required();
        {
            app_->state->enableExtendedGBI(0x64u);
            if (!extended_gbi_logged_) {
                std::fprintf(
                    stderr,
                    "BUMBLE_RT64_PROBE stage=extended_gbi_features"
                    " enabled=1 opcode=0x64 hud_boundary=1 widescreen_ui=%d"
                    " frame_arena=%d grass=%d sky=%d"
                    " collision=%d electric=%d\n",
                    widescreen_extended_gbi ? 1 : 0,
                    frame_arena_extended_gbi ? 1 : 0,
                    grass_extended_gbi ? 1 : 0,
                    sky_extended_gbi ? 1 : 0,
                    collision_extended_gbi ? 1 : 0,
                    electric_extended_gbi ? 1 : 0
                );
                std::fflush(stderr);
                extended_gbi_logged_ = true;
            }
        }
        app_->processDisplayLists(
            app_->core.RDRAM,
            task->t.data_ptr & 0x03FFFFFFu,
            0,
            true
        );
        validate_menu_replacements(main_menu);
        bumble::native_rsp_task::graphics_execution_complete(true);

        const uint32_t count = g_display_lists.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count == 60 || count == 600) {
            std::fprintf(stderr, "BUMBLE_RT64_PROBE stage=display_list_presented count=%" PRIu32 "\n", count);
            std::fflush(stderr);
        }
    }

    void send_dummy_workload(uint32_t fb_address) override {
        if (app_ == nullptr) {
            return;
        }
        app_->state->listProcessBegin();
        app_->state->rdp->setColorImage(
            G_IM_FMT_RGBA,
            G_IM_SIZ_16b,
            320,
            fb_address
        );
        app_->state->rdp->setOtherMode(0x382C30, 0);
        app_->state->rdp->fillRect(0, 0, 320 << 2, 240 << 2);
        app_->state->fullSync();
        app_->state->listProcessEnd();
    }

    void update_screen() override {
        if (app_ == nullptr) {
            return;
        }

        if (bumble::menu_background::configured()) {
            refresh_menu_background_pack(false);
        }
        sync_optional_enhancements(false);
        publish_profile_context();
        bumble::widescreen::publish_render_size(
            app_->swapChain->getWidth(),
            app_->swapChain->getHeight(),
            bumble::graphics_options::widescreen_enabled()
        );

        const bool player_checkpoint_before_update =
            bumble::native_checkpoint::mission1_player_observed();
        if (player_checkpoint_before_update) {
            uint32_t unset_baseline = UINT32_MAX;
            const uint32_t display_list_baseline =
                g_display_lists.load(std::memory_order_acquire);
            g_player_checkpoint_display_list_baseline.compare_exchange_strong(
                unset_baseline,
                display_list_baseline,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            );
        }
        const bool mission2_checkpoint_before_update =
            bumble::native_checkpoint::mission2_player_observed();
        if (mission2_checkpoint_before_update) {
            uint32_t unset_baseline = UINT32_MAX;
            const uint32_t display_list_baseline =
                g_display_lists.load(std::memory_order_acquire);
            g_mission2_checkpoint_display_list_baseline.compare_exchange_strong(
                unset_baseline,
                display_list_baseline,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            );
        }
        const uint32_t campaign_checkpoint_before_update =
            bumble::native_checkpoint::campaign_player_level_observed();
        const uint32_t committed_campaign_level =
            bumble::native_checkpoint::campaign_selection_committed_index();
        if (campaign_checkpoint_before_update != 0u &&
            campaign_checkpoint_before_update == committed_campaign_level) {
            uint32_t unset_baseline = UINT32_MAX;
            const uint32_t display_list_baseline =
                g_display_lists.load(std::memory_order_acquire);
            if (g_campaign_checkpoint_display_list_baseline.compare_exchange_strong(
                    unset_baseline,
                    display_list_baseline,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire
                )) {
                g_campaign_checkpoint_level.store(
                    campaign_checkpoint_before_update,
                    std::memory_order_release
                );
            }
        }
        const bool two_player_checkpoint_before_update =
            bumble::native_checkpoint::two_player_checkpoint_observed();
        if (two_player_checkpoint_before_update) {
            uint32_t unset_baseline = UINT32_MAX;
            const uint32_t display_list_baseline =
                g_display_lists.load(std::memory_order_acquire);
            g_two_player_checkpoint_display_list_baseline.compare_exchange_strong(
                unset_baseline,
                display_list_baseline,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            );
        }
        app_->updateScreen();
        const uint32_t count = g_screen_updates.fetch_add(1, std::memory_order_relaxed) + 1;
        log_frame_pacing_statistics(count);
        const uint32_t display_list_count =
            g_display_lists.load(std::memory_order_acquire);
        const uint32_t display_list_baseline =
            g_player_checkpoint_display_list_baseline.load(std::memory_order_acquire);
        if (display_list_baseline != UINT32_MAX &&
            display_list_count > display_list_baseline &&
            !g_mission1_screen_logged.exchange(true, std::memory_order_acq_rel)) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=mission1_screen_presented"
                " screen_count=%" PRIu32 " display_list_count=%" PRIu32
                " player_checkpoint_display_list_baseline=%" PRIu32 "\n",
                count,
                display_list_count,
                display_list_baseline
            );
            std::fflush(stderr);
        }
        const uint32_t mission2_display_list_baseline =
            g_mission2_checkpoint_display_list_baseline.load(
                std::memory_order_acquire
            );
        if (mission2_display_list_baseline != UINT32_MAX &&
            display_list_count > mission2_display_list_baseline &&
            !g_mission2_screen_logged.exchange(true, std::memory_order_acq_rel)) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=mission2_screen_presented"
                " screen_count=%" PRIu32 " display_list_count=%" PRIu32
                " mission2_checkpoint_display_list_baseline=%" PRIu32 "\n",
                count,
                display_list_count,
                mission2_display_list_baseline
            );
            std::fflush(stderr);
        }
        const uint32_t campaign_display_list_baseline =
            g_campaign_checkpoint_display_list_baseline.load(
                std::memory_order_acquire
            );
        if (campaign_display_list_baseline != UINT32_MAX &&
            display_list_count > campaign_display_list_baseline &&
            !g_campaign_screen_logged.exchange(true, std::memory_order_acq_rel)) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=campaign_screen_presented"
                " level_index=%" PRIu32 " screen_count=%" PRIu32
                " display_list_count=%" PRIu32
                " campaign_checkpoint_display_list_baseline=%" PRIu32 "\n",
                g_campaign_checkpoint_level.load(std::memory_order_acquire),
                count,
                display_list_count,
                campaign_display_list_baseline
            );
            std::fflush(stderr);
        }
        const uint32_t two_player_display_list_baseline =
            g_two_player_checkpoint_display_list_baseline.load(std::memory_order_acquire);
        if (two_player_display_list_baseline != UINT32_MAX &&
            display_list_count > two_player_display_list_baseline &&
            !g_two_player_screen_logged.exchange(true, std::memory_order_acq_rel)) {
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=two_player_screen_presented"
                " screen_count=%" PRIu32 " display_list_count=%" PRIu32
                " two_player_checkpoint_display_list_baseline=%" PRIu32
                " positive_control=%d\n",
                count,
                display_list_count,
                two_player_display_list_baseline,
                bumble::native_checkpoint::two_player_positive_control_observed()
            );
            std::fflush(stderr);
        }
        if (count == 1 || count == 2 || count == 120 || count == 600) {
            std::fprintf(stderr, "BUMBLE_RT64_PROBE stage=screen_presented count=%" PRIu32 "\n", count);
            std::fflush(stderr);
        }
    }

    void shutdown() override {
        if (app_ != nullptr) {
            {
                const std::lock_guard lock(g_active_application_mutex);
                if (g_active_application == app_.get()) {
                    g_active_application = nullptr;
                }
            }
            std::fprintf(
                stderr,
                "BUMBLE_RT64_SHUTDOWN stage=renderer_shutdown_enter\n"
            );
            std::fflush(stderr);
            bumble::frame_pacing::configure(0u);
            // Stop new callbacks now; keep readback alive until Application::end joins them.
            if (widescreen_hud_capture_hook_installed_) {
                RT64::SetRenderCaptureHooks(nullptr, nullptr);
            }
            app_->end();
            if (widescreen_hud_capture_hook_installed_) {
                uninstall_widescreen_hud_capture_hook();
                widescreen_hud_capture_hook_installed_ = false;
            }
            if (frame_pacing_hook_installed_) {
                bumble::frame_pacing::uninstall();
                frame_pacing_hook_installed_ = false;
            }
            std::fprintf(
                stderr,
                "BUMBLE_RT64_SHUTDOWN stage=application_end_returned\n"
            );
            std::fflush(stderr);
            app_.reset();
            std::fprintf(
                stderr,
                "BUMBLE_RT64_SHUTDOWN stage=application_reset_returned\n"
            );
            std::fflush(stderr);
            log_stage("rt64_renderer_shutdown");
        }
    }

    uint32_t get_display_framerate() const override {
        if (app_ == nullptr || app_->sharedQueueResources == nullptr ||
            app_->sharedQueueResources->swapChainRate == 0) {
            return 60;
        }
        return app_->sharedQueueResources->swapChainRate;
    }

    float get_resolution_scale() const override {
        constexpr uint32_t kReferenceHeight = 240u;
        if (app_ == nullptr || app_->sharedQueueResources == nullptr) {
            return 1.0f;
        }
        switch (app_->userConfig.resolution) {
        case RT64::UserConfiguration::Resolution::WindowIntegerScale:
            if (app_->sharedQueueResources->swapChainHeight > 0) {
                return std::max(
                    static_cast<float>(
                        app_->sharedQueueResources->swapChainHeight
                    ) / static_cast<float>(kReferenceHeight),
                    1.0f
                );
            }
            return 1.0f;
        case RT64::UserConfiguration::Resolution::Manual:
            return static_cast<float>(app_->userConfig.resolutionMultiplier);
        case RT64::UserConfiguration::Resolution::Original:
        default:
            return 1.0f;
        }
    }

private:
    static void check_interrupts() {}

    void publish_profile_context() {
        if (!RT64::PerformanceProfiler::captureEnabled()) {
            return;
        }

        const uint32_t world_active = rdram_ != nullptr
            ? read_guest_u32(
                  rdram_,
                  kFrontendStateOffset + kFrontendWorldActiveOffset
              )
            : 0u;
        const uint32_t player_layout = rdram_ != nullptr
            ? read_guest_u32(
                  rdram_,
                  kFrontendStateOffset + kFrontendPlayerLayoutOffset
              )
            : 0u;
        const uint64_t world_state =
            (uint64_t(player_layout) << 32u) | uint64_t(world_active);
        if (world_state != profile_world_state_) {
            profile_world_state_ = world_state;
            RT64::PerformanceProfiler::recordInstant(
                RT64::PerformanceCategory::Frame,
                "Host.Context.World",
                ++profile_context_sequence_,
                world_active,
                player_layout
            );
        }

        const uint32_t frontend_phase =
            bumble::native_checkpoint::last_frontend_phase();
        const uint32_t frontend_descriptor =
            bumble::native_checkpoint::last_frontend_descriptor();
        const uint32_t frontend_item =
            bumble::native_checkpoint::last_frontend_descriptor_item();
        const uint64_t frontend_state =
            (uint64_t(frontend_descriptor) << 32u) | frontend_phase;
        if (frontend_state != profile_frontend_state_ ||
            frontend_item != profile_frontend_item_) {
            profile_frontend_state_ = frontend_state;
            profile_frontend_item_ = frontend_item;
            RT64::PerformanceProfiler::recordInstant(
                RT64::PerformanceCategory::Frame,
                "Host.Context.Frontend",
                ++profile_context_sequence_,
                frontend_phase,
                (uint64_t(frontend_descriptor) << 32u) | frontend_item
            );
        }

        const uint32_t selected_level =
            bumble::native_checkpoint::campaign_selector_index();
        const uint32_t committed_level =
            bumble::native_checkpoint::campaign_selection_committed_index();
        const uint32_t observed_level =
            bumble::native_checkpoint::campaign_player_level_observed();
        const uint32_t grid_slot =
            bumble::native_checkpoint::campaign_grid_selected_slot();
        const bool grid_active =
            bumble::native_checkpoint::campaign_grid_active();
        const uint64_t campaign_state0 =
            (uint64_t(committed_level) << 32u) | selected_level;
        const uint64_t campaign_state1 =
            (uint64_t(grid_active ? 1u : 0u) << 63u) |
            (uint64_t(grid_slot) << 32u) | observed_level;
        if (campaign_state0 != profile_campaign_state0_ ||
            campaign_state1 != profile_campaign_state1_) {
            profile_campaign_state0_ = campaign_state0;
            profile_campaign_state1_ = campaign_state1;
            RT64::PerformanceProfiler::recordInstant(
                RT64::PerformanceCategory::Frame,
                "Host.Context.Campaign",
                ++profile_context_sequence_,
                campaign_state0,
                campaign_state1
            );
        }

        if (world_active != 0u && player_layout == kSinglePlayerLayout &&
            rdram_ != nullptr) {
            const uint64_t sample = ++profile_spatial_sample_sequence_;
            const bumble::native_checkpoint::RuntimePerformanceCounts
                runtime_counts =
                    bumble::native_checkpoint::runtime_performance_counts();
            RT64::PerformanceProfiler::recordCounter(
                "Host.Game.ActiveActors",
                runtime_counts.active_actors,
                sample
            );
            RT64::PerformanceProfiler::recordCounter(
                "Host.Game.RenderedActors",
                runtime_counts.rendered_actors,
                sample
            );
            RT64::PerformanceProfiler::recordCounter(
                "Host.Game.ParticleActors",
                runtime_counts.particle_actors,
                sample
            );
            for (size_t family = 0u;
                 family < runtime_counts.weapon_actors.size();
                 ++family) {
                const auto actor_family =
                    static_cast<bumble::weapon_system::ActorFamily>(family);
                RT64::PerformanceProfiler::recordCounter(
                    bumble::weapon_system::actor_family_profiler_name(
                        actor_family
                    ),
                    runtime_counts.weapon_actors[family],
                    sample
                );
            }
            const uint32_t player = read_guest_u32(
                rdram_,
                kPlayerOneOwnerSlotOffset
            );
            const uint32_t player_offset = player & kGuestRdramMask;
            const bool player_valid =
                (player & 0xE0000000u) == kGuestVirtualBase &&
                player_offset <= kGuestRdramSize -
                    (kPlayerPositionOffset + 12u);
            RT64::PerformanceProfiler::recordCounter(
                "Host.Player.Valid",
                player_valid ? 1.0 : 0.0,
                sample
            );
            if (player_valid) {
                const auto player_component = [&](uint32_t offset) {
                    return std::bit_cast<float>(read_guest_u32(
                        rdram_,
                        player_offset + kPlayerPositionOffset + offset
                    ));
                };
                const float player_x = player_component(0u);
                const float player_y = player_component(4u);
                const float player_z = player_component(8u);
                if (std::isfinite(player_x) && std::isfinite(player_y) &&
                    std::isfinite(player_z)) {
                    RT64::PerformanceProfiler::recordCounter(
                        "Host.Player.X",
                        player_x,
                        sample
                    );
                    RT64::PerformanceProfiler::recordCounter(
                        "Host.Player.Y",
                        player_y,
                        sample
                    );
                    RT64::PerformanceProfiler::recordCounter(
                        "Host.Player.Z",
                        player_z,
                        sample
                    );
                }
            }

            const bumble::widescreen::VisibilityCameraXZ camera =
                bumble::widescreen::visibility_camera_xz();
            RT64::PerformanceProfiler::recordCounter(
                "Host.Camera.Valid",
                camera.valid ? 1.0 : 0.0,
                sample
            );
            if (camera.valid) {
                RT64::PerformanceProfiler::recordCounter(
                    "Host.Camera.EyeX",
                    camera.eye_x,
                    sample
                );
                RT64::PerformanceProfiler::recordCounter(
                    "Host.Camera.EyeZ",
                    camera.eye_z,
                    sample
                );
                RT64::PerformanceProfiler::recordCounter(
                    "Host.Camera.CullX",
                    camera.cull_x,
                    sample
                );
                RT64::PerformanceProfiler::recordCounter(
                    "Host.Camera.CullZ",
                    camera.cull_z,
                    sample
                );
            }

            const bumble::widescreen::TerrainVisibilitySample terrain =
                bumble::widescreen::terrain_visibility_sample();
            RT64::PerformanceProfiler::recordCounter(
                "Host.TerrainVisibility.Valid",
                terrain.valid ? 1.0 : 0.0,
                sample
            );
            if (terrain.valid) {
                RT64::PerformanceProfiler::recordCounter(
                    "Host.TerrainVisibility.OriginalCells",
                    terrain.original_cells,
                    sample
                );
                RT64::PerformanceProfiler::recordCounter(
                    "Host.TerrainVisibility.CandidateCells",
                    terrain.candidate_cells,
                    sample
                );
                RT64::PerformanceProfiler::recordCounter(
                    "Host.TerrainVisibility.VisibleCells",
                    terrain.visible_cells,
                    sample
                );
                RT64::PerformanceProfiler::recordCounter(
                    "Host.TerrainVisibility.FirstRow",
                    terrain.first_row,
                    sample
                );
                RT64::PerformanceProfiler::recordCounter(
                    "Host.TerrainVisibility.LastRow",
                    terrain.last_row,
                    sample
                );
            }
        }
    }

    void log_frame_pacing_statistics(uint64_t source_vi_count) {
        if (app_ == nullptr || app_->swapChain == nullptr ||
            app_->sharedQueueResources == nullptr) {
            return;
        }
        const char* report_path =
            std::getenv("BUMBLE_RT64_FRAME_PACING_REPORT");
        if (report_path == nullptr || report_path[0] == '\0') {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = now - frame_statistics_time_;
        if (elapsed < std::chrono::seconds(1)) {
            return;
        }

        const double elapsed_seconds =
            std::chrono::duration<double>(elapsed).count();
        const uint64_t rendered = app_->sharedQueueResources
            ->renderedFrameCount.load(std::memory_order_relaxed);
        const uint64_t presented = app_->sharedQueueResources
            ->presentedFrameCount.load(std::memory_order_relaxed);
        RT64::WorkloadQueue* workload_queue = app_->workloadQueue.get();
        const uint64_t requested_interpolation_frames =
            workload_queue != nullptr
                ? workload_queue->performanceRequestedInterpolationFrames.load(
                      std::memory_order_relaxed
                  )
                : 0;
        const uint64_t completed_interpolation_frames =
            workload_queue != nullptr
                ? workload_queue->performanceCompletedInterpolationFrames.load(
                      std::memory_order_relaxed
                  )
                : 0;
        const uint64_t skipped_interpolation_frames =
            workload_queue != nullptr
                ? workload_queue->performanceSkippedInterpolationFrames.load(
                      std::memory_order_relaxed
                  )
                : 0;
        const uint64_t superseded_interpolation_sets =
            workload_queue != nullptr
                ? workload_queue->performanceSupersededInterpolationSets.load(
                      std::memory_order_relaxed
                  )
                : 0;
        const uint64_t requested_interpolation_delta =
            requested_interpolation_frames -
            frame_statistics_requested_interpolation_frames_;
        const uint64_t completed_interpolation_delta =
            completed_interpolation_frames -
            frame_statistics_completed_interpolation_frames_;
        const uint64_t skipped_interpolation_delta =
            skipped_interpolation_frames -
            frame_statistics_skipped_interpolation_frames_;
        const uint64_t superseded_interpolation_delta =
            superseded_interpolation_sets -
            frame_statistics_superseded_interpolation_sets_;
        const double source_vi_rate =
            static_cast<double>(source_vi_count - frame_statistics_source_vi_) /
            elapsed_seconds;
        const double rendered_rate =
            static_cast<double>(rendered - frame_statistics_rendered_) /
            elapsed_seconds;
        const double presented_rate =
            static_cast<double>(presented - frame_statistics_presented_) /
            elapsed_seconds;
        const bumble::frame_pacing::Statistics interval_statistics =
            bumble::frame_pacing::consume_statistics();
        const bumble::native_checkpoint::RuntimePerformanceCounts
            runtime_counts =
                bumble::native_checkpoint::runtime_performance_counts();
        const bumble::object_cull_telemetry::PerformanceSample cull_sample =
            bumble::object_cull_telemetry::consume_performance_sample();
        const std::vector<bumble::text_overlay::Observation>
            overlay_observations =
                bumble::text_overlay::active_observations();
        std::array<uint32_t, 6> overlay_kind_counts{};
        for (const bumble::text_overlay::Observation& observation :
             overlay_observations) {
            const size_t kind_index = static_cast<size_t>(observation.kind);
            if (kind_index < overlay_kind_counts.size()) {
                ++overlay_kind_counts[kind_index];
            }
        }
        const uint32_t frontend_phase =
            bumble::native_checkpoint::last_frontend_phase();
        uint32_t target_rate = 0;
        uint32_t display_rate = 0;
        uint32_t source_workload_rate = 0;
        uint32_t shared_refresh_mode = 0;
        int shared_refresh_target = 0;
        {
            const std::scoped_lock lock(
                app_->sharedQueueResources->configurationMutex
            );
            target_rate = app_->sharedQueueResources->targetRate;
            display_rate = app_->sharedQueueResources->swapChainRate;
            source_workload_rate = app_->sharedQueueResources->viOriginalRate;
            shared_refresh_mode = static_cast<uint32_t>(
                app_->sharedQueueResources->userConfig.refreshRate
            );
            shared_refresh_target =
                app_->sharedQueueResources->userConfig.refreshRateTarget;
        }

        {
            static std::string active_report_path;
            static std::ofstream report_stream;
            if (active_report_path != report_path) {
                report_stream.close();
                active_report_path = report_path;
                report_stream.open(
                    active_report_path,
                    std::ios::out | std::ios::trunc
                );
                if (report_stream.is_open()) {
                    report_stream
                        << "source_vi_hz,rendered_hz,presented_hz,p50_ms,p95_ms,"
                           "p99_ms,max_ms,intervals,over_16_67ms,over_33_33ms,"
                           "renderer_cpu_ms,renderer_gpu_ms,gpu_setup_ms,"
                           "gpu_framebuffer_ms,gpu_pair0_raster_ms,"
                           "gpu_pair0_post_ms,gpu_pair1_raster_ms,"
                           "gpu_pair1_post_ms,gpu_framebuffer_remainder_ms,"
                           "matching_cpu_ms,workload_cpu_ms,"
                           "instance_entries,raster_scene_entries,"
                           "specialized_pipelines,uber_pipelines,game_calls,"
                           "submitted_triangles,submitted_vertices,fb_pairs,"
                           "reflection_draws,reflection_plane_culled,"
                           "main_frustum_culled,"
                           "active_actors,rendered_actors,particle_actors,"
                           "frontend_phase,overlay_script,overlay_briefing,"
                           "overlay_gameplay_hud,overlay_credits,overlay_menu,"
                           "overlay_editor,"
                           "player_x,player_y,player_z,player_yaw,"
                           "cull_samples,cull_sample_ms,target_rate,"
                           "display_rate,source_workload_rate,"
                           "app_refresh_mode,app_refresh_target,"
                           "shared_refresh_mode,shared_refresh_target,"
                           "interpolation_requested,interpolation_completed,"
                           "interpolation_skipped,interpolation_superseded_sets,"
                           "workload_lock_wait_ms,workload_build_ms,"
                           "workload_upload_wait_ms,workload_fence_wait_ms,"
                           "workload_relock_wait_ms,present_mutex_wait_ms,"
                           "present_interpolation_wait_ms,present_acquire_ms,"
                           "present_gpu_wait_ms,present_swap_wait_ms,"
                           "present_call_ms,interpolation_set_mismatches,"
                           "last_counter_set_id,last_target_set_id,"
                           "present_acquire_failures,vi_address,"
                           "present_fb_address,present_fb_write_timestamp,"
                           "workload_interpolation_address,"
                           "presented_target_address,"
                           "framebuffer_set_mismatches,"
                           "counter_target_address,"
                           "selected_present_fb_address\n";
                }
            }
            if (report_stream.is_open()) {
                report_stream
                    << source_vi_rate << ','
                    << rendered_rate << ','
                    << presented_rate << ','
                    << interval_statistics.p50_interval_ms << ','
                    << interval_statistics.p95_interval_ms << ','
                    << interval_statistics.p99_interval_ms << ','
                    << interval_statistics.maximum_interval_ms << ','
                    << interval_statistics.interval_count << ','
                    << interval_statistics.intervals_over_16_67_ms << ','
                    << interval_statistics.intervals_over_33_33_ms << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceRendererCpuMs.load()
                            : 0.0) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceRendererGpuMs.load()
                            : 0.0) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceRendererGpuSetupMs.load()
                            : 0.0) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceRendererGpuFramebufferMs.load()
                            : 0.0) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceRendererGpuPair0RasterMs.load()
                            : 0.0) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceRendererGpuPair0PostMs.load()
                            : 0.0) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceRendererGpuPair1RasterMs.load()
                            : 0.0) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceRendererGpuPair1PostMs.load()
                            : 0.0) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceRendererGpuFramebufferRemainderMs.load()
                            : 0.0) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceMatchingCpuMs.load()
                            : 0.0) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceWorkloadCpuMs.load()
                            : 0.0) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceGpuDrawCalls.load()
                            : 0u) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceRasterSceneEntries.load()
                            : 0u) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceSpecializedPipelines.load()
                            : 0u) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceUberPipelines.load()
                            : 0u) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceGameCalls.load()
                            : 0u) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceSubmittedTriangles.load()
                            : 0u) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceSubmittedVertices.load()
                            : 0u) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceFramebufferPairs.load()
                            : 0u) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceReflectionDraws.load()
                            : 0u) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceReflectionPlaneCulled.load()
                            : 0u) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceMainFrustumCulled.load()
                            : 0u) << ','
                    << runtime_counts.active_actors << ','
                    << runtime_counts.rendered_actors << ','
                    << runtime_counts.particle_actors << ','
                    << frontend_phase << ','
                    << overlay_kind_counts[static_cast<size_t>(
                           bumble::text_overlay::TextKind::Script)] << ','
                    << overlay_kind_counts[static_cast<size_t>(
                           bumble::text_overlay::TextKind::Briefing)] << ','
                    << overlay_kind_counts[static_cast<size_t>(
                           bumble::text_overlay::TextKind::GameplayHud)] << ','
                    << overlay_kind_counts[static_cast<size_t>(
                           bumble::text_overlay::TextKind::Credits)] << ','
                    << overlay_kind_counts[static_cast<size_t>(
                           bumble::text_overlay::TextKind::Menu)] << ','
                    << overlay_kind_counts[static_cast<size_t>(
                           bumble::text_overlay::TextKind::Editor)] << ','
                    << runtime_counts.player_x << ','
                    << runtime_counts.player_y << ','
                    << runtime_counts.player_z << ','
                    << runtime_counts.player_yaw << ','
                    << cull_sample.sample_count << ','
                    << (static_cast<double>(cull_sample.sample_nanoseconds) /
                        1000000.0) << ','
                    << target_rate << ','
                    << display_rate << ','
                    << source_workload_rate << ','
                    << static_cast<uint32_t>(app_->userConfig.refreshRate) << ','
                    << app_->userConfig.refreshRateTarget << ','
                    << shared_refresh_mode << ','
                    << shared_refresh_target << ','
                    << requested_interpolation_delta << ','
                    << completed_interpolation_delta << ','
                    << skipped_interpolation_delta << ','
                    << superseded_interpolation_delta << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceRendererLockWaitMs.load()
                            : 0.0) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceRendererBuildMs.load()
                            : 0.0) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceRendererUploadWaitMs.load()
                            : 0.0) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceRendererFenceWaitMs.load()
                            : 0.0) << ','
                    << (workload_queue != nullptr
                            ? workload_queue->performanceRendererRelockWaitMs.load()
                            : 0.0) << ','
                    << (app_->presentQueue != nullptr
                            ? app_->presentQueue->performanceWorkloadMutexWaitMs.load()
                            : 0.0) << ','
                    << (app_->presentQueue != nullptr
                            ? app_->presentQueue->performanceInterpolationWaitMs.load()
                            : 0.0) << ','
                    << (app_->presentQueue != nullptr
                            ? app_->presentQueue->performanceAcquireMs.load()
                            : 0.0) << ','
                    << (app_->presentQueue != nullptr
                            ? app_->presentQueue->performanceGpuWaitMs.load()
                            : 0.0) << ','
                    << (app_->presentQueue != nullptr
                            ? app_->presentQueue->performanceSwapWaitMs.load()
                            : 0.0) << ','
                    << (app_->presentQueue != nullptr
                            ? app_->presentQueue->performancePresentCallMs.load()
                            : 0.0) << ','
                    << (app_->presentQueue != nullptr
                            ? app_->presentQueue
                                  ->performanceInterpolationSetMismatches.load()
                            : 0u) << ','
                    << (app_->presentQueue != nullptr
                            ? app_->presentQueue
                                  ->performanceLastCounterSetId.load()
                            : 0u) << ','
                    << (app_->presentQueue != nullptr
                            ? app_->presentQueue
                                  ->performanceLastTargetSetId.load()
                            : 0u) << ','
                    << (app_->presentQueue != nullptr
                            ? app_->presentQueue
                                  ->performanceAcquireFailures.load()
                            : 0u) << ','
                    << app_->sharedQueueResources->diagnosticViAddress.load()
                    << ','
                    << app_->sharedQueueResources
                           ->diagnosticPresentFramebufferAddress.load() << ','
                    << app_->sharedQueueResources
                           ->diagnosticPresentFramebufferWriteTimestamp.load()
                    << ','
                    << app_->sharedQueueResources
                           ->diagnosticWorkloadInterpolationAddress.load() << ','
                    << app_->sharedQueueResources
                           ->diagnosticPresentedTargetAddress.load() << ','
                    << (app_->presentQueue != nullptr
                            ? app_->presentQueue
                                  ->performanceFramebufferSetMismatches.load()
                            : 0u) << ','
                    << (app_->presentQueue != nullptr
                            ? app_->presentQueue
                                  ->performanceLastCounterTargetAddress.load()
                            : 0u) << ','
                    << (app_->presentQueue != nullptr
                            ? app_->presentQueue
                                  ->performanceLastPresentFramebufferAddress.load()
                            : 0u) << '\n';
            }
        }
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=frame_pacing_statistics"
            " mode=%s source_vi_updates_per_s=%.3f rendered_frames_per_s=%.3f"
            " presented_frames_per_s=%.3f source_vi_total=%" PRIu64
            " rendered_total=%" PRIu64 " presented_total=%" PRIu64
            " logical_target_rate=%" PRIu32 " display_rate=%" PRIu32
            " source_workload_rate=%" PRIu32
            " pacing_owner=rt64_present_queue idle_gpu_work=1"
            " vsync=%d present_intervals=%" PRIu64
            " present_interval_mean_ms=%.3f present_interval_p50_ms=%.3f"
            " present_interval_p95_ms=%.3f present_interval_p99_ms=%.3f"
            " present_interval_max_ms=%.3f"
            " present_intervals_over_150pct=%" PRIu64
            " present_intervals_over_200pct=%" PRIu64
            " present_intervals_over_16_67ms=%" PRIu64
            " present_intervals_over_33_33ms=%" PRIu64
            " interpolation_allocation_safety_ceiling=12"
            " interpolation_policy=fixed_resource_ceiling_uniform_samples"
            " interpolation_sampling=uniform_full_source_interval\n",
            frame_pacing_name(frame_pacing_active_),
            source_vi_rate,
            rendered_rate,
            presented_rate,
            source_vi_count,
            rendered,
            presented,
            target_rate,
            display_rate,
            source_workload_rate,
            app_->swapChain->isVsyncEnabled() ? 1 : 0,
            interval_statistics.interval_count,
            interval_statistics.mean_interval_ms,
            interval_statistics.p50_interval_ms,
            interval_statistics.p95_interval_ms,
            interval_statistics.p99_interval_ms,
            interval_statistics.maximum_interval_ms,
            interval_statistics.intervals_over_150_percent,
            interval_statistics.intervals_over_200_percent,
            interval_statistics.intervals_over_16_67_ms,
            interval_statistics.intervals_over_33_33_ms
        );
        std::fflush(stderr);

        frame_statistics_time_ = now;
        frame_statistics_source_vi_ = source_vi_count;
        frame_statistics_rendered_ = rendered;
        frame_statistics_presented_ = presented;
        frame_statistics_requested_interpolation_frames_ =
            requested_interpolation_frames;
        frame_statistics_completed_interpolation_frames_ =
            completed_interpolation_frames;
        frame_statistics_skipped_interpolation_frames_ =
            skipped_interpolation_frames;
        frame_statistics_superseded_interpolation_sets_ =
            superseded_interpolation_sets;
    }

    bool refresh_menu_background_pack(bool required) {
        if (app_ == nullptr || app_->swapChain == nullptr ||
            app_->textureCache == nullptr) {
            return !required;
        }

        const uint32_t width = app_->swapChain->getWidth();
        const uint32_t height = app_->swapChain->getHeight();
        const bumble::menu_background::Variant variant =
            bumble::graphics_options::widescreen_enabled()
                ? bumble::menu_background::variant_for_window(width, height)
                : bumble::menu_background::Variant::Standard4x3;
        const bool faithful_textures_requested =
            bumble::graphics_options::hd_terrain_enabled();
        const bool enhanced = bumble::graphics_options::enhanced_textures_enabled();
        if (menu_background_pack_loaded_ && variant == menu_background_variant_) {
            if (faithful_texture_pack_loaded_ != faithful_textures_requested ||
                enhanced_texture_pack_selected_ != enhanced) {
                std::vector<bool> selection{faithful_textures_requested && !enhanced, true};
                if (bumble::first_run::enhanced_textures_available()) selection.push_back(faithful_textures_requested && enhanced);
                if (!app_->textureCache->queueReplacementDirectorySelection(selection))
                    return false;
                faithful_texture_pack_loaded_ = faithful_textures_requested;
                enhanced_texture_pack_selected_ = enhanced;
            }
            return true;
        }

        const std::filesystem::path directory =
            bumble::menu_background::replacement_directory(variant);
        const std::filesystem::path faithful_texture_root =
            directory.parent_path().parent_path() / "textures";
        const auto faithful_contract = load_faithful_texture_pack_contract(faithful_texture_root);
        const bool has_enhanced = bumble::first_run::enhanced_textures_available();
        const auto enhanced_contract = has_enhanced ? load_faithful_texture_pack_contract(
            faithful_texture_root.parent_path() / "textures-enhanced", true) : FaithfulTexturePackContract{};
        if (!faithful_contract.valid || (has_enhanced && !enhanced_contract.valid)) return false;
        std::vector<RT64::ReplacementDirectory> replacement_directories{};
        replacement_directories.emplace_back(faithful_contract.pack_path);
        replacement_directories.back().enabled = faithful_textures_requested && !enhanced;
        replacement_directories.emplace_back(directory);
        if (has_enhanced) {
            replacement_directories.emplace_back(enhanced_contract.pack_path);
            replacement_directories.back().enabled = faithful_textures_requested && enhanced;
        }
        if (!app_->textureCache->loadReplacementDirectories(
                replacement_directories)) {
            std::fprintf(
                stderr,
                "BUMBLE_MENU_BACKGROUND stage=rt64_pack_failed variant=%s"
                " path=%s faithful_textures_enabled=%d texture_pack=%s"
                " window_width=%" PRIu32 " window_height=%" PRIu32
                "\n",
                bumble::menu_background::variant_name(variant),
                directory.string().c_str(),
                faithful_textures_requested ? 1 : 0,
                faithful_contract.pack_path.string().c_str(),
                width,
                height
            );
            std::fflush(stderr);
            return false;
        }

        uint32_t resolved_count = 0;
        uint32_t resolved_suppressed_logo_count = 0;
        uint32_t resolved_gameplay_ui_count = 0;
        uint32_t resolved_faithful_count = 0;
        uint32_t resolved_enhanced_count = 0;
        size_t resolved_faithful_path_count = 0u;
        {
            const std::lock_guard lock(app_->textureCache->textureMapMutex);
            const auto& resolved_paths = app_->textureCache->textureMap
                .replacementMap.fileSystemResolvedPaths;
            const size_t expected_file_systems = has_enhanced ? 3u : 2u;
            if (resolved_paths.size() == expected_file_systems) {
                const size_t menu_path_index = 1u;
                resolved_count =
                    static_cast<uint32_t>(resolved_paths[menu_path_index].size());
                for (const uint64_t hash : kSuppressedMenuLogoTextureHashes) {
                    if (resolved_paths[menu_path_index].find(hash) !=
                        resolved_paths[menu_path_index].end()) {
                        ++resolved_suppressed_logo_count;
                    }
                }
                for (const uint64_t hash : kGameplayUiTextureHashes) {
                    if (resolved_paths[menu_path_index].find(hash) !=
                        resolved_paths[menu_path_index].end()) {
                        ++resolved_gameplay_ui_count;
                    }
                }
                {
                    resolved_faithful_path_count = resolved_paths[0].size();
                    for (const uint64_t hash : faithful_contract.hashes) {
                        if (resolved_paths[0].find(hash) !=
                            resolved_paths[0].end()) {
                            ++resolved_faithful_count;
                        }
                    }
                    for (const uint64_t hash : enhanced_contract.hashes)
                        resolved_enhanced_count += resolved_paths[2].count(hash);
                }
            }
        }
        if (resolved_count != 135 ||
            resolved_suppressed_logo_count != 7 ||
            resolved_gameplay_ui_count != 8 ||
            resolved_enhanced_count != enhanced_contract.hashes.size() ||
            (
             (resolved_faithful_count != faithful_contract.hashes.size() ||
              resolved_faithful_path_count != faithful_contract.hashes.size()))) {
            std::fprintf(
                stderr,
                "BUMBLE_MENU_BACKGROUND stage=rt64_pack_failed variant=%s"
                " reason=resolved_hash_contract expected=135 actual=%" PRIu32
                " expected_suppressed_logo_tiles=7"
                " actual_suppressed_logo_tiles=%" PRIu32
                " expected_gameplay_ui_textures=8 actual_gameplay_ui_textures=%" PRIu32
                " expected_faithful_textures=%zu actual_faithful_textures=%" PRIu32
                "\n",
                bumble::menu_background::variant_name(variant),
                resolved_count,
                resolved_suppressed_logo_count,
                resolved_gameplay_ui_count,
                faithful_textures_requested ? faithful_contract.hashes.size() : 0u,
                resolved_faithful_count
            );
            std::fflush(stderr);
            return false;
        }

        menu_background_variant_ = variant;
        menu_background_pack_loaded_ = true;
        faithful_texture_pack_loaded_ = faithful_textures_requested;
        enhanced_texture_pack_selected_ = enhanced;
        faithful_texture_count_ = faithful_textures_requested
            ? faithful_contract.hashes.size()
            : 0u;
        menu_background_replacements_logged_ = false;
        std::fprintf(
            stderr,
            "BUMBLE_MENU_BACKGROUND stage=rt64_pack_ready variant=%s"
            " path=%s window_width=%" PRIu32 " window_height=%" PRIu32
            " aspect_mode=%s faithful_textures_enabled=%d texture_pack=%s"
            " faithful_texture_count=%zu"
            " project_menu_background_strips=120 guest_rom_logo_suppressed=1"
            " transparent_logo_tiles=7"
            " guest_text=native_embedded_roboto"
            " gameplay_ui_textures=8 gameplay_ui_resolution=1024x1024"
            " gameplay_ui_upscale=palette_preserving_scale2x"
            " gameplay_ui_screen_scale=0.5 dynamic_radar=live"
            " replacement_roots=%zu guest_writes=0\n",
            bumble::menu_background::variant_name(variant),
            directory.string().c_str(),
            width,
            height,
            bumble::graphics_options::widescreen_enabled()
                ? "expand"
                : "original",
            faithful_textures_requested ? 1 : 0,
            faithful_contract.pack_path.string().c_str(),
            faithful_texture_count_,
            replacement_directories.size()
        );
        std::fflush(stderr);
        return true;
    }

    void validate_menu_replacements(bool main_menu) {
        if (!main_menu || !high_resolution_textures_active_ ||
            menu_background_replacements_logged_ ||
            app_ == nullptr || app_->textureCache == nullptr) {
            return;
        }

        app_->textureCache->waitForGPUUploads();
        uint32_t logo_dimensions_match = 0;
        {
            const std::lock_guard lock(app_->textureCache->textureMapMutex);
            const auto& texture_map = app_->textureCache->textureMap;
            for (size_t index = 0;
                 index < texture_map.textureReplacements.size();
                 ++index) {
                const RT64::Texture* texture =
                    texture_map.textureReplacements[index];
                if (texture != nullptr) {
                    const uint64_t hash = index < texture_map.hashes.size()
                        ? texture_map.hashes[index]
                        : 0;
                    for (const uint64_t logo_hash :
                         kSuppressedMenuLogoTextureHashes) {
                        if (hash == logo_hash && texture->width == 32u &&
                            texture->height == 32u) {
                            ++logo_dimensions_match;
                            break;
                        }
                    }
                }
            }
        }

        if (logo_dimensions_match != 7) {
            return;
        }

        menu_background_replacements_logged_ = true;
        std::fprintf(
            stderr,
            "BUMBLE_MENU_BACKGROUND stage=rt64_replacements_active"
            " variant=%s project_background_strips_active=120"
            " guest_rom_logo_suppressed=1 transparent_logo_tiles_active=7"
            " guest_text=native_embedded_roboto"
            " guest_writes=0\n",
            bumble::menu_background::variant_name(menu_background_variant_)
        );
        std::fflush(stderr);
    }

    void sync_optional_enhancements(bool force_log) {
        if (app_ == nullptr || app_->textureCache == nullptr ||
            app_->swapChain == nullptr) {
            return;
        }
        const auto frame_pacing = bumble::graphics_options::frame_pacing();
        if (force_log || frame_pacing != frame_pacing_active_) {
            frame_pacing_active_ = frame_pacing;
            const bool interpolation = frame_pacing !=
                bumble::graphics_options::FramePacing::Original30Hz;
            const uint32_t requested_hz =
                requested_presentation_hz(frame_pacing);
            const bool grass_no_interpolation_diagnostic =
                std::getenv("BUMBLE_GRASS_GPU_NO_INTERPOLATION_DIAGNOSTIC") != nullptr;
            app_->userConfig.refreshRate = grass_no_interpolation_diagnostic
                ? RT64::UserConfiguration::RefreshRate::Original
                : interpolation
                    ? RT64::UserConfiguration::RefreshRate::Manual
                    : RT64::UserConfiguration::RefreshRate::Original;
            const uint32_t detected_display_hz =
                app_->sharedQueueResources->swapChainRate;
            app_->userConfig.refreshRateTarget = static_cast<int>(
                bumble::frame_pacing::capped_target_hz(
                    requested_hz,
                    detected_display_hz
                )
            );
            const uint32_t logical_target_hz =
                interpolation && !grass_no_interpolation_diagnostic
                    ? static_cast<uint32_t>(app_->userConfig.refreshRateTarget)
                    : bumble::frame_pacing::kOriginalGameplayHz;
            const bool vsync = true;
            app_->swapChain->setVsyncEnabled(vsync);
            app_->updateUserConfig(false);
            bumble::frame_pacing::configure(logical_target_hz);
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=frame_pacing_applied mode=%s"
                " pacing_owner=rt64_present_queue idle_gpu_work=1"
                " vsync=%d requested_target_rate=%" PRIu32
                " logical_target_rate=%d"
                " interpolation_allocation_safety_ceiling=12"
                " interpolation_policy=fixed_resource_ceiling_uniform_samples"
                " interpolation_sampling=uniform_full_source_interval"
                " simulation_rate=unchanged\n",
                frame_pacing_name(frame_pacing),
                vsync ? 1 : 0,
                requested_hz,
                static_cast<int>(logical_target_hz)
            );
            std::fflush(stderr);
        }
        const auto configured_grass =
            bumble::graphics_options::grass_mode();
        const auto grass_mode = configured_grass ==
                bumble::graphics_options::GrassMode::High
            ? bumble::procedural_grass::Mode::High
            : configured_grass == bumble::graphics_options::GrassMode::Low
                ? bumble::procedural_grass::Mode::Low
                : bumble::procedural_grass::Mode::Off;
        bumble::procedural_grass::set_mode(grass_mode);
        if (force_log || grass_mode_active_ != grass_mode) {
            grass_mode_active_ = grass_mode;
            const char* name = grass_mode ==
                    bumble::procedural_grass::Mode::High
                ? "high"
                : grass_mode == bumble::procedural_grass::Mode::Low
                    ? "low"
                    : "off";
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=procedural_grass mode=%s"
                " material_filter=translucency_scan"
                " low_budget=48 high_budget=96\n",
                name
            );
            std::fflush(stderr);
        }

        const bool high_resolution_textures =
            bumble::graphics_options::high_resolution_textures_enabled();
        auto& replacement_map =
            app_->textureCache->textureMap.replacementMapEnabled;
        bool replacement_state_changed =
            replacement_map.load(std::memory_order_acquire) != high_resolution_textures;
        if (replacement_state_changed) {
            // Lock changes so descriptors and dimensions use the same option value.
            const std::lock_guard lock(app_->textureCache->textureMapMutex);
            replacement_state_changed =
                replacement_map.load(std::memory_order_acquire) != high_resolution_textures;
            replacement_map.store(high_resolution_textures, std::memory_order_release);
        }
        if (replacement_state_changed || force_log ||
            high_resolution_textures_active_ != high_resolution_textures) {
            high_resolution_textures_active_ = high_resolution_textures;
            if (high_resolution_textures) {
                menu_background_replacements_logged_ = false;
            }
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=rt64_texture_replacements enabled=%d"
                " mandatory_pack=project_menu_and_native_text"
                " faithful_pack=%s"
                " faithful_hashes=%zu streaming=1\n",
                high_resolution_textures ? 1 : 0,
                faithful_texture_pack_loaded_ ? "rom_nearest_exact" : "disabled",
                faithful_texture_count_
            );
            std::fflush(stderr);
        }

        const bool requested_raytracing =
            bumble::graphics_options::ray_traced_lighting_enabled();
        const bool supported_raytracing =
            g_raytracing_supported.load(std::memory_order_acquire);
        const bool active_raytracing =
            requested_raytracing && supported_raytracing;
#if defined(RT_ENABLED) && RT_ENABLED
        if (app_->workloadQueue != nullptr) {
            app_->workloadQueue->rtEnabled = active_raytracing;
        }
#endif
        if (force_log || ray_tracing_active_ != active_raytracing ||
            ray_tracing_requested_ != requested_raytracing) {
            ray_tracing_requested_ = requested_raytracing;
            ray_tracing_active_ = active_raytracing;
            std::fprintf(
                stderr,
                "BUMBLE_RT64_PROBE stage=rt64_ray_tracing requested=%d"
                " supported=%d active=%d compiled=%d\n",
                requested_raytracing ? 1 : 0,
                supported_raytracing ? 1 : 0,
                active_raytracing ? 1 : 0,
#if defined(RT_ENABLED) && RT_ENABLED
                1
#else
                0
#endif
            );
            std::fflush(stderr);
        }
    }

    uint8_t* rdram_ = nullptr;
    uint64_t profile_context_sequence_ = 0u;
    uint64_t profile_world_state_ = UINT64_MAX;
    uint64_t profile_frontend_state_ = UINT64_MAX;
    uint32_t profile_frontend_item_ = UINT32_MAX;
    uint64_t profile_campaign_state0_ = UINT64_MAX;
    uint64_t profile_campaign_state1_ = UINT64_MAX;
    uint64_t profile_spatial_sample_sequence_ = 0u;
    std::array<uint8_t, 0x40> header_{};
    std::array<uint8_t, 0x1000> dmem_{};
    std::array<uint8_t, 0x1000> imem_{};
    uint32_t mi_intr_reg_ = 0;
    uint32_t dpc_start_reg_ = 0;
    uint32_t dpc_end_reg_ = 0;
    uint32_t dpc_current_reg_ = 0;
    uint32_t dpc_status_reg_ = 0;
    uint32_t dpc_clock_reg_ = 0;
    uint32_t dpc_bufbusy_reg_ = 0;
    uint32_t dpc_pipebusy_reg_ = 0;
    uint32_t dpc_tmem_reg_ = 0;
    bool menu_background_pack_loaded_ = false;
    bool faithful_texture_pack_loaded_ = false;
    bool enhanced_texture_pack_selected_ = false;
    size_t faithful_texture_count_ = 0u;
    bool menu_background_replacements_logged_ = false;
    bool extended_gbi_logged_ = false;
    bumble::procedural_grass::Mode grass_mode_active_ =
        bumble::procedural_grass::Mode::Off;
    bool high_resolution_textures_active_ = true;
    bool ray_tracing_requested_ = false;
    bool ray_tracing_active_ = false;
    bool frame_pacing_hook_installed_ = false;
    bool widescreen_hud_capture_hook_installed_ = false;
    bumble::graphics_options::FramePacing frame_pacing_active_ =
        bumble::graphics_options::FramePacing::Interpolated120Hz;
    std::chrono::steady_clock::time_point frame_statistics_time_ =
        std::chrono::steady_clock::now();
    uint64_t frame_statistics_source_vi_ = 0;
    uint64_t frame_statistics_rendered_ = 0;
    uint64_t frame_statistics_presented_ = 0;
    uint64_t frame_statistics_requested_interpolation_frames_ = 0;
    uint64_t frame_statistics_completed_interpolation_frames_ = 0;
    uint64_t frame_statistics_skipped_interpolation_frames_ = 0;
    uint64_t frame_statistics_superseded_interpolation_sets_ = 0;
    bumble::menu_background::Variant menu_background_variant_ =
        bumble::menu_background::Variant::Widescreen16x9;
    std::unique_ptr<RT64::Application> app_;
};

} // namespace

std::unique_ptr<ultramodern::renderer::RendererContext> bumble::rt64_renderer::create(
    uint8_t* rdram,
    ultramodern::renderer::WindowHandle window_handle,
    bool developer_mode
) {
    return std::make_unique<BumbleRt64Context>(rdram, window_handle, developer_mode);
}

bool bumble::rt64_renderer::request_shutdown() {
    const std::lock_guard lock(g_active_application_mutex);
    if (g_active_application == nullptr) {
        return false;
    }

    std::fprintf(
        stderr,
        "BUMBLE_RT64_SHUTDOWN stage=early_queue_stop present_stage=%s\n",
        g_active_application->presentQueue != nullptr
            ? g_active_application->presentQueue->presentThreadStage.load(
                  std::memory_order_acquire
              )
            : "no_present_queue"
    );
    std::fflush(stderr);
    if (g_active_application->workloadQueue != nullptr) {
        g_active_application->workloadQueue->stop();
    }
    if (g_active_application->presentQueue != nullptr) {
        g_active_application->presentQueue->stop();
    }
    return true;
}

void bumble::rt64_renderer::release_diagnostic_capture_resources() {
    // Both queues are joined and RenderDevice is still alive: release readback here.
    g_widescreen_hud_capture.readback.reset();
    g_widescreen_hud_capture.readback_size = 0u;
    g_widescreen_hud_capture.row_width = 0u;
    g_widescreen_hud_capture.width = 0u;
    g_widescreen_hud_capture.height = 0u;
    g_widescreen_hud_capture.copy_queued = false;
}

uint32_t bumble::rt64_renderer::display_list_count() {
    return g_display_lists.load(std::memory_order_acquire);
}

uint32_t bumble::rt64_renderer::screen_update_count() {
    return g_screen_updates.load(std::memory_order_acquire);
}

bool bumble::rt64_renderer::mission2_screen_presented() {
    return g_mission2_screen_logged.load(std::memory_order_acquire);
}

void bumble::rt64_renderer::set_fog_scale(float scale) {
    const float clamped = std::clamp(scale, 0.0f, 1.0f);
    const float previous = g_fog_scale.exchange(
        clamped,
        std::memory_order_acq_rel
    );
    bumble::widescreen::publish_fog_scale(clamped);
    if (previous != clamped) {
        std::fprintf(
            stderr,
            "BUMBLE_RT64_PROBE stage=rt64_fog_scale_updated"
            " previous=%.1f current=%.1f\n",
            static_cast<double>(previous),
            static_cast<double>(clamped)
        );
        std::fflush(stderr);
    }
}

float bumble::rt64_renderer::fog_scale() {
    return g_fog_scale.load(std::memory_order_acquire);
}

bool bumble::rt64_renderer::raytracing_supported() {
    return g_raytracing_supported.load(std::memory_order_acquire);
}

void bumble::rt64_renderer::set_contract_validation_enabled(bool enabled) {
    g_contract_validation_enabled.store(enabled, std::memory_order_relaxed);
}
