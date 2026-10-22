module WorkgroupMemory;
import LodestoneAttributes;

// A probe for the workgroup memory cross-check. The four groupshared variables pad differently, and both targets must
// read the same sum: 64 + 32 + 16 + 32 = 144 bytes. The half4 array also needs `enable f16` in WGSL and Float16 in
// SPIR-V. The module declares no axis, so it cooks as one variant.

struct Pair
{
    float Scale;
    float3 Offset;
};

groupshared float3 g_vectors[4];
groupshared Pair g_pair;
groupshared float g_scalars[4];
groupshared half4 g_halves[4];

RWStructuredBuffer<float> Output;

[shader("compute")]
[numthreads(64, 1, 1)]
void ProbeCS(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    if (gi < 4)
    {
        g_vectors[gi] = float3(gi, gi + 1, gi + 2);
        g_scalars[gi] = gi;
        g_halves[gi] = half4(gi);
    }
    if (gi == 0)
    {
        g_pair.Scale = 2.0;
        g_pair.Offset = float3(1.0, 2.0, 3.0);
    }
    GroupMemoryBarrierWithGroupSync();

    float sum = g_pair.Scale * g_pair.Offset.x + g_scalars[gi & 3];
    sum += dot(g_vectors[gi & 3], g_pair.Offset) + float(g_halves[gi & 3].x);
    Output[id.x] = sum;
}
