module GatedWaveSize;
import LodestoneAttributes;

// The control for a gated capability axis. WAVE_SIZE refines USE_WAVE_OPS and changes no capability, so only the
// ungated axis is checked.

[ls_axis_boolean][ls_axis_kind("capability")]
extern static const bool USE_WAVE_OPS = true;

[ls_axis_values("16, 32")][ls_axis_kind("capability")][ls_axis_active_when("USE_WAVE_OPS == 1")]
extern static const uint WAVE_SIZE = 32u;

RWStructuredBuffer<float> Output;

[shader("compute")]
[numthreads(32, 1, 1)]
void SumCS(uint3 id : SV_DispatchThreadID)
{
    float value = float(id.x);
    if (USE_WAVE_OPS)
    {
        value += WaveActiveSum(value) / float(WAVE_SIZE);
    }
    Output[id.x] = value;
}
