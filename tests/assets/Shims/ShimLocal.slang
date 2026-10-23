module ShimLocal;
import LodestoneShims;

// Must fail: a shim extern used as a function-local. A resource cannot be a local variable, so the emitted code has no binding
// for it, and a cross-check or validator rejects the cook (phase F step F2.0).

extern struct Points : IReadBuffer<float4>;

RWStructuredBuffer<float4> Output;

[shader("compute")]
[numthreads(8, 1, 1)]
void BlockCS(uint3 id : SV_DispatchThreadID)
{
    Points points = {};
    Output[id.x] = points.Load(id.x);
}
