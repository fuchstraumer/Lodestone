#include "SlangVariantCompiler.hpp"
#include "CookerErrors.hpp"
#include "SlangCompilerTypes.hpp"
#include "SlangModuleContext.hpp"
#include "compile/Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SlangDiagnosticParser.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationValue.hpp"
#include "slang-com-ptr.h"
#include "slang.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
// will fix this, but for now bring everything from lodestone into scope
using namespace lodestone;

constexpr SlangInt k_WgslTargetIndex = 0;

/** What one entry point's codegen produced. The diagnostic text travels with the code so that a
 * worker thread never touches a sink. coalesced after threads join */
struct GeneratedEntryPoint
{
    std::string Code;
    std::string Diagnostics;
};

GeneratedEntryPoint GenerateOneEntryPoint(slang::IComponentType* linked_program, size_t index)
{
    Slang::ComPtr<slang::IBlob> code;
    Slang::ComPtr<slang::IBlob> diagnostics;
    const bool failed = SLANG_FAILED(linked_program->getEntryPointCode(
        static_cast<SlangInt>(index), k_WgslTargetIndex, code.writeRef(), diagnostics.writeRef()));

    return GeneratedEntryPoint{ .Code = failed ? std::string{} : BlobToString(code.get()),
                                .Diagnostics = BlobToString(diagnostics.get()) };
}

std::vector<std::string> GenerateEntryPointCode(SlangModuleContext& context,
                                                Slang::ComPtr<slang::IComponentType> linked_program,
                                                DiagnosticSink& sink)
{
    const size_t entryPointCount = context.EntryPointCount();
    std::vector<std::string> generated(entryPointCount);

    for (size_t i = 0; i < entryPointCount; ++i)
    {
        GeneratedEntryPoint result = GenerateOneEntryPoint(linked_program, i);
        if (!result.Diagnostics.empty())
        {
            ParseSlangDiagnostics(result.Diagnostics, "getEntryPointCode", sink);
        }
        generated[i] = std::move(result.Code);
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
        const std::string variantModuleName = MakeVariantModuleName(binding.Axis->Name, binding.Value);
        const std::string variantModulePath = MakeVariantModulePath(binding.Axis->Name, binding.Value);
        const std::string variantSource = MakeExportedConstantSource(binding.Axis->Name, binding.Value);

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
    // todo-ship: another location we'll need to update to support further output target formats
    slang::ProgramLayout* programLayout = linkedProgram->getLayout(k_WgslTargetIndex);
    if (programLayout == nullptr)
    {
        return std::unexpected(CookError::ReflectionUnavailable);
    }

    result.ProgramLayout = programLayout;

    std::vector<std::string> entryPointCode = GenerateEntryPointCode(context, linkedProgram, sink);
    if (std::ranges::any_of(entryPointCode, [](const std::string& code) { return code.empty(); }))
    {
        return std::unexpected(CookError::CodeGenerationFailed);
    }

    // extract metadata for each entrypoint
    auto extractMetadataLambda = [&](const auto& ep_tuple)
    {
        const auto&[entryPointIndex, entryPointCode] = ep_tuple;
        const auto castIndex = static_cast<int64_t>(entryPointIndex);
        Slang::ComPtr<slang::IMetadata> metadata;
        linkedProgram->getEntryPointMetadata(castIndex, k_WgslTargetIndex, metadata.writeRef());
        return std::move(metadata);
    };
    result.EntryPointMetadata = entryPointCode |
                                std::views::enumerate |
                                std::views::transform(extractMetadataLambda) |
                                std::ranges::to<std::vector<Slang::ComPtr<slang::IMetadata>>>();

    result.EntryPointStrings = std::move(entryPointCode);

    return result;
}

} // namespace lodestone