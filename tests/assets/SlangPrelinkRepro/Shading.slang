module Shading;

public interface IBrdf
{
    float3 shade(float3 n);
}

public struct Lambert : IBrdf
{
    public float3 shade(float3 n) { return n * max(dot(n, n), 0.0f); }
}
