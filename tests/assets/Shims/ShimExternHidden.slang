module ShimExternHidden;
import LodestoneShims;

// Must fail: the source text declares a shim extern that reflection does not find at module scope. The module-scope walk does
// not enter a namespace, so the two readers disagree.

namespace Hidden
{
    extern struct Samp : ISamplerRef;
}

RWStructuredBuffer<float> Output;

[shader("compute")]
[numthreads(8, 1, 1)]
void MainCS(uint3 id : SV_DispatchThreadID)
{
    Output[id.x] = 1.0f;
}
