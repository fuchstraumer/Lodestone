module HostTypesThreads;
import LodestoneAttributes;

// Four group sizes: a literal, a size that a constant computes, a size that an axis changes, and an expression of the axis.
// The module has two variants.

[ls_axis_values("32, 64")]
extern static const uint GROUP_SIZE = 32u;

static const uint TILE = 8u;

RWStructuredBuffer<uint> output;

[shader("compute")]
[numthreads(64, 1, 1)]
void LiteralCS(uint3 threadId: SV_DispatchThreadID)
{
    output[threadId.x] = threadId.x;
}

[shader("compute")]
[numthreads(TILE * 2u, TILE, 1)]
void ConstantCS(uint3 threadId: SV_DispatchThreadID)
{
    output[threadId.x] = threadId.y;
}

[shader("compute")]
[numthreads(GROUP_SIZE, 1, 1)]
void AxisCS(uint3 threadId: SV_DispatchThreadID)
{
    output[threadId.x] = GROUP_SIZE;
}

// Slang does not fold an expression of link-time constants in the reflection of the size
[shader("compute")]
[numthreads(GROUP_SIZE * 2u, 1, 1)]
void ProductCS(uint3 threadId: SV_DispatchThreadID)
{
    output[threadId.x] = GROUP_SIZE;
}
