module InterfaceAxisTest;
import LodestoneAttributes;
__include ShadeModes;

// interface axis: behaviour chosen per variant, monomorphized to one concrete type, zero dispatch
[ls_axis_interface]
public extern struct SHADE_MODE : IShadeMode = Passthrough;   // default keeps the module standalone-compilable; kind defaults to technique

// a constant axis to cross with, so the space is a real product: 3 modes x 2 = 6 variants
[ls_axis_boolean][ls_axis_kind("technique")]
extern static const bool APPLY_TWICE = false;

RWStructuredBuffer<float3> gPixels;

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    SHADE_MODE mode = {};
    float3 c = mode.apply(gPixels[tid.x]);
    if (APPLY_TWICE)
    {
        c = mode.apply(c);
    }
    gPixels[tid.x] = c;
}
