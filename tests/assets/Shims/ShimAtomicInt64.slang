module ShimAtomicInt64;
import LodestoneShims;

// A 64-bit atomic buffer. IAtomicBuffer accepts int and uint only, so the declaration must fail.

extern struct Wide : IAtomicBuffer<int64_t>;

ParameterBlock<Wide> wide;
RWStructuredBuffer<uint> Output;

[shader("compute")]
[numthreads(1, 1, 1)]
void MainCS(uint3 id : SV_DispatchThreadID)
{
    Output[id.x] = uint(wide.Load(id.x));
}
