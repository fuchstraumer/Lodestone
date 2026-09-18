#include "driver/CookerDriver.hpp"
#include "compile/SymbolTable.hpp"
#include "driver/CookerOptions.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SlangCompiler.hpp"
#include "driver/CookerSteps.hpp"
#include "driver/steps/PrepareCookStep.hpp"
#include "driver/steps/PrepareModuleStep.hpp"
#include "driver/steps/PreparePermutationSpaceStep.hpp"
#include "driver/steps/BuildModuleStep.hpp"
#include "driver/steps/FinalizeModuleStep.hpp"
#include "emit/OutputSink.hpp"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <ranges>
#include <ratio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "magic_enum/magic_enum.hpp"
#include "model/CookedLibrary.hpp"

namespace lodestone
{

namespace
{
    std::string BuildDumpFileName(std::string_view module_name, std::string_view target_name, StageDumpKind kind)
    {
        return std::format("{}_{}_{}.dump", module_name, target_name, magic_enum::enum_name(kind));
    }
} // namespace

CookResult<CookStatistics> RunCookOnce(CookerOptions options,
                                       OutputSink& sink,
                                       DiagnosticSink& diagnostics)
{
    const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    CookStatistics cookStatistics;
    // note that none of these actually hold state, it just makes calling them as functors easier
    const static PrepareCookStep PrepareCook;
    const static PrepareModuleStep PrepareModule;
    const static PreparePermutationSpaceStep PreparePermutationSpace;
    const static BuildModuleStep BuildModule;
    const static FinalizeModuleStep FinalizeModule;

    CookResult<PreparedCook> preparedCookResult = PrepareCook(std::move(options));
    if (!preparedCookResult)
    {
        return std::unexpected(preparedCookResult.error());
    }

    // now, read options from preparedCookResult
    const PreparedCook& preparedCook = *preparedCookResult;
    const SharedCookState& cookState = preparedCook.SharedState;

    std::vector<CookedModule> cookedModules(cookState.Options.ModulePaths.size());

    for (auto&& [moduleIdx, modulePath] : std::views::enumerate(cookState.Options.ModulePaths))
    {
        for (const auto& targetName : cookState.Options.TargetNames)
        {
            CookResult<PreparedCompiler> preparedModuleResult = PrepareModule(cookState,
                                                                              modulePath,
                                                                              targetName);
            
            if (!preparedModuleResult)
            {
                return std::unexpected(preparedModuleResult.error());
            }

            const PreparedCompiler& preparedModule = *preparedModuleResult;
            std::vector<RawAxisDeclaration> rawAxes = preparedModule.Compiler->BuildAxisDeclarations();
            const std::string_view moduleName = cookState.AllModuleNames[static_cast<size_t>(moduleIdx)];
            const SymbolTable& symbolTable = preparedModule.Compiler->GetSymbolTable();
            CookResult<PreparedPermutationSpace> spaceResult = PreparePermutationSpace(cookState,
                                                                                       moduleName,
                                                                                       targetName,
                                                                                       symbolTable,
                                                                                       std::move(rawAxes));
            
            if (!spaceResult)
            {
                return std::unexpected(spaceResult.error());
            }

            const PreparedPermutationSpace& preparedSpace = *spaceResult;

            if (preparedSpace.SpaceDump)
            {
                const std::string fileName = BuildDumpFileName(moduleName, targetName, StageDumpKind::Space);
                CookError writeError = sink.WriteArtifact(fileName, std::move(*preparedSpace.SpaceDump));
                if (!writeError)
                {
                    return std::unexpected(writeError);
                }
            }

            if (preparedSpace.VariantDump)
            {
                const std::string fileName = BuildDumpFileName(moduleName, targetName, StageDumpKind::Variants);
                CookError writeError = sink.WriteArtifact(fileName, std::move(*preparedSpace.VariantDump));
                if (!writeError)
                {
                    return std::unexpected(writeError);
                }
            }

            CookResult<BuiltModule> buildResult = BuildModule(cookState,
                                                              moduleName,
                                                              targetName,
                                                              preparedModule.Compiler.get(),
                                                              preparedSpace.Space,
                                                              preparedSpace.Variants);
            if (!buildResult)
            {
                return std::unexpected(buildResult.error());
            }

            BuiltModule& builtModule = *buildResult;

            if (builtModule.RawModuleDump)
            {
                const std::string fileName = BuildDumpFileName(moduleName, targetName, StageDumpKind::Raw);
                CookError writeError = sink.WriteArtifact(fileName, std::move(*builtModule.RawModuleDump));
                if (!writeError)
                {
                    return std::unexpected(writeError);
                }
            }

            if (builtModule.ResolvedModuleDump)
            {
                const std::string fileName = BuildDumpFileName(moduleName, targetName, StageDumpKind::Resolved);
                CookError writeError = sink.WriteArtifact(fileName, std::move(*builtModule.ResolvedModuleDump));
                if (!writeError)
                {
                    return std::unexpected(writeError);
                }
            }
            
            // operator+= uses atomic_ref to safely update the statistics in a potentially multithreaded context
            // we can improve this in the future, but for now that works just fine
            cookStatistics += builtModule.Statistics;

            // Last step: resolve everything into it's final form.
            CookResult<FinalizedModule> finalizeResult = FinalizeModule(cookState,
                                                                        std::move(builtModule.Module),
                                                                        builtModule.CompiledVariants);

            if (!finalizeResult)
            {
                return std::unexpected(finalizeResult.error());
            }

            if (finalizeResult->Dump)
            {
                const std::string fileName = BuildDumpFileName(moduleName, targetName, StageDumpKind::Cooked);
                CookError writeError = sink.WriteArtifact(fileName, std::move(*finalizeResult->Dump));
                if (!writeError)
                {
                    return std::unexpected(writeError);
                }
            }

            // write into slot: when we thread this, that should just work since the vector is never resized after initial allocation
            cookedModules[static_cast<size_t>(moduleIdx)] = std::move(finalizeResult->Module);
        }
    }

    const std::chrono::steady_clock::time_point endTime = std::chrono::steady_clock::now();
    const std::chrono::duration<double, std::milli> elapsed = endTime - startTime;
    cookStatistics.ElapsedMilliseconds = elapsed.count();
    return cookStatistics;
}

namespace
{

