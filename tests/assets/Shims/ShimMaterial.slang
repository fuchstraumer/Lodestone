module ShimMaterial;
import LodestoneShims;

// A material that reads a buffer, a texture, and a sampler through shim externs, in a parameter block and beside the
// ordinary data of the block, and an entry point that takes a shim as a `uniform` parameter. The cooker links the Bound
// implementation of the profile. ShimControl.slang is the same shader written with the resource types themselves.

public struct PointLight
{
    float4 Position;
    float4 Color;
};

extern struct Lights : IReadBuffer<PointLight>;
extern struct Albedo : ITexture2DRef;
extern struct Linear : ISamplerRef;
extern struct Scratch : IReadWriteBuffer<float4>;

struct Material
{
    Lights Points;
    Albedo Surface;
    Linear Filter;
    uint Count;
};

ParameterBlock<Material> material;
RWStructuredBuffer<float4> Output;

[shader("compute")]
[numthreads(8, 1, 1)]
void BlockCS(uint3 id : SV_DispatchThreadID)
{
    const PointLight light = material.Points.Load(id.x % material.Count);
    const float4 tint = material.Surface.Get().SampleLevel(material.Filter.Get(), float2(0.5f, 0.5f), 0.0f);
    Output[id.x] = light.Color * tint;
}

[shader("compute")]
[numthreads(8, 1, 1)]
void EntryCS(uint3 id : SV_DispatchThreadID, uniform Scratch scratch)
{
    scratch.Store(id.x, scratch.Load(id.x) * 2.0f);
}
