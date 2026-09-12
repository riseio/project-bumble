#include "native_renderer_contracts.hpp"
#include "native_first_run_assets.hpp"
#include "render/rt64_texture_cache.h"
#include "render/rt64_shader_compiler.h"
#include "render/rt64_bumble_shadow_geometry.h"
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
#include <stdexcept>
#include <utility>

namespace {
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
    return passed;
}
}

void bumble::rt64_renderer::validate_texture_contracts(RenderDevice* device, RenderShaderFormat format) {
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
    const uint64_t meshKey = RT64::bumbleShadowGeometryKey(geometry, 0, 3);
    require(!RT64::bumbleShadowGeometryMoving(geometry, 0, 3));
    geometry.posFloats[0] = 2;
    require(RT64::bumbleShadowGeometryKey(geometry, 0, 3) != meshKey);
    geometry.posFloats[0] = 0;
    geometry.worldIndices[0] = 1;
    require(RT64::bumbleShadowGeometryKey(geometry, 0, 3) != meshKey);
    geometry.worldIndices[0] = 0;
    std::swap(geometry.faceIndices[0], geometry.faceIndices[1]);
    require(RT64::bumbleShadowGeometryKey(geometry, 0, 3) != meshKey);
    std::swap(geometry.faceIndices[0], geometry.faceIndices[1]);
    require(RT64::bumbleShadowGeometryKey(geometry, 0, 3) == meshKey);
    geometry.velFloats[7] = .1f;
    require(RT64::bumbleShadowGeometryMoving(geometry, 0, 3));
    geometry.worldTransforms.emplace_back(hlslpp::float4x4::identity());
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
    std::fprintf(stderr, "BUMBLE_SHADOW_GEOMETRY_CONTRACT result=pass same_count_position_index_transform_changes=1 deformation_dynamic=1\n");
    constexpr std::array<float, 16> gradients{
        0, 1e-20f, .25f, 1, 1.41421356237f, 2, 4, 8,
        16, 64, 256, 65536, 1e30f, .5f, 1.4141f, 1.4143f
    };
    constexpr uint64_t bytes = 372 * 4 * sizeof(float);
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
    const RenderDescriptorSetDesc textureDesc(&textureRange, 1, true, 1);
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
    for (uint32_t i = 0; i < 16; ++i) otherPixels.insert(otherPixels.end(), {16, 192, 240, 255});
    const auto otherDDS = first_run::make_faithful_dds(std::move(otherPixels), 4, 4, 3);
    std::unique_ptr<RenderBuffer> otherUpload;
    std::unique_ptr<RT64::Texture> otherTexture(RT64::TextureCache::loadTextureFromBytes(
        device, worker.commandList.get(), otherDDS, otherUpload));
    require(otherTexture != nullptr);
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
    const std::array<float, 4> otherColor{16.f / 255, 192.f / 255, 240.f / 255, 1};
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
    matches &= fogPassed && shadowPassed;
    std::fprintf(stderr, "BUMBLE_FOG_COMPOSITION_CONTRACT result=%s cases=16\n", fogPassed ? "pass" : "fail");
    std::fprintf(stderr, "BUMBLE_SHADOW_RECEIVER_CONTRACT result=%s cases=32 old_self_shadow_reproduced=%d\n",
        shadowPassed ? "pass" : "fail", oldSelfShadowObserved);
    std::fprintf(stderr, "BUMBLE_MIXED_DESCRIPTOR_GPU_CONTRACT result=%s immutable_samplers=2 sampled_before_and_after=1\n", mixedPassed ? "pass" : "fail");
    if (!matches) {
        for (uint32_t i = 256; i < 372; ++i)
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
