// Test material identity with nonzero draw indices.
#include "shaders/RasterDrawIndex.hlsli"
void VSMain(uint vertex : SV_VertexID,
    out float4 position : SV_Position, nointerpolation out uint material : TEXCOORD0) {
    position = float4(vertex == 2 ? 3 : -1, vertex == 1 ? 3 : -1, 0, 1);
    material = getRasterDrawIndex();
}
// Negative control for the old instance-index behavior.
void InstanceVSMain(uint vertex : SV_VertexID, uint instance : SV_InstanceID,
    out float4 position : SV_Position, nointerpolation out uint material : TEXCOORD0) {
    position = float4(vertex == 2 ? 3 : -1, vertex == 1 ? 3 : -1, 0, 1);
    material = instance;
}
float4 PSMain(float4 position : SV_Position, nointerpolation uint material : TEXCOORD0) : SV_Target {
    return float4(float(material) / 255, 0.25, 0.5, 1);
}
