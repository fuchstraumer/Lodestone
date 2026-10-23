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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Slang's own output for `ShadeCS` in EntryPointParams.slang, as assembly. Each row changes one fact.

using lodestone::BindingComparison;
using lodestone::BindingKind;
using lodestone::CookError;
using lodestone::CookResult;
using lodestone::EmittedCodeFacts;
using lodestone::ReflectedBinding;
using lodestone::ResourceAccess;
using lodestone::ResourceShape;
using lodestone::SamplerTexturePlacement;
using lodestone::SpvValidator;
using lodestone::TextureFormat;

namespace
{

// The interface list contains a builtin (gl_GlobalInvocationID), which has no DescriptorSet. The validator must
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

// Slang's own output for EntryPointConstantsTooLarge.slang: the entry point constants as a push constant block of
// two float4x4 and a float4. The block covers 64 + 64 + 16 = 144 bytes.
constexpr std::string_view k_LargeCs = R"(
               OpCapability Shader
               OpExtension "SPV_KHR_storage_buffer_storage_class"
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %LargeCS "main" %Output %entryPointParams %gl_GlobalInvocationID
               OpExecutionMode %LargeCS LocalSize 64 1 1
               OpSource Slang 1
               OpName %RWStructuredBuffer "RWStructuredBuffer"
               OpMemberName %RWStructuredBuffer 0 "__member0"
               OpName %Output "Output"
               OpName %EntryPointParams_std430 "EntryPointParams_std430"
               OpMemberName %EntryPointParams_std430 0 "view"
               OpMemberName %EntryPointParams_std430 1 "projection"
               OpMemberName %EntryPointParams_std430 2 "tint"
               OpName %entryPointParams "entryPointParams"
               OpName %LargeCS "LargeCS"
               OpDecorate %gl_GlobalInvocationID BuiltIn GlobalInvocationId
               OpDecorate %_ptr_StorageBuffer_v4float ArrayStride 16
               OpDecorate %_runtimearr_v4float ArrayStride 16
               OpDecorate %RWStructuredBuffer Block
               OpMemberDecorate %RWStructuredBuffer 0 Offset 0
               OpDecorate %Output Binding 0
               OpDecorate %Output DescriptorSet 0
               OpDecorate %EntryPointParams_std430 Block
               OpMemberDecorate %EntryPointParams_std430 0 Offset 0
               OpMemberDecorate %EntryPointParams_std430 0 RowMajor
               OpMemberDecorate %EntryPointParams_std430 0 MatrixStride 16
               OpMemberDecorate %EntryPointParams_std430 1 Offset 64
               OpMemberDecorate %EntryPointParams_std430 1 RowMajor
               OpMemberDecorate %EntryPointParams_std430 1 MatrixStride 16
               OpMemberDecorate %EntryPointParams_std430 2 Offset 128
       %void = OpTypeVoid
          %3 = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %v3uint = OpTypeVector %uint 3
%_ptr_Input_v3uint = OpTypePointer Input %v3uint
        %int = OpTypeInt 32 1
      %int_0 = OpConstant %int 0
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
%_ptr_StorageBuffer_v4float = OpTypePointer StorageBuffer %v4float
%_runtimearr_v4float = OpTypeRuntimeArray %v4float
%RWStructuredBuffer = OpTypeStruct %_runtimearr_v4float
%_ptr_StorageBuffer_RWStructuredBuffer = OpTypePointer StorageBuffer %RWStructuredBuffer
%mat4v4float = OpTypeMatrix %v4float 4
%EntryPointParams_std430 = OpTypeStruct %mat4v4float %mat4v4float %v4float
%_ptr_PushConstant_EntryPointParams_std430 = OpTypePointer PushConstant %EntryPointParams_std430
%_ptr_PushConstant_mat4v4float = OpTypePointer PushConstant %mat4v4float
      %int_2 = OpConstant %int 2
%_ptr_PushConstant_v4float = OpTypePointer PushConstant %v4float
      %int_1 = OpConstant %int 1
%gl_GlobalInvocationID = OpVariable %_ptr_Input_v3uint Input
     %Output = OpVariable %_ptr_StorageBuffer_RWStructuredBuffer StorageBuffer
%entryPointParams = OpVariable %_ptr_PushConstant_EntryPointParams_std430 PushConstant
    %LargeCS = OpFunction %void None %3
          %4 = OpLabel
          %7 = OpLoad %v3uint %gl_GlobalInvocationID
         %10 = OpCompositeExtract %uint %7 0
         %16 = OpAccessChain %_ptr_StorageBuffer_v4float %Output %int_0 %10
         %26 = OpAccessChain %_ptr_PushConstant_mat4v4float %entryPointParams %int_0
         %27 = OpLoad %mat4v4float %26
         %30 = OpAccessChain %_ptr_PushConstant_v4float %entryPointParams %int_2
         %31 = OpLoad %v4float %30
         %32 = OpVectorTimesMatrix %v4float %31 %27
         %34 = OpAccessChain %_ptr_PushConstant_mat4v4float %entryPointParams %int_1
         %35 = OpLoad %mat4v4float %34
         %36 = OpVectorTimesMatrix %v4float %32 %35
               OpStore %16 %36
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

// One storage image with a declared format, so that the format has a row of its own. Sampled 2 is a storage image.
constexpr std::string_view k_StorageImageCs = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %WriteCS "main" %OutVolume
               OpExecutionMode %WriteCS LocalSize 4 4 4
               OpName %OutVolume "OutVolume"
               OpDecorate %OutVolume DescriptorSet 0
               OpDecorate %OutVolume Binding 0
       %void = OpTypeVoid
     %fnVoid = OpTypeFunction %void
      %float = OpTypeFloat 32
       %uint = OpTypeInt 32 0
     %v3uint = OpTypeVector %uint 3
    %v4float = OpTypeVector %float 4
      %image = OpTypeImage %float 3D 0 0 0 2 R32f
   %ptrImage = OpTypePointer UniformConstant %image
  %OutVolume = OpVariable %ptrImage UniformConstant
     %uint_0 = OpConstant %uint 0
    %float_0 = OpConstant %float 0
    %float_1 = OpConstant %float 1
      %coord = OpConstantComposite %v3uint %uint_0 %uint_0 %uint_0
      %texel = OpConstantComposite %v4float %float_1 %float_0 %float_0 %float_0
    %WriteCS = OpFunction %void None %fnVoid
      %entry = OpLabel
     %loaded = OpLoad %image %OutVolume
               OpImageWrite %loaded %coord %texel
               OpReturn
               OpFunctionEnd
)";

