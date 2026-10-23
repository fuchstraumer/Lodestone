module OffPathNeedsMore;
import LodestoneAttributes;

// Must fail: the off path uses a wave operation and the on path does not.
// A capability axis. The axis is inverted.

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
        value += 1.0f;
    }
    else
    {
        value += WaveActiveSum(value);
    }
    Output[id.x] = value;
}
