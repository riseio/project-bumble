#include "native_renderer_contracts.hpp"
#include "native_first_run_assets.hpp"
#include "native_texture_reconstruction.hpp"
#include "native_electric_effect.hpp"
#include "hle/rt64_framebuffer_manager.h"
#include "hle/rt64_rdp.h"
#include "hle/rt64_workload_queue.h"
#include "xxHash/xxh3.h"
#include "common/rt64_tmem_hasher.h"
#include "common/rt64_recording_mutex.h"
#include "common/rt64_filesystem_zip.h"
#include <miniz/miniz.h>
#include "render/rt64_texture_cache.h"
#include "render/rt64_shader_compiler.h"
#include "render/rt64_bumble_shadow_geometry.h"
#include "render/rt64_bumble_shadow_renderer.h"
#include "shared/rt64_texture_lod.h"
#include "shared/rt64_raster_params.h"
#include "native_texture_contract.spirv.h"
namespace spirv {
#include "native_raster_contract.VS.spirv.h"
#include "native_raster_contract.PS.spirv.h"
#include "native_raster_contract.InstanceVS.spirv.h"
}
#if defined(_WIN32)
#include "native_texture_contract.dxil.h"
#include "render/rt64_raster_shader.h"
#include "shaders/RasterVSLibrary.hlsl.dxil.h"
#include <wrl/client.h>
#include "plume_d3d12.h"
#include <d3d12sdklayers.h>
namespace dxil {
#include "native_raster_contract.VS.dxil.h"
#include "native_raster_contract.PS.dxil.h"
#include "native_raster_contract.InstanceVS.dxil.h"
}
#endif
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <thread>

namespace {
void validate_texture_retirement(RenderDevice* device) {
    RT64::RenderWorker direct(device, "Texture Retirement Direct", RenderCommandListType::DIRECT);
    RT64::RenderWorker copy(device, "Texture Retirement Copy", RenderCommandListType::COPY);
    RT64::ShaderLibrary shaders(false, false);
    RT64::TextureCache cache(&direct, &copy, &direct, 0, &shaders);
    RT64::WorkloadQueue queue;
    queue.ext.textureCache = &cache;
    queue.interpolationGraphicsWorkers[0] = std::make_unique<RT64::RenderWorker>(
        device, "Texture Retirement Slot", RenderCommandListType::DIRECT, &direct);
    auto& worker = *queue.interpolationGraphicsWorkers[0];
    bool passed = true;
    for (bool retiredElsewhere : {false, true, false, true}) {
        const uint64_t lease = cache.incrementLock();
        ++queue.pendingTextureLocks;
        queue.interpolationWorkerTextureLocks[0] = lease;
        cache.textureMap.evictedTextures.push_back(new RT64::Texture());
        worker.commandList->begin();
        worker.commandList->end();
        worker.execute();
        if (retiredElsewhere) worker.waitIfPending();
        queue.retireInterpolationWorker(0);
        passed &= cache.lockCounter == 0 && cache.textureMap.evictedTextures.empty() &&
            queue.pendingTextureLocks == 0 && !queue.interpolationWorkerTextureLocks[0];
        if (queue.interpolationWorkerTextureLocks[0]) cache.decrementLock(lease);
        queue.pendingTextureLocks = 0;
        queue.interpolationWorkerTextureLocks[0] = 0;
        queue.retireInterpolationWorker(0);
        passed &= cache.lockCounter == 0 && queue.pendingTextureLocks == 0;
    }
    const auto first = cache.incrementLock();
    const auto second = cache.incrementLock();
    cache.textureMap.evictedTextures.push_back(new RT64::Texture());
    cache.decrementLock(first);
    passed &= cache.retiredTextures.size() == 1;
    const auto newer = cache.incrementLock();
    cache.decrementLock(second);
    passed &= cache.lockCounter == 1 && cache.retiredTextures.empty();
    cache.decrementLock(newer);

    const auto oldest = cache.incrementLock();
    const auto recent = cache.incrementLock();
    cache.textureMap.evictedTextures.push_back(new RT64::Texture());
    cache.decrementLock(recent);
    passed &= cache.retiredTextures.size() == 1;
    cache.decrementLock(oldest);
    passed &= cache.retiredTextures.empty() && cache.lockCounter == 0;
    if (!passed) throw std::runtime_error("Texture retirement ownership contract failed");
    std::fprintf(stderr, "BUMBLE_TEXTURE_RETIREMENT_CONTRACT result=pass direct=2 sibling=2 repeated=4 overlapping=1 out_of_order=1\n");
}

void validate_archive_contract() {
    auto require = [](bool ok) { if (!ok) throw std::runtime_error("Texture archive contract failed"); };
    const auto path = std::filesystem::current_path() / "cache" / "archive-contract.rtz";
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); }
    } cleanup{path};
    const std::array<uint8_t, 8> payload{1, 3, 5, 7, 9, 11, 13, 15};
    mz_zip_archive archive{};
    require(mz_zip_writer_init_heap(&archive, 0, 0));
    require(mz_zip_writer_add_mem(&archive, "test.bin", payload.data(), payload.size(), 0));
    void* memory = nullptr;
    size_t size = 0;
    require(mz_zip_writer_finalize_heap_archive(&archive, &memory, &size));
    mz_zip_writer_end(&archive);
    std::vector<uint8_t> bytes(static_cast<uint8_t*>(memory), static_cast<uint8_t*>(memory) + size);
    mz_free(memory);
    auto check = [&](const std::vector<uint8_t>& content, bool expected) {
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream.write(reinterpret_cast<const char*>(content.data()), content.size());
            require(bool(stream));
        }
        require(bumble::first_run::validate_texture_archive(path) == expected);
        auto fileSystem = RT64::FileSystemZip::create(path, "");
        std::vector<uint8_t> decoded;
        const bool loaded = fileSystem && fileSystem->load("test.bin", decoded);
        require(loaded == expected);
        if (loaded) require(std::equal(decoded.begin(), decoded.end(), payload.begin(), payload.end()));
    };
    check(bytes, true);
    auto corrupted = bytes;
    corrupted[30 + 8] ^= 0x80;
    check(corrupted, false);
    corrupted = bytes;
    corrupted[28] = corrupted[29] = 255;
    check(corrupted, false);
    bytes.resize(20);
    check(bytes, false);
    std::fprintf(stderr, "BUMBLE_ARCHIVE_CONTRACT result=pass crc=1 bounds=1 truncation=1\n");
}

void validate_recording_mutex() {
    RT64::RecordingMutex recording;
    std::mutex manager;
    unsigned count = 0;
    auto run = [&]() {
        for (unsigned i = 0; i < 1000; ++i) {
            std::unique_lock<RT64::RecordingMutex> recordLock(recording, std::defer_lock);
            std::unique_lock<std::mutex> managerLock(manager, std::defer_lock);
            std::lock(recordLock, managerLock);
            ++count;
        }
    };
    std::thread worker(run);
    run();
    worker.join();
    if (count != 2000 || !recording.try_lock()) {
        throw std::runtime_error("Recording lock contract failed");
    }
    const bool duplicate = recording.try_lock();
    recording.unlock();
    if (duplicate) throw std::runtime_error("Recording lock admitted two owners");
    std::fprintf(stderr, "BUMBLE_RECORDING_LOCK_CONTRACT result=pass\n");
}

