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
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <print>
#include <ranges>
#include <ratio>
#include <string>
#include <string_view>
#include <unordered_map>
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
        return std::format("{}_{}_{}.json", module_name, target_name, magic_enum::enum_name(kind));
    }

    struct NameValuePair
    {
        std::string_view Name;
        std::string_view Value;
    };

    constexpr std::string_view TrimWhitespace(std::string_view str) noexcept
    {
        constexpr std::string_view k_WhiteSpaceChars = " \t\n\r\f";
        const size_t start = str.find_first_not_of(k_WhiteSpaceChars);
        if (start == std::string_view::npos)
        {
            return {};
        }
        const size_t end = str.find_last_not_of(k_WhiteSpaceChars);
        return str.substr(start, end - start + 1);
    }

    std::vector<NameValuePair> ParseVariantDescription(std::string_view description)
    {
        std::vector<NameValuePair> result;
        size_t start = 0;
        while (start < description.size())
        {
            // split between NAME=VALUE first, then use comma as where to close the pair
            size_t equalPos = description.find('=', start);
            if (equalPos == std::string_view::npos)
            {
                break;
            }
            size_t commaPos = description.find(',', equalPos);
            NameValuePair pair;
            pair.Name = description.substr(start, equalPos - start);
            if (commaPos == std::string_view::npos)
            {
                pair.Value = description.substr(equalPos + 1);
            }
            else
            {
                pair.Value = description.substr(equalPos + 1, commaPos - equalPos - 1);
            }

            result.emplace_back(pair);
            if (commaPos == std::string_view::npos)
            {
                break;
            }
            start = commaPos + 1;
        }
        return result;
    }

    CookError DumpSources(const CookedModule& module, std::string_view output_directory, OutputSink& sink)
    {
        // sneaky little attempt to create output directory, since it probably doesn't exist
        std::filesystem::path childDir(output_directory);
        std::filesystem::path sinkDir(sink.Describe());
        std::filesystem::path outputPath = sinkDir / childDir;
        if (!std::filesystem::exists(outputPath))
        {
            // we create this using full path, but later we just use output_directory
            std::filesystem::create_directories(outputPath);
        }
        // collect sources, and prepend the names of the variants that use them to the output files 
        // as a block comment

        // copy to dumpedSources - this means that the indices will be the same, so each variant can
        // directly index it's sources
        std::vector<std::string> dumpedSources = module.Sources;
        // store some metadata about the source usages, such as which variants use it. for single 
        // usages, we'll be able to set the filename easily based on this. for multiple usages,
        // we'll need to figure out how to name it accordingly
        // (this is usually down to the inert axes for an entrypoint)
        // which thus means the common factor among those will be why they deduped... that should
        // set the name, I think. How do we identify that?

        // everything can at least use string views, since the underlying strings are in the input objects

        struct SourceUsage
        {
            std::vector<std::string_view> VariantSuffixes;
            std::vector<std::string_view> VariantDescriptions;
        };
        std::unordered_map<uint32_t, SourceUsage> sourceUsageStrings;
        for (const auto& variant : module.Variants)
        {
            for (uint32_t sourceIdx : variant.SourceIndices)
            {
                auto& usage = sourceUsageStrings[sourceIdx];
                usage.VariantSuffixes.push_back(variant.Suffix);
                usage.VariantDescriptions.push_back(variant.Description);
            }
        }

        CookError result = CookError::Success;
        for (auto& [sourceIdx, usage] : sourceUsageStrings)
        {
            std::string& source = dumpedSources[sourceIdx];
            // prepend the usage comments to the source
            std::string usageComment = "/*\n";
            for (size_t i = 0; i < usage.VariantSuffixes.size(); ++i)
            {
                usageComment += std::format("Variant: {} - {}\n", usage.VariantSuffixes[i], usage.VariantDescriptions[i]);
            }
            usageComment += "*/\n";
            // insert range right up at the opening of the file
            source.insert_range(source.begin(), usageComment);
            // okay, now what do we use as the name?
            // if only one variant uses this source, that's just output_directory/ModuleName+Suffix.(targetExtension)
            if (usage.VariantSuffixes.size() == 1)
            {
                std::string outputName =
                    std::format("{}/{}{}-src{}.wgsl", output_directory, module.Name, usage.VariantSuffixes[0], sourceIdx);
                result = sink.WriteArtifact(outputName, source);
                if (!result)
                {
                    return result;
                }
            }
            else
            {
                // buckle up, this is going to kinda suck.
                struct NameWithValues
                {
                    std::string_view Name;
                    std::vector<std::string_view> Values;
                };
                std::vector<NameWithValues> axisDiffs;

                for (const auto&& [idx, description] : std::views::enumerate(usage.VariantDescriptions))
                {
                    std::vector<NameValuePair> parsed = ParseVariantDescription(description);
                    if (idx == 0)
                    {
                        for (auto&& [name, value] : parsed)
                        {
                            axisDiffs.emplace_back(NameWithValues{TrimWhitespace(name), {TrimWhitespace(value)}});
                        }
                    }
                    else
                    {
                        // Now we only add unique values
                        for (auto&& [idx, pair] : std::views::enumerate(parsed))
                        {
                            std::vector<std::string_view> uniqueValues = axisDiffs[idx].Values;
                            if (std::ranges::find(uniqueValues, TrimWhitespace(pair.Value)) == uniqueValues.end())
                            {
                                axisDiffs[idx].Values.emplace_back(TrimWhitespace(pair.Value));
                            }
                        }
                    }
                }

                // now build the suffix - collapsing information considerably
                std::string dedupSuffix{ "_" };
                for (auto&& [axisName, uniqueValues] : axisDiffs)
                {
                    dedupSuffix += axisName;
                    dedupSuffix += "-(";
                    if (uniqueValues.size() == 1)
                    {
                        dedupSuffix += uniqueValues.front();
                    }
                    else if (uniqueValues.size() <= 3)
                    {
                        for (size_t idx = 0; idx < uniqueValues.size(); ++idx)
                        {
                            if (idx > 0)
                            {
                                dedupSuffix += "-";
                            }
                            dedupSuffix += uniqueValues[idx];
                        }
                    }
                    else
                    {
                        // too many values, don't make the filename stupid long
                        dedupSuffix += "MixedVals";
                    }
                    // hyphen to break up axis:value pairs. _ is then the opening character for a suffix
                    dedupSuffix += ")_";
                }

                // append source idx at very end to make sure there's never a filename collision
                // if there is though, that means the interner is failing! (or disabled? Maybe?)
                std::string outputName =
                    std::format("{}/{}{}src{}.wgsl", output_directory, module.Name, dedupSuffix, sourceIdx);
                result = sink.WriteArtifact(outputName, source);
                if (result != CookError::Success)
                {
                    return result;
                }
            }

        }

        return result;
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

            // This is the actual meat of the operation: this will use a thread pool to build all the permutations of the 
            // current module+target pairing concurrently. This step also runs interning, which deduplicates identical
            // data across permutations. This could probably be pulled out to this level, and ideally at some point I'll
            // do just that (we could fuse it across the *whole* library instead of per-module), but not yet.
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

                if (cookState.Options.DumpSources)
                {
                    const std::string outputDir = std::string{ moduleName } + "_sources";
                    writeError = DumpSources(finalizeResult->Module, outputDir, sink);
                    if (!writeError)
                    {
                        return std::unexpected(writeError);
                    }
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
