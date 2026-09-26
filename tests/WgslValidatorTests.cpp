#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "model/ShaderDataSchema.hpp"
#include "ShaderLibraryTypes.hpp"
#include "target/TargetProfile.hpp"
#include "TestHarness.hpp"
#include "target/WgslValidator.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The cross-check compares what reflection reports against what the emitted WGSL actually declares.
// It is the check that stops a wrong shader from reaching a pipeline. `WgslValidator` parses the
// text with Tint, reads the used bindings from Tint's inspector, and compares them against the
// reflected bindings this file writes by hand.
//
// The asymmetry matters: the emitted artifact decides the group and the binding number, and
// reflection decides the kind, the shape, and the access. A test that fed the same source to both
// sides would prove nothing, so the WGSL text here is fixed and the reflection records are separate.
//
// Tint reports only the bindings an entry point uses, so `MainCS` references every resource. A
// resource the entry point never reads leaves the used set, and then an agreeing reflection record
// reads as a binding the WGSL never declared.

using lodestone::BindingComparison;
using lodestone::BindingKind;
using lodestone::CookResult;
using lodestone::ReflectedBinding;
using lodestone::ResourceAccess;
using lodestone::ResourceShape;
using lodestone::TargetProfile;
using lodestone::WgslValidator;

namespace
{

// Written the way Slang emits it, mangled suffixes included. `MainCS` reads or writes every
// resource, so Tint's inspector reports all eight in the used set.
constexpr std::string_view k_Wgsl = R"(
struct IfftParams_Std140_0 {
    count_0 : u32,
};

@group(0) @binding(0) var<uniform> IfftParams_0 : IfftParams_Std140_0;

@group(0) @binding(1) var<storage, read> InputSpectrum_0 : array<vec4<f32>>;

@group(0) @binding(2) var<storage, read_write> OutputSpectrum_0 : array<vec4<f32>>;

@group(1) @binding(0) var HeightTexture_0 : texture_2d<f32>;

@group(1) @binding(1) var LinearSampler_0 : sampler;

@group(2) @binding(0) var<storage> DefaultAccess_0 : array<u32>;

@group(2) @binding(1) var ShadowedTexture_0 : texture_depth_2d_array;
@group(3) @binding(2) var ShadowSampler_0 : sampler_comparison;

@compute @workgroup_size(64, 1, 1)
fn MainCS(@builtin(global_invocation_id) id : vec3<u32>)
{
    let idx = id.x;
    let uv = vec2<f32>(f32(idx), 0.0);
    let params = f32(IfftParams_0.count_0);
    let inSample = InputSpectrum_0[idx];
    let height = textureSampleLevel(HeightTexture_0, LinearSampler_0, uv, 0.0);
    let shadow = textureSampleCompareLevel(ShadowedTexture_0, ShadowSampler_0, uv, 0i, 0.5);
    let raw = f32(DefaultAccess_0[idx]);
    OutputSpectrum_0[idx] = inSample + vec4<f32>(params + height.x + shadow + raw, 0.0, 0.0, 0.0);
}
)";

ReflectedBinding MakeReflected(std::string_view name,
                               uint32_t group,
                               uint32_t binding,
                               BindingKind kind,
                               ResourceShape shape,
                               ResourceAccess access)
{
    ReflectedBinding reflected;
    reflected.Name = std::string{ name };
    reflected.Placement = lodestone::BoundPlacement{ .Group = group, .Binding = binding };
    reflected.Kind = kind;
    reflected.Shape = shape;
    reflected.Access = access;
    return reflected;
}

ResourceShape MakeTextureShape(ResourceShape baseShape, bool isArray, bool isShadow)
{
    if (isArray)
    {
        baseShape |= ResourceShape::ArrayFlag;
    }

    if (isShadow)
    {
        baseShape |= ResourceShape::ShadowFlag;
    }

    return baseShape;
}

ResourceShape MakeBufferShape(bool is_structured)
{
    return is_structured ? ResourceShape::StructuredBuffer : ResourceShape::ByteAddressBuffer;
}