// workgroup variables that pad differently: 64 + 32 + 16 + 32 = 144, as in WgslValidatorTest
constexpr std::string_view k_WorkgroupCs = R"(
               OpCapability Shader
               OpCapability Float16
               OpCapability StorageUniform16
               OpExtension "SPV_KHR_16bit_storage"
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %MainCS "MainCS" %g_vec3 %g_struct %g_floats %g_half
               OpExecutionMode %MainCS LocalSize 64 1 1
               OpName %g_vec3 "g_vec3"
               OpName %g_struct "g_struct"
               OpName %g_floats "g_floats"
               OpName %g_half "g_half"
       %void = OpTypeVoid
     %fnVoid = OpTypeFunction %void
      %float = OpTypeFloat 32
       %half = OpTypeFloat 16
       %uint = OpTypeInt 32 0
     %uint_3 = OpConstant %uint 3
     %uint_4 = OpConstant %uint 4
    %v3float = OpTypeVector %float 3
     %v4half = OpTypeVector %half 4
    %arrVec3 = OpTypeArray %v3float %uint_4
    %structS = OpTypeStruct %float %v3float
   %arrFloat = OpTypeArray %float %uint_3
   %arrHalf4 = OpTypeArray %v4half %uint_3
    %ptrVec3 = OpTypePointer Workgroup %arrVec3
  %ptrStruct = OpTypePointer Workgroup %structS
   %ptrFloat = OpTypePointer Workgroup %arrFloat
    %ptrHalf = OpTypePointer Workgroup %arrHalf4
     %g_vec3 = OpVariable %ptrVec3 Workgroup
   %g_struct = OpVariable %ptrStruct Workgroup
   %g_floats = OpVariable %ptrFloat Workgroup
     %g_half = OpVariable %ptrHalf Workgroup
     %MainCS = OpFunction %void None %fnVoid
      %entry = OpLabel
               OpReturn
               OpFunctionEnd
)";

