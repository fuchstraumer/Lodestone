module EnumAxisTest;
import LodestoneAttributes;

// The enum's values are deliberately non-ascending and non-contiguous, so a cook that assumed
// "case index == case value" would produce wrong data. Reflection reads each real value, so the
// digit stays the declaration-order position (Low=0, High=1, Medium=2) while the stored value is 5/1/10.
// public so the per-variant synthetic module can import this module and name the enum type, the same
// way an interface axis needs its interface and implementations public.
public enum QualityLevel
{
    Low = 5,
    High = 1,
    Medium = 10,
};

// enum axis: one variant per case, addressed by case name in queries. The default keeps the module
// standalone-compilable, the same way an interface axis keeps its default impl.
[ls_axis_enum]
extern static const QualityLevel QUALITY = QualityLevel::Low;

// cross with a boolean axis so the space is a real product: 3 cases x 2 = 6 variants.
[ls_axis_boolean]
extern static const bool DITHER = false;

RWStructuredBuffer<float3> gPixels;

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    float3 c = gPixels[tid.x];

    int samples = 1;
    if (QUALITY == QualityLevel::High)
    {
        samples = 8;
    }
    else if (QUALITY == QualityLevel::Medium)
    {
        samples = 4;
    }
    c *= float(samples);

    if (DITHER)
    {
        c += 0.5;
    }

    gPixels[tid.x] = c;
}
