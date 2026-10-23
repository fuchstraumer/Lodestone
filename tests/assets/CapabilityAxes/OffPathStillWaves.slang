module OffPathStillWaves;
import LodestoneAttributes;

// Must fail: both paths use a wave operation.
// A capability axis. The axis turns nothing on.

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
        value += WaveActiveSum(value) * 0.5f;
    }
    Output[id.x] = value;
}