// An array of four storage images (a binding array), written through a constant index. Replacing `%uint_0` in the access
// chain with `%index` makes it a dynamic index. The runtime array form is the
// unbounded array of the Indexed access model, which this validator does not read.
constexpr std::string_view k_ImageArrayCs = R"(
               OpCapability Shader
               OpCapability RuntimeDescriptorArray
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %WriteCS "main" %Levels %Index
               OpExecutionMode %WriteCS LocalSize 4 4 4
               OpName %Levels "Levels"
               OpDecorate %Levels DescriptorSet 0
               OpDecorate %Levels Binding 3
       %void = OpTypeVoid
     %fnVoid = OpTypeFunction %void
      %float = OpTypeFloat 32
       %uint = OpTypeInt 32 0
     %v3uint = OpTypeVector %uint 3
    %v4float = OpTypeVector %float 4
      %image = OpTypeImage %float 3D 0 0 0 2 R32f
     %uint_4 = OpConstant %uint 4
      %array = OpTypeArray %image %uint_4
   %ptrArray = OpTypePointer UniformConstant %array
   %ptrImage = OpTypePointer UniformConstant %image
     %Levels = OpVariable %ptrArray UniformConstant
 %ptrPrivate = OpTypePointer Private %uint
      %Index = OpVariable %ptrPrivate Private
     %uint_0 = OpConstant %uint 0
    %float_0 = OpConstant %float 0
    %float_1 = OpConstant %float 1
      %coord = OpConstantComposite %v3uint %uint_0 %uint_0 %uint_0
      %texel = OpConstantComposite %v4float %float_1 %float_0 %float_0 %float_0
    %WriteCS = OpFunction %void None %fnVoid
      %entry = OpLabel
      %index = OpLoad %uint %Index
    %element = OpAccessChain %ptrImage %Levels %uint_0
     %loaded = OpLoad %image %element
               OpImageWrite %loaded %coord %texel
               OpReturn
               OpFunctionEnd
)";

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
    runner.CheckOk(match, "valid SPIR-V and agreeing reflection produce a result, not an error");
    runner.Check(match.has_value() && match->Matches, "reflection that agrees with the module passes");
    runner.Check(match.has_value() && match->Report.empty(), "a passing comparison reports nothing");
    runner.Check(match.has_value() && std::ranges::contains(match->Emitted.CapabilityNames, std::string{ "Shader" }),
                 "the capabilities of the module read back");

    // The pairs are cook output. `ShadeCS` loads the texture and the sampler, combines them with
    // OpSampledImage, and samples that, so the reader follows both loads back to their variables.
    runner.BeginSection("the sampler and texture pairs come from OpSampledImage");
    const std::vector<SamplerTexturePlacement> expectedPairs
    {
        SamplerTexturePlacement{ .TextureGroup = 0u, .TextureBinding = 1u, .SamplerGroup = 0u, .SamplerBinding = 2u },
    };
    if (runner.CheckOk(match,
                       "the validate path traces the image and the sampler of OpSampledImage to their bindings"))
    {
        runner.CheckEqual(match->Emitted.SamplerTexturePairs, expectedPairs,
                          "the validate path traces the image and the sampler of OpSampledImage to their bindings");
    }
    const CookResult<EmittedCodeFacts> readFacts = validator.ReadEmittedCodeFacts(shadeCs, sink);
    if (runner.CheckOk(readFacts, "the read path gives the same pairs, and compares nothing"))
    {
        runner.CheckEqual(readFacts->SamplerTexturePairs, expectedPairs,
                          "the read path gives the same pairs, and compares nothing");
    }

    // An image that does not come from a resource variable cannot name a binding. Dropping the pair would make
    // the sampler look unpaired, so the read fails.
    const std::string undefText = Replace(Replace(k_ShadeCs, "%37 = OpSampledImage %36 %29 %33", "%37 = OpSampledImage %36 %undefImage %33"),
                                          "    %float_0 = OpConstant %float 0",
                                          "    %float_0 = OpConstant %float 0\n%undefImage = OpUndef %28");
    const std::vector<std::byte> undefImage = Assemble(undefText);
    runner.Check(!undefImage.empty(), "the fixture with an undefined image assembles");
    const CookResult<EmittedCodeFacts> unresolved = validator.ReadEmittedCodeFacts(undefImage, sink);
    runner.CheckError(unresolved, CookError::ReflectionSamplerPairUnresolved,
                      "a sampled image that does not trace back to a variable fails with ReflectionSamplerPairUnresolved");

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

    runner.BeginSection("the storage image format is compared");
    // Slang ac945e5 reflected `RWTexture3D<float>` with no `[format]` as Invalid while the target code stated
    // r32float, and nothing failed
    const std::vector<std::byte> storageImage = Assemble(k_StorageImageCs);
    const auto storageReflection = [](TextureFormat format)
    {
        std::vector<ReflectedBinding> reflected{ MakeReflected("OutVolume", 0u, 0u, BindingKind::StorageTexture,
                                                               ResourceShape::Texture3D, ResourceAccess::ReadWrite) };
        reflected[0].StorageFormat = format;
        return reflected;
    };
    const std::vector<ReflectedBinding> formatAgrees = storageReflection(TextureFormat::R32Float);
    std::vector<const ReflectedBinding*> formatAgreesPointers = PointersTo(formatAgrees);
    const CookResult<BindingComparison> formatMatch = validator.ValidateEntryPoint(storageImage, formatAgreesPointers, sink);
    runner.Check(formatMatch.has_value() && formatMatch->Matches, "a storage image with the format of the module matches");

    const std::vector<ReflectedBinding> formatMissing = storageReflection(TextureFormat::Invalid);
    std::vector<const ReflectedBinding*> formatMissingPointers = PointersTo(formatMissing);
    runner.Check(Mismatches(validator.ValidateEntryPoint(storageImage, formatMissingPointers, sink)),
                 "a storage image that reflection reads with no format is a mismatch");

    const std::vector<std::byte> otherFormat = Assemble(Replace(k_StorageImageCs, "2 R32f", "2 Rgba32f"));
    runner.Check(Mismatches(validator.ValidateEntryPoint(otherFormat, formatAgreesPointers, sink)),
                 "a storage image that the module declares with another format is a mismatch");

    std::vector<ReflectedBinding> wrongName = agreeing;
    wrongName[1].Name = "normalMap";
    std::vector<const ReflectedBinding*> wrongNamePointers = PointersTo(wrongName);
    runner.Check(Mismatches(validator.ValidateEntryPoint(shadeCs, wrongNamePointers, sink)),
                 "a resource of another name at the right slot is a mismatch");

    runner.BeginSection("a binding array is read, and its length is compared");
    const std::vector<std::byte> imageArray = Assemble(k_ImageArrayCs);
    runner.Check(!imageArray.empty(), "the binding array fixture assembles");
    const auto arrayReflection = [](uint32_t array_count)
    {
        std::vector<ReflectedBinding> reflected{ MakeReflected("Levels", 0u, 3u, BindingKind::StorageTexture,
                                                               ResourceShape::Texture3D, ResourceAccess::ReadWrite) };
        reflected[0].StorageFormat = TextureFormat::R32Float;
        reflected[0].ArrayCount = array_count;
        return reflected;
    };
    const std::vector<ReflectedBinding> fourLevels = arrayReflection(4u);
    std::vector<const ReflectedBinding*> fourLevelsPointers = PointersTo(fourLevels);
    const CookResult<BindingComparison> arrayMatch = validator.ValidateEntryPoint(imageArray, fourLevelsPointers, sink);
    runner.Check(arrayMatch.has_value() && arrayMatch->Matches, "an array of four images matches reflection with four");

    const std::vector<ReflectedBinding> oneLevel = arrayReflection(1u);
    std::vector<const ReflectedBinding*> oneLevelPointers = PointersTo(oneLevel);
    runner.Check(Mismatches(validator.ValidateEntryPoint(imageArray, oneLevelPointers, sink)),
                 "an array that reflection reads as one binding is a mismatch");
    const std::vector<ReflectedBinding> threeLevels = arrayReflection(3u);
    std::vector<const ReflectedBinding*> threeLevelsPointers = PointersTo(threeLevels);
    runner.Check(Mismatches(validator.ValidateEntryPoint(imageArray, threeLevelsPointers, sink)),
                 "an array of another length is a mismatch");

    const std::vector<std::byte> unbounded =
        Assemble(Replace(k_ImageArrayCs, "OpTypeArray %image %uint_4", "OpTypeRuntimeArray %image"));
    const CookResult<BindingComparison> unboundedResult = validator.ValidateEntryPoint(unbounded, fourLevelsPointers, sink);
    runner.Check(Mismatches(unboundedResult) && unboundedResult->Report.find("unbounded") != std::string::npos,
                 "an unbounded array is reported as unsupported until the Indexed access model reads it");

    runner.BeginSection("an illegal module is an error, not a mismatch");
    // Every module needs exactly one OpMemoryModel. Without it the text still assembles, and spirv-val
    // rejects it.
    const std::vector<std::byte> noMemoryModel = Assemble(Replace(k_ShadeCs, "OpMemoryModel Logical GLSL450", ""));
    runner.Check(!noMemoryModel.empty(), "the illegal fixture assembles");
    const CookResult<BindingComparison> illegal = validator.ValidateEntryPoint(noMemoryModel, agreeingPointers, sink);
    runner.CheckError(illegal, CookError::TargetValidationEntryPointInvalid,
                      "spirv-val rejects the module and the validator returns TargetValidationEntryPointInvalid");

    const std::vector<std::byte> partialWord(shadeCs.begin(), shadeCs.end() - 1);
    const CookResult<BindingComparison> partial = validator.ValidateEntryPoint(partialWord, agreeingPointers, sink);
    runner.CheckError(partial, CookError::TargetValidationEntryPointParseFailed,
                      "code that is not whole words is a parse failure");

    runner.BeginSection("the push constant extent is the end of the last member, from the decorations");
    const CookResult<EmittedCodeFacts> noPush = validator.ReadEmittedCodeFacts(shadeCs, sink);
    runner.Check(noPush.has_value() && !noPush->PushConstantExtent.has_value(),
                 "a module with no push constant variable has no extent");

    auto extentOf = [&](const std::string& text) -> std::optional<uint32_t>
    {
        const std::vector<std::byte> code = Assemble(text);
        const CookResult<EmittedCodeFacts> facts = validator.ReadEmittedCodeFacts(code, sink);
        return facts.has_value() ? facts->PushConstantExtent : std::nullopt;
    };
    const std::string large{ k_LargeCs };
    runner.CheckEqual(extentOf(large), 144u, "Slang's block of two float4x4 and a float4 covers 144 bytes");
    runner.CheckEqual(extentOf(Replace(large, "%EntryPointParams_std430 2 Offset 128", "%EntryPointParams_std430 2 Offset 200")),
                      216u,
                      "the extent reads the member offsets, so a gap before the last member counts");
    // 2 columns of float4: RowMajor 3 * 16 + 2 * 4 = 56, ColMajor 1 * 16 + 4 * 4 = 32
    const std::string twoColumns = Replace(Replace(large, "%mat4v4float = OpTypeMatrix %v4float 4", "%mat4v4float = OpTypeMatrix %v4float 2"),
                                           "%EntryPointParams_std430 2 Offset 128",
                                           "%EntryPointParams_std430 2 Offset 0");
    runner.CheckEqual(extentOf(twoColumns), 64u + 56u, "a RowMajor matrix strides its rows");
    runner.CheckEqual(extentOf(Replace(twoColumns, "%EntryPointParams_std430 1 RowMajor", "%EntryPointParams_std430 1 ColMajor")),
                      64u + 32u,
                      "a ColMajor matrix strides its columns");

    runner.BeginSection("the capability names and the workgroup memory come from the module");
    const std::vector<std::byte> workgroupCs = Assemble(k_WorkgroupCs);
    runner.Check(!workgroupCs.empty(), "the workgroup fixture assembles");
    const CookResult<EmittedCodeFacts> workgroupFacts = validator.ReadEmittedCodeFacts(workgroupCs, sink);
    runner.CheckOk(workgroupFacts, "the read path reads the workgroup fixture");
    if (runner.CheckOk(workgroupFacts,
                       "each OpCapability and OpExtension reads back as a name, sorted, an alias by its primary name"))
    {
        runner.CheckEqual(workgroupFacts->CapabilityNames,
                          std::vector<std::string>{ "Float16", "SPV_KHR_16bit_storage", "Shader", "UniformAndStorageBuffer16BitAccess" },
                          "each OpCapability and OpExtension reads back as a name, sorted, an alias by its primary name");
    }
    if (runner.CheckOk(workgroupFacts,
                       "each workgroup variable counts RoundUp(16, std430 size): 64 + 32 + 16 + 32 bytes"))
    {
        runner.CheckEqual(workgroupFacts->WorkgroupMemoryBytes, 144u,
                          "each workgroup variable counts RoundUp(16, std430 size): 64 + 32 + 16 + 32 bytes");
    }
    if (runner.CheckOk(noPush, "a module with no workgroup variable uses no workgroup memory"))
    {
        runner.CheckEqual(noPush->WorkgroupMemoryBytes, 0u,
                          "a module with no workgroup variable uses no workgroup memory");
    }

    runner.BeginSection("a dynamic index into a binding array states the device feature it needs");
    const CookResult<EmittedCodeFacts> constantIndex = validator.ReadEmittedCodeFacts(imageArray, sink);
    if (runner.CheckOk(constantIndex, "a constant index into the array needs no dynamic indexing feature"))
    {
        runner.CheckEqual(std::ranges::find(constantIndex->CapabilityNames, "StorageImageArrayDynamicIndexing"),
                          constantIndex->CapabilityNames.end(),
                          "a constant index into the array needs no dynamic indexing feature");
    }
    const std::vector<std::byte> dynamicArray =
        Assemble(Replace(k_ImageArrayCs, "OpAccessChain %ptrImage %Levels %uint_0", "OpAccessChain %ptrImage %Levels %index"));
    runner.Check(!dynamicArray.empty(), "the dynamic index fixture assembles");
    const CookResult<EmittedCodeFacts> dynamicIndex = validator.ReadEmittedCodeFacts(dynamicArray, sink);
    runner.Check(dynamicIndex.has_value() &&
                     std::ranges::find(dynamicIndex->CapabilityNames, "StorageImageArrayDynamicIndexing") !=
                         dynamicIndex->CapabilityNames.end(),
                 "an index that is not a constant adds StorageImageArrayDynamicIndexing, which Slang does not state");

    return runner.Report();
}
