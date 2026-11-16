module PolicyGraphControl;
import PolicyLeaves;

// The same shader with the type written in the source. It has no axis, so it cooks one variant.

typealias GRAPH = Composed<LeafA, LeafB>;

ParameterBlock<GRAPH> gGraph;
RWStructuredBuffer<float> gOutput;

[shader("compute")]
[numthreads(64, 1, 1)]
void GraphCS(uint3 id : SV_DispatchThreadID)
{
    gOutput[id.x] = gGraph.Eval(float3(id));
}
