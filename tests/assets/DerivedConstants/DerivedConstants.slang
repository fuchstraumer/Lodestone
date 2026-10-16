module DerivedConstants;
import LodestoneAttributes;
__include DerivedConstantsFragment;

// size expressions name derived constants: module-scope `static const` values computed from axes. each
// declaration below tests one case of the scope filter (reflection decides scope, the text supplies the
// expression)

[ls_axis_values("4, 8")]
extern static const uint GRID_X = 4u;

[ls_axis_values("2, 4")]
extern static const uint GRID_Y = 2u;

// module scope: captured
static const uint CLUSTER_COUNT = GRID_X * GRID_Y;

// module scope, names another derived constant: captured
static const uint LIGHT_SLOTS = CLUSTER_COUNT * LIGHTS_PER_CLUSTER;

struct ClusterInfo
{
    // a struct member: not module scope, so not captured
    static const uint MEMBER_ONLY = 3u;
    uint count;
};

// sized by a derived constant two levels deep: GRID_X * GRID_Y * 16 gives 128, 256, 256, and 512
[ls_element_count("LIGHT_SLOTS")]
RWStructuredBuffer<uint> gLightIndices;

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    // a function local: not module scope, so not captured
    static const uint LOCAL_ONLY = 7u;
    gLightIndices[tid.x] = CLUSTER_COUNT + LIGHT_SLOTS + LOCAL_ONLY + ClusterInfo.MEMBER_ONLY;
}
