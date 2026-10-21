module EntryPointConstants;

// The entry point constants: the `uniform` parameters of ordinary data on an entry point. Slang delivers them to WGSL
// in a uniform buffer named `entryPointParams` (std140 layout), and to SPIR-V in a push constant block (std430
// layout), so the byte size can differ between the two targets of one variant.
//
// The module registers no axis, so it cooks as one variant.

RWStructuredBuffer<float4> Output;

// A struct parameter that contains a resource and two scalars. The resource is a binding of its own, and the scalars
// are the constants. std140 rounds the struct up to 16 bytes, and std430 packs it in 8.
struct MaterialParams
{
    StructuredBuffer<float4> Tints;
    uint TintIndex;
    float Scale;
};

[shader("compute")]
[numthreads(64, 1, 1)]
void MixedCS(uint3 threadId: SV_DispatchThreadID, uniform MaterialParams material)
{
    Output[threadId.x] = material.Tints[material.TintIndex] * material.Scale;
}

// Loose parameters. A float4 aligns to 16 bytes in both layouts, so the uint follows at offset 16.
[shader("compute")]
[numthreads(64, 1, 1)]
void LooseCS(uint3 threadId: SV_DispatchThreadID, uniform float4 tint, uniform uint count)
{
    Output[threadId.x] = tint * float(count);
}

// The data is declared and never read. Both targets remove it, so the entry point has no constants.
[shader("compute")]
[numthreads(64, 1, 1)]
void UnusedCS(uint3 threadId: SV_DispatchThreadID, uniform float4 tint)
{
    Output[threadId.x] = float4(0.0f, 0.0f, 0.0f, 0.0f);
}

// Resources only. Slang adds no buffer around resources, so the entry point has no constants.
[shader("compute")]
[numthreads(64, 1, 1)]
void ResourcesOnlyCS(uint3 threadId: SV_DispatchThreadID, uniform StructuredBuffer<float4> source)
{
    Output[threadId.x] = source[threadId.x];
}
