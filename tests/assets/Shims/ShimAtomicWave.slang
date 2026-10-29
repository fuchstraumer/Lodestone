module ShimAtomicWave;
import LodestoneShims;

// Two entry points over one atomic buffer. Only WaveCS calls the wave-aggregated add, so only it needs the subgroup
// capabilities.

extern struct Counters : IAtomicBuffer<uint>;

struct Params
{
    Counters Totals;
};

ParameterBlock<Params> params;
RWStructuredBuffer<uint> Output;

[shader("compute")]
[numthreads(64, 1, 1)]
void WaveCS(uint3 id : SV_DispatchThreadID)
{
    Output[id.x] = AtomicAddWaveAggregated(params.Totals, 0u, 1u);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void PlainCS(uint3 id : SV_DispatchThreadID)
{
    Output[id.x] = params.Totals.Add(0u, 1u);
}
