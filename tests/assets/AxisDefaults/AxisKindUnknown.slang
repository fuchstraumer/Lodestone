module AxisKindUnknown;
import LodestoneAttributes;

// a misspelled axis kind: the cook must fail with AxisKindUnknown and suggest "Capability". it used to read as
// no kind, which turned off the capability axis checks without a word
[ls_axis_boolean][ls_axis_kind("capabilty")]
extern static const bool USE_WAVE_OPS = true;

RWStructuredBuffer<float> gOutput;

[shader("compute")]
[numthreads(32, 1, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    float value = float(tid.x);
    if (USE_WAVE_OPS)
    {
        value = WaveActiveSum(value);
    }
    gOutput[tid.x] = value;
}
