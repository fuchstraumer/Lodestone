module HostTypesSized;
import LodestoneAttributes;

// An array member that an axis sizes: the struct has a different size in each variant, so the cook fails.

[ls_axis_values("2, 4")]
extern static const uint COUNT = 2u;

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
