module RoleNeedsCount;
import LodestoneAttributes;

// A 4-byte stride with no element count cannot be checked: ResourceRoleNeedsElementCount.

[ls_resource_role("IndirectDispatch")]
RWStructuredBuffer<uint> Args;

[shader("compute")]
[numthreads(64, 1, 1)]
void FailCS(uint3 threadId: SV_DispatchThreadID)
{
    Args[threadId.x] = threadId.x;
}
