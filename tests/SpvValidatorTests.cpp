#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "model/ShaderDataSchema.hpp"
#include "ShaderLibraryTypes.hpp"
#include "target/SpvValidator.hpp"
#include "target/TargetProfile.hpp"
#include "TestHarness.hpp"

#include "spirv-tools/libspirv.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The SPIR-V twin of WgslValidatorTest. The cross-check compares reflection against what the emitted SPIR-V
// declares. The emitted module decides the set and the binding. Reflection decides the kind, the shape, and
// the access.
//
// The module is text: SPIR-V assembly, assembled in the test with the SPIRV-Tools assembler. It is Slang's
// own output for `ShadeCS` in tests/assets/EntryPointParams.slang (spirv-dis, 2026-09-28), with the function
// body kept, so spirv-val accepts it. Each row edits one word of the text or one field of the reflection, so
// each row changes one fact against a known match.

using lodestone::BindingComparison;
using lodestone::BindingKind;
using lodestone::CookError;
using lodestone::CookResult;
using lodestone::ReflectedBinding;
using lodestone::ResourceAccess;
using lodestone::ResourceShape;
using lodestone::SpvValidator;

namespace
{

// The interface list holds a builtin (gl_GlobalInvocationID), which has no DescriptorSet. The validator must
// skip it, not read it as set 0 binding 0.
constexpr std::string_view k_ShadeCs = R"(
               OpCapability Shader
               OpExtension "SPV_KHR_storage_buffer_storage_class"
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %ShadeCS "main" %Output %entryPointParams_albedoMap %entryPointParams_albedoSampler %gl_GlobalInvocationID
               OpExecutionMode %ShadeCS LocalSize 64 1 1
               OpSource Slang 1
               OpName %uv "uv"
               OpName %RWStructuredBuffer "RWStructuredBuffer"
               OpMemberName %RWStructuredBuffer 0 "__member0"
               OpName %Output "Output"
               OpName %entryPointParams_albedoMap "entryPointParams.albedoMap"
               OpName %entryPointParams_albedoSampler "entryPointParams.albedoSampler"
               OpName %__sampled "__sampled"
               OpName %ShadeCS "ShadeCS"
               OpDecorate %gl_GlobalInvocationID BuiltIn GlobalInvocationId
               OpDecorate %_ptr_StorageBuffer_v4float ArrayStride 16
               OpDecorate %_runtimearr_v4float ArrayStride 16
               OpDecorate %RWStructuredBuffer Block
               OpMemberDecorate %RWStructuredBuffer 0 Offset 0
               OpDecorate %Output Binding 0
               OpDecorate %Output DescriptorSet 0
               OpDecorate %entryPointParams_albedoMap Binding 1
               OpDecorate %entryPointParams_albedoMap DescriptorSet 0
               OpDecorate %entryPointParams_albedoSampler Binding 2
               OpDecorate %entryPointParams_albedoSampler DescriptorSet 0
       %void = OpTypeVoid
          %3 = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %v3uint = OpTypeVector %uint 3
%_ptr_Input_v3uint = OpTypePointer Input %v3uint
     %v2uint = OpTypeVector %uint 2
      %float = OpTypeFloat 32
    %v2float = OpTypeVector %float 2
%float_0_00390625 = OpConstant %float 0.00390625
        %int = OpTypeInt 32 1
      %int_0 = OpConstant %int 0
    %v4float = OpTypeVector %float 4
%_ptr_StorageBuffer_v4float = OpTypePointer StorageBuffer %v4float
%_runtimearr_v4float = OpTypeRuntimeArray %v4float
%RWStructuredBuffer = OpTypeStruct %_runtimearr_v4float
%_ptr_StorageBuffer_RWStructuredBuffer = OpTypePointer StorageBuffer %RWStructuredBuffer
         %28 = OpTypeImage %float 2D 2 0 0 1 Unknown
%_ptr_UniformConstant_28 = OpTypePointer UniformConstant %28
         %32 = OpTypeSampler
%_ptr_UniformConstant_32 = OpTypePointer UniformConstant %32
         %36 = OpTypeSampledImage %28
    %float_0 = OpConstant %float 0
%gl_GlobalInvocationID = OpVariable %_ptr_Input_v3uint Input
     %Output = OpVariable %_ptr_StorageBuffer_RWStructuredBuffer StorageBuffer
%entryPointParams_albedoMap = OpVariable %_ptr_UniformConstant_28 UniformConstant
%entryPointParams_albedoSampler = OpVariable %_ptr_UniformConstant_32 UniformConstant
    %ShadeCS = OpFunction %void None %3
          %4 = OpLabel
          %7 = OpLoad %v3uint %gl_GlobalInvocationID
         %11 = OpVectorShuffle %v2uint %7 %7 0 1
         %14 = OpConvertUToF %v2float %11
         %uv = OpVectorTimesScalar %v2float %14 %float_0_00390625
         %17 = OpLoad %v3uint %gl_GlobalInvocationID
         %18 = OpCompositeExtract %uint %17 0
         %23 = OpAccessChain %_ptr_StorageBuffer_v4float %Output %int_0 %18
         %29 = OpLoad %28 %entryPointParams_albedoMap
         %33 = OpLoad %32 %entryPointParams_albedoSampler
         %37 = OpSampledImage %36 %29 %33
  %__sampled = OpImageSampleExplicitLod %v4float %37 %uv Lod %float_0
         %40 = OpCopyObject %v4float %__sampled
               OpStore %23 %40
               OpReturn
               OpFunctionEnd
)";

