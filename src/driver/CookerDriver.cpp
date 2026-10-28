#include "driver/CookerDriver.hpp"
#include "compile/SymbolTable.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SlangCompiler.hpp"
#include "driver/CookerOptions.hpp"
#include "driver/CookerSteps.hpp"
#include "driver/steps/PrepareCookStep.hpp"
#include "driver/steps/PrepareModuleStep.hpp"
#include "driver/steps/PreparePermutationSpaceStep.hpp"
#include "driver/steps/BuildModuleStep.hpp"
#include "driver/steps/FinalizeModuleStep.hpp"
#include "emit/DedupeReport.hpp"
#include "emit/DepFile.hpp"
#include "emit/OutputSink.hpp"
#include "emit/ResourceReport.hpp"
#include "emit/ShaderManifestEmitter.hpp"
#include "emit/ShaderManifestVerify.hpp"
#include "emit/StageDump.hpp"
#include "model/AxisInfluence.hpp"
#include "model/CookedLibrary.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifestFormat.hpp"
#include "target/TargetProfile.hpp"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <ratio>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include "magic_enum/magic_enum.hpp"

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

    /** Writes a stage dump that the step built, and does nothing for a stage that was not asked for. */
    CookError WriteStageDump(OutputSink& sink,
                             std::optional<std::string>& dump,
                             std::string_view module_name,
                             std::string_view target_name,
                             StageDumpKind kind)
    {
        if (!dump.has_value())
        {
            return CookError::Success;
        }

        return sink.WriteArtifact(BuildDumpFileName(module_name, target_name, kind), std::move(*dump));
    }

    /** One (module, target) pair through every per-module step. The returned module points into the space that
     * this function appends to `permutation_spaces`, so the caller keeps that vector until the emit is done. */
    CookResult<CookedModule> CookEnvironment(const SharedCookState& cook_state,
                                             size_t module_index,
                                             const std::string& target_name,
                                             OutputSink& sink,
                                             std::vector<std::unique_ptr<PermutationSpace>>& permutation_spaces,
                                             std::vector<std::string>& dependency_paths,
                                             CookStatistics& cook_statistics)
    {
        //NOLINTBEGIN(readability-identifier-naming)
        const static PrepareModuleStep PrepareModule;
        const static PreparePermutationSpaceStep PreparePermutationSpace;
        const static BuildModuleStep BuildModule;
        const static FinalizeModuleStep FinalizeModule;
        //NOLINTEND(readability-identifier-naming)

        const std::filesystem::path& modulePath = cook_state.Options.ModulePaths[module_index];
        CookResult<PreparedCompiler> preparedModuleResult = PrepareModule(cook_state, modulePath, target_name);
        if (!preparedModuleResult)
        {
            return std::unexpected(preparedModuleResult.error());
        }

        const PreparedCompiler& preparedModule = *preparedModuleResult;
        if (cook_state.Options.WriteDepfile)
        {
            for (const std::string& dependencyPath : preparedModule.Compiler->DependencyFilePaths())
            {
                dependency_paths.push_back(NormalizeDependencyPath(dependencyPath));
            }
        }

        CookResult<std::vector<RawAxisDeclaration>> rawAxesResult = preparedModule.Compiler->BuildAxisDeclarations();
        if (!rawAxesResult)
        {
            return std::unexpected(rawAxesResult.error());
        }

        const std::string_view moduleName = cook_state.AllModuleNames[module_index];
        const SymbolTable& symbolTable = preparedModule.Compiler->GetSymbolTable();
        CookResult<PreparedPermutationSpace> spaceResult = PreparePermutationSpace(cook_state,
                                                                                   moduleName,
                                                                                   target_name,
                                                                                   symbolTable,
                                                                                   std::move(rawAxesResult.value()));
        if (!spaceResult)
        {
            return std::unexpected(spaceResult.error());
        }

        PreparedPermutationSpace& preparedSpace = *spaceResult;
        const CookError spaceDumpError =
            WriteStageDump(sink, preparedSpace.SpaceDump, moduleName, target_name, StageDumpKind::Space);
        if (!spaceDumpError)
        {
            return std::unexpected(spaceDumpError);
        }

        const CookError variantDumpError =
            WriteStageDump(sink, preparedSpace.VariantDump, moduleName, target_name, StageDumpKind::Variants);
        if (!variantDumpError)
        {
            return std::unexpected(variantDumpError);
        }

        // todo: intern across the whole library, not per module
        permutation_spaces.emplace_back(std::make_unique<PermutationSpace>(std::move(preparedSpace.Space)));
        CookResult<BuiltModule> buildResult = BuildModule(cook_state,
                                                          moduleName,
                                                          target_name,
                                                          preparedModule.Compiler.get(),
                                                          *permutation_spaces.back(),
                                                          preparedSpace.Variants);
        if (!buildResult)
        {
            return std::unexpected(buildResult.error());
        }

        BuiltModule& builtModule = *buildResult;
        const CookError rawDumpError =
            WriteStageDump(sink, builtModule.RawModuleDump, moduleName, target_name, StageDumpKind::Raw);
        if (!rawDumpError)
        {
            return std::unexpected(rawDumpError);
        }

        const CookError resolvedDumpError =
            WriteStageDump(sink, builtModule.ResolvedModuleDump, moduleName, target_name, StageDumpKind::Resolved);
        if (!resolvedDumpError)
        {
            return std::unexpected(resolvedDumpError);
        }

        const CookError internedDumpError =
            WriteStageDump(sink, builtModule.InternedModuleDump, moduleName, target_name, StageDumpKind::Interned);
        if (!internedDumpError)
        {
            return std::unexpected(internedDumpError);
        }

        cook_statistics += builtModule.Statistics;

        CookResult<FinalizedModule> finalizeResult = FinalizeModule(cook_state,
                                                                    std::move(builtModule.Module),
                                                                    builtModule.CompiledVariants);
        if (!finalizeResult)
        {
            return std::unexpected(finalizeResult.error());
        }

        const CookError cookedDumpError =
            WriteStageDump(sink, finalizeResult->Dump, moduleName, target_name, StageDumpKind::Cooked);
        if (!cookedDumpError)
        {
            return std::unexpected(cookedDumpError);
        }

        // independent of --dump-stage: it was once inside the Cooked dump branch, and wrote nothing alone
        if (cook_state.Options.DumpSources)
        {
            const std::string outputDir = std::format("{}_{}_sources", moduleName, target_name);
            const ShaderCodeFormat codeFormat =
                CodeFormatFromLanguage(cook_state.TargetProfiles.at(target_name).Language);
            const CookError writeError = DumpShaderSources(finalizeResult->Module, outputDir, codeFormat, sink);
            if (!writeError)
            {
                return std::unexpected(writeError);
            }
        }

        return std::move(finalizeResult->Module);
    }
} // namespace

