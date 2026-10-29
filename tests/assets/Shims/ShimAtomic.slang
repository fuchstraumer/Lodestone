module ShimAtomic;
import LodestoneShims;

// An atomic buffer shim in a parameter block. The Bound profiles bind RWStructuredBuffer<Atomic<uint>>, and the Pointer
// profiles store a pointer to Atomic<uint> in the uniform buffer of the block.

extern struct Counters : IAtomicBuffer<uint>;

struct Params
{
    Counters Totals;
    uint Limit;
};

ParameterBlock<Params> params;
RWStructuredBuffer<uint> Output;

[shader("compute")]
[numthreads(64, 1, 1)]
void CountCS(uint3 id : SV_DispatchThreadID)
{
    const uint ticket = params.Totals.Add(0u, 1u);
    const uint previous = params.Totals.CompareExchange(1u, ticket, ticket + 1u);
    const uint flags = params.Totals.Or(2u, 1u << (id.x & 31u)) + params.Totals.Increment(3u);
    Output[id.x] = ticket + previous + flags + params.Limit;
}
