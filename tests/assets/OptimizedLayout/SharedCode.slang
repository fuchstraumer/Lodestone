// Probe for the read pass over emitted code: the axis only sizes a buffer, so every variant emits the same code, and
// the cook must read that code once for each target and compare each variant with its own reflection.
module SharedCode;
import LodestoneAttributes;

[ls_axis_values("4, 8, 16")]
extern static const uint COUNT = 4u;

[ls_element_count("COUNT")]
RWStructuredBuffer<uint> gOutput;

[shader("compute")]
[numthreads(64, 1, 1)]
void SharedCS(uint3 tid : SV_DispatchThreadID)
{
    gOutput[tid.x] = tid.x;
}
