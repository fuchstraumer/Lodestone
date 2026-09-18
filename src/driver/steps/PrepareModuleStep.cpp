#include "driver/steps/PrepareModuleStep.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "compile/SlangCompiler.hpp"
#include "driver/CookerSteps.hpp"
#include "target/TargetProfile.hpp"
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace lodestone
{

CookResult<PreparedCompiler> PrepareModuleStep::operator()(const SharedCookState& shared_state,
                                                           std::filesystem::path module_path,
                                                           std::string_view target_name) const
{
    // first step: initialize the compiler and have it begin building this particular module
    SlangCompilerCreateInfo createInfo;
    createInfo.ModulePath = std::move(module_path);
    createInfo.ModuleCacheDirectory = shared_state.CacheDirectory;
    createInfo.OptimizationLevel = shared_state.Options.OptimizationLevel;
    createInfo.MultithreadVariantBuild = shared_state.Options.MultithreadEntryPointCodegen;
    
    CookResult<TargetProfile> targetProfileResult = FindTargetProfile(target_name);
    if (!targetProfileResult)
    {
        return std::unexpected(targetProfileResult.error());
    }
    createInfo.AccessModel = PlacementKindFromAccessModel(targetProfileResult->Access);
    

    PreparedCompiler result{ std::make_unique<SlangCompiler>() };
    DiagnosticSink& diagnostics = *shared_state.Diagnostics;
    const CookError initializeResult = result.Compiler->Initialize(createInfo, diagnostics);
    if (!initializeResult)
    {
        return std::unexpected(initializeResult);
    }

    // todo-ship: not safe to write to the diagnostic sink from multiple threads currently
    const std::string_view moduleName = result.Compiler->ModuleName();
    const std::string infoStr =
        std::format("module {} declares {} entrypoints", moduleName, result.Compiler->EntryPointCount());
    ReportInfo(diagnostics, infoStr);

    return std::move(result);
}

}
