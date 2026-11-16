module HostTypesVariants;
import LodestoneAttributes;

// A generic struct, so that the host identifier is built from the full Slang name, and a binding that one boolean axis
// removes, so that the header names the axis. The module has two variants.

[ls_axis_boolean]
extern static const bool USE_TINT = false;

struct HasIf<T>
{
    T value;
    float pad;
};

ConstantBuffer<HasIf<float4>> generic;
StructuredBuffer<float4> tint;
RWStructuredBuffer<float4> output;

[shader("compute")]
[numthreads(64, 1, 1)]
void VariantCS(uint3 threadId: SV_DispatchThreadID)
{
    float4 value = generic.value * generic.pad;
    if (USE_TINT)
    {
        value *= tint[threadId.x];
    }

    output[threadId.x] = value;
}
