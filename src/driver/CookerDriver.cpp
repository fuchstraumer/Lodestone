#include "driver/CookerDriver.hpp"
#include "ShaderLibraryTypes.hpp"
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
#include "emit/DedupeReport.hpp"
#include "emit/Depfile.hpp"
#include "emit/OutputSink.hpp"
#include "emit/ResourceReport.hpp"
#include "emit/ShaderManifestEmitter.hpp"
#include "emit/ShaderManifestVerify.hpp"
#include "target/TargetProfile.hpp"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <ranges>
#include <ratio>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include "emit/StageDump.hpp"
#include "magic_enum/magic_enum.hpp"
#include "model/AxisInfluence.hpp"
#include "model/CookedLibrary.hpp"

namespace lodestone
{

namespace
{
    constexpr std::string_view k_DedupeReportFileName = "ShaderLibrary.dedupe.txt";

    std::string BuildDumpFileName(std::string_view module_name, std::string_view target_name, StageDumpKind kind)
    {
        return std::format("{}_{}_{}.json", module_name, target_name, magic_enum::enum_name(kind));
    }

    CookedLibrary MakeEmptyLibrary(const SharedCookState& cook_state);
    CookError EmitLibraryArtifacts(const CookedLibrary& library, OutputSink& sink, DiagnosticSink& diagnostics);