void validate_tmem_contracts() {
    auto require = [](bool ok) { if (!ok) throw std::runtime_error("TMEM ownership contract failed"); };
    RT64::LoadTile tile{};
    tile.line = 8;
    tile.siz = G_IM_SIZ_8b;
    tile.fmt = G_IM_FMT_CI;
    require(!RT64::TMEMHasher::requiresRawTMEM(tile, 64, 32, 2));
    require(RT64::TMEMHasher::requiresRawTMEM(tile, 64, 33, 2));
    require(!RT64::TMEMHasher::requiresRawTMEM(tile, 64, 33, 0));
    tile.line = 0;
    require(RT64::TMEMHasher::requiresRawTMEM(tile, 1, 1, 0));

    RT64::FramebufferManager manager;
    RT64::FramebufferManager::RegionTMEM region{};
    region.tmemStart = 16;
    region.tmemEnd = 24;
    region.syncRequired = true;
    manager.activeRegionsTMEM.push_back(region);
    auto check = [&](uint32_t first, uint32_t end, uint8_t siz = G_IM_SIZ_8b,
                     uint8_t fmt = G_IM_FMT_CI, bool tlut = false, uint8_t palette = 0) {
        return manager.checkRegionsTMEM(first, end, 8, siz, fmt, 0, tlut, palette);
    };
    require(check(0, 17).syncRequired);
    require(!check(0, 16).syncRequired);
    require(!check(24, 25).syncRequired);
    manager.synchronizeRegionsTMEM();
    require(!check(0, 32).syncRequired);

    manager.activeRegionsTMEM.clear();
    region.tmemStart = 0;
    region.tmemEnd = 8;
    region.syncRequired = false;
    region.fbTile = {0, G_IM_SIZ_8b, G_IM_FMT_CI, 0, 0, 8, 8, 8, 0};
    region.tileCopyId = 1;
    manager.activeRegionsTMEM.push_back(region);
    require(check(0, 8).valid());
    require(!check(0, 9).valid());
    region = {};
    region.tmemStart = 288;
    region.tmemEnd = 304;
    region.syncRequired = true;
    manager.activeRegionsTMEM.push_back(region);
    require(check(0, 8, G_IM_SIZ_8b, G_IM_FMT_CI, true).syncRequired);
    require(check(0, 8, G_IM_SIZ_4b, G_IM_FMT_CI, true, 2).syncRequired);
    require(!check(0, 8, G_IM_SIZ_4b, G_IM_FMT_CI, true, 1).syncRequired);
    require(check(32, 40, G_IM_SIZ_32b, G_IM_FMT_RGBA).syncRequired);
    std::fprintf(stderr, "BUMBLE_TMEM_CONTRACT result=pass\n");
}

bool validate_shadow_material(RenderDevice* device, RenderShaderFormat format) {
    RT64::SamplerLibrary samplers;
    auto fill = [&](RT64::SamplerSet &set) {
        for (auto* slot : {&set.wrapWrap, &set.wrapMirror, &set.wrapClamp,
                &set.mirrorWrap, &set.mirrorMirror, &set.mirrorClamp,
                &set.clampWrap, &set.clampMirror, &set.clampClamp, &set.borderBorder}) {
            *slot = device->createSampler(RenderSamplerDesc{});
        }
    };
    fill(samplers.linear);
    fill(samplers.nearest);
    samplers.shadowComparison = device->createSampler(RenderSamplerDesc{});
    RT64::RenderWorker worker(device, "Bumble Shadow Material Contract", RenderCommandListType::DIRECT);
    RT64::BumbleShadowRenderer shadow(RT64::BumbleShadowRenderer::CachePolicy::DynamicPerPresentation,
        "Shadow material contract", 8);
    if (!shadow.ensureResources(&worker, format, samplers)) return false;
    RT64::FramebufferRendererDescriptorCommonSet common(samplers, false, device);
    RT64::FramebufferRendererDescriptorTextureSet textures(device, 1);
    auto upload = [&](const void* data, size_t bytes, RenderBufferFlags flags) {
        auto result = device->createBuffer(RenderBufferDesc::UploadBuffer(bytes, flags));
        std::memcpy(result->map(), data, bytes);
        result->unmap();
        return result;
    };
    const float positions[] = {-1, -1, .5f, 1, -1, 3, .5f, 1, 3, -1, .5f, 1};
    const float uv[] = {0, 0, 0, 0, 0, 0};
    const uint32_t indices[] = {0, 1, 2};
    auto vertices = upload(positions, sizeof(positions), RenderBufferFlag::VERTEX);
    auto coordinates = upload(uv, sizeof(uv), RenderBufferFlag::VERTEX);
    auto indexBuffer = upload(indices, sizeof(indices), RenderBufferFlag::INDEX);
    auto readback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(8 * 256));
    const RenderVertexBufferView positionView(vertices.get(), sizeof(positions));
    const RenderVertexBufferView uvView(coordinates.get(), sizeof(uv));
    const RenderIndexBufferView indexView(indexBuffer.get(), sizeof(indices), RenderFormat::R32_UINT);
    bool passed = true;
    std::array<float, 64> coverage{};
    for (uint32_t test = 0; test < 14; ++test) {
        interop::RDPParams params{};
        params.primColor = {0, 0, 0, float(test & 1)};
        if (test >= 12) params.primColor.w = .5f;
        params.blendColor = {0, 0, 0, .5f};
        auto rdp = upload(&params, sizeof(params), RenderBufferFlag::STORAGE);
        const uint32_t alpha = (test & 1) ? 0xFF000000u : 0u;
        const uint32_t colors[] = {alpha, alpha, alpha};
        auto materialColor = upload(colors, sizeof(colors), RenderBufferFlag::STORAGE);
        common.setBuffer(common.instanceRDPParams, rdp.get(), sizeof(params), RenderBufferStructuredView(sizeof(params)));
        common.setBuffer(common.bumbleMaterialColor, materialColor.get(), sizeof(colors));
        RT64::BumbleShadowRenderer::Constants material{};
        const bool textured = test >= 8 && test < 12;
        const uint32_t input = textured ? 1u : ((test & 3) < 2 ? 3u : 4u);
        material.renderParams.ccL = (7u << 12) | (7u << 9);
        material.renderParams.ccH = (7u << 12) | (input << 9) |
            (7u << 21) | (7u << 18) | (7u << 3) | input;
        material.renderParams.omL = G_AC_THRESHOLD;
        if (test >= 12) material.renderParams.omL = G_AC_DITHER;
        shadow.reset();
        auto light = hlslpp::float4x4::identity();
        if (test == 13) light[3][0] = .25f;
        shadow.begin(light, test, test);
        const bool deferred = test >= 4 && test < 10;
        interop::RDPTile tile{};
        tile.shifts = tile.shiftt = 1;
        tile.cms = tile.cmt = G_TX_CLAMP;
        tile.nativeSampler = NATIVE_SAMPLER_CLAMP_CLAMP;
        interop::GPUTile gpuTile{};
        gpuTile.tcScale = gpuTile.ulScale = {1, 1};
        gpuTile.textureDimensions = {1, 1, 1};
        auto tiles = upload(&tile, sizeof(tile), RenderBufferFlag::STORAGE);
        auto gpuTiles = upload(&gpuTile, sizeof(gpuTile), RenderBufferFlag::STORAGE);
        auto texture = device->createTexture(RenderTextureDesc::Texture2D(1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
        uint32_t pixel[64]{};
        pixel[0] = alpha | 0xFFFFFFu;
        auto textureUpload = upload(pixel, sizeof(pixel), RenderBufferFlag::NONE);
        if (textured) {
            interop::RenderFlags flags{};
            flags.usesTexture0 = true;
            flags.dynamicTiles = true;
            material.renderParams.flags = flags;
            material.tileCount = 1;
            common.setBuffer(common.RDPTiles, tiles.get(), sizeof(tile), RenderBufferStructuredView(sizeof(tile)));
            common.setBuffer(common.GPUTiles, gpuTiles.get(), sizeof(gpuTile), RenderBufferStructuredView(sizeof(gpuTile)));
            textures.setTexture(0, texture.get(), RenderTextureLayout::SHADER_READ);
        }
        shadow.addCasterRange(0, 3, material, true, deferred ? 1u : UINT32_MAX);
        auto* cmd = worker.commandList.get();
        cmd->begin();
        auto produceTexture = [&] {
            cmd->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture.get(), RenderTextureLayout::COPY_DEST));
            cmd->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.get()),
                RenderTextureCopyLocation::PlacedFootprint(textureUpload.get(), RenderFormat::R8G8B8A8_UNORM, 1, 1, 1, 64));
            cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(texture.get(), RenderTextureLayout::SHADER_READ));
        };
        if (textured && !deferred) produceTexture();
        shadow.record(&worker, positionView, indexView, uvView, common.get(), textures.get());
        if (deferred) {
            passed &= shadow.recordedContentKey == UINT64_MAX && shadow.deferredRangeCount == 1;
            shadow.record(&worker, positionView, indexView, uvView, common.get(), textures.get(), 0);
            passed &= shadow.deferredRangeCount == 1;
            if (textured) produceTexture();
            shadow.record(&worker, positionView, indexView, uvView, common.get(), textures.get(), 1);
            passed &= shadow.recordedContentKey == test && shadow.deferredRangeCount == 0;
        }
        cmd->barriers(RenderBarrierStage::COPY,
            RenderTextureBarrier(shadow.texture(), RenderTextureLayout::COPY_SOURCE));
        cmd->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readback.get(),
            RenderFormat::D32_FLOAT, 8, 8, 1, 64), RenderTextureCopyLocation::Subresource(shadow.texture()));
        cmd->end(); worker.execute(); worker.wait();
        const RenderRange range(0, 8 * 256);
        const auto* bytes = static_cast<const uint8_t*>(readback->map(0, &range));
        if (!bytes) return false;
        for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; ++x) {
            float depth;
            std::memcpy(&depth, bytes + y * 256 + x * 4, 4);
            if (test == 12) coverage[y * 8 + x] = depth;
            else if (test == 13) {
                if (x > 0) passed &= depth == coverage[y * 8 + x - 1];
            }
            else passed &= test & 1 ? (depth > .49f && depth < .6f) : depth == 1.0f;
        }
        if (test == 12)
            passed &= std::count_if(coverage.begin(), coverage.end(),
                [](float depth) { return depth > .49f && depth < .6f; }) == 32;
        readback->unmap();
    }
    std::fprintf(stderr, "BUMBLE_SHADOW_MATERIAL_CONTRACT result=%s cases=14\n", passed ? "pass" : "fail");
    return passed;
}

