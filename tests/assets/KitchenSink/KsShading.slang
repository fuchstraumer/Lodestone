module KsShading;
import LodestoneAttributes;

// The declaring module for every interface axis in the set. No axis lives here. A consumer module
// imports this module and declares an [ls_axis_interface] extern struct over one of these interfaces.
//
// This is the interface-axis half of the cross-module resolution proof. The interface and its tagged
// implementations sit here, and the axis sits in another module. The current interface-axis read finds
// its implementations through an __include fragment inside one module. An import crosses a module
// boundary instead, so the read must walk the conformance up to this module. That is the gap this set
// exercises for interface axes, in step with the enum gap in KsTypes.

public interface IBrdf
{
    float3 shade(float3 albedo, float3 n, float3 l);
}

[ls_axis_interface_impl("IBrdf")]
public struct Lambert : IBrdf
{
    public float3 shade(float3 albedo, float3 n, float3 l)
    {
        return albedo * max(dot(n, l), 0.0f);
    }
}

[ls_axis_interface_impl("IBrdf")]
public struct Ggx : IBrdf
{
    public float3 shade(float3 albedo, float3 n, float3 l)
    {
        const float ndotl = max(dot(n, l), 0.0f);
        return albedo * (ndotl * ndotl);
    }
}

[ls_axis_interface_impl("IBrdf")]
public struct OrenNayar : IBrdf
{
    public float3 shade(float3 albedo, float3 n, float3 l)
    {
        const float ndotl = max(dot(n, l), 0.0f);
        return albedo * (ndotl * 0.75f + 0.25f);
    }
}

public interface INoiseSource
{
    float sample(float3 p);
}

[ls_axis_interface_impl("INoiseSource")]
public struct ValueNoise : INoiseSource
{
    public float sample(float3 p)
    {
        return frac(sin(dot(p, float3(12.9898f, 78.233f, 37.719f))) * 43758.5453f);
    }
}

[ls_axis_interface_impl("INoiseSource")]
public struct PerlinNoise : INoiseSource
{
    public float sample(float3 p)
    {
        const float3 f = frac(p);
        return dot(f, float3(0.3333f, 0.3333f, 0.3333f));
    }
}

[ls_axis_interface_impl("INoiseSource")]
public struct WorleyNoise : INoiseSource
{
    public float sample(float3 p)
    {
        const float3 c = floor(p) + 0.5f;
        return length(frac(p) - (c - floor(p)));
    }
}
