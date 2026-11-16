// Probe for a sampler pair through a function parameter (DiamondDogs request C23). Slang gives a helper that is not
// inlined a copy for each global texture, but passes one SamplerState as a parameter. The SPIR-V pairs must trace the
// sampler through each call, and agree with the pairs that Tint reads from the WGSL.
module SamplerHelper;

Texture2D<float4> albedo;
SamplerState linearSampler;
SamplerState pointSampler;
RWStructuredBuffer<float4> output;

[noinline]
float4 SampleWith(Texture2D<float4> texture, SamplerState sampler, float2 uv)
{
    return texture.SampleLevel(sampler, uv, 0.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void HelperCS(uint3 id : SV_DispatchThreadID)
{
    const float2 uv = float2(id.xy) / 64.0f;
    output[id.y * 64u + id.x] = SampleWith(albedo, linearSampler, uv) + SampleWith(albedo, pointSampler, uv);
}
