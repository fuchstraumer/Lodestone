#include "SlangVariantCompiler.hpp"
#include "CookerErrors.hpp"
#include "SlangCompilerTypes.hpp"
#include "SlangModuleContext.hpp"
#include "Diagnostics.hpp"
#include "compile/SlangDiagnosticParser.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationValue.hpp"
#include "slang-com-ptr.h"
#include "slang.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
using namespace lodestone;

constexpr uint32_t k_SpirvMagicNumber = 0x07230203u;
constexpr size_t k_SpirvHeaderBytes = 5u * sizeof(uint32_t);

bool IsSpirvModule(const std::vector<std::byte>& code) noexcept;
// Slang's SPIR-V emit dereferences a null layout for such a parameter and crashes, so this runs before code generation
CookError CheckVaryingParameters(slang::ProgramLayout* program_layout, DiagnosticSink& sink);
bool HoldsOpaqueType(slang::TypeLayoutReflection* type_layout) noexcept;

/** Codegen output, with its diagnostics, so that worker threads never touch a sink. */
struct GeneratedEntryPoint
{
    std::vector<std::byte> Code;
    std::string Diagnostics;
    bool CallFailed{ false };
};

GeneratedEntryPoint GenerateOneEntryPoint(slang::IComponentType* linked_program, size_t index, SlangInt target_index);
CookResult<std::vector<LinkedVariant::EpCode>> GenerateEntryPointCode(SlangModuleContext& context, Slang::ComPtr<slang::IComponentType> linked_program, SlangInt target_index, DiagnosticSink& sink);
CookResult<Slang::ComPtr<slang::IComponentType>> LinkVariant(SlangModuleContext& context, const VariantDescriptor& descriptor, DiagnosticSink& sink);

} // namespace

namespace lodestone
{

CookResult<LinkedVariant> SlangVariantCompiler::CompileVariant(SlangModuleContext& context,
                                                               const VariantDescriptor& descriptor,
                                                               DiagnosticSink& sink)
{
    LinkedVariant result;

    CookResult<Slang::ComPtr<slang::IComponentType>> linkResult = LinkVariant(context, descriptor, sink);
    if (!linkResult)
    {
        return std::unexpected(linkResult.error());
    }

    Slang::ComPtr<slang::IComponentType> linkedProgram = linkResult.value();
    result.LinkedProgram = linkedProgram;
    slang::ProgramLayout* programLayout = linkedProgram->getLayout(k_BaseTargetIndex);
    if (programLayout == nullptr)
    {
        return std::unexpected(CookError::ReflectionUnavailable);
    }

    result.ProgramLayout = programLayout;

    const CookError varyingError = CheckVaryingParameters(programLayout, sink);
    if (varyingError != CookError::Success)
    {
        return std::unexpected(varyingError);
    }

    CookResult<std::vector<LinkedVariant::EpCode>> generatedCode =
        GenerateEntryPointCode(context, linkedProgram, k_BaseTargetIndex, sink);
    if (!generatedCode)
    {
        return std::unexpected(generatedCode.error());
    }
    std::vector<LinkedVariant::EpCode> entryPointCode = std::move(*generatedCode);

    if (context.CompilesDeliveredForm())
    {
        CookResult<std::vector<LinkedVariant::EpCode>> deliveredCode =
            GenerateEntryPointCode(context, linkedProgram, k_DeliveredTargetIndex, sink);
        if (!deliveredCode)
        {
            return std::unexpected(deliveredCode.error());
        }
        result.DeliveredEntryPointCode = std::move(*deliveredCode);
    }

    for (size_t i = 0; i < entryPointCode.size(); ++i)
    {
        const auto castIndex = static_cast<int64_t>(i);
        Slang::ComPtr<slang::IMetadata> metadata;
        linkedProgram->getEntryPointMetadata(castIndex, k_BaseTargetIndex, metadata.writeRef());
        result.EntryPointMetadata.push_back(metadata);
    }

    result.EntryPointCode = std::move(entryPointCode);

    return result;
}

} // namespace lodestone

