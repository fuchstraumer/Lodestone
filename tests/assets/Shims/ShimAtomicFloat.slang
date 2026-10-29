module ShimAtomicFloat;
import LodestoneShims;

// A float atomic buffer. IAtomicBuffer accepts int and uint only, so the declaration must fail.

extern struct Weights : IAtomicBuffer<float>;

ParameterBlock<Weights> weights;
RWStructuredBuffer<uint> Output;

[shader("compute")]
[numthreads(1, 1, 1)]
void MainCS(uint3 id : SV_DispatchThreadID)
{
    Output[id.x] = uint(weights.Load(id.x));
}
