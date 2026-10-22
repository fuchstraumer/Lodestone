module CmKernel;
import CmShared;

// Imports a module that sits in another folder, and is cooked in the same run (the shape of DiamondDogs' ddColor).

RWStructuredBuffer<float> Output;

[shader("compute")]
[numthreads(64, 1, 1)]
void BiasCS(uint3 id : SV_DispatchThreadID)
{
    Output[id.x] = AddBias(float(id.x));
}