CookResult<CookStatistics> RunCookOnce(CookerOptions options,
                                       OutputSink& sink,
                                       DiagnosticSink& diagnostics)
{
    const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    CookStatistics cookStatistics;
    //NOLINTNEXTLINE(readability-identifier-naming)
    const static PrepareCookStep PrepareCook;

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

    for (size_t moduleIndex = 0u; moduleIndex < moduleCount; ++moduleIndex)
    {
        for (auto&& [targetIdx, targetName] : std::views::enumerate(cookState.Options.TargetNames))
        {
            CookResult<CookedModule> cooked = CookEnvironment(cookState,
                                                              moduleIndex,
                                                              targetName,
                                                              sink,
                                                              permutationSpaces,
                                                              dependencyPaths,
                                                              cookStatistics);
            if (!cooked)
            {
                return std::unexpected(cooked.error());
            }

            const size_t environmentIndex = (static_cast<size_t>(targetIdx) * moduleCount) + moduleIndex;
            library.Environments[environmentIndex] = std::move(*cooked);
        }

        cookStatistics.ModulesCooked += 1;
    }

    if (cookStatistics.ReflectionMismatches != 0u)
    {
        const std::string errStr = std::format("{} entry point variants disagree with the emitted target text",
                                               cookStatistics.ReflectionMismatches);
        return std::unexpected(ReportError(diagnostics, CookError::ReflectionMismatch, errStr));
    }

    // each validator reports its own record
    const CookError pairsError = VerifySamplerPairsAcrossProfiles(library, diagnostics);
    if (!pairsError)
    {
        return std::unexpected(pairsError);
    }

    const CookError factsError = VerifySourceFactsAcrossProfiles(library, diagnostics);
    if (!factsError)
    {
        return std::unexpected(factsError);
    }

    const CookError capabilityAxisError = VerifyCapabilityAxes(library, diagnostics);
    if (!capabilityAxisError)
    {
        return std::unexpected(capabilityAxisError);
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
    CookResult<CookStatistics> RunCookTwiceAndCompare(CookerOptions options,
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
                                                          .Forms = PlacementFormsOf(profile.Forms),
                                                          .CodeFormat = CodeFormatFromLanguage(profile.Language),
                                                          .SamplerTexturePairsRecorded = profile.Validator != nullptr,
                                                          .CodeFactsRead = profile.Validator != nullptr,
                                                          .OptimizationLevel = cook_state.Options.OptimizationLevel,
                                                          .DebugLevel = cook_state.Options.DebugLevel,
                                                          .HeapLowering = profile.DescriptorHeapSet.has_value()
                                                                              ? manifest::DescriptorHeapLowering::MutableDescriptor
                                                                              : manifest::DescriptorHeapLowering::None,
                                                          .DescriptorHeapSet = profile.DescriptorHeapSet.value_or(0u) });
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