    std::string NormalizeDependencyPath(const std::filesystem::path& path)
    {
        std::error_code error;
        const std::filesystem::path absolutePath = std::filesystem::absolute(path, error);
        return (error ? path : absolutePath).lexically_normal().generic_string();
    }
} // namespace

CookResult<CookStatistics> RunCookOnce(CookerOptions options,
                                       OutputSink& sink,
                                       DiagnosticSink& diagnostics)
{
    const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    CookStatistics cookStatistics;
    //NOLINTBEGIN(readability-identifier-naming)
    const static PrepareCookStep PrepareCook;
    const static PrepareModuleStep PrepareModule;
    const static PreparePermutationSpaceStep PreparePermutationSpace;
    const static BuildModuleStep BuildModule;
    const static FinalizeModuleStep FinalizeModule;
    //NOLINTEND(readability-identifier-naming)

    CookResult<PreparedCook> preparedCookResult = PrepareCook(std::move(options), diagnostics);
    if (!preparedCookResult)
    {
        return std::unexpected(preparedCookResult.error());
    }

    const PreparedCook& preparedCook = *preparedCookResult;
    const SharedCookState& cookState = preparedCook.SharedState;

    CookedLibrary library = MakeEmptyLibrary(cookState);
    const size_t moduleCount = library.ModuleNames.size();
    // cooked modules point into their spaces, which must outlive the loop
    std::vector<std::unique_ptr<PermutationSpace>> permutationSpaces;
    permutationSpaces.reserve(library.Environments.size());
    std::vector<std::string> dependencyPaths;

    for (auto&& [moduleIdx, modulePath] : std::views::enumerate(cookState.Options.ModulePaths))
    {
        for (auto&& [targetIdx, targetName] : std::views::enumerate(cookState.Options.TargetNames))
        {
            CookResult<PreparedCompiler> preparedModuleResult = PrepareModule(cookState,
                                                                              modulePath,
                                                                              targetName);
            
            if (!preparedModuleResult)
            {
                return std::unexpected(preparedModuleResult.error());
            }

            const PreparedCompiler& preparedModule = *preparedModuleResult;
            if (cookState.Options.WriteDepfile)
            {
                for (const std::string& dependencyPath : preparedModule.Compiler->DependencyFilePaths())
                {
                    dependencyPaths.push_back(NormalizeDependencyPath(dependencyPath));
                }
            }

            CookResult<std::vector<RawAxisDeclaration>> rawAxesResult = preparedModule.Compiler->BuildAxisDeclarations();
            if (!rawAxesResult)
            {
                return std::unexpected(rawAxesResult.error());
            }
            
            std::vector<RawAxisDeclaration> rawAxes = std::move(rawAxesResult.value());
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

            PreparedPermutationSpace& preparedSpace = *spaceResult;

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

            // todo: intern across the whole library, not per module
            permutationSpaces.emplace_back(std::make_unique<PermutationSpace>(std::move(preparedSpace.Space)));
            CookResult<BuiltModule> buildResult = BuildModule(cookState,
                                                              moduleName,
                                                              targetName,
                                                              preparedModule.Compiler.get(),
                                                              *permutationSpaces.back(),
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

            if (builtModule.InternedModuleDump)
            {
                const std::string fileName = BuildDumpFileName(moduleName, targetName, StageDumpKind::Interned);
                CookError writeError = sink.WriteArtifact(fileName, std::move(*builtModule.InternedModuleDump));
                if (!writeError)
                {
                    return std::unexpected(writeError);
                }
            }
            
            cookStatistics += builtModule.Statistics;

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

            // independent of --dump-stage: it was once inside the Cooked dump branch, and wrote nothing alone
            if (cookState.Options.DumpSources)
            {
                const std::string outputDir = std::format("{}_{}_sources", moduleName, targetName);
                const ShaderCodeFormat codeFormat =
                    CodeFormatFromLanguage(cookState.TargetProfiles.at(targetName).Language);
                CookError writeError = DumpShaderSources(finalizeResult->Module, outputDir, codeFormat, sink);
                if (!writeError)
                {
                    return std::unexpected(writeError);
                }
            }

            const size_t environmentIndex =
                (static_cast<size_t>(targetIdx) * moduleCount) + static_cast<size_t>(moduleIdx);
            library.Environments[environmentIndex] = std::move(finalizeResult->Module);
        }

        cookStatistics.ModulesCooked += 1;
    }

    if (cookStatistics.ReflectionMismatches != 0u)
    {
        const std::string errStr = std::format("{} entry point variants disagree with the emitted target text",
                                               cookStatistics.ReflectionMismatches);
        return std::unexpected(ReportError(diagnostics, CookError::ReflectionMismatch, errStr));
    }

    const CookError pairsError = VerifySamplerPairsAcrossProfiles(library, diagnostics);
    if (pairsError != CookError::Success)
    {
        return std::unexpected(ReportError(diagnostics,
                                           pairsError,
                                           "two profiles read different sampler and texture pairs from one variant"));
    }

    const CookError factsError = VerifySourceFactsAcrossProfiles(library, diagnostics);
    if (factsError != CookError::Success)
    {
        return std::unexpected(ReportError(diagnostics,
                                           factsError,
                                           "two profiles read different workgroup memory or capabilities from one variant"));
    }

    const CookError capabilityAxisError = VerifyCapabilityAxes(library, diagnostics);
    if (capabilityAxisError != CookError::Success)
    {
        return std::unexpected(ReportError(diagnostics,
                                           capabilityAxisError,
                                           "an axis of kind capability does not change the capabilities that the code needs"));
    }

    const CookError emitError = EmitLibraryArtifacts(library, sink, diagnostics);
    if (!emitError)
    {
        return std::unexpected(emitError);
    }

    if (cookState.Options.WriteDepfile)
    {
        if (cookState.Options.PolicyFile.has_value())
        {
            dependencyPaths.push_back(NormalizeDependencyPath(cookState.Options.PolicyFile.value()));
        }
        std::ranges::sort(dependencyPaths);
        const auto [duplicatesBegin, duplicatesEnd] = std::ranges::unique(dependencyPaths);
        dependencyPaths.erase(duplicatesBegin, duplicatesEnd);

        const std::string target = NormalizeDependencyPath(cookState.Options.OutputPath / k_ManifestFileName);
        const CookError depfileError = sink.WriteArtifact(k_DepfileName, GenerateDepfile(target, dependencyPaths));
        if (!depfileError)
        {
            return std::unexpected(depfileError);
        }
    }

    const std::chrono::steady_clock::time_point endTime = std::chrono::steady_clock::now();
    const std::chrono::duration<double, std::milli> elapsed = endTime - startTime;
    cookStatistics.ElapsedMilliseconds = elapsed.count();
    return cookStatistics;
}

namespace
{

    /** A difference means an unordered container's order reached the output. */
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

namespace
{
    CookedLibrary MakeEmptyLibrary(const SharedCookState& cook_state)
    {
        CookedLibrary library;
        library.ModuleNames = cook_state.AllModuleNames;
        library.Profiles.reserve(cook_state.Options.TargetNames.size());
        for (const std::string& targetName : cook_state.Options.TargetNames)
        {
            const TargetProfile& profile = cook_state.TargetProfiles.at(targetName);
            library.Profiles.push_back(CookedProfile{ .TargetName = targetName,
                                                          .AccessModel = PlacementKindFromAccessModel(profile.Access),
                                                          .CodeFormat = CodeFormatFromLanguage(profile.Language),
                                                          .SamplerTexturePairsRecorded = profile.Validator != nullptr,
                                                          .CodeFactsRead = profile.Validator != nullptr });
        }

        library.Environments.resize(library.Profiles.size() * library.ModuleNames.size());
        return library;
    }

    CookError EmitLibraryArtifacts(const CookedLibrary& library, OutputSink& sink, DiagnosticSink& diagnostics)
    {
        CookResult<std::vector<std::byte>> manifest = EmitShaderManifest(library, diagnostics);
        if (!manifest)
        {
            return manifest.error();
        }

        const CookError roundTripError = VerifyManifestRoundTrip(library, *manifest, diagnostics);
        if (!roundTripError)
        {
            return roundTripError;
        }

        const CookError manifestWriteError = sink.WriteArtifact(k_ManifestFileName, *manifest);
        if (!manifestWriteError)
        {
            return manifestWriteError;
        }

        const CookResult<ResourceReport> resourceReport = BuildResourceReport(*manifest, diagnostics);
        if (!resourceReport)
        {
            return resourceReport.error();
        }

        const CookError reportWriteError =
            sink.WriteArtifact(k_ResourceReportFileName, FormatResourceReport(resourceReport.value()));
        if (!reportWriteError)
        {
            return reportWriteError;
        }

        return sink.WriteArtifact(k_DedupeReportFileName, GenerateDedupeReport(library));
    }
} // namespace

CookResult<CookStatistics> RunCook(CookerOptions options, OutputSink& sink)
{
    StderrDiagnosticSink diagnostics;
    return RunCook(std::move(options), sink, diagnostics);
}

CookResult<CookStatistics> RunCook(CookerOptions options, OutputSink& sink, DiagnosticSink& diagnostics)
{
    const std::string_view artifactNames[4]
    {
        k_ManifestFileName,
        k_DedupeReportFileName,
        k_ResourceReportFileName,
        k_DepfileName
    };

    // a failed cook must leave no stale manifest for a reader to take as its output
    for (const std::string_view artifactName : artifactNames)
    {
        if (sink.RemoveArtifact(artifactName) != CookError::Success)
        {
            return std::unexpected(ReportError(diagnostics,
                                               CookError::OutputWriteFailed,
                                               std::format("cannot remove '{}' of an earlier cook from '{}'",
                                                           artifactName,
                                                           sink.Describe())));
        }
    }

    if (options.VerifyDeterministic)
    {
        return RunCookTwiceAndCompare(std::move(options), sink, diagnostics);
    }

    return RunCookOnce(std::move(options), sink, diagnostics);
}

} // namespace lodestone
