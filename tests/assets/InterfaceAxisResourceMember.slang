module InterfaceAxisResourceMember;
import LodestoneAttributes;

// Must fail: an implementation of an interface axis holds a resource. An interface axis is used as a local variable, and
// binding layout is fixed before link-time specialization, so the resource would have no binding. A shim extern
// (phase F step F2) is a parameter type instead, and may hold one.

public interface ISampleSource { float4 Sample(float2 uv); }

[ls_axis_interface_impl("ISampleSource")]
public struct Flat : ISampleSource { public float4 Sample(float2 uv) { return float4(0.5f); } }

[ls_axis_interface_impl("ISampleSource")]
public struct Textured : ISampleSource
{
    Texture2D<float4> Source;
    SamplerState Filter;
    public float4 Sample(float2 uv) { return Source.SampleLevel(Filter, uv, 0.0f); }
}

[ls_axis_interface]
public extern struct SOURCE : ISampleSource = Flat;

RWStructuredBuffer<float4> Output;

[shader("compute")]
[numthreads(8, 1, 1)]
void MainCS(uint3 id : SV_DispatchThreadID)
{
    SOURCE source = {};
    Output[id.x] = source.Sample(float2(0.5f, 0.5f));
}
