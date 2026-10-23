module AxisDefaultUnread;
import LodestoneAttributes;

// no code reads DETAIL, so the axis is pruned, and nothing needs its enum initializer as a value

public enum Detail
{
    Low,
    High,
};

[ls_axis_enum]
extern static const Detail DETAIL = Detail::High;

[ls_axis_boolean]
extern static const bool USE_FOG = true;

RWStructuredBuffer<float> gValues;

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    gValues[tid.x] = USE_FOG ? 0.5f : 1.0f;
}
