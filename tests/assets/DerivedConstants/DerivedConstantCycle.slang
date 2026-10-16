module DerivedConstantCycle;
import LodestoneAttributes;

[ls_axis_values("4, 8")]
extern static const uint GRID_X = 4u;

// Slang rejects this cycle at the root module load (E39999), so the cycle check of stage 4 only runs on
// constants that the source text reads differently than Slang does
static const uint FIRST = SECOND + GRID_X;
static const uint SECOND = FIRST * 2u;

[ls_element_count("FIRST")]
RWStructuredBuffer<uint> gOutput;

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    gOutput[tid.x] = GRID_X;
}
