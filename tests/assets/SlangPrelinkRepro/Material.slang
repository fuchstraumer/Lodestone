module Material;
import Shading;

typealias BRDF = Lambert;

RWStructuredBuffer<float3> OutColor;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    BRDF brdf = {};
    OutColor[tid.x] = brdf.shade(normalize(OutColor[tid.x]));
}
