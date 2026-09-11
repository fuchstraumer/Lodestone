module ParameterBlocks;
import LodestoneAttributes;

// A probe for the parameter block walk, and the acceptance test for phase E step E0b.
//
// Slang gives a parent scope only descriptor ranges for a block, so the binding range of a block
// carries a descriptor set index of -1 and the ordinary range walk drops it. The contents live in a
// sub-object range, and only `CollectSubObjectDrafts` reaches them.
//
// The module covers each case the walk must tell apart:
//
//  - `MaterialBlock` holds resources alone. Slang emits no container for it.
//  - `TuningBlock` holds ordinary data alone. Slang emits a container, and nothing else.
//  - `Output` is a plain buffer at global scope. It reports a sub-object range as well, and the walk
//    must refuse it, because it is not a block.
//  - `OuterBlock` holds a block of its own. The walk must add the spaces together and must not
//    assume one level. The contents of the inner block carry the whole chain in their scope name.
//  - `OwnBlockACS` and `OwnBlockBCS` each declare a block of their own, of one type. Slang gave the
//    two blocks separate spaces here, so ownership and a placement query agree in this module.
//    Ownership is still what decides, because nothing states that Slang must separate them.
//
// The module registers no permutation space, so it cooks as one variant.

struct Material
{
    [ls_extent_2d("256", "128")]
    Texture2D<float4> AlbedoMap;
    SamplerState Sampler;
};

struct Tuning
{
    float4 Tint;
    float2 Scale;
};

struct Detail
{
    Texture2D<float4> DetailMap;
    ParameterBlock<Material> Surface;
};

ParameterBlock<Material> MaterialBlock;
ParameterBlock<Tuning> TuningBlock;
ParameterBlock<Detail> OuterBlock;
RWStructuredBuffer<float4> Output;

[shader("compute")]
[numthreads(64, 1, 1)]
void GlobalBlocksCS(uint3 threadId: SV_DispatchThreadID)
{
    const float2 uv = float2(threadId.xy) * TuningBlock.Scale;
    Output[threadId.x] =
        MaterialBlock.AlbedoMap.SampleLevel(MaterialBlock.Sampler, uv, 0.0f) * TuningBlock.Tint;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void NestedBlockCS(uint3 threadId: SV_DispatchThreadID)
{
    const float2 uv = float2(threadId.xy) * (1.0f / 256.0f);
    Output[threadId.x] =
        OuterBlock.DetailMap.SampleLevel(OuterBlock.Surface.Sampler, uv, 0.0f) +
        OuterBlock.Surface.AlbedoMap.SampleLevel(OuterBlock.Surface.Sampler, uv, 0.0f);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void OwnBlockACS(uint3 threadId: SV_DispatchThreadID, uniform ParameterBlock<Material> surface)
{
    const float2 uv = float2(threadId.xy) * (1.0f / 256.0f);
    Output[threadId.x] = surface.AlbedoMap.SampleLevel(surface.Sampler, uv, 0.0f);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void OwnBlockBCS(uint3 threadId: SV_DispatchThreadID, uniform ParameterBlock<Material> detail)
{
    const float2 uv = float2(threadId.xy) * (1.0f / 512.0f);
    Output[threadId.x] += detail.AlbedoMap.SampleLevel(detail.Sampler, uv, 0.0f);
}
