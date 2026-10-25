#include "driver/steps/BuildModuleStep.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "ShaderLibraryTypes.hpp"
#include "VulkanCapabilityTable.hpp"
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
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <tuple>
#include <vector>

namespace lodestone
{

namespace
{
    void RecordVariantStatistics(const CompiledVariant& variant, CookStatistics& statistics);
    void ReportEntryPointReflection(const CompiledVariant& variant,
                                    size_t entry_point_index,
                                    DiagnosticSink& diagnostics);
    void ReportVariantIfRequested(const CookerOptions& options,
                                  const CompiledVariant& variant,
                                  DiagnosticSink& diagnostics);
    // always reads the cook output facts. With `validate`, also runs the cross-check on the same parse and returns the
    // mismatch count. A target with no reader fails only when `validate` is set
    CookResult<uint32_t> ReadEmittedCode(const TargetProfile& target,
                                         bool validate,
                                         CompiledVariant& variant,
                                         DiagnosticSink& diagnostics);
    // maps placement pairs to positions in the used bindings. Pairs that do not resolve fail, since a dropped pair
    // would hide a sampler constraint
    CookError ResolveSamplerTexturePairs(CompiledVariant& variant,
                                         size_t entry_point_index,
                                         std::span<const SamplerTexturePlacement> placements,
                                         std::optional<uint32_t> descriptor_heap_set,
                                         DiagnosticSink& diagnostics);
    // no block in the code means Slang removed unread data. A block that reflection does not state, or that covers
    // more bytes, is a mismatch
    CookError ReconcileEntryPointConstants(const TargetProfile& target,
                                           std::span<const ReflectedBinding> bindings,
                                           CompiledEntryPoint& entry_point,
                                           std::optional<uint32_t> push_constant_extent,
                                           DiagnosticSink& diagnostics);
    // the code decides which bindings an entry point uses: an optimizer removes bindings after Slang's metadata
    // marks them used. Removes each bound binding the code does not declare, and each value stored in it
    void KeepDeclaredBindings(CompiledVariant& variant,
                              size_t entry_point_index,
                              std::span<const std::pair<uint32_t, uint32_t>> declared_placements);
    CookError ApplyEmittedCodeFacts(const TargetProfile& target,
                                    CompiledVariant& variant,
                                    size_t entry_point_index,
                                    const EmittedCodeFacts& facts,
                                    DiagnosticSink& diagnostics);
    void ReportUnreferencedBindings(const CompiledVariant& variant, DiagnosticSink& diagnostics);
    [[nodiscard]] CookError RecordFootprintDeclarations(InternedModule& interned_module,
                                                        const RawVariant& raw,
                                                        const DerivedConstantTable& derived_constants,
                                                        DiagnosticSink& sink)
    {
        if (interned_module.Space == nullptr)
        {
            return CookError::Success;
        }

        std::vector<FootprintAxisDeclaration>& declarations = interned_module.FootprintDeclarations;
        for (const RawSizeAttribute& attribute : raw.SizeAttributes)
        {
            const RawBinding& binding = raw.Bindings[attribute.BindingIndex];
            const auto position = std::ranges::lower_bound(declarations,
                                                           std::tie(binding.ScopeName, binding.Name),
                                                           std::less{},
                                                           [](const FootprintAxisDeclaration& entry)
                                                           { return std::tie(entry.ScopeName, entry.Name); });
            if (position != declarations.end() && position->ScopeName == binding.ScopeName &&
                position->Name == binding.Name)
            {
                continue;
            }

            CookResult<std::vector<uint32_t>> axes = CollectDeclaredFootprintAxes(raw,
                                                                                 attribute.BindingIndex,
                                                                                 interned_module.Space->Axes(),
                                                                                 derived_constants,
                                                                                 sink);
            if (!axes)
            {
                return axes.error();
            }

            declarations.insert(position,
                                FootprintAxisDeclaration{ .ScopeName = binding.ScopeName,
                                                          .Name = binding.Name,
                                                          .Axes = std::move(axes.value()) });
        }

        return CookError::Success;
    }

    [[nodiscard]] CookResult<CookStatistics> CompileModuleVariants(const SharedCookState& shared_state,
                                                                   const TargetProfile& target,
                                                                   SlangCompiler& compiler,
                                                                   const VariantSet& variant_set,
                                                                   InternedModule& interned_module,
                                                                   RawModule& raw_module,
                                                                   const DerivedConstantTable& derived_constants,
                                                                   std::vector<CompiledVariant>& out_module_variants);
    // names the module file, the target, and the variant, as the record that caused the failure can name none
    [[nodiscard]] CookError ReportVariantFailure(const SharedCookState& shared_state,
                                                 const TargetProfile& target,
                                                 std::string_view module_name,
                                                 const VariantDescriptor& variant,
                                                 CookError error);

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
        // this should not happen, since bootstrap looks it up too, but I've been wrong before
        return std::unexpected(targetProfileResult.error());
    }

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

