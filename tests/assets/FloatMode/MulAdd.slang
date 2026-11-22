// Probe for --fp-mode: a multiply and an add that a compiler may fuse. Precise mode must decorate them NoContraction.
module MulAdd;

RWStructuredBuffer<float> gOutput;
StructuredBuffer<float> gA;
StructuredBuffer<float> gB;
StructuredBuffer<float> gC;

[shader("compute")]
[numthreads(64, 1, 1)]
void MulAddCS(uint3 tid : SV_DispatchThreadID)
{
    gOutput[tid.x] = gA[tid.x] * gB[tid.x] + gC[tid.x];
}
