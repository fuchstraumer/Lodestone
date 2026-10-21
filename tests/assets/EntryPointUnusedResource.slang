// The entry point takes a resource it never reads, beside ordinary data that it reads. Slang collects the
// parameters in one constant buffer, and its metadata marks every slot of that buffer as used when any part is used
// (shader-slang/slang#13789). The cook removes `unusedBuf` from the visibility because the emitted code does not
// declare it. AccessModelRejectTest cooks this module, and SlangMetadataCanaryTest checks that the bug still exists.
RWStructuredBuffer<float4> Output;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uniform float4 tint,
          uniform StructuredBuffer<float4> usedBuf,
          uniform StructuredBuffer<float4> unusedBuf,
          uint3 tid : SV_DispatchThreadID)
{
    Output[tid.x] = usedBuf[tid.x] * tint;
}