    // dump rawModule, not rawModuleResult: the move above empties it
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
            const CompiledEntryPoint& entryPoint = variant.EntryPoints[i];
            if (!validate)
            {
                const CookResult<EmittedCodeFacts> facts =
                    target.Validator->ReadEmittedCodeFacts(entryPoint.Code, diagnostics);

                if (!facts)
                {
                    return std::unexpected(facts.error());
                }

                KeepDeclaredBindings(variant, i, facts->DeclaredPlacements);
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

            CookResult<BindingComparison> comparison =
                target.Validator->ValidateEntryPoint(entryPoint.Code, used, diagnostics);

            if (!comparison)
            {
                return std::unexpected(comparison.error());
            }

            // a reflected binding that the code does not declare is unused, and leaves the visibility below
            std::erase_if(comparison->Mismatches, [](const BindingMismatch& mismatch)
            {
                return mismatch.Kind == BindingMismatchKind::ReflectedOnly;
            });

            if (!comparison->Matches())
            {
                ++mismatchCount;
                std::string warningStr = std::format("REFLECTION MISMATCH in {}{} ({}) for target {}:",
                                                     entryPoint.Name,
                                                     entryPoint.VariantSuffix,
                                                     variant.VariantDescription,
                                                     target.Name);
                for (const BindingMismatch& mismatch : comparison->Mismatches)
                {
                    warningStr += '\n';
                    warningStr += FormatBindingMismatch(mismatch);
                }
                ReportWarning(diagnostics, warningStr);
                continue;
            }

            KeepDeclaredBindings(variant, i, comparison->Emitted.DeclaredPlacements);
            const CookError applied = ApplyEmittedCodeFacts(target, variant, i, comparison->Emitted, diagnostics);

            if (applied != CookError::Success)
            {
                return std::unexpected(applied);
            }
        }