/** The text with one piece replaced. The piece must occur once, or the row would edit something else. */
std::string Replace(std::string_view text, std::string_view from, std::string_view to)
{
    std::string result{ text };
    const size_t at = result.find(from);
    if ((at != std::string::npos) && (result.find(from, at + 1u) == std::string::npos))
    {
        result.replace(at, from.size(), to);
        return result;
    }

    return {};
}

/** Assembles SPIR-V text into the bytes a cook stores. Empty when the text does not assemble. */
std::vector<std::byte> Assemble(std::string_view text)
{
    spv_context context = spvContextCreate(SPV_ENV_VULKAN_1_2);
    spv_binary binary = nullptr;
    spv_diagnostic diagnostic = nullptr;
    const spv_result_t assembled = spvTextToBinary(context, text.data(), text.size(), &binary, &diagnostic);
    std::vector<std::byte> bytes;
    if ((assembled == SPV_SUCCESS) && (binary != nullptr))
    {
        bytes.resize(binary->wordCount * sizeof(uint32_t));
        std::memcpy(bytes.data(), binary->code, bytes.size());
    }

    spvBinaryDestroy(binary);
    spvDiagnosticDestroy(diagnostic);
    spvContextDestroy(context);
    return bytes;
}

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

/** What Slang's reflection says about `ShadeCS`. The scope is `entryPointParams` on the two parameters. */
std::vector<ReflectedBinding> MakeAgreeingReflection()
{
    std::vector<ReflectedBinding> reflected;
    reflected.push_back(MakeReflected("Output", 0u, 0u, BindingKind::StorageBuffer, ResourceShape::StructuredBuffer,
                                      ResourceAccess::ReadWrite));
    reflected.push_back(MakeReflected("albedoMap", 0u, 1u, BindingKind::Texture, ResourceShape::Texture2D,
                                      ResourceAccess::ReadOnly));
    reflected.push_back(MakeReflected("albedoSampler", 0u, 2u, BindingKind::Sampler, ResourceShape::Invalid,
                                      ResourceAccess::ReadOnly));
    reflected[1].ScopeName = "entryPointParams";
    reflected[2].ScopeName = "entryPointParams";
    return reflected;
}

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

/** True when the comparison ran and reports a mismatch. */
bool Mismatches(const CookResult<BindingComparison>& comparison)
{
    return comparison.has_value() && !comparison->Matches && !comparison->Report.empty();
}

} // namespace

