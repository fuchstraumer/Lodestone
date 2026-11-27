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
#include "emit/ArtifactNames.hpp"
#include "emit/DedupeReport.hpp"
#include "emit/DepFile.hpp"
#include "emit/HostHeaderEmitter.hpp"
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
#include <array>
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
    std::string BuildDumpFileName(std::string_view module_name, std::string_view target_name, StageDumpKind kind)
    {
        return std::format("{}_{}_{}.json", module_name, target_name, magic_enum::enum_name(kind));
    }

    /** Adds the setup timings of a compiler to the process timings when the scope of the compiler ends, so that
     * an error return does not lose them. */
    class CompilerTimingsReporter
    {
    public:
        explicit CompilerTimingsReporter(const SlangCompiler& _compiler) noexcept;
        ~CompilerTimingsReporter();
        CompilerTimingsReporter(const CompilerTimingsReporter&) = delete;
        CompilerTimingsReporter& operator=(const CompilerTimingsReporter&) = delete;

    private:
        const SlangCompiler& compiler;
    };

    CookedLibrary MakeEmptyLibrary(const SharedCookState& cook_state);
    CookError EmitLibraryArtifacts(const CookedLibrary& library,
                                   const CookerOptions& options,
                                   OutputSink& sink,
                                   DiagnosticSink& diagnostics);

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

        const std::chrono::steady_clock::time_point stage1Start = std::chrono::steady_clock::now();

        const std::filesystem::path& modulePath = cook_state.Options.ModulePaths[module_index];
        CookResult<PreparedCompiler> preparedModuleResult = PrepareModule(cook_state, modulePath, target_name);
        if (!preparedModuleResult)
        {
            return std::unexpected(preparedModuleResult.error());
        }

        const PreparedCompiler& preparedModule = *preparedModuleResult;
        const CompilerTimingsReporter timingsReporter{ *preparedModule.Compiler };
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

        const std::chrono::steady_clock::time_point stage1End = std::chrono::steady_clock::now();
        const std::chrono::duration<double, std::milli> stage1Elapsed = stage1End - stage1Start;

        const std::chrono::steady_clock::time_point stage2Start = std::chrono::steady_clock::now();

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

        const std::chrono::steady_clock::time_point stage2End = std::chrono::steady_clock::now();
        const std::chrono::duration<double, std::milli> stage2Elapsed = stage2End - stage2Start;

        AddPhaseTimings(PhaseTimings{
            .Stage1_DeclareMs = stage1Elapsed.count(),
            .Stage2_EnumerateMs = stage2Elapsed.count()
        });

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

    const std::chrono::steady_clock::time_point prepStart = std::chrono::steady_clock::now();
    CookResult<PreparedCook> preparedCookResult = PrepareCook(std::move(options), diagnostics);
    const std::chrono::steady_clock::time_point prepEnd = std::chrono::steady_clock::now();
    if (!preparedCookResult)
    {
        return std::unexpected(preparedCookResult.error());
    }
    const std::chrono::duration<double, std::milli> prepElapsed = prepEnd - prepStart;

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

    const std::chrono::steady_clock::time_point crossStart = std::chrono::steady_clock::now();

    // each validator reports its own record
    const CookError pairsError = VerifySamplerPairsAcrossProfiles(library, diagnostics);
    if (!pairsError)
    {
        return std::unexpected(pairsError);
    }

    const CookError hostTypesError = VerifyHostTypesAcrossProfiles(library, diagnostics);
    if (!hostTypesError)
    {
        return std::unexpected(hostTypesError);
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

    const std::chrono::steady_clock::time_point crossEnd = std::chrono::steady_clock::now();
    const std::chrono::duration<double, std::milli> crossElapsed = crossEnd - crossStart;

    const std::chrono::steady_clock::time_point emitStart = std::chrono::steady_clock::now();

    const CookError emitError = EmitLibraryArtifacts(library, cookState.Options, sink, diagnostics);
    if (!emitError)
    {
        return std::unexpected(emitError);
    }

    if (cookState.Options.WriteDepfile)
    {
        for (const std::filesystem::path& policyFile : cookState.Options.PolicyFiles)
        {
            dependencyPaths.push_back(NormalizeDependencyPath(policyFile));
        }

        std::ranges::sort(dependencyPaths);
        const auto [duplicatesBegin, duplicatesEnd] = std::ranges::unique(dependencyPaths);
        dependencyPaths.erase(duplicatesBegin, duplicatesEnd);

        const std::string target = NormalizeDependencyPath(
            cookState.Options.OutputPath / MakeArtifactName(cookState.Options.OutputName, k_ManifestSuffix));
        const std::string depfileName = MakeArtifactName(cookState.Options.OutputName, k_DepfileSuffix);
        const CookError depfileError = sink.WriteArtifact(depfileName, GenerateDepfile(target, dependencyPaths));
        if (!depfileError)
        {
            return std::unexpected(depfileError);
        }
    }

    const std::chrono::steady_clock::time_point emitEnd = std::chrono::steady_clock::now();
    const std::chrono::duration<double, std::milli> emitElapsed = emitEnd - emitStart;

    AddPhaseTimings(PhaseTimings{
        .PrepareCookMs = prepElapsed.count(),
        .Validate_CrossProfileMs = crossElapsed.count(),
        .Stage8_EmitMs = emitElapsed.count(),
        .CooksRun = 1u
    });

    const std::chrono::steady_clock::time_point endTime = std::chrono::steady_clock::now();
    const std::chrono::duration<double, std::milli> elapsed = endTime - startTime;
    cookStatistics.ElapsedMilliseconds = elapsed.count();
    return cookStatistics;
}

