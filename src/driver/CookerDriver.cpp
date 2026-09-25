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
#include "emit/OutputSink.hpp"
#include "emit/ShaderManifestEmitter.hpp"
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
#include <utility>
#include <vector>
#include "emit/StageDump.hpp"
#include "magic_enum/magic_enum.hpp"
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

    /** An empty grid of (profile, module) environments, in the order the targets were named. */
    CookedLibrary MakeEmptyLibrary(const SharedCookState& cook_state);
    /** Stage 8: the bundle, its round trip, and the dedup report, all from the one frozen library. */
    CookError EmitLibraryArtifacts(const CookedLibrary& library, OutputSink& sink);
} // namespace

CookResult<CookStatistics> RunCookOnce(CookerOptions options,
                                       OutputSink& sink,
                                       DiagnosticSink& diagnostics)
{
    const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    CookStatistics cookStatistics;
    // note that none of these actually hold state, it just makes calling them as functors easier
    //NOLINTBEGIN(readability-identifier-naming)
    const static PrepareCookStep PrepareCook;
    const static PrepareModuleStep PrepareModule;
    const static PreparePermutationSpaceStep PreparePermutationSpace;
    const static BuildModuleStep BuildModule;
    const static FinalizeModuleStep FinalizeModule;
    //NOLINTEND(readability-identifier-naming)

    CookResult<PreparedCook> preparedCookResult = PrepareCook(std::move(options));
    if (!preparedCookResult)
    {
        return std::unexpected(preparedCookResult.error());
    }

    // now, read options from preparedCookResult
    const PreparedCook& preparedCook = *preparedCookResult;
    const SharedCookState& cookState = preparedCook.SharedState;

    CookedLibrary library = MakeEmptyLibrary(cookState);
    const size_t moduleCount = library.ModuleNames.size();
    // For now, cooked modules point into their own permutation spcae, and the libraries have to outlive
    // this loop. So, we keep the space in unique_ptrs (for now) so all axes stored in outputs remain valid.
    std::vector<std::unique_ptr<PermutationSpace>> permutationSpaces;
    permutationSpaces.reserve(library.Environments.size());

    for (auto&& [moduleIdx, modulePath] : std::views::enumerate(cookState.Options.ModulePaths))
    {
        for (auto&& [targetIdx, targetName] : std::views::enumerate(cookState.Options.TargetNames))
        {
            // Each module's preparation involves a slang bootstrap compile, which creates the compiler
            // we'll use later and walks the root (no specializations) source code of the module.
            CookResult<PreparedCompiler> preparedModuleResult = PrepareModule(cookState,
                                                                              modulePath,
                                                                              targetName);
            
            if (!preparedModuleResult)
            {
                return std::unexpected(preparedModuleResult.error());
            }

            const PreparedCompiler& preparedModule = *preparedModuleResult;
            // The preparation step allows us to do exactly this part - figure out what axes exist in the module's source code.
            std::vector<RawAxisDeclaration> rawAxes = preparedModule.Compiler->BuildAxisDeclarations();
            const std::string_view moduleName = cookState.AllModuleNames[static_cast<size_t>(moduleIdx)];
            const SymbolTable& symbolTable = preparedModule.Compiler->GetSymbolTable();
            // The permutation space needs the symbol table for the active module + the raw axes to build itself
            // With that, it can fully validate and get set up to generate all the possible permutations
            // (Which are returned in the variants table out of this - not actually containing source code,
            //  but just keys based on whatever axes and permutations exist in the module's source code)
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

            // This is the actual meat of the operation: this will use a thread pool to build all the permutations of the 
            // current module+target pairing concurrently. This step also runs interning, which deduplicates identical
            // data across permutations. This could probably be pulled out to this level, and ideally at some point I'll
            // do just that (we could fuse it across the *whole* library instead of per-module), but not yet.
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
            
            // operator+= uses atomic_ref to safely update the statistics in a potentially multithreaded context
            // we can improve this in the future, but for now that works just fine
            cookStatistics += builtModule.Statistics;

            // Last step: resolve everything into it's final form. This mostly just "consumes" the memory of the interner,
            // moving most of it's members into itself: but it also performs some final validation of the source code
            // and reflection data to make sure that interning/deduplication has been correctly applied.
            // The key "extra data" from this is just information on the efficiency of dedupe per field type.
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

            // Independent of --dump-stage. This block was once inside the Cooked dump branch, so
            // --dump-sources alone wrote nothing.
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

            // write into slot: when we thread this, that should just work since the vector is never resized after initial allocation
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

    const CookError emitError = EmitLibraryArtifacts(library, sink);
    if (!emitError)
    {
        return std::unexpected(emitError);
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
                                                          .CodeFormat = CodeFormatFromLanguage(profile.Language) });
        }

        library.Environments.resize(library.Profiles.size() * library.ModuleNames.size());
        return library;
    }

    CookError EmitLibraryArtifacts(const CookedLibrary& library, OutputSink& sink)
    {
        CookResult<std::vector<std::byte>> manifest = EmitShaderManifest(library);
        if (!manifest)
        {
            return manifest.error();
        }

        const CookError roundTripError = VerifyManifestRoundTrip(library, *manifest);
        if (!roundTripError)
        {
            return roundTripError;
        }

        const CookError manifestWriteError = sink.WriteArtifact(k_ManifestFileName, *manifest);
        if (!manifestWriteError)
        {
            return manifestWriteError;
        }

        return sink.WriteArtifact(k_DedupeReportFileName, GenerateDedupeReport(library));
    }
} // namespace

CookResult<CookStatistics> RunCook(CookerOptions options, OutputSink& sink)
{
    // create one diag sink for the *whole* cook
    StderrDiagnosticSink diagnostics;

    if (options.VerifyDeterministic)
    {
        return RunCookTwiceAndCompare(std::move(options), sink, diagnostics);
    }

    return RunCookOnce(std::move(options), sink, diagnostics);
}

} // namespace lodestone
