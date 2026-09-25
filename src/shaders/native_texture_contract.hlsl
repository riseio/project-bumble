// Texture sampling tests for the smoke replay.
#include "shaders/TextureSampler.hlsli"
#include "shaders/BumbleSurfaceFog.hlsli"
#include "shaders/BumbleShadowReceiver.hlsli"
#include "shaders/BumbleLightingGrade.hlsli"
RWStructuredBuffer<float4> outputValues : register(u1);
Texture2D<float4> beforeSamplers : register(t0, space2);
SamplerState immutableFirst : register(s1, space2);
SamplerState immutableSecond : register(s2, space2);
Texture2D<float4> afterSamplers : register(t3, space2);
Texture2D<float> receiverPlane : register(t4, space2);
static const float gradients[16] = {
    0.0f, 1e-20f, 0.25f, 1.0f, 1.41421356237f, 2.0f, 4.0f, 8.0f,
    16.0f, 64.0f, 256.0f, 65536.0f, 1e30f, 0.5f, 1.4141f, 1.4143f
};
[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i < 148) {
        uint index = i;
        uint width = 13, height = 9, mip = 0;
        while (index >= width * height) {
            index -= width * height;
            width = max(width >> 1, 1);
            height = max(height >> 1, 1);
            ++mip;
        }
        outputValues[406 + i] = gTextures[3].Load(int3(index % width, index / width, mip));
    }
    if (i < 2) {
        OtherMode mode = (OtherMode)0;
        RDPTile tile = (RDPTile)0;
        tile.shifts = tile.shiftt = 1;
        tile.cms = tile.cmt = G_TX_CLAMP;
        tile.lrs = tile.lrt = 12;
        GPUTile gpu = (GPUTile)0;
        gpu.ulScale = gpu.tcScale = 9.0f.xx;
        gpu.texelMask = uint2(0xFFFFFFFF, 0xFFFFFFFF);
        gpu.textureIndex = 2;
        gpu.originalAlphaTextureIndex = 1;
        gpu.textureDimensions = float3(36, 36, 1);
        gpu.flags = i == 0 ? 0x42 : 0x2;
        outputValues[404 + i] = sampleTexture(mode, 0, 1.0f.xx, 0.0f.xx, 0.0f.xx, tile, gpu, false);
    }
    if (i < 32) {
        const float slope = float(i + 1) * 1024.0f;
        const float3 receiver = float3(.49f, .51f, .5f);
        const float2 gradient = float2(slope, -slope);
        const float empty = bumbleShadowReceiverVisibility(1.0f.xxxx, receiver,
            .125f.xx, gradient, 0, true);
        const float oldEmpty = bumbleShadowReceiverVisibility(.999f.xxxx, receiver,
            .125f.xx, gradient, 0, true);
        const float x = float(i) / 31.0f;
        const float expected = lerp(x, x * x * (3 - 2 * x), .5f);
        const float3 graded = bumbleLightingContrast(x.xxx);
        outputValues[372 + i] = float4(empty, oldEmpty, graded.x, abs(graded.x - expected));
    }
    OtherMode fogMode = (OtherMode)0;
    fogMode.H = G_CYC_2CYCLE;
    fogMode.L = ((3u << 14) | (2u << 10)) << 16;
    if (i < 12) {
        Blender::Inputs inputs;
        inputs.blendColor = 0;
        inputs.fogColor = float4(.8, .85, .9, 1);
        inputs.shadeAlpha = float(i % 3) * .5f;
        const float3 lit = float3(.2, .3, .4) * (float(i / 3) + 1) * .5f;
        outputValues[324 + i] = float4(bumbleComposeSurfaceFog(
            fogMode, 0, inputs, lit, 1), bumbleCanDeferSurfaceFog(fogMode, 0));
    }
    if (i < 4) {
        if (i == 0) fogMode.H = G_CYC_1CYCLE;
        if (i == 1) fogMode.L |= 1u << 14; // FORCE_BL
        if (i == 2) fogMode.L |= (2u << 12) << 16; // final blend-colour output
        if (i == 3) fogMode.L = 0; // not a fog cycle
        outputValues[336 + i] = bumbleCanDeferSurfaceFog(fogMode, 0) ? 1 : 0;
    }
    if (i < 32) {
        float2 uv = float2(3.0f + float(i % 8) / 8, 3.0f + float(i % 8) / 16) / 8;
        if (i == 30) uv = float2(.001f, .001f);
        if (i == 31) uv = float2(.999f, .999f);
        const float angle = float(i / 8) * .7f;
        const float2 dx = float2(cos(angle), sin(angle)) * .01f;
        const float2 dy = float2(-sin(angle), cos(angle)) * .02f;
        const float2 expectedGradient = float2(-.008f, -.016f);
        const float2 gradient = bumbleShadowDepthGradient(
            float3(dx, dot(expectedGradient, dx)), float3(dy, dot(expectedGradient, dy)));
        const float depth = .6f + dot(expectedGradient, uv);
        const float4 depths = receiverPlane.Gather(immutableFirst, uv);
        const float self = bumbleShadowReceiverVisibility(depths, float3(uv, depth),
            .125f.xx, gradient, .000001f, true);
        const float occluded = bumbleShadowReceiverVisibility(depths, float3(uv, depth + .02f),
            .125f.xx, gradient, .000001f, true);
        const float4 oldDepths = receiverPlane.Gather(immutableFirst, uv + .0625f);
        const float oldVisibility = dot(float4(depth - .00105f <= oldDepths), .25f.xxxx);
        // Isolate lane order and bilinear weights from the plane equation.
        const float2 fraction = frac(uv * 8 - .5f);
        const float mixed = bumbleShadowReceiverVisibility(float4(1, 0, 0, 0),
            float3(uv, .5f), .125f.xx, 0, 0, true);
        const float mixedExpected = (1 - fraction.x) * fraction.y;
        outputValues[340 + i] = float4(self, occluded,
            length(gradient - expectedGradient) + abs(mixed - mixedExpected), oldVisibility);
    }
    if (i < 256) {
        float mip = textureMipLevel(gradients[i % 16], float(i / 16));
        outputValues[i] = float4(mip, floor(mip), min(floor(mip) + 1, float(i / 16)), frac(mip));
    }
    if (i == 0) {
        outputValues[256] = gTextures[0].Load(int3(0, 0, 0));
        outputValues[257] = float4(textureLODTileBase(0), textureLODTileBase(0.5f), textureLODTileBase(1), textureLODTileBase(2));
        outputValues[322] = beforeSamplers.SampleLevel(immutableFirst, float2(.5, .5), 0);
        outputValues[323] = afterSamplers.SampleLevel(immutableSecond, float2(.5, .5), 0);
    }
    if (i < 64) {
        OtherMode mode = (OtherMode)0;
        mode.H = ((i / 16) == 1 ? G_TF_BILERP : ((i / 16) == 2 ? G_TF_AVERAGE : G_TF_POINT));
        RDPTile tile = (RDPTile)0;
        tile.shifts = tile.shiftt = 1;
        tile.masks = tile.maskt = 4;
        tile.lrs = tile.lrt = 12;
        tile.cms = tile.cmt = G_TX_CLAMP;
        tile.nativeSampler = NATIVE_SAMPLER_NONE;
        GPUTile gpu = (GPUTile)0;
        gpu.ulScale = gpu.tcScale = float2(1, 1);
        gpu.texelMask = uint2(0xFFFFFFFF, 0xFFFFFFFF);
        gpu.textureDimensions = float3(4, 4, 3);
        gpu.flags = 0x10;
        uint flags = (i / 16) == 3 ? (1u << 4) : 0;
        float derivative = sqrt(gradients[i % 16]);
        outputValues[258 + i] = sampleTexture(mode, flags, float2(1, 1),
            float2(derivative, 0), float2(0, derivative), tile, gpu, false);
    }
}
