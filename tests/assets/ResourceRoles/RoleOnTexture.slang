module RoleOnTexture;
import LodestoneAttributes;

// A role on a texture fails with ResourceRoleKindMismatch.

[ls_resource_role("IndirectDraw")]
RWTexture2D<float4> Target;

[shader("compute")]
[numthreads(64, 1, 1)]
void FailCS(uint3 threadId: SV_DispatchThreadID)
{
    Target[threadId.xy] = float4(1.0, 0.0, 0.0, 1.0);
}
