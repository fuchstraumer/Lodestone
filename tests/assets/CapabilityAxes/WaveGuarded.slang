module WaveGuarded;
import LodestoneAttributes;

// The control: only the on path uses a wave operation.
// A capability axis. The off path uses none.

[ls_axis_boolean][ls_axis_kind("capability")]
extern static const bool USE_WAVE_OPS = true;

RWStructuredBuffer<float> Output;

[shader("compute")]
[numthreads(32, 1, 1)]
void SumCS(uint3 id : SV_DispatchThreadID)
{
    float value = float(id.x);
    if (USE_WAVE_OPS)
    {
        value += WaveActiveSum(value);
    }
    else
    {
        value += 1.0f;
    }
    Output[id.x] = value;
}
