module ShimConstantBuffer;

import LodestoneShims;

public struct Item
{
    public float4 value;
};

extern struct ItemBuffer : IReadBuffer<Item>;

struct Geo
{
    ItemBuffer buf;
    float4 tint;
};

ConstantBuffer<Geo> geo;

RWStructuredBuffer<float4> output;

[shader("compute")]
[numthreads(64, 1, 1)]
void MainCS(uint3 id : SV_DispatchThreadID)
{
    output[id.x] = geo.buf.Load(id.x).value * geo.tint;
}