// The reflection that agrees with `k_Wgsl` on every used binding. A sampler carries no shape or
// access, so it takes `Invalid` for both; the validator reads neither for a sampler.
std::vector<ReflectedBinding> MakeAgreeingReflection()
{
    std::vector<ReflectedBinding> reflected;
    reflected.push_back(MakeReflected("IfftParams", 0u, 0u, BindingKind::UniformBuffer,
                                      ResourceShape::Invalid, ResourceAccess::ReadOnly));
    // A float4 storage array is a raw buffer, not a structured one. The distinction does not reach
    // the WGSL kind either way; see the orthogonality section below.
    reflected.push_back(MakeReflected("InputSpectrum", 0u, 1u, BindingKind::StorageBuffer,
                                      MakeBufferShape(false), ResourceAccess::ReadOnly));
    reflected.push_back(MakeReflected("OutputSpectrum", 0u, 2u, BindingKind::StorageBuffer,
                                      MakeBufferShape(false), ResourceAccess::ReadWrite));
    reflected.push_back(MakeReflected("HeightTexture", 1u, 0u, BindingKind::Texture,
                                      MakeTextureShape(ResourceShape::Texture2D, false, false),
                                      ResourceAccess::ReadOnly));
    reflected.push_back(MakeReflected("LinearSampler", 1u, 1u, BindingKind::Sampler,
                                      ResourceShape::Invalid, ResourceAccess::ReadOnly));
    reflected.push_back(MakeReflected("DefaultAccess", 2u, 0u, BindingKind::StorageBuffer,
                                      MakeBufferShape(false), ResourceAccess::ReadOnly));
    // A depth texture: base shape plus the shadow and array flags. The shadow flag is what makes
    // the reflected kind resolve to Tint's `kDepthTexture`.
    reflected.push_back(MakeReflected("ShadowedTexture", 2u, 1u, BindingKind::Texture,
                                      MakeTextureShape(ResourceShape::Texture2D, true, true),
                                      ResourceAccess::ReadOnly));
    ReflectedBinding shadowSampler = MakeReflected("ShadowSampler", 3u, 2u, BindingKind::Sampler,
                                                   ResourceShape::Invalid, ResourceAccess::ReadOnly);
    // A comparison sampler is a reflection fact the CPU consumes. Tint's inspector reports every
    // sampler as `kSampler`, comparison or not, so the validator cannot and does not cross-check it.
    shadowSampler.IsComparisonSampler = true;
    reflected.push_back(shadowSampler);
    return reflected;
}

// The validator takes a span of pointers, so each caller keeps its records in a named vector and
// hands out pointers into it. The span is copied before it is sorted, so one vector serves many
// calls.
std::vector<const ReflectedBinding*> PointersTo(const std::vector<ReflectedBinding>& reflected)
{
    std::vector<const ReflectedBinding*> pointers;
    pointers.reserve(reflected.size());
    for (const ReflectedBinding& binding : reflected)
    {
        pointers.push_back(&binding);
    }

    return pointers;
}

} // namespace

