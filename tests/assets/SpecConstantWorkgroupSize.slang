// A codegen row that must fail. For WGSL, Slang reports error E55205 on a specialization constant in
// `numthreads`, returns a success code, and emits `@workgroup_size(1, 1, 1)`. The cook must fail with
// CodeGenerationFailed, and must not ship that wrong size.
[SpecializationConstant]
const uint GroupSize = 64;

RWStructuredBuffer<uint> Output;

[shader("compute")]
[numthreads(GroupSize, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    Output[tid.x] = tid.x;
}