bool validate_raster_identity(RenderDevice* device, RenderShaderFormat format) {
    using namespace plume;
    RT64::RenderWorker worker(device, "Bumble Raster Identity", RenderCommandListType::DIRECT);
    RenderPipelineLayoutBuilder builder;
    builder.begin();
    builder.addPushConstant(0, 0, sizeof(interop::RasterParams), RenderShaderStageFlag::VERTEX);
    builder.end();
    auto layout = builder.create(device);
    const void* vsCode = spirv::BumbleRasterContractVS;
    const void* psCode = spirv::BumbleRasterContractPS;
    size_t vsSize = sizeof(spirv::BumbleRasterContractVS), psSize = sizeof(spirv::BumbleRasterContractPS);
    const void* apiCode = spirv::BumbleRasterContractInstanceVS;
    size_t apiSize = sizeof(spirv::BumbleRasterContractInstanceVS);
#if defined(_WIN32)
    if (format == RenderShaderFormat::DXIL) {
        vsCode = dxil::BumbleRasterContractVS; vsSize = sizeof(dxil::BumbleRasterContractVS);
        psCode = dxil::BumbleRasterContractPS; psSize = sizeof(dxil::BumbleRasterContractPS);
        apiCode = dxil::BumbleRasterContractInstanceVS; apiSize = sizeof(dxil::BumbleRasterContractInstanceVS);
    }
#endif
    auto vs = device->createShader(vsCode, vsSize, "VSMain", format);
    auto ps = device->createShader(psCode, psSize, "PSMain", format);
    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = layout.get(); desc.vertexShader = vs.get(); desc.pixelShader = ps.get();
    desc.renderTargetCount = 1; desc.renderTargetFormat[0] = RenderFormat::R8G8B8A8_UNORM;
    desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
    std::unique_ptr<RenderPipeline> pipeline;
    try { pipeline = device->createGraphicsPipeline(desc); }
    catch (...) {
#if defined(_WIN32)
        if (format == RenderShaderFormat::DXIL) {
            Microsoft::WRL::ComPtr<ID3D12InfoQueue> messages;
            if (SUCCEEDED(static_cast<D3D12Device*>(device)->d3d->QueryInterface(IID_PPV_ARGS(&messages)))) {
                for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
                    SIZE_T size = 0; messages->GetMessage(i, nullptr, &size);
                    std::vector<uint8_t> storage(size);
                    auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                    if (SUCCEEDED(messages->GetMessage(i, message, &size)))
                        std::fprintf(stderr, "BUMBLE_RASTER_IDENTITY validation=%s\n", message->pDescription);
                }
            }
        }
#endif
        throw;
    }
    auto apiVS = device->createShader(apiCode, apiSize, "InstanceVSMain", format);
    desc.vertexShader = apiVS.get();
    auto apiPipeline = device->createGraphicsPipeline(desc);
    auto target = device->createTexture(RenderTextureDesc::Texture2D(8, 1, 1,
        RenderFormat::R8G8B8A8_UNORM, RenderTextureFlag::RENDER_TARGET));
    const RenderTexture* attachment = target.get();
    auto framebuffer = device->createFramebuffer(RenderFramebufferDesc(&attachment, 1));
    auto readback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(256));
    const std::array<uint32_t, 3> triangleIndices{0, 1, 2};
    auto indexBuffer = device->createBuffer(RenderBufferDesc::UploadBuffer(sizeof(triangleIndices), RenderBufferFlag::INDEX));
    std::memcpy(indexBuffer->map(), triangleIndices.data(), sizeof(triangleIndices));
    indexBuffer->unmap();
    const RenderIndexBufferView indexView(indexBuffer.get(), sizeof(triangleIndices), RenderFormat::R32_UINT);
    auto* cmd = worker.commandList.get();
    cmd->begin();
    cmd->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.get(), RenderTextureLayout::COLOR_WRITE));
    cmd->setFramebuffer(framebuffer.get());
    cmd->setPipeline(pipeline.get());
    cmd->setGraphicsPipelineLayout(layout.get());
    cmd->setIndexBuffer(&indexView);
    constexpr std::array<uint32_t, 4> indices{0, 1, 17, 251};
    for (uint32_t x = 0; x < indices.size(); ++x) {
        cmd->setViewports(RenderViewport(float(x), 0, 1, 1));
        cmd->setScissors(RenderRect(x, 0, x + 1, 1));
        interop::RasterParams params{}; params.renderIndex = indices[x];
        cmd->setGraphicsPushConstants(0, &params);
        if (x % 2) cmd->drawIndexedInstanced(3, 1, 0, 0, 0);
        else cmd->drawInstanced(3, 1, 0, 0);
    }
    cmd->setPipeline(apiPipeline.get());
    for (uint32_t x = 0; x < indices.size(); ++x) {
        cmd->setViewports(RenderViewport(float(x + 4), 0, 1, 1));
        cmd->setScissors(RenderRect(x + 4, 0, x + 5, 1));
        if (x % 2) cmd->drawIndexedInstanced(3, 1, 0, 0, indices[x]);
        else cmd->drawInstanced(3, 1, 0, indices[x]);
    }
    cmd->barriers(RenderBarrierStage::COPY,
        RenderBufferBarrier(readback.get(), RenderBufferAccess::WRITE),
        RenderTextureBarrier(target.get(), RenderTextureLayout::COPY_SOURCE));
    cmd->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readback.get(),
        RenderFormat::R8G8B8A8_UNORM, 8, 1, 1, 64), RenderTextureCopyLocation::Subresource(target.get()));
    cmd->end(); worker.execute(); worker.wait();
    const RenderRange range(0, 256);
    const auto* pixels = static_cast<const uint8_t*>(readback->map(0, &range));
    bool passed = pixels != nullptr;
    if (pixels) for (uint32_t x = 0; x < indices.size(); ++x) {
        std::fprintf(stderr, "BUMBLE_RASTER_IDENTITY draw=%u expected=%u actual=%u\n", x, indices[x], pixels[x * 4]);
        passed &= pixels[x * 4] == indices[x] && pixels[x * 4 + 1] == 64 &&
            pixels[x * 4 + 2] == 128 && pixels[x * 4 + 3] == 255;
        const uint32_t expectedAPI = format == RenderShaderFormat::DXIL ? 0 : indices[x];
        std::fprintf(stderr, "BUMBLE_INSTANCE_API_CONTRACT first_instance=%u expected=%u actual=%u\n",
            indices[x], expectedAPI, pixels[(x + 4) * 4]);
        passed &= pixels[(x + 4) * 4] == expectedAPI;
    }
    readback->unmap();
    std::fprintf(stderr, "BUMBLE_RASTER_IDENTITY result=%s\n", passed ? "pass" : "fail");
    desc.vertexShader = vs.get();
    desc.depthEnabled = true;
    desc.depthWriteEnabled = true;
    desc.depthFunction = RenderComparisonFunction::LESS_EQUAL;
    desc.depthTargetFormat = RenderFormat::D32_FLOAT;
    auto waterPipeline = device->createGraphicsPipeline(desc);
    desc.depthWriteEnabled = false;
    auto readOnlyWaterPipeline = device->createGraphicsPipeline(desc);
    desc.renderTargetBlend[0] = RenderBlendDesc::AlphaBlend();
    auto effectPipeline = device->createGraphicsPipeline(desc);
    auto depth = device->createTexture(RenderTextureDesc::Texture2D(8, 1, 1,
        RenderFormat::D32_FLOAT, RenderTextureFlag::DEPTH_TARGET));
    auto waterFramebuffer = device->createFramebuffer(RenderFramebufferDesc(&attachment, 1, depth.get()));
    cmd->begin();
    const RenderTextureBarrier waterBarriers[] = {
        RenderTextureBarrier(target.get(), RenderTextureLayout::COLOR_WRITE),
        RenderTextureBarrier(depth.get(), RenderTextureLayout::DEPTH_WRITE)
    };
    cmd->barriers(RenderBarrierStage::GRAPHICS, waterBarriers, 2);
    cmd->setFramebuffer(waterFramebuffer.get());
    cmd->setGraphicsPipelineLayout(layout.get());
    cmd->clearDepth(true, 1.0f);
    cmd->clearColor(0, RenderColor{0, 0, 0, 1});
    auto draw = [&](const RenderPipeline* selected, float z, uint32_t color) {
        cmd->setPipeline(selected);
        interop::RasterParams params{};
        params.renderIndex = color;
        params.bumbleWaterPlaneHeight = z;
        cmd->setGraphicsPushConstants(0, &params);
        cmd->drawInstanced(3, 1, 0, 0);
    };
    for (uint32_t x = 0; x < 5; ++x) {
        cmd->setViewports(RenderViewport(float(x), 0, 1, 1));
        cmd->setScissors(RenderRect(x, 0, x + 1, 1));
        if (x == 2) draw(waterPipeline.get(), .1f, 64);
        if (x == 3) draw(effectPipeline.get(), .2f, 224 | 256);
        draw(x == 4 ? readOnlyWaterPipeline.get() : waterPipeline.get(), .5f, 32);
        if (x != 3) draw(effectPipeline.get(), (x == 1 || x == 4) ? .8f : .2f, 224 | 256);
    }
    cmd->barriers(RenderBarrierStage::COPY,
        RenderBufferBarrier(readback.get(), RenderBufferAccess::WRITE),
        RenderTextureBarrier(target.get(), RenderTextureLayout::COPY_SOURCE));
    cmd->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readback.get(),
        RenderFormat::R8G8B8A8_UNORM, 8, 1, 1, 64), RenderTextureCopyLocation::Subresource(target.get()));
    cmd->end(); worker.execute(); worker.wait();
    pixels = static_cast<const uint8_t*>(readback->map(0, &range));
    bool waterPassed = pixels != nullptr;
    constexpr uint8_t expectedWater[] = {128, 32, 64, 32, 128};
    if (pixels) for (uint32_t x = 0; x < 5; ++x)
        waterPassed &= std::abs(int(pixels[x * 4]) - int(expectedWater[x])) <= 1;
    readback->unmap();
    std::fprintf(stderr, "BUMBLE_WATER_EFFECT_CONTRACT result=%s cases=5\n", waterPassed ? "pass" : "fail");
    passed &= waterPassed;
    return passed;
}
}

