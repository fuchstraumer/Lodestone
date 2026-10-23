module ShimControl;

// The control for ShimMaterial.slang: the same shader with the resource types written directly. A cook of the two must
// give the same resources at the same places.

public struct PointLight
{
    float4 Position;
    float4 Color;
};

struct Material
{
    StructuredBuffer<PointLight> Points;
    Texture2D<float4> Surface;
    SamplerState Filter;
    uint Count;
};

ParameterBlock<Material> material;
RWStructuredBuffer<float4> Output;

[shader("compute")]
[numthreads(8, 1, 1)]
void BlockCS(uint3 id : SV_DispatchThreadID)
{
    const PointLight light = material.Points[id.x % material.Count];
    const float4 tint = material.Surface.SampleLevel(material.Filter, float2(0.5f, 0.5f), 0.0f);
    Output[id.x] = light.Color * tint;
}

[shader("compute")]
[numthreads(8, 1, 1)]
void EntryCS(uint3 id : SV_DispatchThreadID, uniform RWStructuredBuffer<float4> scratch)
{
    scratch[id.x] = scratch[id.x] * 2.0f;
}
