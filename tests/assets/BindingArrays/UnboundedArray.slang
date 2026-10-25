module UnboundedArray;
import LodestoneAttributes;

// Unbounded arrays of textures and of storage buffers. One entry point indexes them with a dynamically uniform value, and
// one with NonUniformResourceIndex. Each buffer states its size, so the renderer chooses only the descriptor count.

Texture2D<float4> Views[];
[ls_element_count("64")]
RWStructuredBuffer<float> Buffers[];
RWStructuredBuffer<float> Output;

[shader("compute")]
[numthreads(8, 1, 1)]
void UniformCS(uint3 id : SV_DispatchThreadID, uniform uint level)
{
    Output[id.x] = Views[level].Load(int3(id.xy, 0)).x + Buffers[level][id.x];
}

[shader("compute")]
[numthreads(8, 1, 1)]
void NonUniformCS(uint3 id : SV_DispatchThreadID)
{
    Output[id.x] = Views[NonUniformResourceIndex(id.x)].Load(int3(id.xy, 0)).x;
}
