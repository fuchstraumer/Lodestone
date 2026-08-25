#include "compile/SlangCompiler.hpp"
#include "CookerErrors.hpp"
#include "compile/Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SlangDiagnosticParser.hpp"
#include "impl/SlangModuleContext.hpp"
#include "impl/ThreadPool.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationSpace.hpp"


#include <cstddef>
#include <expected>
#include <memory>
#include <slang.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


namespace lodestone
{

SlangCompiler::SlangCompiler() noexcept
    : compilePool{ nullptr },
      bootstrapContext{ nullptr }
{
}

SlangCompiler::~SlangCompiler() = default;

CookError SlangCompiler::Initialize(const SlangCompilerCreateInfo& create_info, DiagnosticSink& sink)
{
    compilePool = std::make_unique<ThreadPool>();
    bootstrapContext = std::make_unique<SlangModuleContext>();

    CookError initResult = bootstrapContext->Initialize(create_info, sink);
    if (initResult != CookError::Success)
    {
        return initResult;
    }

    initResult = bootstrapContext->RunBootstrap();
    if (initResult != CookError::Success)
    {
        return initResult;
    }

    return initResult;
}

CookResult<RawModule> SlangCompiler::PrepareRawModule(const PermutationSpace& space)
{
    if (compilePool == nullptr || bootstrapContext == nullptr)
    {
        return std::unexpected(CookError::CompilerNotInitialized);
    }
    const std::vector<std::string_view> sourceViews = bootstrapContext->ModuleSourceStringViews();
    CookResult<std::vector<ExternConstantDefault>> defaults =
        space.CollectUndrivenExternDefaults(sourceViews);
    if (!defaults)
    {
        return std::unexpected(defaults.error());
    }

    RawModule module;
    module.Name = bootstrapContext->ModuleName();
    module.EntryPointNames = bootstrapContext->EntryPointNames();
    module.ExternDefaults = std::move(defaults.value());
    return module;
}

SlangCompiler::CompileResultList SlangCompiler::Compile(const std::vector<VariantDescriptor>& variants, DiagnosticSink& sink) const
{
    return compilePool->Compile(variants, sink);
}

std::string_view SlangCompiler::ModuleName() const noexcept
{
    return bootstrapContext ? bootstrapContext->ModuleName() : std::string_view{};
}

size_t SlangCompiler::EntryPointCount() const noexcept
{
    return bootstrapContext ? bootstrapContext->EntryPointNames().size() : 0;
}

const std::vector<std::string>& SlangCompiler::EntryPointNames() const noexcept
{
    return bootstrapContext->EntryPointNames();
}

const std::vector<std::string>& SlangCompiler::ModuleSourceStrings() const noexcept
{
    return bootstrapContext->ModuleSourceStrings();
}

std::vector<std::string_view> SlangCompiler::ModuleSourceStringViews() const noexcept
{
    return bootstrapContext->ModuleSourceStringViews();
}

} // namespace lodestone