    /** Cooks twice into memory and compares every artifact. Enumeration order is sorted and the
     * interner numbers entries in first-encounter order, so two cooks of one input must agree byte
     * for byte. A difference means an unordered container's iteration order reached the output, which
     * otherwise shows up months later as a rebuild that changes nothing. */
    CookResult<CookStatistics> RunCookTwiceAndCompare(const CookerOptions& options,
                                                      OutputSink& sink,
                                                      DiagnosticSink& diagnostics)
    {
        ReportInfo(diagnostics, "determinism check: cooking twice into memory");

        std::string firstName = std::string{ sink.Describe() } + "_first";
        MemoryOutputSink first{ firstName };
        const CookResult<CookStatistics> firstResult = RunCookOnce(options, first, diagnostics);
        if (!firstResult)
        {
            return firstResult;
        }

        std::string secondName = std::string{ sink.Describe() } + "_second";
        MemoryOutputSink second{ secondName };
        const CookResult<CookStatistics> secondResult = RunCookOnce(options, second, diagnostics);
        if (!secondResult)
        {
            return secondResult;
        }

        const auto& firstArtifacts = first.GetArtifacts();
        const auto& secondArtifacts = second.GetArtifacts();
        if (firstArtifacts.size() != secondArtifacts.size())
        {
            const std::string errStr =
                std::format("Determinism Failed: first pass had {} artifacts, second had {}",
                            firstArtifacts.size(),
                            secondArtifacts.size());
            return std::unexpected(ReportError(diagnostics, CookError::CookNotDeterministic, errStr));
        }

        auto [mismatchIterFirst, mismatchIterSecond] = std::ranges::mismatch(firstArtifacts, secondArtifacts);
        if (mismatchIterFirst != firstArtifacts.end() && mismatchIterSecond != secondArtifacts.end())
        {
            if (mismatchIterFirst->first == mismatchIterSecond->first)
            {
                const std::string errStr =
                    std::format("Determinism failed - mismatch in file contents for artifact '{}'",
                                mismatchIterFirst->first);
                return std::unexpected(ReportError(diagnostics, CookError::CookNotDeterministic, errStr));
            }
            else
            {
                // mismatch in artifact names
                const std::string errStr = std::format("Determinism failed - mismatch in artifact names: '{}' vs '{}'",
                                                       mismatchIterFirst->first,
                                                       mismatchIterSecond->first);
                return std::unexpected(ReportError(diagnostics, CookError::CookNotDeterministic, errStr));
            }
        }

        const std::string determinismStr = std::format("determinism verified: {} artifacts identical across two cooks",
                                                       first.GetArtifacts().size() + 1u);
        ReportInfo(diagnostics, determinismStr);

        for (const auto& [name, content] : first.GetArtifacts())
        {
            const CookError artifactResult = sink.WriteArtifact(name, content);
            if (artifactResult != CookError::Success)
            {
                return std::unexpected(artifactResult);
            }
        }

        return secondResult;
    }

} // namespace

CookResult<CookStatistics> RunCook(const CookerOptions& options, OutputSink& sink)
{
    // create one diag sink for the *whole* cook
    StderrDiagnosticSink diagnostics;

    if (options.VerifyDeterministic)
    {
        return RunCookTwiceAndCompare(options, sink, diagnostics);
    }

    return RunCookOnce(options, sink, diagnostics);
}

} // namespace lodestone
