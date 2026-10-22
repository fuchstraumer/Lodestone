module BindingArrays;
import LodestoneAttributes;

// Fixed-size binding arrays, one of each resource kind, indexed by an entry point constant (the shape of
// nvpro_pyramid's `imageMipLevels[16]`). The sampler and the texture array pair through a dynamic index. Not cookable for
// WGSL: Slang rejects an array of resources there (E56105).

[format("rgba8")] RWTexture2D<float4> Levels[4];
Texture2D<float4> Views[3];
SamplerState Samp;
StructuredBuffer<float> Weights[2];
ConstantBuffer<float4> Tints[2];
RWStructuredBuffer<float> Output;

[shader("compute")]
[numthreads(8, 8, 1)]
void MainCS(uint3 id : SV_DispatchThreadID, uniform uint level)
{
    float4 sampled = Views[level % 3].SampleLevel(Samp, float2(0, 0), 0);
    float4 value = sampled * Weights[level & 1][0] + Tints[level & 1];
    Levels[level & 3][id.xy] = value;
    Output[id.x] = value.x;
}
