module RoleUnknown;
import LodestoneAttributes;

// A misspelled role fails with ResourceRoleUnknown, and the error suggests IndirectDraw.

[ls_resource_role("IndirectDraws")]
RWStructuredBuffer<uint> Args;

[shader("compute")]
[numthreads(64, 1, 1)]
void FailCS(uint3 threadId: SV_DispatchThreadID)
{
    Args[threadId.x] = threadId.x;
}
