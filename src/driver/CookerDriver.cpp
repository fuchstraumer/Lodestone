#include "driver/CookerDriver.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "ShaderLibraryTypes.hpp"
#include "VariantKey.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SlangCompiler.hpp"
#include "compile/SymbolTable.hpp"
#include "driver/CookerOptions.hpp"
#include "emit/DedupeReport.hpp"
#include "emit/OutputSink.hpp"
#include "emit/ShaderManifestEmitter.hpp"
#include "emit/StageDump.hpp"
#include "model/CookedLibrary.hpp"
#include "model/ResolveStage.hpp"
#include "model/ShaderDataSchema.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationValue.hpp"
#include "permute/PolicyDocument.hpp"
#include "target/TargetProfile.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <ranges>
#include <ratio>
#include <span>
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

    /** Why the cross-check will or will not run for this cook. */
    std::string_view DescribeCrossCheckState(const TargetProfile& target,
                                             const CookerOptions& options) noexcept
    {
        if (target.Validator == nullptr)
        {
            return "no validator given/available for this target";
        }

        return options.ValidateAgainstEmittedText ? "on" : "off by --no-validate";
    }

    CookError EmitLibraryModules(const std::vector<CookedModule>& modules, OutputSink& sink)
    {
        for (const CookedModule& module : modules)
        {
            const std::string manifest = EmitShaderManifest(module);

            CookError manifestResult = VerifyManifestRoundTrip(module, manifest);
            if (manifestResult != CookError::Success)
            {
                return manifestResult;
            }

            auto makeManifestFileName = [](const std::string_view module_name)
            {
                return std::string(module_name) + ".ldmanifest";
            };

            manifestResult = sink.WriteArtifact(makeManifestFileName(module.Name), manifest);
            if (manifestResult != CookError::Success)
            {
                return manifestResult;
            }
        }

        return CookError::Success;
    }

} // namespace

CookResult<CookStatistics> RunCookOnce(const CookerOptions& options,
                                       OutputSink& sink,
                                       DiagnosticSink& diagnostics)
{
    

    if (statistics.ReflectionMismatches != 0u)
    {
        return std::unexpected(CookError::ReflectionMismatch);
    }

    const CookError emitResult = EmitLibraryArtifacts(library, sink);
    if (emitResult != CookError::Success)
    {
        return std::unexpected(emitResult);
    }

    const std::chrono::steady_clock::time_point endTime = std::chrono::steady_clock::now();
    const std::chrono::duration<double, std::milli> elapsed = endTime - startTime;
    statistics.ElapsedMilliseconds = elapsed.count();

    return statistics;
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

        // reset the permutation space: remember to remove this once we fix this 
        cookPermutationSpace.reset();

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
