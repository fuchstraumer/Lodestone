module DerivedConstantShadow;
import LodestoneAttributes;

[ls_axis_values("4, 8")]
extern static const uint GRID_X = 4u;

[ls_element_count("GRID_X")]
RWStructuredBuffer<uint> gOutput;

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    // a function local with the name of the axis: Slang accepts it, and the cook rejects it
    static const uint GRID_X = 3u;
    gOutput[tid.x] = GRID_X;
}
