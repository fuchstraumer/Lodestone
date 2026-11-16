module PolicyGraph;
import LodestoneAttributes;
__exported import PolicyLeaves;

// One tagged type, and an instantiation of Composed that the policy file adds.

[ls_axis_interface_impl("IGraph")]
public struct Constant : IGraph
{
    public float Value;
    public float Eval(float3 position) { return Value; }
}

[ls_axis_interface]
public extern struct GRAPH : IGraph = Constant;

[ls_axis_boolean][ls_axis_kind("tuning")]
extern static const bool USE_BIAS = false;

ParameterBlock<GRAPH> gGraph;
RWStructuredBuffer<float> gOutput;

[shader("compute")]
[numthreads(64, 1, 1)]
void GraphCS(uint3 id : SV_DispatchThreadID)
{
    float result = gGraph.Eval(float3(id));
    if (USE_BIAS)
    {
        result += 1.0f;
    }
    gOutput[id.x] = result;
}