namespace
{
using namespace lodestone;

bool IsSpirvModule(const std::vector<std::byte>& code) noexcept
{
    if (code.size() < k_SpirvHeaderBytes || (code.size() % sizeof(uint32_t)) != 0u)
    {
        return false;
    }

    uint32_t magic = 0u;
    std::memcpy(&magic, code.data(), sizeof(magic));
    return magic == k_SpirvMagicNumber;
}

CookError CheckVaryingParameters(slang::ProgramLayout* program_layout, DiagnosticSink& sink)
{
    auto isVarying = [](SlangParameterCategory category)
    {
        return category == SLANG_PARAMETER_CATEGORY_VARYING_INPUT || category == SLANG_PARAMETER_CATEGORY_VARYING_OUTPUT;
    };

    const SlangUInt entryPointCount = program_layout->getEntryPointCount();
    for (SlangUInt entryPointIndex = 0u; entryPointIndex < entryPointCount; ++entryPointIndex)
    {
        slang::EntryPointReflection* entryPoint = program_layout->getEntryPointByIndex(entryPointIndex);
        const unsigned int parameterCount = entryPoint->getParameterCount();
        for (unsigned int parameterIndex = 0u; parameterIndex < parameterCount; ++parameterIndex)
        {
            slang::VariableLayoutReflection* parameter = entryPoint->getParameterByIndex(parameterIndex);
            bool varying = false;
            bool uniform = false;
            const unsigned int categoryCount = parameter->getCategoryCount();
            for (unsigned int categoryIndex = 0u; categoryIndex < categoryCount; ++categoryIndex)
            {
                const auto category = static_cast<SlangParameterCategory>(parameter->getCategoryByIndex(categoryIndex));
                varying = varying || isVarying(category);
                uniform = uniform || (!isVarying(category) && category != SLANG_PARAMETER_CATEGORY_NONE);
            }

            // a resource field that Slang did not lay out as uniform shows in the type only, not in the categories
            if ((varying && uniform) || (!uniform && HoldsOpaqueType(parameter->getTypeLayout())))
            {
                const char* parameterName = parameter->getName();
                return ReportError(sink,
                                   CookError::SlangVaryingParameterHoldsResource,
                                   std::format("entry point {} parameter {} is not a uniform parameter, and holds a "
                                               "resource or a pointer that Slang gives no layout. Move it to a global, a "
                                               "block, or a uniform parameter",
                                               entryPoint->getName(),
                                               parameterName != nullptr ? parameterName : "?"));
            }
        }
    }

    return CookError::Success;
}

bool HoldsOpaqueType(slang::TypeLayoutReflection* type_layout) noexcept
{
    if (type_layout == nullptr)
    {
        return false;
    }

    switch (type_layout->getKind())
    {
    case slang::TypeReflection::Kind::Struct:
    {
        const unsigned int fieldCount = type_layout->getFieldCount();
        for (unsigned int fieldIndex = 0u; fieldIndex < fieldCount; ++fieldIndex)
        {
            if (HoldsOpaqueType(type_layout->getFieldByIndex(fieldIndex)->getTypeLayout()))
            {
                return true;
            }
        }

        return false;
    }
    case slang::TypeReflection::Kind::Array:
        return HoldsOpaqueType(type_layout->getElementTypeLayout());
    case slang::TypeReflection::Kind::Resource:
    case slang::TypeReflection::Kind::SamplerState:
    case slang::TypeReflection::Kind::ConstantBuffer:
    case slang::TypeReflection::Kind::ParameterBlock:
    case slang::TypeReflection::Kind::TextureBuffer:
    case slang::TypeReflection::Kind::ShaderStorageBuffer:
    case slang::TypeReflection::Kind::Pointer:
        return true;
    default:
        return false;
    }
}

GeneratedEntryPoint GenerateOneEntryPoint(slang::IComponentType* linked_program, size_t index, SlangInt target_index)
{
    Slang::ComPtr<slang::IBlob> code;
    Slang::ComPtr<slang::IBlob> diagnostics;
    const bool failed = SLANG_FAILED(linked_program->getEntryPointCode(
        static_cast<SlangInt>(index), target_index, code.writeRef(), diagnostics.writeRef()));

    return GeneratedEntryPoint{ .Code = failed ? std::vector<std::byte>{} : BlobToBytes(code.get()),
                                .Diagnostics = BlobToString(diagnostics.get()),
                                .CallFailed = failed };
}

CookResult<std::vector<LinkedVariant::EpCode>> GenerateEntryPointCode(SlangModuleContext& context,
                                                                      Slang::ComPtr<slang::IComponentType> linked_program,
                                                                      SlangInt target_index,
                                                                      DiagnosticSink& sink)
{
    // Slang can report an error with a success code (E55205 for a spec constant in numthreads on WGSL)
    const size_t entryPointCount = context.EntryPointCount();
    std::vector<LinkedVariant::EpCode> generated(entryPointCount);
    bool anyEntryPointFailed = false;

    for (size_t i = 0; i < entryPointCount; ++i)
    {
        GeneratedEntryPoint result = GenerateOneEntryPoint(linked_program, i, target_index);
        const int32_t failureCount = result.Diagnostics.empty()
                                         ? 0
                                         : ParseSlangDiagnostics(result.Diagnostics, "getEntryPointCode", sink);
        const bool badSpirv = (context.Language() == TargetLanguage::Spirv) && !IsSpirvModule(result.Code);
        if (result.CallFailed || (failureCount > 0) || result.Code.empty() || badSpirv)
        {
            anyEntryPointFailed = true;
        }
        generated[i] = std::move(result.Code);
    }

    if (anyEntryPointFailed)
    {
        return std::unexpected(CookError::CodeGenerationFailed);
    }

    return generated;
}

CookResult<Slang::ComPtr<slang::IComponentType>> LinkVariant(SlangModuleContext& context,
                                                             const VariantDescriptor& descriptor,
                                                             DiagnosticSink& sink)
{
    std::vector<slang::IComponentType*> components = context.BaseComponents();
    components.reserve(context.BaseComponents().size() + (descriptor.Active.size() * 2u) + 1u);
    if (context.ShimLinkModule() != nullptr)
    {
        components.push_back(context.ShimLinkModule());
    }

    if (context.HeapLoweringModule() != nullptr)
    {
        components.push_back(context.HeapLoweringModule());
    }

    for (const PermutationBinding& binding : descriptor.Active)
    {
        const std::string variantModuleName = MakeVariantModuleName(*binding.Axis, binding.Value);
        const std::string variantModulePath = MakeVariantModulePath(*binding.Axis, binding.Value);
        const std::string variantSource = MakeExportedConstantSource(*binding.Axis, binding.Value);

        Slang::ComPtr<slang::IBlob> diagnostics;
        slang::IModule* variantModule =
            context.Session()->loadModuleFromSourceString(variantModuleName.c_str(),
                                                          variantModulePath.c_str(),
                                                          variantSource.c_str(),
                                                          diagnostics.writeRef());
        ReportDiagnostics(sink, "loadModuleFromSourceString", diagnostics.get());

        if (variantModule == nullptr)
        {
            return std::unexpected(CookError::VariantModuleCreationFailed);
        }

        components.push_back(variantModule);

        if (slang::IModule* formatModule = context.ShimFormatLinkModule(binding.Axis->Name, binding.Value.AsUInt()))
        {
            components.push_back(formatModule);
        }
    }

    Slang::ComPtr<slang::IBlob> diagnostics;
    Slang::ComPtr<slang::IComponentType> composite;
    context.Session()->createCompositeComponentType(components.data(),
                                                    static_cast<SlangInt>(components.size()),
                                                    composite.writeRef(),
                                                    diagnostics.writeRef());
    ReportDiagnostics(sink, "createCompositeComponentType", diagnostics.get());

    if (composite == nullptr)
    {
        return std::unexpected(CookError::CompositeCreationFailed);
    }

    Slang::ComPtr<slang::IComponentType> linked;
    if (SLANG_FAILED(composite->link(linked.writeRef(), diagnostics.writeRef())))
    {
        ReportDiagnostics(sink, "link", diagnostics.get());
        return std::unexpected(CookError::LinkFailed);
    }

    return linked;
}

} // namespace