void bumble::rt64_renderer::validate_texture_contracts(RenderDevice* device, RenderShaderFormat format) {
    validate_texture_retirement(device);
    validate_archive_contract();
    {
        using namespace bumble::textures;
        Image flat{12, 12, std::vector<uint8_t>(12 * 12 * 4, 127)};
        if (reconstruct_material(flat).rgba != reconstruct(flat).rgba)
            throw std::runtime_error("Flat texture reconstruction changed");
        Image edge = flat;
        constexpr std::array<uint8_t, 12> profile{32,32,32,32,48,96,160,208,224,224,224,224};
        for (unsigned y = 0; y < 12; ++y) for (unsigned x = 0; x < 12; ++x) {
            const size_t p = (y * 12 + x) * 4;
            for (unsigned c = 0; c < 3; ++c) edge.rgba[p + c] = profile[x];
            edge.rgba[p + 3] = uint8_t(x * 11 + y * 7);
        }
        const auto baseline = filter_surface(reconstruct(restore_detail(clean_grain(edge))));
        const auto result = reconstruct_material(edge);
        uint32_t checkpoints = 0;
        const auto checked = reconstruct_material(edge, [&]() { ++checkpoints; });
        bool cancelled = false;
        try {
            reconstruct_material(edge, []() { throw std::runtime_error("cancelled"); });
        } catch (const std::runtime_error&) { cancelled = true; }
        bool valid = result.width == 108 && result.height == 108 && result.rgba != baseline.rgba &&
            result.rgba == checked.rgba && checkpoints > 0 && cancelled;
        for (unsigned y = 0; y < 108; ++y) for (unsigned x = 0; x < 108; ++x) {
            const size_t p = (y * 108 + x) * 4;
            valid &= result.rgba[p + 3] == baseline.rgba[p + 3];
            if (x < 18 || y < 18 || x + 18 >= 108 || y + 18 >= 108)
                for (unsigned c = 0; c < 3; ++c) valid &= result.rgba[p + c] == baseline.rgba[p + c];
        }
        const auto odd = make_dds({3, 1, {0,0,0,255, 0,0,0,255, 255,255,255,255}});
        valid &= odd.size() == 164 && odd[160] == 85;
        bool rejected = false;
        try { reconstruct_material({4, 4, {1, 2}}); } catch (const std::exception&) { rejected = true; }
        if (!valid || !rejected) throw std::runtime_error("Texture reconstruction contract failed");
        std::fprintf(stderr, "BUMBLE_RECONSTRUCTION_CONTRACT result=pass scale=9 alpha=1 seam=1 odd_mips=1\n");
    }
    validate_recording_mutex();
    if (!bumble::electric_effect::validate_actor_lifetime()) {
        throw std::runtime_error("Effect actor lifetime contract failed");
    }
    std::fprintf(stderr, "BUMBLE_EFFECT_LIFETIME_CONTRACT result=pass\n");
    validate_tmem_contracts();
    if (!validate_shadow_material(device, format)) throw std::runtime_error("Shadow material GPU contract failed");
    using namespace plume;
    auto require = [](bool ok) { if (!ok) throw std::runtime_error("Texture GPU contract failed"); };
    const bool rasterIdentityPassed = validate_raster_identity(device, format);
    RT64::DrawData geometry;
    RT64::Projection shadowProjection;
    shadowProjection.type = RT64::Projection::Type::Perspective;
    RT64::GameCall scopedCall{};
    scopedCall.callDesc.extendedType = RT64::DrawExtendedType::None;
    scopedCall.callDesc.triangleCount = 1;
    require(RT64::bumbleWorldPerspectiveGeometry(shadowProjection, scopedCall));
    scopedCall.callDesc.extendedFlags.bumbleHud = 1;
    require(!RT64::bumbleWorldPerspectiveGeometry(shadowProjection, scopedCall));
    scopedCall.callDesc.extendedFlags.bumbleHud = 0;
    require(RT64::bumbleWorldPerspectiveGeometry(shadowProjection, scopedCall));
    scopedCall.callDesc.extendedType = RT64::DrawExtendedType::BumbleHudBegin;
    require(!RT64::bumbleWorldPerspectiveGeometry(shadowProjection, scopedCall));
    std::fprintf(stderr, "BUMBLE_SHADOW_HUD_CONTRACT result=pass world_admitted=1 perspective_hud_excluded=1 scope_return_world=1 marker_excluded=1\n");
    geometry.posFloats = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    geometry.velFloats.resize(9, 0);
    geometry.worldIndices = {0, 0, 0};
    geometry.faceIndices = {0, 1, 2};
    geometry.worldTransforms.emplace_back(hlslpp::float4x4::identity());
    geometry.worldTransforms.emplace_back(hlslpp::float4x4::identity());
    geometry.worldTransforms[1][3][0] = 32.0f;
    RT64::BumbleShadowGeometryCache shadowCache;
    auto geometryKey = [&]() {
        shadowCache.reset(geometry);
        return shadowCache.key(geometry, 0, 3);
    };
    const uint64_t meshKey = geometryKey();
    {
        auto materialData = geometry;
        materialData.tcFloats.resize(6, 0);
        materialData.tcVelFloats.resize(6, 0);
        materialData.normColBytes.resize(12, 255);
        materialData.lookAtIndices.resize(3, 0);
        materialData.rdpParams.resize(1);
        RT64::GameCall call{};
        call.callDesc.triangleCount = 1;
        bool animated = false;
        auto key = [&] {
            shadowCache.reset(materialData);
            animated = false;
            return shadowCache.materialKey(materialData, call, animated);
        };
        const auto baseline = key();
        require(!animated);
        materialData.normColBytes[3] = 0;
        require(key() != baseline);
        materialData.normColBytes[3] = 255;
        materialData.tcFloats[0] = 1;
        require(key() != baseline);
        materialData.tcFloats[0] = 0;
        materialData.rdpParams[0].primColor.w = .5f;
        require(key() != baseline);
        materialData.rdpParams[0].primColor.w = 0;
        require(key() == baseline);
        materialData.tcVelFloats[0] = .1f;
        key(); require(animated);
        materialData.tcVelFloats[0] = 0;
        materialData.lookAtIndices[0] = 1;
        key(); require(animated);
        shadowCache.reset(geometry);
    }
    require(!RT64::bumbleShadowGeometryMoving(geometry, 0, 3));
    geometry.posFloats[0] = 2;
    require(geometryKey() != meshKey);
    geometry.posFloats[0] = 0;
    geometry.worldIndices[0] = 1;
    require(geometryKey() != meshKey);
    geometry.worldIndices[0] = 0;
    std::swap(geometry.faceIndices[0], geometry.faceIndices[1]);
    require(geometryKey() != meshKey);
    std::swap(geometry.faceIndices[0], geometry.faceIndices[1]);
    require(geometryKey() == meshKey);
    require(shadowCache.key(geometry, 0, 3) == meshKey);
    geometry.worldTransforms[1] = geometry.worldTransforms[0];
    geometry.worldIndices = {1, 1, 1};
    require(geometryKey() == meshKey);
    geometry.posFloats.insert(geometry.posFloats.begin(), {7, 8, 9});
    geometry.worldIndices.insert(geometry.worldIndices.begin(), 0);
    geometry.faceIndices = {1, 2, 3};
    require(geometryKey() == meshKey);
    require(shadowCache.key(geometry, 0, 2) != meshKey);
    geometry.worldTransforms[1][3][0] = 64.0f;
    require(geometryKey() != meshKey);
    geometry.posFloats.erase(geometry.posFloats.begin(), geometry.posFloats.begin() + 3);
    geometry.worldIndices = {0, 0, 0};
    geometry.faceIndices = {0, 1, 2};
    require(geometryKey() == meshKey);
    geometry.velFloats[7] = .1f;
    require(RT64::bumbleShadowGeometryMoving(geometry, 0, 3));
    geometry.worldTransformGroups = {0};
    geometry.transformGroups.resize(1);
    geometry.transformGroups[0].matrixId = 0x80012340u;
    geometry.worldTransformMotion = {0};
    require(!RT64::bumbleShadowTransformsMoving(geometry, 0, 0));
    auto moved = geometry.worldTransforms[0];
    moved[3][0] = 32.0f;
    require(!RT64::bumbleShadowTransformChanged(geometry.worldTransforms[0], geometry.worldTransforms[0]));
    require(RT64::bumbleShadowTransformChanged(geometry.worldTransforms[0], moved));
    geometry.prevWorldTransforms = {moved};
    require(!RT64::bumbleShadowTransformsMoving(geometry, 0, 0));
    geometry.worldTransformMotion[0] = 1;
    require(RT64::bumbleShadowTransformsMoving(geometry, 0, 0));
    geometry.prevWorldTransforms = geometry.worldTransforms;
    require(RT64::bumbleShadowTransformsMoving(geometry, 0, 0));
    geometry.worldTransformMotion[0] = 0;
    geometry.prevWorldTransforms.clear();
    require(!RT64::bumbleShadowTransformsMoving(geometry, 0, 0));
    std::fprintf(stderr, "BUMBLE_SHADOW_MOTION_CONTRACT result=pass stationary_object_id=1 moving_endpoints=1 presentation_independent=1 unmatched_static=1\n");
    std::fprintf(stderr, "BUMBLE_SHADOW_GEOMETRY_CONTRACT result=pass content_changes=1 relocation_reuse=1 partial_range=1 deformation_dynamic=1\n");
    constexpr std::array<float, 16> gradients{
        0, 1e-20f, .25f, 1, 1.41421356237f, 2, 4, 8,
        16, 64, 256, 65536, 1e30f, .5f, 1.4141f, 1.4143f
    };
    RT64::BumbleShadowOwnership ownership;
    ownership.beginLevel(1);
    geometry.worldTransformMotion.resize(geometry.worldTransforms.size(), 0);
    geometry.worldTransformGroups.resize(geometry.worldTransforms.size(), 0);
    geometry.velFloats.assign(geometry.velFloats.size(), 0);
    ownership.observe(geometry);
    ownership.assign(geometry);
    require(!RT64::bumbleShadowCasterDynamic(geometry, 0, 0));
    auto wing = geometry;
    wing.worldTransformMotion[0] = 1;
    ownership.observe(geometry);
    ownership.observe(wing);
    ownership.assign(geometry);
    ownership.assign(wing);
    require(RT64::bumbleShadowCasterDynamic(geometry, 0, 0));
    require(RT64::bumbleShadowCasterDynamic(wing, 0, 0));
    wing.worldTransformMotion[0] = 0;
    wing.prevWorldTransforms.clear();
    ownership.observe(wing);
    ownership.assign(wing);
    require(RT64::bumbleShadowCasterDynamic(wing, 0, 0));
    require(!ownership.beginLevel(1));
    ownership.observe(wing);
    ownership.assign(wing);
    require(RT64::bumbleShadowCasterDynamic(wing, 0, 0));
    require(ownership.beginLevel(2));
    ownership.observe(wing);
    ownership.assign(wing);
    require(!RT64::bumbleShadowCasterDynamic(wing, 0, 0));
    wing.velFloats[0] = 1;
    ownership.observe(geometry);
    ownership.observe(wing);
    ownership.assign(geometry);
    require(RT64::bumbleShadowCasterDynamic(geometry, 0, 0));
    for (uint32_t id : {uint32_t(G_EX_ID_AUTO), uint32_t(G_EX_ID_IGNORE)}) {
        ownership.beginLevel(ownership.level + 1);
        geometry.transformGroups[0].matrixId = id;
        wing.transformGroups[0].matrixId = id;
        ownership.observe(geometry);
        ownership.observe(wing);
        ownership.assign(geometry);
        require(!RT64::bumbleShadowCasterDynamic(geometry, 0, 0));
    }
    std::fprintf(stderr, "BUMBLE_SHADOW_OWNERSHIP_CONTRACT result=pass hierarchy=1 pause=1 unmatched=1 deformation=1 level_reset=1\n");
    constexpr uint32_t uploadTexels = 13 * 9 + 6 * 4 + 3 * 2 + 1;
    constexpr uint64_t bytes = (406 + uploadTexels) * 4 * sizeof(float);
    RT64::RenderWorker worker(device, "Bumble Texture Contract", RenderCommandListType::DIRECT);
    std::unique_ptr<RenderBuffer> upload;
    for (size_t length = 0; length < 4; ++length) {
        const std::vector<uint8_t> invalid(length, 0);
        require(RT64::TextureCache::loadTextureFromBytes(device, worker.commandList.get(), invalid, upload) == nullptr);
    }
    std::vector<uint8_t> pixels;
    for (uint32_t i = 0; i < 16; ++i) pixels.insert(pixels.end(), {128, 64, 32, 255});
    auto dds = first_run::make_faithful_dds(std::move(pixels), 4, 4, 3);
    require(dds.size() == 148 + (16 + 4 + 1) * 4 && dds[128] == 28);
    // Reject invalid metadata before submitting GPU commands.
    for (const auto [offset, value] : std::array<std::pair<size_t, uint32_t>, 4>{{
            {28, 32}, {16, UINT32_MAX}, {140, 2}, {132, 4}}}) {
        auto invalid = dds;
        std::memcpy(invalid.data() + offset, &value, sizeof(value));
        require(RT64::TextureCache::loadTextureFromBytes(device, worker.commandList.get(), invalid, upload) == nullptr);
    }
    auto truncated = dds;
    truncated.pop_back();
    require(RT64::TextureCache::loadTextureFromBytes(device, worker.commandList.get(), truncated, upload) == nullptr);
    auto output = device->createBuffer(RenderBufferDesc::DefaultBuffer(bytes, RenderBufferFlag::UNORDERED_ACCESS));
    auto readback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(bytes));
    require(output && readback);
    const RenderDescriptorRange ranges[] = {
        {RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER, 1, 1}
    };
    const RenderDescriptorSetDesc setDesc(ranges, 1);
    auto set = device->createDescriptorSet(setDesc);
    const RenderDescriptorRange textureRange(RenderDescriptorRangeType::TEXTURE, 0, 8192);
    const RenderDescriptorSetDesc textureDesc(&textureRange, 1, true, 4);
    auto textureSet = device->createDescriptorSet(textureDesc);
    RenderSamplerDesc samplerDesc;
    samplerDesc.addressU = samplerDesc.addressV = RenderTextureAddressMode::CLAMP;
    auto sampler = device->createSampler(samplerDesc);
    const RenderSampler* immutableSamplers[] = {sampler.get(), sampler.get()};
    const RenderDescriptorRange mixedRanges[] = {
        {RenderDescriptorRangeType::TEXTURE, 0, 1},
        {RenderDescriptorRangeType::SAMPLER, 1, 1, immutableSamplers},
        {RenderDescriptorRangeType::SAMPLER, 2, 1, immutableSamplers + 1},
        {RenderDescriptorRangeType::TEXTURE, 3, 1},
        {RenderDescriptorRangeType::TEXTURE, 4, 1}
    };
    const RenderDescriptorSetDesc mixedDesc(mixedRanges, 5);
    auto mixedSet = device->createDescriptorSet(mixedDesc);
    RenderPipelineLayoutBuilder builder;
    builder.begin();
    builder.addDescriptorSet(setDesc);
    builder.addDescriptorSet(textureDesc);
    builder.addDescriptorSet(mixedDesc);
    builder.end();
    auto layout = builder.create(device);
    const void* code = BumbleTextureContractSPIRV;
    uint64_t codeSize = sizeof(BumbleTextureContractSPIRV);
