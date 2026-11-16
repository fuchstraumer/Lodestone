module NestedLevels;
import LodestoneAttributes;

// The control: each higher level needs every capability of the level below it.
// A capability axis with three integer values.

[ls_axis_values("1, 2, 3")][ls_axis_kind("capability")]
extern static const uint LEVEL = 1;

RWStructuredBuffer<float> Output;

[shader("compute")]
[numthreads(32, 1, 1)]
void SumCS(uint3 id : SV_DispatchThreadID)
{
    float value = float(id.x);
    if (LEVEL >= 2)
    {
        value += WaveActiveSum(value);
    }
    if (LEVEL >= 3)
    {
        half narrow = half(value);
        value += float(narrow * narrow);
    }
    Output[id.x] = value;
}
