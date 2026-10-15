module AxisDefaultNotDeclared;
import LodestoneAttributes;

// the initializer is not one of the declared values: the cook must fail with AxisDefaultNotDeclared
[ls_axis_values("128, 256")]
extern static const uint IFFT_SIZE = 512u;

RWStructuredBuffer<uint> gOutput;

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    gOutput[tid.x] = IFFT_SIZE;
}
