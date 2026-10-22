module UnboundedArray;
import LodestoneAttributes;

// An unbounded array of textures is the Indexed access model. Until phase F step F4, the SPIR-V cross-check reports it.

Texture2D<float4> Views[];
RWStructuredBuffer<float> Output;

[shader("compute")]
[numthreads(8, 1, 1)]
void MainCS(uint3 id : SV_DispatchThreadID, uniform uint level)
{
    Output[id.x] = Views[level].Load(int3(id.xy, 0)).x;
}
