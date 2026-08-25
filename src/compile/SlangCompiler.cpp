#include "compile/SlangCompiler.hpp"
#include "impl/SlangModuleContext.hpp"
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

SlangCompiler::SlangCompiler() noexcept : compilePool{ nullptr }, bootstrapContext{ nullptr }
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


} // namespace lodestone
