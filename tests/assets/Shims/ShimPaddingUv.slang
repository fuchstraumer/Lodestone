module ShimPaddingUv;

import LodestoneShims;

// A float2 after a float3 is at offset 12 in the scalar layout and at offset 16 in the std430 layout, so the cook fails
// on every profile.
public struct UvVertex
{
    public float3 position;
    public float2 uv;
};

extern struct Vertices : IReadBuffer<UvVertex>;

Vertices vertices;

RWStructuredBuffer<float4> output;

[shader("compute")]
[numthreads(64, 1, 1)]
void MainCS(uint3 id : SV_DispatchThreadID)
{
    const UvVertex vertex = vertices.Load(id.x);
    output[id.x] = float4(vertex.position, vertex.uv.x);
}