        return mismatchCount;
    }

    void KeepDeclaredBindings(CompiledVariant& variant,
                              size_t entry_point_index,
                              std::span<const std::pair<uint32_t, uint32_t>> declared_placements)
    {
        CompiledEntryPoint& entryPoint = variant.EntryPoints[entry_point_index];
        std::vector<uint32_t>& used = entryPoint.Reflection.UsedBindingIndices;
        EntryPointConstants& constants = entryPoint.Reflection.Constants;
        const std::optional<uint32_t> constantsBuffer =
            constants.Delivery == EntryPointConstantsDelivery::UniformBuffer ? std::optional{ used[constants.Binding] }
                                                                             : std::nullopt;

        auto isDeclared = [declared_placements](uint32_t group, uint32_t binding)
        {
            return std::ranges::find(declared_placements, std::pair{ group, binding }) != declared_placements.end();
        };

        std::erase_if(used, [&](uint32_t binding_index)
        {
            const ResourcePlacement& placement = variant.Bindings[binding_index].Placement;
            if (const BoundPlacement* bound = GetBoundPlacement(placement))
            {
                return !isDeclared(bound->Group, bound->Binding);
            }
            const StoredValuePlacement* stored = GetStoredValuePlacement(placement);
            return stored != nullptr && stored->Container == StoredValueContainer::UniformBuffer &&
                   !isDeclared(stored->Block.Group, stored->Block.Binding);
        });

        if (!constantsBuffer)
        {
            return;
        }

        // the constants name their buffer by position, so a removal before it moves it
        const auto position = std::ranges::find(used, *constantsBuffer);
        if (position == used.end())
        {
            constants = EntryPointConstants{};
            return;
        }
        constants.Binding = static_cast<uint32_t>(std::distance(used.begin(), position));
    }

    /** Drops the names the profile's Vulkan version enables alone, such as `Shader`. */
    std::vector<std::string> WithoutGuaranteedCapabilities(const TargetProfile& target,
                                                           std::span<const std::string> names)
    {
        std::vector<std::string> kept;
        kept.reserve(names.size());
        for (const std::string& name : names)
        {
            const std::optional<vulkan::CapabilityEntry> entry =
                target.VulkanApiVersion == 0u ? std::nullopt : vulkan::FindCapability(name);
            const bool guaranteed =
                entry && entry->CoreVersion != 0u && entry->CoreVersion <= target.VulkanApiVersion;
            if (!guaranteed)
            {
                kept.push_back(name);
            }
        }
        return kept;
    }

    CookError ApplyEmittedCodeFacts(const TargetProfile& target,
                                    CompiledVariant& variant,
                                    size_t entry_point_index,
                                    const EmittedCodeFacts& facts,
                                    DiagnosticSink& diagnostics)
    {
        const CompiledEntryPoint& entryPoint = variant.EntryPoints[entry_point_index];
        if (!facts.EntryPointName.empty() && facts.EntryPointName != entryPoint.Name)
        {
            return ReportError(diagnostics,
                               CookError::ReflectionMismatch,
                               std::format("{}{} for target {}: the code names its entry point '{}'",
                                           entryPoint.Name,
                                           entryPoint.VariantSuffix,
                                           target.Name,
                                           facts.EntryPointName));
        }

        variant.EntryPoints[entry_point_index].CodeFacts = SourceCodeFacts{
            .CapabilityNames = WithoutGuaranteedCapabilities(target, facts.CapabilityNames),
            .WorkgroupMemoryBytes = facts.WorkgroupMemoryBytes
        };

        const CookError resolved =
            ResolveSamplerTexturePairs(variant, entry_point_index, facts.SamplerTexturePairs, target.DescriptorHeapSet, diagnostics);
        if (resolved != CookError::Success)
        {
            return resolved;
        }

        return ReconcileEntryPointConstants(target,
                                            variant.Bindings,
                                            variant.EntryPoints[entry_point_index],
                                            facts.PushConstantExtent,
                                            diagnostics);
    }

    CookError ReconcileEntryPointConstants(const TargetProfile& target,
                                           std::span<const ReflectedBinding> bindings,
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
                // a value stored in push constants that the code removed is not visible either
                std::erase_if(entry_point.Reflection.UsedBindingIndices,
                              [bindings](uint32_t binding_index)
                              {
                                  const StoredValuePlacement* stored = GetStoredValuePlacement(bindings[binding_index].Placement);
                                  return stored != nullptr && stored->Container == StoredValueContainer::PushConstants;
                              });
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
                                         std::optional<uint32_t> descriptor_heap_set,
                                         DiagnosticSink& diagnostics)
    {
        CompiledEntryPoint& entryPoint = variant.EntryPoints[entry_point_index];
        const std::vector<uint32_t>& used = entryPoint.Reflection.UsedBindingIndices;

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
            // a pair through the descriptor heap names no handle, so the pairs of handles are not recorded yet
            if (descriptor_heap_set.has_value() &&
                (placement.TextureGroup == *descriptor_heap_set || placement.SamplerGroup == *descriptor_heap_set))
            {
                continue;
            }

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
                return std::unexpected(ReportVariantFailure(shared_state,
                                                                target,
                                                                raw_module.Name,
                                                                currVariant,
                                                                result.error()));
            }

            const RawVariant& currRawVariant = result.value();
            CookResult<ResolveContext> contextResult = MakeResolveContext(currVariant.Canonical,
                                                                          raw_module.ExternDefaults,
                                                                          derived_constants,
                                                                          currRawVariant,
                                                                          *shared_state.Diagnostics);
            if (!contextResult)
            {
                return std::unexpected(ReportVariantFailure(shared_state,
                                                                  target,
                                                                  raw_module.Name,
                                                                  currVariant,
                                                                  contextResult.error()));
            }

            CookResult<CompiledVariant> variantResult = ResolveVariant(currRawVariant, contextResult.value(), *shared_state.Diagnostics);
            if (!variantResult)
            {
                return std::unexpected(ReportVariantFailure(shared_state,
                                                                  target,
                                                                  raw_module.Name,
                                                                  currVariant,
                                                                  variantResult.error()));
            }

            const CookError declarationError = RecordFootprintDeclarations(interned_module,
                                                                           currRawVariant,
                                                                           derived_constants,
                                                                           *shared_state.Diagnostics);
            if (declarationError != CookError::Success)
            {
                return std::unexpected(declarationError);
            }

            if (keepRawVariants)
            {
                raw_module.Variants.emplace_back(std::move(*result));
            }

            CompiledVariant& variant = variantResult.value();
            RecordVariantStatistics(variant, localStats);
            ReportVariantIfRequested(shared_state.Options, variant, *shared_state.Diagnostics);

            const CookResult<uint32_t> mismatches = ReadEmittedCode(target,
                                                                    shared_state.Options.ValidateAgainstEmittedText,
                                                                    variant,
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

            if (interned_module.EntryPoints.empty())
            {
                interned_module.EntryPoints.reserve(variant.EntryPoints.size());
                for (const CompiledEntryPoint& entryPoint : variant.EntryPoints)
                {
                    interned_module.EntryPoints.emplace_back(entryPoint.Name, entryPoint.Reflection.Stage);
                }
            }

            const CookError appendResult =
                AppendVariantToModule(interned_module, variant, currVariant, *shared_state.Diagnostics);
            if (appendResult != CookError::Success)
            {
                return std::unexpected(appendResult);
            }

            out_module_variants.emplace_back(std::move(*variantResult));
        }

        return localStats;
    }

    CookError ReportVariantFailure(const SharedCookState& shared_state,
                                   const TargetProfile& target,
                                   std::string_view module_name,
                                   const VariantDescriptor& variant,
                                   CookError error)
    {
        // module names are the stems of the module paths, in the same order
        const auto nameIter = std::ranges::find(shared_state.AllModuleNames, module_name);
        const size_t moduleIndex = static_cast<size_t>(std::distance(shared_state.AllModuleNames.begin(), nameIter));
        const std::string modulePath = shared_state.Options.ModulePaths[moduleIndex].generic_string();

        const std::string assignment = DescribeAssignment(variant.Canonical);
        return ReportError(*shared_state.Diagnostics,
                           error,
                           ShaderLocation{ .File = modulePath },
                           std::format("module '{}' for target '{}' failed in variant [{}]: {}",
                                       module_name,
                                       target.Name,
                                       assignment.empty() ? std::string_view{ "no axes" } : std::string_view{ assignment },
                                       ToString(error)));
    }
}

}
