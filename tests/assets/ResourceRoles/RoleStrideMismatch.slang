module RoleStrideMismatch;
import LodestoneAttributes;

// A 12-byte stride does not fit a 16-byte draw: ResourceRoleStrideMismatch.

struct DispatchArgs
{
    uint x;
    uint y;
    uint z;
};

[ls_resource_role("IndirectDraw")]
RWStructuredBuffer<DispatchArgs> Args;

[shader("compute")]
[numthreads(64, 1, 1)]
void FailCS(uint3 threadId: SV_DispatchThreadID)
{
    Args[threadId.x].x = threadId.x;
}