int main()
{
    lodestone::tests::TestRunner runner{ "WgslValidatorTests" };

    const WgslValidator validator;
    lodestone::StderrDiagnosticSink sink;

    runner.BeginSection("agreeing reflection passes the cross-check");
    const std::vector<ReflectedBinding> agreeing = MakeAgreeingReflection();
    std::vector<const ReflectedBinding*> agreeingPointers = PointersTo(agreeing);
    const CookResult<BindingComparison> match = validator.ValidateEntryPoint(std::as_bytes(std::span{ k_Wgsl }), agreeingPointers, sink);
    runner.Check(match.has_value(), "valid WGSL and agreeing reflection produce a result, not an error");
    runner.Check(match.has_value() && match->Matches, "reflection that agrees with the emitted text passes");
    runner.Check(match.has_value() && match->Report.empty(), "a passing comparison reports nothing");

    runner.BeginSection("a kind mismatch fails the cross-check");
    // A uniform block that reflection calls a storage buffer still emits valid WGSL. WebGPU rejects
    // the bind group layout at run time, so this cross-check is the only place the error is findable.
    std::vector<ReflectedBinding> wrongKind = MakeAgreeingReflection();
    wrongKind[0].Kind = BindingKind::StorageBuffer;
    std::vector<const ReflectedBinding*> wrongKindPointers = PointersTo(wrongKind);
    const CookResult<BindingComparison> kindMismatch =
        validator.ValidateEntryPoint(std::as_bytes(std::span{ k_Wgsl }), wrongKindPointers, sink);
    runner.Check(kindMismatch.has_value() && !kindMismatch->Matches,
                 "a uniform declared as a storage buffer fails");
    runner.Check(kindMismatch.has_value() && !kindMismatch->Report.empty(),
                 "a failing comparison says what disagreed");

    runner.BeginSection("an access mismatch fails the cross-check");
    // `OutputSpectrum` is `read_write` in the WGSL, so reflection calling it read-only makes the
    // reflected kind resolve to `kReadOnlyStorageBuffer` against Tint's `kStorageBuffer`.
    std::vector<ReflectedBinding> wrongAccess = MakeAgreeingReflection();
    wrongAccess[2].Access = ResourceAccess::ReadOnly;
    std::vector<const ReflectedBinding*> wrongAccessPointers = PointersTo(wrongAccess);
    const CookResult<BindingComparison> accessMismatch =
        validator.ValidateEntryPoint(std::as_bytes(std::span{ k_Wgsl }), wrongAccessPointers, sink);
    runner.Check(accessMismatch.has_value() && !accessMismatch->Matches,
                 "a read-write storage buffer reflected as read-only fails");

    runner.BeginSection("the shadow flag distinguishes a depth texture");
    // `ShadowedTexture` is a `texture_depth_2d_array`. Dropping the shadow flag makes the reflected
    // kind resolve to a plain sampled texture, which disagrees with Tint's `kDepthTexture`.
    std::vector<ReflectedBinding> notDepth = MakeAgreeingReflection();
    notDepth[6].Shape = MakeTextureShape(ResourceShape::Texture2D, true, false);
    std::vector<const ReflectedBinding*> notDepthPointers = PointersTo(notDepth);
    const CookResult<BindingComparison> depthMismatch =
        validator.ValidateEntryPoint(std::as_bytes(std::span{ k_Wgsl }), notDepthPointers, sink);
    runner.Check(depthMismatch.has_value() && !depthMismatch->Matches,
                 "a depth texture reflected without the shadow flag fails");

    runner.BeginSection("storage-buffer shape is orthogonal to the wgsl kind");
    // WGSL has no structured/raw distinction: a storage buffer's Tint kind follows from the access
    // alone. So flipping the reflected shape from raw to structured leaves the cross-check passing.
    // This answers whether a `float4` buffer must be "structured" to validate: it need not be.
    std::vector<ReflectedBinding> structured = MakeAgreeingReflection();
    structured[1].Shape = MakeBufferShape(true);
    std::vector<const ReflectedBinding*> structuredPointers = PointersTo(structured);
    const CookResult<BindingComparison> structuredResult =
        validator.ValidateEntryPoint(std::as_bytes(std::span{ k_Wgsl }), structuredPointers, sink);
    runner.Check(structuredResult.has_value() && structuredResult->Matches,
                 "a storage buffer reflected as structured still matches, because the shape is not a wgsl kind");

    runner.BeginSection("a name mismatch fails the cross-check");
    std::vector<ReflectedBinding> wrongName = MakeAgreeingReflection();
    wrongName[1].Name = "SomeOtherBuffer";
    std::vector<const ReflectedBinding*> wrongNamePointers = PointersTo(wrongName);
    const CookResult<BindingComparison> nameMismatch =
        validator.ValidateEntryPoint(std::as_bytes(std::span{ k_Wgsl }), wrongNamePointers, sink);
    runner.Check(nameMismatch.has_value() && !nameMismatch->Matches,
                 "a binding whose name disagrees fails");

    runner.BeginSection("a binding that only one side knows fails the cross-check");
    std::vector<ReflectedBinding> missing = MakeAgreeingReflection();
    missing.pop_back();
    std::vector<const ReflectedBinding*> missingPointers = PointersTo(missing);
    const CookResult<BindingComparison> missingBinding =
        validator.ValidateEntryPoint(std::as_bytes(std::span{ k_Wgsl }), missingPointers, sink);
    runner.Check(missingBinding.has_value() && !missingBinding->Matches,
                 "a wgsl declaration with no reflection record fails");

    std::vector<ReflectedBinding> extra = MakeAgreeingReflection();
    extra.push_back(MakeReflected("GhostBuffer", 4u, 0u, BindingKind::StorageBuffer,
                                  MakeBufferShape(false), ResourceAccess::ReadOnly));
    std::vector<const ReflectedBinding*> extraPointers = PointersTo(extra);
    const CookResult<BindingComparison> extraBinding =
        validator.ValidateEntryPoint(std::as_bytes(std::span{ k_Wgsl }), extraPointers, sink);
    runner.Check(extraBinding.has_value() && !extraBinding->Matches,
                 "a reflection record the wgsl never declares fails");

    runner.BeginSection("invalid wgsl is a parse error, not a mismatch");
    // A parse failure is a different outcome from a mismatch: the validator returns an error, not a
    // `BindingComparison`. The sink prints the Tint diagnostics on purpose.
    constexpr std::string_view k_Broken = "this is not valid wgsl @group";
    const CookResult<BindingComparison> broken = validator.ValidateEntryPoint(std::as_bytes(std::span{ k_Broken }), agreeingPointers, sink);
    runner.Check(!broken.has_value(), "source Tint cannot parse produces an error");
    runner.Check(!broken.has_value() && broken.error() == lodestone::CookError::TargetValidationEntryPointParseFailed,
                 "the error names the parse failure");

    // The seam must not change the answer: `FindTargetProfile` has to hand back a real `WgslValidator`,
    // and reaching the validator through the profile pointer gives the same result as calling one
    // built here directly. If these disagree, the profile wired up something with a different opinion.
    runner.BeginSection("the target profile supplies a working validator");
    const CookResult<TargetProfile> wgslProfile = lodestone::FindTargetProfile("wgsl");
    runner.Check(wgslProfile.has_value(), "the build has a wgsl profile");
    runner.Check(wgslProfile.has_value() && wgslProfile->Access == lodestone::AccessModel::Bound,
                 "wgsl places a resource by group and binding");
    runner.Check(wgslProfile.has_value() && wgslProfile->Validator != nullptr,
                 "wgsl can read its own output, so it supplies a validator");
    runner.Check(!lodestone::FindTargetProfile("hlsl"),
                 "a target this build does not have resolves to nothing");

    if (wgslProfile.has_value() && wgslProfile->Validator != nullptr)
    {
        const CookResult<BindingComparison> throughProfile =
            wgslProfile->Validator->ValidateEntryPoint(std::as_bytes(std::span{ k_Wgsl }), agreeingPointers, sink);
        runner.Check(throughProfile.has_value() && match.has_value() &&
                         throughProfile->Matches == match->Matches,
                     "the profile validator agrees with a direct one on output that matches");

        const CookResult<BindingComparison> mismatchThroughProfile =
            wgslProfile->Validator->ValidateEntryPoint(std::as_bytes(std::span{ k_Wgsl }), extraPointers, sink);
        runner.Check(mismatchThroughProfile.has_value() && extraBinding.has_value() &&
                         mismatchThroughProfile->Matches == extraBinding->Matches &&
                         mismatchThroughProfile->Report == extraBinding->Report,
                     "the profile validator agrees with a direct one on output that does not, report included");
    }

    return runner.Report();
}