namespace
{

    CompilerTimingsReporter::CompilerTimingsReporter(const SlangCompiler& _compiler) noexcept :
      compiler{ _compiler }
    {}

    CompilerTimingsReporter::~CompilerTimingsReporter()
    {
        AddPhaseTimings(PhaseTimings{ .Compile = compiler.Timings() });
    }

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
                                                          .FloatingPointMode = cook_state.Options.FloatingPointMode,
                                                          .HeapLowering = profile.HeapLowering,
                                                          .DescriptorHeapSet = profile.DescriptorHeapSet.value_or(0u) });
        }

        library.Environments.resize(library.Profiles.size() * library.ModuleNames.size());
        return library;
    }

    CookError EmitLibraryArtifacts(const CookedLibrary& library,
                                   const CookerOptions& options,
                                   OutputSink& sink,
                                   DiagnosticSink& diagnostics)
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

        const CookError manifestWriteError = sink.WriteArtifact(MakeArtifactName(options.OutputName, k_ManifestSuffix), *manifest);
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
            sink.WriteArtifact(MakeArtifactName(options.OutputName, k_ResourceReportSuffix),
                               FormatResourceReport(resourceReport.value()));
        if (!reportWriteError)
        {
            return reportWriteError;
        }

        const CookError dedupeWriteError = sink.WriteArtifact(MakeArtifactName(options.OutputName, k_DedupeReportSuffix),
                           GenerateDedupeReport(library));
        if (!dedupeWriteError)
        {
            return dedupeWriteError;
        }

        if (options.HostTypesInclude.empty())
        {
            return CookError::Success;
        }

        // the cook does not remove an earlier host header, and the sink keeps the timestamp of one whose text is
        // unchanged. A failed cook leaves the old header beside no manifest, so a consumer fails on the manifest.
        const std::string inlineName = MakeArtifactName(options.OutputName, k_HostInlineSuffix);
        const CookResult<HostHeaderFiles> hostFiles =
            EmitHostHeader(library, options.HostTypesInclude, options.HostNamespace, inlineName, diagnostics);
        if (!hostFiles)
        {
            return hostFiles.error();
        }

        // the inline file comes first, so that a header on disk never names an inline file that does not exist
        const CookError inlineWriteError = sink.WriteArtifact(inlineName, hostFiles->Inline);
        if (!inlineWriteError)
        {
            return inlineWriteError;
        }

        return sink.WriteArtifact(MakeArtifactName(options.OutputName, k_HostHeaderSuffix), hostFiles->Header);
    }
} // namespace

CookResult<CookStatistics> RunCook(CookerOptions options, OutputSink& sink)
{
    StderrDiagnosticSink diagnostics;
    return RunCook(std::move(options), sink, diagnostics);
}

CookResult<CookStatistics> RunCook(CookerOptions options, OutputSink& sink, DiagnosticSink& diagnostics)
{
    const std::array<std::string, 4u> artifactNames
    {
        MakeArtifactName(options.OutputName, k_ManifestSuffix),
        MakeArtifactName(options.OutputName, k_DedupeReportSuffix),
        MakeArtifactName(options.OutputName, k_ResourceReportSuffix),
        MakeArtifactName(options.OutputName, k_DepfileSuffix)
    };

    // a failed cook must leave no stale manifest for a reader to take as its output. Only the artifacts of this
    // output name are removed, so cooks with other names can share the folder
    for (const std::string& artifactName : artifactNames)
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