#if defined(_WIN32)
    if (format == RenderShaderFormat::DXIL) {
        code = BumbleTextureContractDXIL;
        codeSize = sizeof(BumbleTextureContractDXIL);
    }
#endif
    auto shader = device->createShader(code, codeSize, "CSMain", format);
    RenderComputePipelineDesc pipelineDesc;
    pipelineDesc.pipelineLayout = layout.get();
    pipelineDesc.computeShader = shader.get();
    pipelineDesc.threadGroupSizeX = 64;
    pipelineDesc.threadGroupSizeY = pipelineDesc.threadGroupSizeZ = 1;
    auto pipeline = device->createComputePipeline(pipelineDesc);
    const RenderBufferStructuredView view(4 * sizeof(float));
    set->setBuffer(0, output.get(), bytes, &view);
    worker.commandList->begin();
    auto plane = device->createTexture(RenderTextureDesc::Texture2D(8, 8, 1,
        RenderFormat::R32_FLOAT));
    auto planeUpload = device->createBuffer(RenderBufferDesc::UploadBuffer(8 * 256));
    require(plane && planeUpload);
    auto* planeBytes = static_cast<unsigned char*>(planeUpload->map());
    for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; ++x) {
        const float depth = .6f - .001f * (float(x) + .5f) - .002f * (float(y) + .5f);
        std::memcpy(planeBytes + y * 256 + x * sizeof(float), &depth, sizeof(depth));
    }
    planeUpload->unmap();
    worker.commandList->barriers(RenderBarrierStage::COPY,
        RenderTextureBarrier(plane.get(), RenderTextureLayout::COPY_DEST));
    worker.commandList->copyTextureRegion(RenderTextureCopyLocation::Subresource(plane.get()),
        RenderTextureCopyLocation::PlacedFootprint(planeUpload.get(), RenderFormat::R32_FLOAT, 8, 8, 1, 64));
    worker.commandList->barriers(RenderBarrierStage::COMPUTE,
        RenderTextureBarrier(plane.get(), RenderTextureLayout::SHADER_READ));
    mixedSet->setTexture(4, plane.get(), RenderTextureLayout::SHADER_READ);
    std::unique_ptr<RT64::Texture> texture(RT64::TextureCache::loadTextureFromBytes(
        device, worker.commandList.get(), dds, upload));
    require(texture && texture->format == RenderFormat::R8G8B8A8_UNORM);
    textureSet->setTexture(0, texture->texture.get(), RenderTextureLayout::SHADER_READ);
    mixedSet->setTexture(0, texture->texture.get(), RenderTextureLayout::SHADER_READ);
    std::vector<uint8_t> otherPixels;
    for (uint32_t i = 0; i < 16; ++i) otherPixels.insert(otherPixels.end(), {16, 192, 240, 128});
    const auto otherDDS = first_run::make_faithful_dds(std::move(otherPixels), 4, 4, 3);
    std::unique_ptr<RenderBuffer> otherUpload;
    std::unique_ptr<RT64::Texture> otherTexture(RT64::TextureCache::loadTextureFromBytes(
        device, worker.commandList.get(), otherDDS, otherUpload));
    require(otherTexture != nullptr);
    textureSet->setTexture(1, otherTexture->texture.get(), RenderTextureLayout::SHADER_READ);
    bumble::textures::Image enhancedImage{4, 4, std::vector<uint8_t>(4 * 4 * 4, 255)};
    const auto enhancedDDS = bumble::textures::make_dds(bumble::textures::reconstruct_material(enhancedImage));
    std::unique_ptr<RenderBuffer> enhancedUpload;
    std::unique_ptr<RT64::Texture> enhancedTexture(RT64::TextureCache::loadTextureFromBytes(
        device, worker.commandList.get(), enhancedDDS, enhancedUpload));
    require(enhancedTexture != nullptr);
    textureSet->setTexture(2, enhancedTexture->texture.get(), RenderTextureLayout::SHADER_READ);
    worker.commandList->barriers(RenderBarrierStage::COMPUTE,
        RenderTextureBarrier(enhancedTexture->texture.get(), RenderTextureLayout::SHADER_READ));
    auto patternedDDS = first_run::make_faithful_dds(
        std::vector<uint8_t>(13 * 9 * 4, 255), 13, 9, 4);
    require(patternedDDS.size() == 148 + uploadTexels * 4);
    uint32_t texel = 0;
    for (uint32_t mip = 0; mip < 4; ++mip) {
        for (uint32_t y = 0; y < std::max(9u >> mip, 1u); ++y) {
            for (uint32_t x = 0; x < std::max(13u >> mip, 1u); ++x, ++texel) {
                patternedDDS[148 + texel * 4] = uint8_t(17 * x + 31 * mip);
                patternedDDS[149 + texel * 4] = uint8_t(23 * y + 43 * mip);
                patternedDDS[150 + texel * 4] = uint8_t(11 * x + 7 * y + 59 * mip);
                patternedDDS[151 + texel * 4] = uint8_t(255 - 37 * mip);
            }
        }
    }
    std::unique_ptr<RenderBuffer> patternedUpload;
    RT64::RenderWorker copyWorker(device, "Bumble Texture Upload Contract", RenderCommandListType::COPY);
    copyWorker.commandList->begin();
    std::unique_ptr<RT64::Texture> patternedTexture(RT64::TextureCache::loadTextureFromBytes(
        device, copyWorker.commandList.get(), patternedDDS, patternedUpload));
    require(patternedTexture != nullptr);
    copyWorker.commandList->end();
    copyWorker.execute();
    copyWorker.wait();
    textureSet->setTexture(3, patternedTexture->texture.get(), RenderTextureLayout::SHADER_READ);
    worker.commandList->barriers(RenderBarrierStage::COMPUTE,
        RenderTextureBarrier(patternedTexture->texture.get(), RenderTextureLayout::SHADER_READ));
    mixedSet->setTexture(3, otherTexture->texture.get(), RenderTextureLayout::SHADER_READ);
    worker.commandList->barriers(RenderBarrierStage::COMPUTE,
        RenderTextureBarrier(otherTexture->texture.get(), RenderTextureLayout::SHADER_READ));
    worker.commandList->barriers(RenderBarrierStage::COMPUTE,
        RenderTextureBarrier(texture->texture.get(), RenderTextureLayout::SHADER_READ));
    worker.commandList->barriers(RenderBarrierStage::COMPUTE,
        RenderBufferBarrier(output.get(), RenderBufferAccess::WRITE));
    worker.commandList->setPipeline(pipeline.get());
    worker.commandList->setComputePipelineLayout(layout.get());
    worker.commandList->setComputeDescriptorSet(set.get(), 0);
    worker.commandList->setComputeDescriptorSet(textureSet.get(), 1);
    worker.commandList->setComputeDescriptorSet(mixedSet.get(), 2);
    worker.commandList->dispatch(4, 1, 1);
    worker.commandList->barriers(RenderBarrierStage::COPY,
        RenderBufferBarrier(output.get(), RenderBufferAccess::READ));
    worker.commandList->barriers(RenderBarrierStage::COPY,
        RenderBufferBarrier(readback.get(), RenderBufferAccess::WRITE));
    worker.commandList->copyBufferRegion(readback->at(0), output->at(0), bytes);
    worker.commandList->end();
    worker.execute();
    worker.wait();
    const RenderRange readRange(0, bytes);
    const float* values = static_cast<const float*>(readback->map(0, &readRange));
    require(values != nullptr);
    bool matches = true;
    bool uploadPassed = true;
    for (uint32_t i = 0; i < uploadTexels; ++i)
        for (uint32_t c = 0; c < 4; ++c)
            uploadPassed &= std::abs(values[(406 + i) * 4 + c] -
                float(patternedDDS[148 + i * 4 + c]) / 255.f) < .0001f;
    matches &= uploadPassed;
    std::fprintf(stderr, "BUMBLE_TEXTURE_UPLOAD_CONTRACT result=%s texels=%u mips=4 npot=1 copy_to_direct=1\n",
        uploadPassed ? "pass" : "fail", uploadTexels);
    for (uint32_t i = 0; i < 256; ++i) {
        const double expected = std::clamp(.5 * std::log2(std::max(double(gradients[i % 16]), 1.0)) - .25, 0.0, double(i / 16));
        const std::array<double, 4> expectedValues{expected, std::floor(expected), std::min(std::floor(expected) + 1, double(i / 16)), expected - std::floor(expected)};
        matches &= std::abs(interop::textureMipLevel(gradients[i % 16], float(i / 16)) - expected) < .0001;
        for (uint32_t c = 0; c < 4; ++c) matches &= std::isfinite(values[i * 4 + c]) && std::abs(values[i * 4 + c] - expectedValues[c]) < .0001;
    }
    const std::array<float, 4> expectedColor{128.f / 255, 64.f / 255, 32.f / 255, 1};
    for (uint32_t c = 0; c < 4; ++c) matches &= std::abs(values[256 * 4 + c] - expectedColor[c]) < .0001;
    for (uint32_t i = 258; i < 322; ++i)
        for (uint32_t c = 0; c < 4; ++c)
            matches &= std::isfinite(values[i * 4 + c]) && std::abs(values[i * 4 + c] - expectedColor[c]) < .0001;
    matches &= values[257 * 4] == -16 && values[257 * 4 + 1] == -1 && values[257 * 4 + 2] == 0 && values[257 * 4 + 3] == 1;
    const std::array<float, 4> otherColor{16.f / 255, 192.f / 255, 240.f / 255, 128.f / 255};
    const bool alphaPassed = std::abs(values[404 * 4 + 3] - 128.f / 255) < .0001f &&
        values[405 * 4 + 3] == 1 && values[404 * 4] == 1;
    matches &= alphaPassed;
    std::fprintf(stderr, "BUMBLE_REPLACEMENT_ALPHA_CONTRACT result=%s cases=2\n", alphaPassed ? "pass" : "fail");
    bool mixedPassed = true;
    for (uint32_t c = 0; c < 4; ++c) {
        mixedPassed &= std::abs(values[322 * 4 + c] - expectedColor[c]) < .0001;
        mixedPassed &= std::abs(values[323 * 4 + c] - otherColor[c]) < .0001;
    }
    matches &= mixedPassed;
    bool fogPassed = true;
    for (uint32_t i = 0; i < 12; ++i) {
        const float alpha = float(i % 3) * .5f;
        for (uint32_t c = 0; c < 3; ++c) {
            const float lit = (.2f + .1f * c) * (float(i / 3) + 1) * .5f;
            const float expected = lit * (1 - alpha) + (.8f + .05f * c) * alpha;
            fogPassed &= std::abs(values[(324 + i) * 4 + c] - expected) < .0001f;
        }
        fogPassed &= values[(324 + i) * 4 + 3] == 1;
    }
    for (uint32_t i = 336; i < 340; ++i) fogPassed &= values[i * 4] == 0;
    bool shadowPassed = true, oldSelfShadowObserved = false;
    for (uint32_t i = 340; i < 372; ++i) {
        shadowPassed &= values[i * 4] == 1 && values[i * 4 + 1] == 0 &&
            std::abs(values[i * 4 + 2]) < .0001f;
        oldSelfShadowObserved |= values[i * 4 + 3] < 1;
    }
    shadowPassed &= oldSelfShadowObserved;
    bool contrastPassed = true;
    for (uint32_t i = 372; i < 404; ++i) {
        shadowPassed &= values[i * 4] == 1 && values[i * 4 + 1] < 1;
        contrastPassed &= values[i * 4 + 3] < 1e-6f;
        if (i > 372) contrastPassed &= values[i * 4 + 2] > values[(i - 1) * 4 + 2];
    }
    matches &= contrastPassed;
    std::fprintf(stderr, "BUMBLE_LIGHTING_CONTRAST_CONTRACT result=%s cases=32\n", contrastPassed ? "pass" : "fail");
    matches &= fogPassed && shadowPassed;
    std::fprintf(stderr, "BUMBLE_FOG_COMPOSITION_CONTRACT result=%s cases=16\n", fogPassed ? "pass" : "fail");
    std::fprintf(stderr, "BUMBLE_SHADOW_RECEIVER_CONTRACT result=%s cases=32 old_self_shadow_reproduced=%d\n",
        shadowPassed ? "pass" : "fail", oldSelfShadowObserved);
    std::fprintf(stderr, "BUMBLE_MIXED_DESCRIPTOR_GPU_CONTRACT result=%s immutable_samplers=2 sampled_before_and_after=1\n", mixedPassed ? "pass" : "fail");
    if (!matches) {
        for (uint32_t i = 256; i < 406; ++i)
            std::fprintf(stderr, "BUMBLE_TEXTURE_GPU_CONTRACT sample=%u value=%.9g,%.9g,%.9g,%.9g\n", i,
                values[i * 4], values[i * 4 + 1], values[i * 4 + 2], values[i * 4 + 3]);
    }
    readback->unmap();
    require(matches);
    require(rasterIdentityPassed);
