module InterfaceAxisActiveWhen;
import LodestoneAttributes;

// TINT is active only when USE_TINT is true, so the space has 1 + 2 = 3 variants. A cook that ignores the
// gate on the interface axis has 2 x 2 = 4 variants, and InterfaceAxisActiveWhenTest fails.

public interface ITint { float3 apply(float3 c); }

[ls_axis_interface_impl("ITint")]
public struct NoTint : ITint { public float3 apply(float3 c) { return c; } }
[ls_axis_interface_impl("ITint")]
public struct WarmTint : ITint { public float3 apply(float3 c) { return c * float3(1.0, 0.9, 0.8); } }

[ls_axis_boolean]
extern static const bool USE_TINT = false;

[ls_axis_interface]
[ls_axis_interface_active_when("USE_TINT")]
public extern struct TINT : ITint = NoTint;

RWStructuredBuffer<float3> gPixels;

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    float3 c = gPixels[tid.x];
    if (USE_TINT)
    {
        TINT tint = {};
        c = tint.apply(c);
    }
    gPixels[tid.x] = c;
}