int main()
{
    lodestone::tests::TestRunner runner{ "SpvValidatorTests" };

    const SpvValidator validator;
    lodestone::StderrDiagnosticSink sink;
    const std::vector<std::byte> shadeCs = Assemble(k_ShadeCs);
    runner.Check(!shadeCs.empty(), "the fixture assembles");
    if (shadeCs.empty())
    {
        return runner.Report();
    }

    const std::vector<ReflectedBinding> agreeing = MakeAgreeingReflection();
    std::vector<const ReflectedBinding*> agreeingPointers = PointersTo(agreeing);

    runner.BeginSection("agreeing reflection passes the cross-check");
    const CookResult<BindingComparison> match = validator.ValidateEntryPoint(shadeCs, agreeingPointers, sink);
    runner.Check(match.has_value(), "valid SPIR-V and agreeing reflection produce a result, not an error");
    runner.Check(match.has_value() && match->Matches, "reflection that agrees with the module passes");
    runner.Check(match.has_value() && match->Report.empty(), "a passing comparison reports nothing");
    runner.Check(match.has_value() && std::ranges::contains(match->Capabilities, std::string{ "Shader" }),
                 "the capabilities of the module read back");

    runner.BeginSection("the module decides the set and the binding");
    const std::vector<std::byte> movedTexture =
        Assemble(Replace(k_ShadeCs, "%entryPointParams_albedoMap Binding 1", "%entryPointParams_albedoMap Binding 7"));
    const CookResult<BindingComparison> moved = validator.ValidateEntryPoint(movedTexture, agreeingPointers, sink);
    runner.Check(Mismatches(moved), "a texture at another binding than reflection states is a mismatch");

    std::vector<ReflectedBinding> missing = agreeing;
    missing.pop_back();
    std::vector<const ReflectedBinding*> missingPointers = PointersTo(missing);
    runner.Check(Mismatches(validator.ValidateEntryPoint(shadeCs, missingPointers, sink)),
                 "a binding the module declares and reflection lacks is a mismatch");

    std::vector<ReflectedBinding> extra = agreeing;
    extra.push_back(MakeReflected("Unused", 3u, 0u, BindingKind::Sampler, ResourceShape::Invalid,
                                  ResourceAccess::ReadOnly));
    std::vector<const ReflectedBinding*> extraPointers = PointersTo(extra);
    runner.Check(Mismatches(validator.ValidateEntryPoint(shadeCs, extraPointers, sink)),
                 "a binding reflection states and the module lacks is a mismatch");

    runner.BeginSection("reflection decides the kind, the shape, and the access");
    std::vector<ReflectedBinding> wrongKind = agreeing;
    wrongKind[1].Kind = BindingKind::Sampler;
    std::vector<const ReflectedBinding*> wrongKindPointers = PointersTo(wrongKind);
    runner.Check(Mismatches(validator.ValidateEntryPoint(shadeCs, wrongKindPointers, sink)),
                 "a texture that reflection calls a sampler is a mismatch");

    std::vector<ReflectedBinding> wrongAccess = agreeing;
    wrongAccess[0].Access = ResourceAccess::ReadOnly;
    std::vector<const ReflectedBinding*> wrongAccessPointers = PointersTo(wrongAccess);
    runner.Check(Mismatches(validator.ValidateEntryPoint(shadeCs, wrongAccessPointers, sink)),
                 "a read-write storage buffer that reflection calls read-only is a mismatch");

    const std::vector<std::byte> depthTexture =
        Assemble(Replace(k_ShadeCs, "OpTypeImage %float 2D 2 0 0 1", "OpTypeImage %float 2D 1 0 0 1"));
    runner.Check(Mismatches(validator.ValidateEntryPoint(depthTexture, agreeingPointers, sink)),
                 "a depth texture (Depth 1) against a plain one is a shape mismatch");

    std::vector<ReflectedBinding> wrongName = agreeing;
    wrongName[1].Name = "normalMap";
    std::vector<const ReflectedBinding*> wrongNamePointers = PointersTo(wrongName);
    runner.Check(Mismatches(validator.ValidateEntryPoint(shadeCs, wrongNamePointers, sink)),
                 "a resource of another name at the right slot is a mismatch");

    runner.BeginSection("an illegal module is an error, not a mismatch");
    // Every module needs exactly one OpMemoryModel. Without it the text still assembles, and spirv-val
    // rejects it.
    const std::vector<std::byte> noMemoryModel = Assemble(Replace(k_ShadeCs, "OpMemoryModel Logical GLSL450", ""));
    runner.Check(!noMemoryModel.empty(), "the illegal fixture assembles");
    const CookResult<BindingComparison> illegal = validator.ValidateEntryPoint(noMemoryModel, agreeingPointers, sink);
    runner.Check(!illegal.has_value() && (illegal.error() == CookError::TargetValidationEntryPointInvalid),
                 "spirv-val rejects the module and the validator returns TargetValidationEntryPointInvalid");

    const std::vector<std::byte> partialWord(shadeCs.begin(), shadeCs.end() - 1);
    const CookResult<BindingComparison> partial = validator.ValidateEntryPoint(partialWord, agreeingPointers, sink);
    runner.Check(!partial.has_value() && (partial.error() == CookError::TargetValidationEntryPointParseFailed),
                 "code that is not whole words is a parse failure");

    return runner.Report();
}
