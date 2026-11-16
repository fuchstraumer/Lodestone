module HostTypesDefault;

// An array member that a link-time constant sizes, and no axis drives the constant. Slang reports the element count
// as unknown, so the cooker derives it from the size and the stride. The module is one variant.

extern static const uint COUNT = 3u;

struct Sized
{
    float data[COUNT];
};

ConstantBuffer<Sized> sized;
RWStructuredBuffer<float4> output;

[shader("compute")]
[numthreads(64, 1, 1)]
void SizedCS(uint3 threadId: SV_DispatchThreadID)
{
    output[threadId.x] = float4(sized.data[1], 0.0f, 0.0f, 0.0f);
}
