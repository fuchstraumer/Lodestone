module InterfaceAxisGenericImpl;
import LodestoneAttributes;

// Must fail: a generic type is tagged as an interface axis value. The link line names no type arguments, so no variant
// could use it. Before the check, the cook dropped the type in silence.

public interface ILayer { float Eval(float3 position); }

public interface IGraph { float Eval(float3 position); }

[ls_axis_interface_impl("IGraph")]
public struct Plain : IGraph { public float Eval(float3 position) { return 1.0f; } }

[ls_axis_interface_impl("IGraph")]
public struct Composed<A : ILayer, B : ILayer> : IGraph
{
    public float Eval(float3 position)
    {
        A first = {};
        B second = {};
        return first.Eval(position) * second.Eval(position);
    }
}

[ls_axis_interface]
public extern struct GRAPH : IGraph = Plain;

RWStructuredBuffer<float> Output;

[shader("compute")]
[numthreads(64, 1, 1)]
void MainCS(uint3 id : SV_DispatchThreadID)
{
    GRAPH graph = {};
    Output[id.x] = graph.Eval(float3(id));
}
