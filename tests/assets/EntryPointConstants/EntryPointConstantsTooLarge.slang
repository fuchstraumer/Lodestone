module EntryPointConstantsTooLarge;

// Two float4x4 and a float4 are 144 bytes. Vulkan guarantees 128 bytes of push constants, so the SPIR-V cook must
// fail with EntryPointConstantsTooLarge. WGSL delivers the same data in a uniform buffer, which has no such limit.

RWStructuredBuffer<float4> Output;

[shader("compute")]
[numthreads(64, 1, 1)]
void LargeCS(uint3 threadId: SV_DispatchThreadID, uniform float4x4 view, uniform float4x4 projection, uniform float4 tint)
{
    Output[threadId.x] = mul(projection, mul(view, tint));
}
