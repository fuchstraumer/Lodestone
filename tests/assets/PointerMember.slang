module PointerMember;

// A probe for phase E step E0c. This module must FAIL the cook.
//
// Slang accepts a pointer inside a host shareable structure and emits it for WGSL. The emitted text
// declares `ptr<, PointLight_0>` with an empty address space, and no WGSL implementation accepts
// that form. Slang returns success, so the defect leaves the compiler unreported.
//
// The four validators cannot find it. Each one compares the cooker against itself.
// `WgslBindingScanner` reads `@group` and `@binding`, and the bad declaration carries neither. Every
// other binding agrees with reflection, so the cook exits 0 and writes invalid output.
//
// The check that finds it is in stage 3. Slang states the type kind, and the target profile states
// the access model. A pointer under a bound access model fails the cook and names the member.
//
// The entry point reads `Points` and `Count`, so neither field is removed before reflection.
//
// `Lights` is a plain `ConstantBuffer`, and not a push constant. Slang reports a push constant as
// `BindingType::PushConstant`, `FromSlangBindingType` has no row for it, and the walk then stops at
// an invalid binding kind before it reads any member. A constant buffer maps to
// `BindingKind::UniformBuffer`, which is the branch that reads the members.

struct PointLight
{
    float3 Position;
    float Radius;
    float3 Color;
    float Intensity;
};

struct Lights
{
    PointLight* Points;
    uint Count;
};

ConstantBuffer<Lights> LightData;

RWStructuredBuffer<float4> Output;

[shader("compute")]
[numthreads(64, 1, 1)]
void ShadeCS(uint3 threadId: SV_DispatchThreadID)
{
    float3 total = float3(0.0f);
    for (uint i = 0u; i < LightData.Count; ++i)
    {
        total += LightData.Points[i].Color * LightData.Points[i].Intensity;
    }

    Output[threadId.x] = float4(total, 1.0f);
}