#if defined(_WIN32)
    if (format == RenderShaderFormat::DXIL) {
        RT64::ShaderCompiler compiler;
        Microsoft::WRL::ComPtr<IDxcBlob> compiled;
        bool rejected = false;
        try { compiler.compile("invalid shader syntax", L"CSMain", L"cs_6_3", format, &compiled); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected && !compiled);
        compiler.compile("[numthreads(1,1,1)] void CSMain() {}", L"CSMain", L"cs_6_3", format, &compiled);
        require(compiled != nullptr);
        Microsoft::WRL::ComPtr<IDxcBlob> entry, library, linked;
        const auto rasterText = RT64::RasterShader::generateShaderText(RT64::ShaderDescription{}, false);
        compiler.compile(rasterText.vertexShader, L"VSMain", L"lib_6_3", format, &entry);
        Microsoft::WRL::ComPtr<IDxcBlobEncoding> libraryEncoding;
        require(SUCCEEDED(compiler.dxcUtils->CreateBlobFromPinned(RasterVSLibraryBlobDXIL,
            sizeof(RasterVSLibraryBlobDXIL), DXC_CP_ACP, &libraryEncoding)));
        require(SUCCEEDED(libraryEncoding.As(&library)));
        IDxcBlob* libraries[] = {entry.Get(), library.Get()};
        const wchar_t* names[] = {L"RasterVSEntry", L"RasterVSLibrary"};
        compiler.link(L"VSMain", L"vs_6_3", libraries, names, 2, &linked);
        require(linked != nullptr);
        std::fprintf(stderr, "BUMBLE_RASTER_LIBRARY_CONTRACT result=pass generated_wrapper_linked=1\n");
        // Driver rejection must return failure.
        const uint32_t invalidCode = 0;
        auto badShader = device->createShader(&invalidCode, sizeof(invalidCode), "CSMain", format);
        pipelineDesc.computeShader = badShader.get();
        rejected = false;
        try { auto invalidPipeline = device->createComputePipeline(pipelineDesc); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected);
        std::fprintf(stderr, "BUMBLE_DXC_CONTRACT result=pass compile_failure_recovery=1 invalid_pipeline_rejected=1\n");
    }
#endif
    std::fprintf(stderr, "BUMBLE_TEXTURE_GPU_CONTRACT result=pass mip_cases=256 manual_sampler_cases=64 guest_lod_cases=4 malformed_lengths=4 malformed_dds=5 faithful_unorm_sample=128,64,32,255\n");
}
