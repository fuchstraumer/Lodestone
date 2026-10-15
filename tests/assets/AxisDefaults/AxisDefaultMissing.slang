module AxisDefaultMissing;
import LodestoneAttributes;

// an axis with no initializer: the cook must fail with AxisDefaultMissing, because graph code fills an
// axis it does not pin with the declared default
[ls_axis_values("1, 2")]
extern static const uint COUNT;

RWStructuredBuffer<uint> gOutput;

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    gOutput[tid.x] = COUNT;
}
