module ShimPaddingTwoFloat3;

import LodestoneShims;

// A float3 after a float3 is at offset 12 in the scalar layout and at offset 16 in the std430 layout, so the cook fails
// on every profile.
public struct PairVertex
{
    public float3 a;
    public float3 b;
};

extern struct Vertices : IReadBuffer<PairVertex>;

Vertices vertices;

RWStructuredBuffer<float4> output;

[shader("compute")]
[numthreads(64, 1, 1)]
void MainCS(uint3 id : SV_DispatchThreadID)
{
    const PairVertex vertex = vertices.Load(id.x);
    output[id.x] = float4(vertex.a + vertex.b, 0.0f);
}
