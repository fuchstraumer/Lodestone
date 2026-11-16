module PolicyLeaves;

// Types for the interface axis in PolicyGraph.slang. Composed is generic and untagged, so only the policy file can
// add an instantiation of it as a value of the axis.

public interface ILeaf { float Eval(float3 position); }

public interface IGraph { float Eval(float3 position); }

public struct LeafA : ILeaf
{
    public float4 Tint;
    public float Eval(float3 position) { return Tint.x * position.x; }
}

public struct LeafB : ILeaf
{
    public float Scale;
    public float3 Offset;
    public float Eval(float3 position) { return Scale * dot(position, Offset); }
}

// does not conform to ILeaf
public struct BadLeaf
{
    public float Value;
}

public struct Composed<A : ILeaf, B : ILeaf> : IGraph
{
    public A First;
    public B Second;
    public float Eval(float3 position) { return First.Eval(position) * Second.Eval(position); }
}

public typealias AliasOfComposed = Composed<LeafA, LeafB>;
