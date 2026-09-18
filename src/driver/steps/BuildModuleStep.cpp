#include "driver/steps/BuildModuleStep.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SlangCompiler.hpp"
#include "driver/CookerOptions.hpp"
#include "driver/CookerSteps.hpp"
#include "emit/StageDump.hpp"
#include "model/CookedLibrary.hpp"
#include "model/ResolveStage.hpp"
#include "model/ShaderDataSchema.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationSpace.hpp"
#include "target/TargetProfile.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lodestone
{

namespace
{
    // just adds the stats from `variant` to the running cook statistics
    void RecordVariantStatistics(const CompiledVariant& variant, CookStatistics& statistics);
    // writes a bunch of information to the diagnostics sink about the given variant's entry point reflection
    void ReportEntryPointReflection(const CompiledVariant& variant,
                                    size_t entry_point_index,
                                    DiagnosticSink& diagnostics);
    void ReportVariantIfRequested(const CookerOptions& options,
                                  const CompiledVariant& variant,
                                  DiagnosticSink& diagnostics);
    // reflection cross check: reads emitted text back and compares it against what the reflection
    // claims for the source text. each target decides how to read it's own output, so if `target`
    // is not valid or doesn't contain a validator the function will simply return 0 mismatches.
    uint32_t ValidateResolvedLibrary(const TargetProfile& target,
                                     const CompiledVariant& variant,
                                     DiagnosticSink& diagnostics);
    void ReportUnreferencedBindings(const CompiledVariant& variant, DiagnosticSink& diagnostics);
    [[nodiscard]] CookResult<CookStatistics> CompileModuleVariants(const SharedCookState& shared_state,
                                                                   const TargetProfile& target,
                                                                   SlangCompiler& compiler,
                                                                   const VariantSet& variant_set,
                                                                   InternedModule& interned_module,
                                                                   RawModule& raw_module,
                                                                   std::vector<CompiledVariant>& out_module_variants);
}

CookResult<BuiltModule> BuildModuleStep::operator()(const SharedCookState& shared_state,
                                                    const std::string_view& module_name,
                                                    const std::string_view& target_name,
                                                    SlangCompiler* compiler,
                                                    const PermutationSpace& space,
                                                    const VariantSet& variants) const
{
    // Implementation goes here
    InternedModule internedModule;
    if (!shared_state.Options.DedupeEnabled)
    {
        internedModule.DisableDedupe();
    }
    internedModule.Name = module_name;
    internedModule.Space = &space;
    internedModule.SpaceSize = variants.SpaceSize;
    internedModule.VariantKeys = variants.Variants |
                                 std::views::transform(&VariantDescriptor::Key) |
                                 std::ranges::to<std::vector>();

    std::vector<CompiledVariant> compiledVariants;
    compiledVariants.reserve(variants.Variants.size());

    CookResult<RawModule> rawModuleResult = compiler->PrepareRawModule(space);
    if (!rawModuleResult)
    {
        return std::unexpected(rawModuleResult.error());
    }


    RawModule rawModule{ std::move(*rawModuleResult) };
    CookResult<TargetProfile> targetProfileResult = FindTargetProfile(target_name);
    if (!targetProfileResult)
    {
        // this should not happen, since we also look this up during bootstrap, but I've been wrong before
        return std::unexpected(targetProfileResult.error());
    }
    CookResult<CookStatistics> compileVariantsResult = CompileModuleVariants(shared_state,
                                                                             *targetProfileResult,
                                                                             *compiler,
                                                                             variants,
                                                                             internedModule,
                                                                             rawModule,
                                                                             compiledVariants);
    
    if (!compileVariantsResult)
    {
        return std::unexpected(compileVariantsResult.error());
    }

    // Raw module dump can't actually happen until after the variants have all been built, weirdly enough
    std::optional<std::string> rawModuleDump = std::nullopt;
    if (IsStageDumpRequested(shared_state.Options, StageDumpKind::Raw))
    {
        rawModuleDump = DumpRawModule(rawModuleResult.value());
    }

    std::optional<std::string> resolvedModuleDump = std::nullopt;
    if (IsStageDumpRequested(shared_state.Options, StageDumpKind::Resolved))
    {
        // this should probably be renamed, since it's a bit confusing now
        resolvedModuleDump = DumpInternedModule(internedModule);
    }

    return BuiltModule{
        .Module = std::move(internedModule),
        .CompiledVariants = std::move(compiledVariants),
        .RawModuleDump = std::move(rawModuleDump),
        .ResolvedModuleDump = std::move(resolvedModuleDump),
        .Statistics = *compileVariantsResult
    };
}

namespace
{

    void RecordVariantStatistics(const CompiledVariant& variant, CookStatistics& statistics)
    {
        // because we plan to thread this step in the future, and I don't want this blowing up,
        // we use atomic references to safely update the statistics from multiple threads.
        std::atomic_ref<uint32_t> variantsCompiledRef(statistics.VariantsCompiled);
        std::atomic_ref<uint32_t> entryPointsCompiledRef(statistics.EntryPointsCompiled);
        std::atomic_ref<size_t> totalSourceBytesRef(statistics.TotalSourceBytes);

        ++variantsCompiledRef;
        entryPointsCompiledRef += static_cast<uint32_t>(variant.EntryPoints.size());

        for (const CompiledEntryPoint& entryPoint : variant.EntryPoints)
        {
            totalSourceBytesRef += entryPoint.Code.size();
        }
    }

    void ReportEntryPointReflection(const CompiledVariant& variant,
                                    size_t entry_point_index,
                                    DiagnosticSink& diagnostics)
    {
        const CompiledEntryPoint& entryPoint = variant.EntryPoints[entry_point_index];
        const std::string entryPointInfoStr = std::format("  {}{} [{}] workgroup {}x{}x{}",
                                                          entryPoint.Name,
                                                          entryPoint.VariantSuffix,
                                                          ToString(entryPoint.Reflection.Stage),
                                                          entryPoint.Reflection.Workgroup.X,
                                                          entryPoint.Reflection.Workgroup.Y,
                                                          entryPoint.Reflection.Workgroup.Z);
        ReportInfo(diagnostics, entryPointInfoStr);

        for (const ResolvedBindingView& resolved : BuildEntryPointLayoutView(variant, entry_point_index))
        {
            const std::string bindingInfoStr = std::format("    {}{}",
                                                           DescribeBinding(*resolved.Resource),
                                                           DescribeFootprint(*resolved.Footprint));
            ReportInfo(diagnostics, bindingInfoStr);

            const std::string members = DescribeUniformMembers(*resolved.Resource);
            if (!members.empty())
            {
                ReportInfo(diagnostics, members);
            }
        }

        const std::string raster = DescribeRasterState(entryPoint.Reflection.Raster);
        if (!raster.empty())
        {
            ReportInfo(diagnostics, raster);
        }
    }

    void ReportVariantIfRequested(const CookerOptions& options,
                                  const CompiledVariant& variant,
                                  DiagnosticSink& diagnostics)
    {
        if (!options.ReportReflection)
        {
            return;
        }

        ReportInfo(diagnostics, std::format("variant [{}]", variant.VariantDescription));
        for (size_t i = 0u; i < variant.EntryPoints.size(); ++i)
        {
            ReportEntryPointReflection(variant, i, diagnostics);
        }
    }

    uint32_t ValidateResolvedLibrary(const TargetProfile& target,
                                     const CompiledVariant& variant,
                                     DiagnosticSink& diagnostics)
    {
        if (target.Validator == nullptr)
        {
            return 0u;
        }

        uint32_t mismatchCount = 0u;

        for (size_t i = 0u; i < variant.EntryPoints.size(); ++i)
        {
            const CompiledEntryPoint& entryPoint = variant.EntryPoints[i];
            auto extractBinding = [&variant](const uint32_t binding_index)-> const ReflectedBinding*
            {
                return &variant.Bindings[binding_index];
            };

            std::vector<const ReflectedBinding*> used = variant.EntryPoints[i].Reflection.UsedBindingIndices |
                                                        std::views::transform(extractBinding) |
                                                        std::ranges::to<std::vector>();

            const BindingComparison comparison = target.Validator->ValidateEntryPoint(entryPoint.Code, used);

            if (!comparison.Matches)
            {
                ++mismatchCount;
                const std::string warningStr =
                    std::format("REFLECTION MISMATCH in {}{} ({}) for target {}:\n{}",
                                entryPoint.Name,
                                entryPoint.VariantSuffix,
                                variant.VariantDescription,
                                target.Name,
                                comparison.Report);
                ReportWarning(diagnostics, warningStr);
            }
        }

        return mismatchCount;
    }

    void ReportUnreferencedBindings(const CompiledVariant& variant, DiagnosticSink& diagnostics)
    {
        std::vector<uint8_t> used(variant.Bindings.size(), uint8_t{ 0 });
        for (const CompiledEntryPoint& entryPoint : variant.EntryPoints)
        {
            for (const auto& bindingIndex : entryPoint.Reflection.UsedBindingIndices)
            {
                used[bindingIndex] = static_cast<uint8_t>(true);
            }
        }

        for (const auto [index, isUsed] : std::views::enumerate(used))
        {
            if (!static_cast<bool>(isUsed))
            {
                ReportWarning(diagnostics,
                              std::format("unreferenced binding in [{}]: {} is declared but no entrypoint reads it",
                                variant.VariantDescription,
                                DescribeBinding(variant.Bindings[static_cast<size_t>(index)])));
            }
        }
    }

    [[nodiscard]] CookResult<CookStatistics> CompileModuleVariants(const SharedCookState& shared_state,
                                                                   const TargetProfile& target,
                                                                   SlangCompiler& compiler,
                                                                   const VariantSet& variant_set,
                                                                   InternedModule& interned_module,
                                                                   RawModule& raw_module,
                                                                   std::vector<CompiledVariant>& out_module_variants)
    {
        const bool keepRawVariants = IsStageDumpRequested(shared_state.Options, StageDumpKind::Raw);
        CookStatistics localStats{};
        auto compileResultsList = compiler.Compile(variant_set.Variants, *shared_state.Diagnostics);

        for (auto&& [idx, result] : std::views::enumerate(compileResultsList))
        {
            const auto& currVariant = variant_set.Variants[static_cast<size_t>(idx)];
            if (!result)
            {
                const std::string errStr = std::format("variant [{}] failed: {}",
                                                       DescribeAssignment(currVariant.Canonical),
                                                       ToString(result.error()));
                return std::unexpected(ReportError(*shared_state.Diagnostics, result.error(), errStr));
            }

            const ResolveContext context =
                MakeResolveContext(currVariant.Canonical, raw_module.ExternDefaults);
            CookResult<CompiledVariant> variantResult = ResolveVariant(result.value(), context, *shared_state.Diagnostics);
            if (!variantResult)
            {
                const std::string errStr = std::format("variant [{}] failed: {}",
                                                       DescribeAssignment(currVariant.Canonical),
                                                       ToString(variantResult.error()));
                return std::unexpected(ReportError(*shared_state.Diagnostics, variantResult.error(), errStr));
            }

            if (keepRawVariants)
            {
                raw_module.Variants.emplace_back(std::move(*result));
            }

            const CompiledVariant& variant = variantResult.value();
            RecordVariantStatistics(variant, localStats);
            ReportVariantIfRequested(shared_state.Options, variant, *shared_state.Diagnostics);

            if (shared_state.Options.ValidateAgainstEmittedText)
            {
                const uint32_t mismatches = ValidateResolvedLibrary(target, variant, *shared_state.Diagnostics);
                std::atomic_ref<uint32_t> mismatchesRef(localStats.ReflectionMismatches);
                mismatchesRef += mismatches;
            }

            if (shared_state.Options.ReportReflection)
            {
                ReportUnreferencedBindings(variant, *shared_state.Diagnostics);
            }

            // every variant shares the same entry points, so only add them once
            if (interned_module.EntryPoints.empty())
            {
                interned_module.EntryPoints.reserve(variant.EntryPoints.size());
                for (const CompiledEntryPoint& entryPoint : variant.EntryPoints)
                {
                    interned_module.EntryPoints.emplace_back(entryPoint.Name, entryPoint.Reflection.Stage);
                }
            }

            const CookError appendResult =
                AppendVariantToModule(interned_module, variant, currVariant.Canonical);
            if (appendResult != CookError::Success)
            {
                return std::unexpected(appendResult);
            }

            out_module_variants.emplace_back(std::move(*variantResult));
        }

        return localStats;
    }
}

}
