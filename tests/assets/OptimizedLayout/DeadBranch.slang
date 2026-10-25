// Probe for the optimized layout: a branch on an interface method that returns a constant false (the shape of the
// pyramid's BilinearIsExact). Slang's IR keeps the sampler and the groupshared array in the Max variant, and
// spirv-opt (above --O0) removes them from the SPIR-V. The cook must take the used bindings from the emitted code.
module DeadBranch;
import LodestoneAttributes;

public interface IReduction
{
    bool UsesSampler();
    float4 Reduce(float4 a, float4 b);
}

[ls_axis_interface_impl("IReduction")]
public struct Exact : IReduction
{
    public bool UsesSampler()
    {
        return true;
    }

    public float4 Reduce(float4 a, float4 b)
    {
        return 0.5f * (a + b);
    }
}

[ls_axis_interface_impl("IReduction")]
public struct Max : IReduction
{
    public bool UsesSampler()
    {
        return false;
    }

    public float4 Reduce(float4 a, float4 b)
    {
        return max(a, b);
    }
}

[ls_axis_interface]
public extern struct REDUCTION : IReduction = Exact;

uniform Texture2D<float4> Input;
uniform SamplerState Linear;
uniform RWStructuredBuffer<float4> Output;

groupshared float4 s_Cache[16];

[shader("compute")]
[numthreads(64, 1, 1)]
void ReduceCS(uint3 tid : SV_DispatchThreadID)
{
    REDUCTION r = {};
    if (r.UsesSampler())
    {
        s_Cache[tid.x % 16] = Input.SampleLevel(Linear, float2(tid.xy) / 64.0f, 0.0f);
        GroupMemoryBarrierWithGroupSync();
        Output[tid.x] = s_Cache[(tid.x + 1) % 16];
    }
    else
    {
        Output[tid.x] = r.Reduce(Input.Load(int3(tid.xy, 0)), Input.Load(int3(tid.xy + 1, 0)));
    }
}
