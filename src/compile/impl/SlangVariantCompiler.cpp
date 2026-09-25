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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
// will fix this, but for now bring everything from lodestone into scope
using namespace lodestone;

// Each session holds one target (SlangModuleContext::Initialize), so its index is always 0.
constexpr SlangInt k_TargetIndex = 0;
constexpr uint32_t k_SpirvMagicNumber = 0x07230203u;
constexpr size_t k_SpirvHeaderBytes = 5u * sizeof(uint32_t);

/** The manifest reads SPIR-V as words, so a payload that is not whole words cannot reach it. */
bool IsSpirvModule(std::string_view code) noexcept
{
    if (code.size() < k_SpirvHeaderBytes || (code.size() % sizeof(uint32_t)) != 0u)
    {
        return false;
    }

    uint32_t magic = 0u;
    std::memcpy(&magic, code.data(), sizeof(magic));
    return magic == k_SpirvMagicNumber;
}

/** What one entry point's codegen produced. The diagnostic text travels with the code so that a
 * worker thread never touches a sink. coalesced after threads join */
struct GeneratedEntryPoint
{
    std::string Code;
    std::string Diagnostics;
    bool CallFailed{ false };
};

GeneratedEntryPoint GenerateOneEntryPoint(slang::IComponentType* linked_program, size_t index)
{
    Slang::ComPtr<slang::IBlob> code;
    Slang::ComPtr<slang::IBlob> diagnostics;
    const bool failed = SLANG_FAILED(linked_program->getEntryPointCode(
        static_cast<SlangInt>(index), k_TargetIndex, code.writeRef(), diagnostics.writeRef()));

    return GeneratedEntryPoint{ .Code = failed ? std::string{} : BlobToString(code.get()),
                                .Diagnostics = BlobToString(diagnostics.get()),
                                .CallFailed = failed };
}

CookResult<std::vector<std::string>> GenerateEntryPointCode(SlangModuleContext& context,
                                                            Slang::ComPtr<slang::IComponentType> linked_program,
                                                            DiagnosticSink& sink)
{
    // Slang can report an error and still return a success code with code text. Measured with a
    // specialization constant in `numthreads` for WGSL: error E55205, a success code, and a wrong
    // `@workgroup_size(1, 1, 1)`. So an error record fails the entry point as a failed call does.
    const size_t entryPointCount = context.EntryPointCount();
    std::vector<std::string> generated(entryPointCount);
    bool anyEntryPointFailed = false;

    for (size_t i = 0; i < entryPointCount; ++i)
    {
        GeneratedEntryPoint result = GenerateOneEntryPoint(linked_program, i);
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
    components.reserve(context.BaseComponents().size() + descriptor.Active.size());

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
    slang::ProgramLayout* programLayout = linkedProgram->getLayout(k_TargetIndex);
    if (programLayout == nullptr)
    {
        return std::unexpected(CookError::ReflectionUnavailable);
    }

    result.ProgramLayout = programLayout;

    CookResult<std::vector<std::string>> generatedCode = GenerateEntryPointCode(context, linkedProgram, sink);
    if (!generatedCode)
    {
        return std::unexpected(generatedCode.error());
    }
    std::vector<std::string> entryPointCode = std::move(*generatedCode);

    for (size_t i = 0; i < entryPointCode.size(); ++i)
    {
        const auto castIndex = static_cast<int64_t>(i);
        Slang::ComPtr<slang::IMetadata> metadata;
        linkedProgram->getEntryPointMetadata(castIndex, k_TargetIndex, metadata.writeRef());
        result.EntryPointMetadata.push_back(metadata);
    }

    result.EntryPointStrings = std::move(entryPointCode);

    return result;
}

} // namespace lodestone