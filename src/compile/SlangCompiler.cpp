#include "compile/SlangCompiler.hpp"
#include "impl/ThreadPool.hpp"
#include "CookerErrors.hpp"
#include "compile/Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SlangDiagnosticParser.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationSpace.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <ranges>
#include <string>
#include <vector>
#include <slang.h>

namespace lodestone
{

SlangCompiler::SlangCompiler() noexcept : compilePool{ nullptr }
{
}

SlangCompiler::~SlangCompiler() = default;

CookError SlangCompiler::Initialize(const SlangCompilerCreateInfo& create_info, DiagnosticSink& sink)
{
    compilePool = std::make_unique<ThreadPool>(create_info);
}

CookResult<RawModule> SlangCompiler::PrepareRawModule(const PermutationSpace& space)
{
    if (impl == nullptr)
    {
        return std::unexpected(CookError::CompilerNotInitialized);
    }

    const std::vector<std::string_view> sourceViews{ impl->moduleSourceTexts.begin(),
                                                     impl->moduleSourceTexts.end() };
    CookResult<std::vector<ExternConstantDefault>> defaults =
        space.CollectUndrivenExternDefaults(sourceViews);
    if (!defaults)
    {
        return std::unexpected(defaults.error());
    }

    RawModule module;
    module.Name = impl->moduleName;
    module.EntryPointNames = impl->entryPointNames;
    module.ExternDefaults = std::move(defaults.value());
    return module;
}

CookResult<RawVariant> SlangCompiler::CompileVariantRaw(const VariantDescriptor& descriptor)
{
    if (impl == nullptr)
    {
        return std::unexpected(CookError::CompilerNotInitialized);
    }

    CookResult<Slang::ComPtr<slang::IComponentType>> linkResult = impl->linkVariant(descriptor.Active);
    if (!linkResult)
    {
        return std::unexpected(linkResult.error());
    }

    slang::IComponentType* linkedProgram = linkResult.value().get();
    slang::ProgramLayout* programLayout = linkedProgram->getLayout();
    if (programLayout == nullptr)
    {
        return std::unexpected(CookError::ReflectionUnavailable);
    }

    const std::vector<std::string> generatedCode = impl->generateEntryPointCode(linkedProgram);

    RawVariant variant;
    variant.VariantSuffix = MakeAssignmentSuffix(descriptor.Canonical);
    variant.VariantDescription = DescribeAssignment(descriptor.Canonical);
    variant.VariantIndex = static_cast<uint32_t>(descriptor.Index);

    // The global scope is the same for every entry point of this variant. Extract it once, and let
    // each entry point say which of those bindings it reads. An entry point then appends the
    // parameters it declares itself.
    const CookError bindingsError =
        impl->extractRawBindings(programLayout, variant.Bindings, variant.SizeAttributes);
    if (bindingsError != CookError::Success)
    {
        return std::unexpected(bindingsError);
    }

    variant.EntryPoints.reserve(impl->entryPointNames.size());

    // The global sort is done. Each entry point appends after it, in declaration order, so a second
    // sort across the full list is never correct: two placements can be equal across two scopes.
    const size_t globalBindingCount = variant.Bindings.size();

    for (size_t i = 0; i < impl->entryPointNames.size(); ++i)
    {
        if (generatedCode[i].empty())
        {
            return std::unexpected(CookError::CodeGenerationFailed);
        }

        std::vector<RawBindingDraft> entryPointDrafts;
        CookResult<RawEntryPoint> rawEntryPoint =
            impl->extractRawEntryPoint(linkedProgram,
                                       programLayout,
                                       static_cast<SlangInt>(i),
                                       std::span{ variant.Bindings }.first(globalBindingCount),
                                       entryPointDrafts);
        if (!rawEntryPoint)
        {
            return std::unexpected(rawEntryPoint.error());
        }

        // An entry point owns its own parameters by construction, so ownership states the visibility
        // and the placement query never sees these rows.
        const auto ownedBase = static_cast<uint32_t>(variant.Bindings.size());
        AppendBindingDrafts(entryPointDrafts, variant.Bindings, variant.SizeAttributes);
        rawEntryPoint.value().UsedBindingIndices.append_range(
            std::views::iota(ownedBase, static_cast<uint32_t>(variant.Bindings.size())));

        rawEntryPoint.value().VariantSuffix = variant.VariantSuffix;
        rawEntryPoint.value().TargetText = generatedCode[i];
        variant.EntryPoints.emplace_back(std::move(rawEntryPoint.value()));
    }

    return variant;
}

std::string_view SlangCompiler::GetModuleName() const noexcept
{
    if (impl == nullptr)
    {
        return {};
    }

    return impl->moduleName;
}

std::span<const std::string> SlangCompiler::GetEntryPointNames() const noexcept
{
    if (impl == nullptr)
    {
        return {};
    }

    return impl->entryPointNames;
}

std::span<const std::string> SlangCompiler::GetModuleSourceTexts() const noexcept
{
    if (impl == nullptr)
    {
        return {};
    }

    return impl->moduleSourceTexts;
}

} // namespace lodestone
