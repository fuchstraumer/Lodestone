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

static std::unique_ptr<PermutationSpace> cookPermutationSpace;

namespace
{

    std::expected<std::filesystem::path, std::error_code> EnsureModuleCacheDirectory(
        const std::filesystem::path& cache_directory)
    {
        std::error_code filesystemError;

        if (!std::filesystem::exists(cache_directory, filesystemError))
        {
            std::filesystem::create_directories(cache_directory, filesystemError);
        }

        // transform to canonical before returning (and since we know it exists), since it can be a little
        // more robust
        std::filesystem::path canonicalCacheDirectory =
            std::filesystem::canonical(cache_directory, filesystemError);

        if (filesystemError)
        {
            return std::unexpected(filesystemError);
        }

        return canonicalCacheDirectory;
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

    /** Slang emits only the bindings an entry point actually references, so the WGSL for one entry
     * point is compared against the subset of program-scope bindings that entry point uses. */
    std::vector<const ReflectedBinding*> SelectBindingsUsedByEntryPoint(const CompiledVariant& variant,
                                                                        size_t entry_point_index)
    {
        auto extractBinding = [&variant](uint32_t binding_index) -> const ReflectedBinding*
        {
            return &variant.Bindings[binding_index];
        };
        return variant.EntryPoints[entry_point_index].Reflection.UsedBindingIndices |
               std::views::transform(extractBinding) |
               std::ranges::to<std::vector<const ReflectedBinding*>>();
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
                ReportWarning(
                    diagnostics,
                    std::format("unreferenced binding in [{}]: {} is declared but no entrypoint reads it",
                                variant.VariantDescription,
                                DescribeBinding(variant.Bindings[static_cast<size_t>(index)])));
            }
        }
    }

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

    /** The reflection cross-check, after stage 4. It reads the emitted text back and compares it
     * against what reflection claims, so a disagreement is found by two opinions rather than by one
     * opinion trusted twice.
     *
     * The target decides how to read its own output. A target with no validator returns no
     * mismatches, and that is honest only because `PrepareModuleCompiler` already said the target
     * supplies none. */
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
            std::vector<const ReflectedBinding*> used = SelectBindingsUsedByEntryPoint(variant, i);
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

    /** Replays every variant through the finished tables and compares the result against the text the
     * compiler produced. This is the one check that makes a wrong shader impossible to ship: an index
     * mistake, a table hole, or a bad collapse all show up here, and all of them fail the cook. */
    const CompiledVariant* FindCompiledVariant(std::span<const CompiledVariant> compiled,
                                               uint64_t variant_index) noexcept
    {
        // `compiled` is sorted in ascending order already: we can use lower_bound to find variant idx in
        // log2(n)
        auto candidateIter = std::ranges::lower_bound(
            compiled, variant_index, std::less<uint64_t>{}, &CompiledVariant::VariantIndex);
        if (candidateIter != compiled.end() && candidateIter->VariantIndex == variant_index)
        {
            return std::to_address(candidateIter);
        }
        return nullptr;
    }

    /** Replays every layout through the finished tables and compares it against the bindings the
     * compiler produced.
     *
     * The source table has a second opinion, and until this check the layout table had none.
     * `CheckManifestLayout` compares the manifest against the table it was written from, so it can
     * prove the serialization is faithful and cannot see a wrong collapse. */
    CookError VerifyLayoutRoundTrip(const CookedModule& module,
                                    std::span<const CompiledVariant> compiled,
                                    DiagnosticSink& diagnostics)
    {
        CookError lastError = CookError::Success;

        for (const LibraryVariant& variant : module.Variants)
        {
            const CompiledVariant* origin = FindCompiledVariant(compiled, variant.Index);
            if (origin == nullptr)
            {
                continue;
            }

            for (size_t i = 0u; i < origin->EntryPoints.size(); ++i)
            {
                const CookResult<ShaderLayoutView> resolved = ResolveLayoutView(module, variant, i);
                if (!resolved)
                {
                    lastError = ReportError(diagnostics,
                                            resolved.error(),
                                            std::format("LAYOUT ROUND TRIP: could not resolve the layout for "
                                                        "{} [{}]",
                                                        origin->EntryPoints[i].Name,
                                                        variant.Description));
                    continue;
                }
                if (resolved.value() == BuildEntryPointLayoutView(*origin, i))
                {
                    continue;
                }

                const std::string errStr = std::format("LAYOUT ROUND TRIP FAILED for {} [{}]: the tables "
                                                       "return different bindings than the compiler produced",
                                                       origin->EntryPoints[i].Name,
                                                       variant.Description);
                lastError = ReportError(diagnostics, CookError::LibraryRoundTripFailed, errStr);
            }
        }

        return lastError;
    }

    CookError VerifyLibraryRoundTrip(const CookedModule& module,
                                     std::span<const CompiledVariant> compiled,
                                     DiagnosticSink& diagnostics)
    {
        if (module.Variants.size() != compiled.size())
        {
            const std::string errStr = std::format("module {} holds {} variants but the cook produced {}",
                                                   module.Name,
                                                   module.Variants.size(),
                                                   compiled.size());
            return ReportError(diagnostics, CookError::LibraryRoundTripFailed, errStr);
        }

        CookError lastError = CookError::Success;
        for (const LibraryVariant& variant : module.Variants)
        {
            const CompiledVariant* origin = FindCompiledVariant(compiled, variant.Index);
            if (origin == nullptr)
            {
                const std::string errStr =
                    std::format("variant index {} is in the library but not in the cook", variant.Index);
                lastError = ReportError(diagnostics, CookError::LibraryRoundTripFailed, errStr);
                continue;
            }

            for (size_t i = 0u; i < origin->EntryPoints.size(); ++i)
            {
                if (ResolveSource(module, variant, i) != origin->EntryPoints[i].Code)
                {
                    const std::string errStr = std::format("ROUND TRIP FAILED for {} [{}]: the table returns "
                                                           "different text than the compiler produced",
                                                           origin->EntryPoints[i].Name,
                                                           variant.Description);
                    lastError = ReportError(diagnostics, CookError::LibraryRoundTripFailed, errStr);
                }
            }
        }

        return lastError;
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

    /** Writes the header and manifest files for one cooked library */
    CookError EmitLibraryArtifacts(const CookedLibrary& library, OutputSink& sink)
    {
        const CookError manifestResult = EmitLibraryModules(library.Modules, sink);
        if (!manifestResult)
        {
            return manifestResult;
        }

        const std::string report = GenerateDedupeReport(library);
        const CookError dedupeResult = sink.WriteArtifact("ShaderLibrary.dedupe.txt", report);
        if (!dedupeResult)
        {
            return dedupeResult;
        }

        return CookError::Success;
    }

    /** Builds the dump only when the flag asked for it, because a dump of a large module costs real
     * work. The dump then goes out through the sink, so the determinism check compares it against
     * the second cook exactly as it compares every other artifact. */
    template<typename BuildDumpFn>
    CookError WriteStageDumpIfRequested(const CookerOptions& options,
                                        OutputSink& sink,
                                        std::string_view module_name,
                                        StageDumpKind kind,
                                        BuildDumpFn build_dump)
    {
        if (!IsStageDumpRequested(options, kind))
        {
            return CookError::Success;
        }

        return sink.WriteArtifact(MakeStageDumpFileName(module_name, kind), build_dump());
    }

    CookError BootstrapCompiler(const CookerOptions& options,
                                const std::filesystem::path& module_path,
                                const TargetProfile& target_profile,
                                SlangCompiler& compiler,
                                DiagnosticSink& diagnostics)
    {
        SlangCompilerCreateInfo createInfo;
        createInfo.ModulePath = module_path;
        createInfo.ModuleCacheDirectory = options.ModuleCacheDirectory;
        createInfo.OptimizationLevel = options.OptimizationLevel;
        createInfo.MultithreadVariantBuild = options.MultithreadEntryPointCodegen;
        createInfo.AccessModel = PlacementKindFromAccessModel(target_profile.Access);

        const CookError initializeResult = compiler.Initialize(createInfo, diagnostics);
        if (!initializeResult)
        {
            return initializeResult;
        }

        const std::string_view moduleName = compiler.ModuleName();
        const std::string infoStr =
            std::format("module {} declares {} entrypoints", moduleName, compiler.EntryPointCount());
        ReportInfo(diagnostics, infoStr);

        return CookError::Success;
    }

    AxisKind AxisKindFromString(std::string_view str)
    {
        if (str.empty())
        {
            return AxisKind::None;
        }
        else
        {
            // make sure to use case-insensitive, otherwise "tuning" would not match AxisKind::Tuning
            std::optional<AxisKind> kind = magic_enum::enum_cast<AxisKind>(str, magic_enum::case_insensitive);
            if (kind.has_value())
            {
                return kind.value();
            }
            else
            {
                return AxisKind::None;
            }
        }
    }

    CookResult<std::vector<PermutationValue>> ValuesFromStr(const std::string_view str,
                                                            DiagnosticSink& sink)
    {
        std::vector<PermutationValue> values;

        auto csvView = str |
                       std::views::split(',');
        
        for (auto chunk : csvView)
        {
            std::string_view valueStr = std::string_view(std::ranges::data(chunk), std::ranges::size(chunk));
            // we have to trim leading and trailing whitespace, if it's present, as from_chars will fail 
            // if we don't make sure to trim it out
            const size_t firstNonSpace = valueStr.find_first_not_of(" \t\r\n");
            if (firstNonSpace == std::string_view::npos)
            {
                continue;
            }
            const size_t lastNonSpace = valueStr.find_last_not_of(" \t\r\n");
            valueStr = valueStr.substr(firstNonSpace, lastNonSpace - firstNonSpace + 1);

            uint32_t value{ 0u };
            std::from_chars_result result = std::from_chars(valueStr.data(),
                                                            valueStr.data() + valueStr.size(),
                                                            value);
            if (result.ec != std::errc())
            {
                const std::string_view sysErrStr = magic_enum::enum_name(result.ec);
                const std::string errStr =
                    std::format("Failed to parse value '{}', error code: {}", valueStr, sysErrStr);
                return std::unexpected(ReportError(sink, CookError::FromCharsFailed, errStr));
            }
            
            values.emplace_back(value);
        }

        return values;
    }

    CookResult<PermutationSpace> BuildPermutationSpace(const SymbolTable& symbol_table,
                                                   std::span<std::string_view> module_names,
                                                   std::vector<RawAxisDeclaration> raw_axes,
                                                   DiagnosticSink& sink)
    {
        // First step: prune axes in raw axes that aren't actually used
        auto extractNameStrView = [](const RawAxisDeclaration& raw_axis)
        {
            return std::string_view{ raw_axis.Name };
        };
        std::vector<std::string_view> axisNamesVec = raw_axes |
                                                     std::views::transform(extractNameStrView) |
                                                     std::ranges::to<std::vector<std::string_view>>();
        std::vector<std::string_view> missingAxisNames = symbol_table.MissingTokens(module_names, axisNamesVec);

        // build the condensed span - use views and filter to remove the axes from raw_axes that aren't
        // used in any of the source code for the given modules.
        auto filterUnusedAxis = [&missingAxisNames](const RawAxisDeclaration& raw_axis)
        {
            return std::ranges::find(missingAxisNames, raw_axis.Name) == missingAxisNames.end();
        };
        std::vector<RawAxisDeclaration> filteredAxes = raw_axes |
                                                       std::views::as_rvalue |
                                                       std::views::filter(filterUnusedAxis) |
                                                       std::ranges::to<std::vector<RawAxisDeclaration>>();
        
        // Just for info sake (and because we can filter this), if missingAxisNames is not empty, we can log which axes were missing.
        if (!missingAxisNames.empty())
        {
            auto foldStrNames = [](std::span<std::string_view> names)
            {
                return std::ranges::fold_left(names, std::string{}, [](std::string acc, std::string_view name)
                {
                    if (!acc.empty())
                    {
                        acc += ", ";
                    }
                    acc += name;
                    return acc;
                });
            };

            std::string messageStr =
                std::format("The following axes were declared but not used in any module: {}", foldStrNames(missingAxisNames));
            ReportInfo(sink, std::move(messageStr));
        }

        // Second step: build the axes, using the filtered list of only the axes that are actually used
        std::vector<PermutationAxis> axes;
        for (RawAxisDeclaration& rawAxis : filteredAxes)
        {
            const AxisKind kind = AxisKindFromString(rawAxis.Kind);
            // values extraction - fork on boolean, if not boolean it's just a comma split
            std::vector<PermutationValue> values;
            if (rawAxis.IsBooleanAxis)
            { 
                axes.emplace_back(rawAxis.Name,
                                std::vector<PermutationValue>{ PermutationValue{ false }, PermutationValue{ true } },
                                kind,
                                EarliestBindingTime::Cook,
                                AxisValueDomain::Boolean,
                                rawAxis.ActiveWhen);
            }
            else if (rawAxis.IsInterfaceAxis)
            {
                // build the expanded list of permutation values for the interface axis.
                // each value is just the index of that interface implementation in the list of all implementations
                for (uint32_t i = 0; std::cmp_less(i, rawAxis.InterfaceImpls.size()); ++i)
                {
                    values.emplace_back(PermutationValue::MakeType(i));
                }
                axes.emplace_back(rawAxis.Name,
                                values,
                                kind,
                                EarliestBindingTime::Cook,
                                AxisValueDomain::Type,
                                rawAxis.ActiveWhen,
                                rawAxis.RootName,
                                rawAxis.InterfaceImpls);
            }
            else if (rawAxis.IsEnumAxis)
            {
                for (uint32_t i = 0; std::cmp_less(i, rawAxis.EnumCases.size()); ++i)
                {
                    values.emplace_back(PermutationValue::MakeEnum(i));
                }
                axes.emplace_back(rawAxis.Name,
                                values,
                                kind,
                                EarliestBindingTime::Cook,
                                AxisValueDomain::Enum,
                                rawAxis.ActiveWhen,
                                rawAxis.RootName,
                                rawAxis.EnumCases);
            }
            else
            {
                CookResult<std::vector<PermutationValue>> splitValues = ValuesFromStr(rawAxis.AxisValues, sink);
                if (!splitValues)
                {
                    return std::unexpected(splitValues.error());
                }
                values = std::move(*splitValues);
                axes.emplace_back(rawAxis.Name,
                                values,
                                kind,
                                EarliestBindingTime::Cook,
                                AxisValueDomain::Integral,
                                rawAxis.ActiveWhen);
            }

        }

        return PermutationSpace{ axes };
    }

    /** Everything the cook measures for one compiled variant, before it reaches the tables. */
    void RecordVariantStatistics(const CompiledVariant& variant, CookStatistics& statistics)
    {
        ++statistics.VariantsCompiled;
        statistics.EntryPointsCompiled += static_cast<uint32_t>(variant.EntryPoints.size());

        for (const CompiledEntryPoint& entryPoint : variant.EntryPoints)
        {
            statistics.TotalSourceBytes += entryPoint.Code.size();
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

    /** Names each entry point once, from the first variant. Every variant holds the same set. */
    void CaptureEntryPointsOnce(InternedModule& interned_module, const CompiledVariant& variant)
    {
        if (!interned_module.EntryPoints.empty())
        {
            return;
        }

        interned_module.EntryPoints.reserve(variant.EntryPoints.size());
        for (const CompiledEntryPoint& entryPoint : variant.EntryPoints)
        {
            interned_module.EntryPoints.push_back(
                LibraryEntryPoint{ .Name = entryPoint.Name, .Stage = entryPoint.Reflection.Stage });
        }
    }

    /** @brief Runs Slang compiler on each variant (which contains multiple entry points, remember),
     * and then takes that result and "resolves" it by evaluating our custom meta-language for sizes
     * and resource descriptors etc. This is also when the index tables are built as well. */
    [[nodiscard]] CookError CompileModuleVariants(const CookerOptions& options,
                                                  const TargetProfile& target,
                                                  SlangCompiler& compiler,
                                                  const VariantSet& variant_set,
                                                  InternedModule& interned_module,
                                                  RawModule& raw_module,
                                                  std::vector<CompiledVariant>& out_module_variants,
                                                  CookStatistics& statistics,
                                                  DiagnosticSink& diagnostics)
    {
        const bool keepRawVariants = IsStageDumpRequested(options, StageDumpKind::Raw);
        auto compileResultsList = compiler.Compile(variant_set.Variants, diagnostics);

        for (auto&& [idx, result] : std::views::enumerate(compileResultsList))
        {
            const auto& currVariant = variant_set.Variants[idx];
            if (!result)
            {
                const std::string errStr = std::format("variant [{}] failed: {}",
                                                       DescribeAssignment(currVariant.Canonical),
                                                       ToString(result.error()));
                return ReportError(diagnostics, result.error(), errStr);
            }

            const ResolveContext context =
                MakeResolveContext(currVariant.Canonical, raw_module.ExternDefaults);
            CookResult<CompiledVariant> variantResult = ResolveVariant(result.value(), context, diagnostics);
            if (!variantResult)
            {
                const std::string errStr = std::format("variant [{}] failed: {}",
                                                       DescribeAssignment(currVariant.Canonical),
                                                       ToString(variantResult.error()));
                return ReportError(diagnostics, variantResult.error(), errStr);
            }

            if (keepRawVariants)
            {
                raw_module.Variants.emplace_back(std::move(*result));
            }

            const CompiledVariant& variant = variantResult.value();
            RecordVariantStatistics(variant, statistics);
            ReportVariantIfRequested(options, variant, diagnostics);

            if (options.ValidateAgainstEmittedText)
            {
                statistics.ReflectionMismatches += ValidateResolvedLibrary(target, variant, diagnostics);
            }

            if (options.ReportReflection)
            {
                ReportUnreferencedBindings(variant, diagnostics);
            }

            CaptureEntryPointsOnce(interned_module, variant);

            const CookError appendResult =
                AppendVariantToModule(interned_module, variant, currVariant.Canonical);
            if (appendResult != CookError::Success)
            {
                return appendResult;
            }

            out_module_variants.emplace_back(std::move(*variantResult));
        }

        return CookError::Success;
    }

    /**@brief Take `InternedModule` and package it into `CookedModule`. */
    CookResult<CookedModule> FinalizeModule(InternedModule&& interned_module,
                                            std::span<const CompiledVariant> module_variants,
                                            const ModulePolicyEntry& module_policy,
                                            DiagnosticSink& diagnostics)
    {
        CookedModule cookedModule = FreezeModuleTables(std::move(interned_module));
        const CookError roundTripResult = VerifyLibraryRoundTrip(cookedModule, module_variants, diagnostics);
        if (!roundTripResult)
        {
            return std::unexpected(roundTripResult);
        }

        const CookError layoutResult = VerifyLayoutRoundTrip(cookedModule, module_variants, diagnostics);
        if (!layoutResult)
        {
            return std::unexpected(layoutResult);
        }

        const std::string roundTripStr = std::format("module {} round trip verified: {} "
                                                     "variants resolve to the text the compiler produced",
                                                     cookedModule.Name,
                                                     cookedModule.Variants.size());
        ReportInfo(diagnostics, roundTripStr);
        // only check module policy if there are inert axes specified for entry points
        if (!module_policy.InertAxesForEntryPoints.empty())
        {
            const CookError policyError = EnforceModulePolicy(cookedModule, module_policy, diagnostics);
            if (!policyError)
            {
                return std::unexpected(policyError);
            }
        }

        return cookedModule;
    }

    CookError CookModule(const CookerOptions& options,
                         const std::filesystem::path& module_path,
                         const PolicyDocument& policy_document,
                         OutputSink& sink,
                         DiagnosticSink& diagnostics,
                         CookedLibrary& out_library,
                         CookStatistics& statistics)
    {
        // `ParseCommandLine` already rejected invalid target names, so this cannot be null.
        const TargetProfile* target = FindTargetProfile(options.TargetName);
        if (target == nullptr)
        {
            return CookError::UnknownTargetProfile;
        }

        // This function *just* initializes the compiler: permutation space is built *after* this step
        // since it relies on an initial parse/build of the slang backend module data
        SlangCompiler compiler;
        const CookError prepareResult =
            BootstrapCompiler(options, module_path, *target, compiler, diagnostics);
        if (!prepareResult)
        {
            return prepareResult;
        }

        // now we can build the permutation space
        // todo-ship: this only contains one module name, bc as per comment above we're waiting to expand
        // this to multi-modules
        std::string_view localModuleNmae = compiler.ModuleName();
        std::span<std::string_view> moduleNameSpan{ &localModuleNmae, 1 };
        CookResult<PermutationSpace> spaceResult = BuildPermutationSpace(compiler.GetSymbolTable(),
                                                                         moduleNameSpan,
                                                                         std::move(compiler.BuildAxisDeclarations()),
                                                                         diagnostics);
        if (!spaceResult)
        {
            return spaceResult.error();
        }

        cookPermutationSpace = std::make_unique<PermutationSpace>(std::move(*spaceResult));
        
        // verify constraints on space are valid
        if (const CookError constraintResult = cookPermutationSpace->ValidateConstraints(diagnostics); !constraintResult)
        {
            return constraintResult;
        }

        // print check state because it makes sure unchecked cooks don't look like checked ones
        const std::string crossCheckStr = std::format("target {} ({} access), cross-check {}",
                                                      target->Name,
                                                      ToString(target->Access),
                                                      DescribeCrossCheckState(*target, options));
        ReportInfo(diagnostics, crossCheckStr);

        // get policy now, to get max variant count so enumeration can check against it
        const std::string_view moduleName = compiler.ModuleName();
        const TargetPolicy& currTargetPolicy =
            policy_document.FindTargetPolicy(moduleName, options.TargetName);

        // validate policy against active permutation space
        const CookError policyValidationResult = policy_document.ValidateAgainstSpace(moduleName, *cookPermutationSpace, diagnostics);
        if (!policyValidationResult)
        {
            return policyValidationResult;
        }
        
        // expand permutation space into the final set of variants this build will be constructing
        const CookResult<VariantSet> variantSet = cookPermutationSpace->EnumerateVariants(currTargetPolicy, diagnostics);
        if (!variantSet)
        {
            return variantSet.error();
        }

        const std::string variantStatsStr =
            std::format("module {} expands to {} variants over an index space of {}",
                        moduleName,
                        variantSet.value().Variants.size(),
                        variantSet.value().SpaceSize);
        ReportInfo(diagnostics, variantStatsStr);

        PermutationSpace& space = *cookPermutationSpace;
        auto dumpPermutationSpace = [&moduleName, &space]()
        {
            return DumpPermutationSpace(moduleName, space);
        };

        const CookError spaceDumpResult =
            WriteStageDumpIfRequested(options, sink, moduleName, StageDumpKind::Space, dumpPermutationSpace);
        if (spaceDumpResult != CookError::Success)
        {
            return spaceDumpResult;
        }

        auto dumpVariantSet = [&moduleName, &variantSet]()
        {
            return DumpVariantSet(moduleName, variantSet.value());
        };

        const CookError variantDumpResult =
            WriteStageDumpIfRequested(options, sink, moduleName, StageDumpKind::Variants, dumpVariantSet);
        if (variantDumpResult != CookError::Success)
        {
            return variantDumpResult;
        }

        InternedModule internedModule;
        if (!options.DedupeEnabled)
        {
            DisableDedupe(internedModule);
        }
        internedModule.Name = moduleName;
        // todo-ship: this is a quick fix to fix permutation space lifespan issues. it is likely
        // we'll need a cook context class to persist other state across cooks, and soon
        internedModule.Space = cookPermutationSpace.get();
        internedModule.SpaceSize = variantSet->SpaceSize;
        internedModule.VariantKeys = variantSet->Variants |
                                     std::views::transform(&VariantDescriptor::Key) |
                                     std::ranges::to<std::vector<VariantKey>>();

        std::vector<CompiledVariant> moduleVariants;
        moduleVariants.reserve(variantSet.value().Variants.size());

        CookResult<RawModule> rawModuleResult = compiler.PrepareRawModule(*cookPermutationSpace);
        if (!rawModuleResult)
        {
            return rawModuleResult.error();
        }

        RawModule rawModule = std::move(rawModuleResult.value());
        const CookError compileVariantsResult = CompileModuleVariants(options,
                                                                      *target,
                                                                      compiler,
                                                                      variantSet.value(),
                                                                      internedModule,
                                                                      rawModule,
                                                                      moduleVariants,
                                                                      statistics,
                                                                      diagnostics);
        if (compileVariantsResult != CookError::Success)
        {
            return compileVariantsResult;
        }

        auto dumpRawModule = [&]()
        {
            return DumpRawModule(rawModule);
        };
        const CookError rawDumpResult =
            WriteStageDumpIfRequested(options, sink, moduleName, StageDumpKind::Raw, dumpRawModule);
        if (rawDumpResult != CookError::Success)
        {
            return rawDumpResult;
        }

        auto dumpResolvedModule = [&]()
        {
            return DumpResolvedModule(moduleName, moduleVariants);
        };
        const CookError resolvedDumpResult =
            WriteStageDumpIfRequested(options, sink, moduleName, StageDumpKind::Resolved, dumpResolvedModule);
        if (resolvedDumpResult != CookError::Success)
        {
            return resolvedDumpResult;
        }

        // Written before the freeze, because this is the one dump whose subject stops existing. Every
        // other dump reads a value that outlives the call.
        auto dumpInternedModule = [&]()
        {
            return DumpInternedModule(internedModule);
        };
        const CookError internedDumpResult =
            WriteStageDumpIfRequested(options, sink, moduleName, StageDumpKind::Interned, dumpInternedModule);
        if (internedDumpResult != CookError::Success)
        {
            return internedDumpResult;
        }

        const ModulePolicyEntry* modulePolicyPtr = policy_document.FindModule(moduleName);
        const ModulePolicyEntry& modulePolicy = modulePolicyPtr != nullptr ? *modulePolicyPtr : ModulePolicyEntry{};
        CookResult<CookedModule> finalized = FinalizeModule(std::move(internedModule),
                                                            moduleVariants,
                                                            modulePolicy,
                                                            diagnostics);
        if (!finalized)
        {
            return finalized.error();
        }

        CookedModule cookedModule = std::move(*finalized);

        auto dumpCookedModule = [&]()
        {
            return DumpCookedModule(cookedModule);
        };
        const CookError cookedDumpResult =
            WriteStageDumpIfRequested(options, sink, moduleName, StageDumpKind::Cooked, dumpCookedModule);
        if (cookedDumpResult != CookError::Success)
        {
            return cookedDumpResult;
        }

        out_library.Modules.push_back(std::move(cookedModule));
        ++statistics.ModulesCooked;
        return CookError::Success;
    }

} // namespace

CookResult<CookStatistics> RunCookOnce(const CookerOptions& options,
                                       OutputSink& sink,
                                       DiagnosticSink& diagnostics)
{
    const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    const std::expected<std::filesystem::path, std::error_code> cacheDirectoryResult =
        EnsureModuleCacheDirectory(options.ModuleCacheDirectory);

    if (!cacheDirectoryResult)
    {
        return std::unexpected(CookError::DirectoryDoesNotExist);
    }

    PolicyDocument policyDoc{};
    if (options.PolicyFile)
    {
        const std::filesystem::path& policyFilePath = *options.PolicyFile;
        const std::string policyFilePathStr = policyFilePath.string();
        PolicyDocResult<PolicyDocument> policyDocResult = PolicyDocument::Load(policyFilePathStr);
        if (!policyDocResult)
        {
            // this will have to be cleaned up before too long, what a mess
            const PolicyParseError& policyError = policyDocResult.error();
            Diagnostic policyDiag
            {
                .Severity=DiagnosticSeverity::Fatal,
                .Code="CookError::PolicyDocumentLoadFailed",
                .File=policyFilePath.string(),
                .Range=
                {
                    .StartLine=static_cast<int32_t>(policyError.Line),
                    .StartColumn=static_cast<int32_t>(policyError.Column)
                },
                .Message=policyError.Message,
                .Context="RunCookOnce",
                .Related={}
            };
            diagnostics.Report(std::move(policyDiag));
            return std::unexpected(CookError::PolicyDocumentLoadFailed);
        }
        policyDoc = std::move(*policyDocResult);
    }

    CookStatistics statistics;
    CookedLibrary library;

    for (const std::filesystem::path& modulePath : options.ModulePaths)
    {
        const std::string cookingStr = std::format("cooking {}", modulePath.string());
        ReportInfo(diagnostics, cookingStr);
        const CookError moduleResult =
            CookModule(options, modulePath, policyDoc, sink, diagnostics, library, statistics);
        if (!moduleResult)
        {
            return std::unexpected(moduleResult);
        }
    }

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
