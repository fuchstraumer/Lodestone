module AxisDefaults;
import LodestoneAttributes;

// every default below is deliberately not the first value of its axis, so a cook that confused the
// declared default with the canonical (first) value fails AxisDefaultTest

public interface ITint { float3 apply(float3 c); }

[ls_axis_interface_impl("ITint")]
public struct NoTint : ITint { public float3 apply(float3 c) { return c; } }
[ls_axis_interface_impl("ITint")]
public struct WarmTint : ITint { public float3 apply(float3 c) { return c * float3(1.0, 0.9, 0.8); } }

// non-ascending tags, so a cook that compared the tag with the case index would read the wrong case
public enum Detail
{
    Low = 5,
    High = 1,
    Medium = 10,
};

// interface axis: Slang reflection does not expose this initializer, so the source text is the only reader
[ls_axis_interface]
public extern struct TINT : ITint = WarmTint;

// enum axis: the default is the last case, and reflection reads its tag (10) as a second opinion
[ls_axis_enum]
extern static const Detail DETAIL = Detail::Medium;

// a literal initializer: the source text and Slang reflection both read it, and must agree
[ls_axis_values("128, 256, 512")]
extern static const uint IFFT_SIZE = 512u;

// an expression initializer: Slang reflection returns no value for it, so the source text stands alone
[ls_axis_values("2, 4")]
extern static const uint SAMPLES = 2u * 2u;

[ls_axis_boolean]
extern static const bool USE_FOG = true;

RWStructuredBuffer<float3> gPixels;

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    TINT tint = {};
    float3 c = tint.apply(gPixels[tid.x]) * float(IFFT_SIZE + SAMPLES);
    if (DETAIL == Detail::High)
    {
        c *= 2.0;
    }
    if (USE_FOG)
    {
        c *= 0.5;
    }
    gPixels[tid.x] = c;
}
