module ShimPadded;

import LodestoneShims;

// The scalar layout of this element equals its std430 layout: every float3 is followed by one 4-byte value, and the
// float4 and the two float2 start at multiples of their alignment. The stride is 64 bytes on every profile.
public struct PaddedVertex
{
    public float3 position;
    public float pad0;
    public float3 normal;
    public float pad1;
    public float4 tangent;
    public float2 uv0;
    public float2 uv1;
};

extern struct Vertices : IReadBuffer<PaddedVertex>;

struct Geometry
{
    Vertices vertices;
};

ConstantBuffer<Geometry> geometry;

RWStructuredBuffer<float4> output;

[shader("compute")]
[numthreads(64, 1, 1)]
void MainCS(uint3 id : SV_DispatchThreadID)
{
    const PaddedVertex vertex = geometry.vertices.Load(id.x);
    output[id.x] = float4(vertex.position + vertex.normal, vertex.uv0.x + vertex.uv1.y) * vertex.tangent;
}
