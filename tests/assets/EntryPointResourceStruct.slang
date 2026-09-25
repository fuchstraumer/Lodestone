// A reflection cross-check row that must fail. The entry point takes a `uniform` struct that holds a
// resource. Slang emits the struct's data as `entryPointParams`, and the cross-check expects the name
// the reflection gives it. The two disagree today, so the cook must fail with ReflectionMismatch.
struct Material
{
    StructuredBuffer<float4> Tints;
    uint TintIndex;
};

RWStructuredBuffer<float4> Output;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uniform Material material, uint3 tid : SV_DispatchThreadID)
{
    Output[tid.x] = material.Tints[material.TintIndex];
}
