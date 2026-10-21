module RoleCountMismatch;
import LodestoneAttributes;

// 6 words are not a whole number of 4-word draws: ResourceRoleStrideMismatch.

[ls_resource_role("IndirectDraw")]
[ls_element_count("6")]
RWStructuredBuffer<uint> Args;

[shader("compute")]
[numthreads(64, 1, 1)]
void FailCS(uint3 threadId: SV_DispatchThreadID)
{
    Args[threadId.x] = threadId.x;
}
