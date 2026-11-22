module ShimAccumulator64;
import LodestoneShims;

// A 64-bit accumulator shim in a parameter block. The Bound form on SPIR-V is a 64-bit atomic for each element, the Bound form
// on WGSL is two 32-bit atomics, and the Pointer form is a pointer to a 64-bit atomic.

extern struct Sums : IAtomicAccumulator64;

struct Params
{
    Sums Totals;
    int Scale;
};

ParameterBlock<Params> params;
RWStructuredBuffer<uint> Output;

[shader("compute")]
[numthreads(64, 1, 1)]
void AccumulateCS(uint3 id : SV_DispatchThreadID)
{
    // odd threads add a negative value, which sets the high word to all ones
    const int magnitude = int(id.x + 1u) * params.Scale;
    const Wide64 term = Wide64.FromInt((id.x & 1u) != 0u ? -magnitude : magnitude);

    params.Totals.Add(0u, term);
    params.Totals.Sub(1u, term);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void ClearCS(uint3 id : SV_DispatchThreadID)
{
    params.Totals.Store(id.x, Wide64.Make(0u, 0u));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void ReadCS(uint3 id : SV_DispatchThreadID)
{
    const Wide64 total = params.Totals.Load(id.x);

    Output[id.x] = total.low ^ total.high ^ asuint(total.ToFloat());
}
