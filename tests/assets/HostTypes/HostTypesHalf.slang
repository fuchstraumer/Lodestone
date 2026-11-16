module HostTypesHalf;

// 16-bit members. A scalar aligns to its own size, and a vector of two to twice that.

struct Packed
{
    half scale;
    half2 uv;
    uint tag;
};

ConstantBuffer<Packed> packed;
RWStructuredBuffer<float4> output;

[shader("compute")]
[numthreads(64, 1, 1)]
void PackedCS(uint3 threadId: SV_DispatchThreadID)
{
    output[threadId.x] = float4(float(packed.scale), float(packed.uv.x), float(packed.tag), 0.0f);
}
