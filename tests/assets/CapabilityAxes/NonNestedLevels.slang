module NonNestedLevels;
import LodestoneAttributes;

// Must fail: level 1 needs a wave operation, and level 2 needs a 16-bit type instead.
// A capability axis whose capability sets do not nest.

[ls_axis_values("1, 2")][ls_axis_kind("capability")]
extern static const uint LEVEL = 1;

RWStructuredBuffer<float> Output;

[shader("compute")]
[numthreads(32, 1, 1)]
void SumCS(uint3 id : SV_DispatchThreadID)
{
    float value = float(id.x);
    if (LEVEL == 1)
    {
        value += WaveActiveSum(value);
    }
    else
    {
        half narrow = half(value);
        value += float(narrow * narrow);
    }
    Output[id.x] = value;
}
