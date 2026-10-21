module TagInvalid;
import LodestoneAttributes;

// A tag with a slash fails with ResourceTagInvalid.

[ls_resource_tags("bad/tag")]
RWStructuredBuffer<uint> Output;

[shader("compute")]
[numthreads(64, 1, 1)]
void FailCS(uint3 threadId: SV_DispatchThreadID)
{
    Output[threadId.x] = threadId.x;
}
