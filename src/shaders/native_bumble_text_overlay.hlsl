Texture2D<float4> gInput : register(t1);
SamplerState gSampler : register(s2);

struct TextOverlayConstants {
    float opacity;
    float3 padding;
};

[[vk::push_constant]]
ConstantBuffer<TextOverlayConstants> gConstants : register(b0, space0);

void VSMain(
    in uint id : SV_VertexID,
    out float4 position : SV_Position,
    out float2 uv : TEXCOORD0
) {
    uv.x = (id == 2) ? 2.0f : 0.0f;
    uv.y = (id == 1) ? 2.0f : 0.0f;
    position = float4(
        uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f),
        1.0f,
        1.0f
    );
}

float4 PSMain(in float4 position : SV_Position, in float2 uv : TEXCOORD0)
    : SV_TARGET {
    uint width = 0;
    uint height = 0;
    gInput.GetDimensions(width, height);
    const float2 half_pixel = 0.5f / float2(width, height);
    float4 color = gInput.SampleLevel(
        gSampler,
        clamp(uv, half_pixel, 1.0f - half_pixel),
        0.0f
    );
    color.a *= gConstants.opacity;
    return color;
}
