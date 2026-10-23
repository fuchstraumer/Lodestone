module ShimDirect;
import LodestoneShims;

// The Bound shim implementations used directly, with no extern. A parameter block holds one of each resource class, and
// one entry point takes a shim as a `uniform` parameter. The module registers no axis, so it cooks as one variant.

struct Material
{
    BoundReadBuffer<float4> Points;
    BoundTexture2D Albedo;
    BoundSampler Linear;
    uint Count;
};

ParameterBlock<Material> material;
RWStructuredBuffer<float4> Output;

[shader("compute")]
[numthreads(8, 1, 1)]
void BlockCS(uint3 id : SV_DispatchThreadID)
{
    const float4 point = material.Points.Load(id.x % material.Count);
    const float4 tint = material.Albedo.Get().SampleLevel(material.Linear.Get(), float2(0.5f, 0.5f), 0.0f);
    Output[id.x] = point * tint;
}

[shader("compute")]
[numthreads(8, 1, 1)]
void EntryCS(uint3 id : SV_DispatchThreadID, uniform BoundReadWriteBuffer<float4> Scratch)
{
    Scratch.Store(id.x, Scratch.Load(id.x) * 2.0f);
}
