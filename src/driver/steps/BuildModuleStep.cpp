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
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lodestone
{

namespace
{
    void RecordVariantStatistics(const CompiledVariant& variant, CookStatistics& statistics);
    // writes a bunch of information to the diagnostics sink about the given variant's entry point reflection
    void ReportEntryPointReflection(const CompiledVariant& variant,
                                    size_t entry_point_index,
                                    DiagnosticSink& diagnostics);
    void ReportVariantIfRequested(const CookerOptions& options,
                                  const CompiledVariant& variant,
                                  DiagnosticSink& diagnostics);
    // reads the emitted code of each entry point back with the target's reader. It always reads the sampler and
    // texture pairs, which are cook output, into `variant`. When `validate` is true, it also runs the reflection
    // cross check on the same parse, and returns the mismatch count. A target with no reader fails when
    // `validate` is true, and otherwise leaves the pairs empty (the profile records that they were not read).
    // The capabilities the validator reads are merged into `observed_capabilities`, sorted and unique.
    CookResult<uint32_t> ReadEmittedCode(const TargetProfile& target,
                                         bool validate,
                                         CompiledVariant& variant,
                                         std::vector<std::string>& observed_capabilities,
                                         DiagnosticSink& diagnostics);
    // maps the pairs a target reader gives, by placement, to positions in the entry point's used bindings, then
    // sorts them and removes duplicates. A placement that no used binding has, or a pair that is not a texture and
    // a sampler, fails: the reader and reflection disagree, and a dropped pair would hide a sampler constraint.
    CookError ResolveSamplerTexturePairs(CompiledVariant& variant,
                                         size_t entry_point_index,
                                         std::span<const SamplerTexturePlacement> placements,
                                         DiagnosticSink& diagnostics);
    // settles the entry point constants of one entry point against its emitted code. Reflection states a push
    // constant block whenever the parameters contain ordinary data, and Slang's metadata does not report whether the
    // entry point reads it, so the code decides: no block in the code means that Slang removed unused data. A block
    // that reflection does not state, or that covers more bytes than reflection's size, is a reflection mismatch. A
    // block larger than the profile's limit fails with `EntryPointConstantsTooLarge`.
    CookError ReconcileEntryPointConstants(const TargetProfile& target,
                                           CompiledEntryPoint& entry_point,
                                           std::optional<uint32_t> push_constant_extent,
                                           DiagnosticSink& diagnostics);
    // Workaround for shader-slang/slang#13789. Slang's metadata reports every resource in the element of the entry
    // point constants' buffer as used when any part of the buffer is used. This removes such a resource from the
    // entry point's visibility when the emitted code does not declare it, and prints a note for each one.
    // SlangMetadataCanaryTest fails when Slang fixes the issue: then delete this function, its call, and
    // `ConstantsElementBindings`.
    CookError ApplySlangWorkaround13789(const TargetProfile& target,
                                        CompiledVariant& variant,
                                        size_t entry_point_index,
                                        DiagnosticSink& diagnostics);
    // the two steps that turn the facts of one entry point's emitted code into cook output
    CookError ApplyEmittedCodeFacts(const TargetProfile& target,
                                    CompiledVariant& variant,
                                    size_t entry_point_index,
                                    const EmittedCodeFacts& facts,
                                    DiagnosticSink& diagnostics);
    void ReportUnreferencedBindings(const CompiledVariant& variant, DiagnosticSink& diagnostics);
    [[nodiscard]] CookResult<CookStatistics> CompileModuleVariants(const SharedCookState& shared_state,
                                                                   const TargetProfile& target,
                                                                   SlangCompiler& compiler,
                                                                   const VariantSet& variant_set,
                                                                   InternedModule& interned_module,
                                                                   RawModule& raw_module,
                                                                   const DerivedConstantTable& derived_constants,
                                                                   std::vector<CompiledVariant>& out_module_variants);
}

CookResult<BuiltModule> BuildModuleStep::operator()(const SharedCookState& shared_state,
                                                    const std::string_view& module_name,
                                                    const std::string_view& target_name,
                                                    SlangCompiler* compiler,
                                                    const PermutationSpace& space,
                                                    const VariantSet& variants) const
{
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

    // points into rawModule.DerivedConstants, which the variant compile below does not change
    const CookResult<DerivedConstantTable> derivedConstants =
        MakeDerivedConstantTable(rawModule.DerivedConstants, space.Axes(), rawModule.ExternDefaults, *shared_state.Diagnostics);
    if (!derivedConstants)
    {
        const std::string errStr = std::format("module '{}' failed: {}", module_name, ToString(derivedConstants.error()));
        return std::unexpected(ReportError(*shared_state.Diagnostics, derivedConstants.error(), errStr));
    }

    CookResult<CookStatistics> compileVariantsResult = CompileModuleVariants(shared_state,
                                                                             *targetProfileResult,
                                                                             *compiler,
                                                                             variants,
                                                                             internedModule,
                                                                             rawModule,
                                                                             derivedConstants.value(),
                                                                             compiledVariants);

    if (!compileVariantsResult)
    {
        return std::unexpected(compileVariantsResult.error());
    }

    // The raw dump comes after the compile, because the compile keeps the raw variants in rawModule.
    // Dump rawModule, not rawModuleResult: the move above empties rawModuleResult.
    std::optional<std::string> rawModuleDump = std::nullopt;
    if (IsStageDumpRequested(shared_state.Options, StageDumpKind::Raw))
    {
        rawModuleDump = DumpRawModule(rawModule);
    }

    std::optional<std::string> resolvedModuleDump = std::nullopt;
    if (IsStageDumpRequested(shared_state.Options, StageDumpKind::Resolved))
    {
        resolvedModuleDump = DumpResolvedModule(module_name, compiledVariants);
    }

    std::optional<std::string> internedModuleDump = std::nullopt;
    if (IsStageDumpRequested(shared_state.Options, StageDumpKind::Interned))
    {
        internedModuleDump = DumpInternedModule(internedModule);
    }

    return BuiltModule{
        .Module = std::move(internedModule),
        .CompiledVariants = std::move(compiledVariants),
        .RawModuleDump = std::move(rawModuleDump),
        .ResolvedModuleDump = std::move(resolvedModuleDump),
        .InternedModuleDump = std::move(internedModuleDump),
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

    CookResult<uint32_t> ReadEmittedCode(const TargetProfile& target,
                                         bool validate,
                                         CompiledVariant& variant,
                                         std::vector<std::string>& observed_capabilities,
                                         DiagnosticSink& diagnostics)
    {
        if (target.Validator == nullptr)
        {
            if (validate)
            {
                return std::unexpected(CookError::TargetValidatorUnavailable);
            }
            return 0u;
        }

        uint32_t mismatchCount = 0u;

        for (size_t i = 0u; i < variant.EntryPoints.size(); ++i)
        {
            const CookError workaround = ApplySlangWorkaround13789(target, variant, i, diagnostics);
            if (workaround != CookError::Success)
            {
                return std::unexpected(workaround);
            }

            const CompiledEntryPoint& entryPoint = variant.EntryPoints[i];
            if (!validate)
            {
                const CookResult<EmittedCodeFacts> facts =
                    target.Validator->ReadEmittedCodeFacts(entryPoint.Code, diagnostics);

                if (!facts)
                {
                    return std::unexpected(facts.error());
                }

                const CookError applied = ApplyEmittedCodeFacts(target, variant, i, *facts, diagnostics);

                if (applied != CookError::Success)
                {
                    return std::unexpected(applied);
                }

                continue;
            }

            auto extractBinding = [&variant](const uint32_t binding_index)-> const ReflectedBinding*
            {
                return &variant.Bindings[binding_index];
            };

            std::vector<const ReflectedBinding*> used = entryPoint.Reflection.UsedBindingIndices |
                                                        std::views::transform(extractBinding) |
                                                        std::ranges::to<std::vector>();

            const CookResult<BindingComparison> comparison =
                target.Validator->ValidateEntryPoint(entryPoint.Code, used, diagnostics);

            if (!comparison)
            {
                // because most validators load a backend for parsing the source code, there are
                // failures not related to reflection mismatches that may occur
                // (e.g, the backend parser failed to parse the entry point code)
                return std::unexpected(comparison.error());
            }

            // a module declares a handful of capabilities, so a sorted insert stays cheap
            for (const std::string& capability : comparison->Capabilities)
            {
                const auto position = std::ranges::lower_bound(observed_capabilities, capability);
                if ((position == observed_capabilities.end()) || (*position != capability))
                {
                    observed_capabilities.insert(position, capability);
                }
            }

            if (!comparison->Matches)
            {
                ++mismatchCount;
                const std::string warningStr =
                    std::format("REFLECTION MISMATCH in {}{} ({}) for target {}:\n{}",
                                entryPoint.Name,
                                entryPoint.VariantSuffix,
                                variant.VariantDescription,
                                target.Name,
                                comparison->Report);
                ReportWarning(diagnostics, warningStr);
                // a mismatch fails the cook, and the pairs of a mismatched entry point need not resolve
                continue;
            }

            const CookError applied = ApplyEmittedCodeFacts(target, variant, i, comparison->Emitted, diagnostics);

            if (applied != CookError::Success)
            {
                return std::unexpected(applied);
            }
        }

        return mismatchCount;
    }

    CookError ApplySlangWorkaround13789(const TargetProfile& target,
                                        CompiledVariant& variant,
                                        size_t entry_point_index,
                                        DiagnosticSink& diagnostics)
    {
        CompiledEntryPoint& entryPoint = variant.EntryPoints[entry_point_index];
        std::vector<uint32_t>& elementBindings = entryPoint.Reflection.ConstantsElementBindings;
        if (elementBindings.empty())
        {
            return CookError::Success;
        }

        const CookResult<EmittedCodeFacts> facts = target.Validator->ReadEmittedCodeFacts(entryPoint.Code, diagnostics);
        if (!facts)
        {
            return facts.error();
        }

        // the element follows the container in placement order, so a removal never moves the container's position
        std::vector<uint32_t>& used = entryPoint.Reflection.UsedBindingIndices;
        for (const uint32_t bindingIndex : elementBindings)
        {
            const ReflectedBinding& binding = variant.Bindings[bindingIndex];
            const BoundPlacement* placement = GetBoundPlacement(binding.Placement);
            const auto key = std::pair{ placement->Group, placement->Binding };
            const bool declared = std::ranges::find(facts->DeclaredPlacements, key) != facts->DeclaredPlacements.end();
            const auto position = std::ranges::find(used, bindingIndex);
            if (declared || position == used.end())
            {
                continue;
            }

            used.erase(position);
            ReportWarning(diagnostics,
                       std::format("workaround for shader-slang/slang#13789: {}{} for target {} does not use '{}', "
                                   "which Slang's metadata reports as used",
                                   entryPoint.Name,
                                   entryPoint.VariantSuffix,
                                   target.Name,
                                   binding.Name));
        }

        return CookError::Success;
    }

    CookError ApplyEmittedCodeFacts(const TargetProfile& target,
                                    CompiledVariant& variant,
                                    size_t entry_point_index,
                                    const EmittedCodeFacts& facts,
                                    DiagnosticSink& diagnostics)
    {
        const CookError resolved =
            ResolveSamplerTexturePairs(variant, entry_point_index, facts.SamplerTexturePairs, diagnostics);
        if (resolved != CookError::Success)
        {
            return resolved;
        }

        return ReconcileEntryPointConstants(target,
                                            variant.EntryPoints[entry_point_index],
                                            facts.PushConstantExtent,
                                            diagnostics);
    }

    CookError ReconcileEntryPointConstants(const TargetProfile& target,
                                           CompiledEntryPoint& entry_point,
                                           std::optional<uint32_t> push_constant_extent,
                                           DiagnosticSink& diagnostics)
    {
        EntryPointConstants& constants = entry_point.Reflection.Constants;
        const bool reflected = constants.Delivery == EntryPointConstantsDelivery::PushConstant;
        if (!push_constant_extent)
        {
            if (reflected)
            {
                constants = EntryPointConstants{};
            }
            return CookError::Success;
        }

        if (!reflected || *push_constant_extent > constants.ByteSize)
        {
            const std::string errStr = std::format(
                "REFLECTION MISMATCH in {}{} for target {}: the code declares a push constant block of {} bytes, "
                "and reflection states {}",
                entry_point.Name,
                entry_point.VariantSuffix,
                target.Name,
                *push_constant_extent,
                reflected ? std::format("{} bytes", constants.ByteSize) : std::string{ "none" });
            return ReportError(diagnostics, CookError::ReflectionMismatch, errStr);
        }

        if (constants.ByteSize > target.MaxPushConstantBytes)
        {
            const std::string errStr = std::format(
                "entry point {}{} for target {} has a push constant block of {} bytes, and every device of the "
                "target supports only {}. Move part of the data into a buffer",
                entry_point.Name,
                entry_point.VariantSuffix,
                target.Name,
                constants.ByteSize,
                target.MaxPushConstantBytes);
            return ReportError(diagnostics, CookError::EntryPointConstantsTooLarge, errStr);
        }

        return CookError::Success;
    }

    CookError ResolveSamplerTexturePairs(CompiledVariant& variant,
                                         size_t entry_point_index,
                                         std::span<const SamplerTexturePlacement> placements,
                                         DiagnosticSink& diagnostics)
    {
        CompiledEntryPoint& entryPoint = variant.EntryPoints[entry_point_index];
        const std::vector<uint32_t>& used = entryPoint.Reflection.UsedBindingIndices;

        // the position in `used` of the binding at (group, binding), when it has the expected kind
        auto findPosition = [&](uint32_t group, uint32_t binding, BindingKind kind) -> std::optional<uint32_t>
        {
            for (size_t position = 0u; position < used.size(); ++position)
            {
                const ReflectedBinding& candidate = variant.Bindings[used[position]];
                const BoundPlacement* placement = GetBoundPlacement(candidate.Placement);

                if (placement != nullptr && placement->Group == group && placement->Binding == binding)
                {
                    return candidate.Kind == kind ? std::optional<uint32_t>{ static_cast<uint32_t>(position) }
                                                  : std::nullopt;
                }

            }
            return std::nullopt;
        };

        std::vector<SamplerTexturePair> pairs;
        pairs.reserve(placements.size());
        for (const SamplerTexturePlacement& placement : placements)
        {

            const std::optional<uint32_t> texture =
                findPosition(placement.TextureGroup, placement.TextureBinding, BindingKind::Texture);
            const std::optional<uint32_t> sampler =
                findPosition(placement.SamplerGroup, placement.SamplerBinding, BindingKind::Sampler);

            if (!texture || !sampler)
            {
                const std::string errStr =
                    std::format("{}{} ({}): the emitted code samples the texture at ({}, {}) with the sampler at "
                                "({}, {}), and reflection has no used {} at that placement",
                                entryPoint.Name,
                                entryPoint.VariantSuffix,
                                variant.VariantDescription,
                                placement.TextureGroup,
                                placement.TextureBinding,
                                placement.SamplerGroup,
                                placement.SamplerBinding,
                                !texture ? "texture" : "sampler");
                return ReportError(diagnostics, CookError::ReflectionSamplerPairUnresolved, errStr);
            }
            
            pairs.push_back(SamplerTexturePair{ .Texture = *texture, .Sampler = *sampler });
        }

        // a reader reports one pair for each use, so one pair can appear many times
        std::ranges::sort(pairs);
        const auto duplicates = std::ranges::unique(pairs);
        pairs.erase(duplicates.begin(), duplicates.end());
        entryPoint.SamplerTexturePairs = std::move(pairs);
        return CookError::Success;
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
                                                                   const DerivedConstantTable& derived_constants,
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

            const RawVariant& currRawVariant = result.value();
            CookResult<ResolveContext> contextResult = MakeResolveContext(currVariant.Canonical,
                                                                          raw_module.ExternDefaults,
                                                                          derived_constants,
                                                                          currRawVariant,
                                                                          *shared_state.Diagnostics);
            if (!contextResult)
            {
                const std::string errStr = std::format("variant [{}] failed: {}",
                                                       DescribeAssignment(currVariant.Canonical),
                                                       ToString(contextResult.error()));
                return std::unexpected(ReportError(*shared_state.Diagnostics, contextResult.error(), errStr));
            }

            CookResult<CompiledVariant> variantResult = ResolveVariant(currRawVariant, contextResult.value(), *shared_state.Diagnostics);
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

            CompiledVariant& variant = variantResult.value();
            RecordVariantStatistics(variant, localStats);
            ReportVariantIfRequested(shared_state.Options, variant, *shared_state.Diagnostics);

            // the reader runs with --no-validate too, because the sampler and texture pairs are cook output
            const CookResult<uint32_t> mismatches = ReadEmittedCode(target,
                                                                    shared_state.Options.ValidateAgainstEmittedText,
                                                                    variant,
                                                                    interned_module.ObservedCapabilities,
                                                                    *shared_state.Diagnostics);
            if (!mismatches)
            {
                return std::unexpected(mismatches.error());
            }
            std::atomic_ref<uint32_t> mismatchesRef(localStats.ReflectionMismatches);
            mismatchesRef += *mismatches;

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
                AppendVariantToModule(interned_module, variant, currVariant);
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
