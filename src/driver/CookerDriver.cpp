#include "driver/CookerDriver.hpp"
#include "CookerErrors.hpp"
#include "compile/Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SlangCompiler.hpp"
#include "driver/CookerOptions.hpp"
#include "emit/DedupeReport.hpp"
#include "emit/OutputSink.hpp"
#include "emit/ShaderLibraryEmitter.hpp"
#include "emit/ShaderManifestEmitter.hpp"
#include "emit/StageDump.hpp"
#include "model/CookedLibrary.hpp"
#include "model/ResolveStage.hpp"
#include "model/ShaderDataSchema.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationRegistry.hpp"
#include "permute/PermutationSpace.hpp"
#include "target/TargetProfile.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <print>
#include <ranges>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace lodestone
{

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

    void ReportEntryPointReflection(const CompiledVariant& variant, size_t entry_point_index)
    {
        const CompiledEntryPoint& entryPoint = variant.EntryPoints[entry_point_index];
        std::println(stderr,
                     "[shader_cooker]   {}{} [{}] workgroup {}x{}x{}",
                     entryPoint.Name,
                     entryPoint.VariantSuffix,
                     ToString(entryPoint.Reflection.Stage),
                     entryPoint.Reflection.Workgroup.X,
                     entryPoint.Reflection.Workgroup.Y,
                     entryPoint.Reflection.Workgroup.Z);

        for (const ResolvedBindingView& resolved : BuildEntryPointLayoutView(variant, entry_point_index))
        {
            std::println(stderr,
                         "[shader_cooker]     {}{}",
                         DescribeBinding(*resolved.Resource),
                         DescribeFootprint(*resolved.Footprint));

            const std::string members = DescribeUniformMembers(*resolved.Resource);
            if (!members.empty())
            {
                std::print(stderr, "{}", members);
            }
        }

        const std::string raster = DescribeRasterState(entryPoint.Reflection.Raster);
        if (!raster.empty())
        {
            std::print(stderr, "{}", raster);
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

    void ReportUnreferencedBindings(const CompiledVariant& variant)
    {
        // i think we could flatten this even more with a vector of bools or bytes, but that's kinda ugly
        auto extractUsedBindingIndices = [](const CompiledEntryPoint& entry_point)
        {
            return entry_point.Reflection.UsedBindingIndices;
        };
        auto allUsedBindingIndices = variant.EntryPoints | std::views::transform(extractUsedBindingIndices) |
                                     std::views::join | std::ranges::to<std::vector<uint32_t>>();

        std::ranges::sort(allUsedBindingIndices);
        // clear out duplicates
        auto [beginDuplicates, endDuplicates] = std::ranges::unique(allUsedBindingIndices);
        allUsedBindingIndices.erase(beginDuplicates, endDuplicates);
        std::vector<uint32_t> unusedBindingIndices;
        std::ranges::set_difference(std::views::iota(0u, static_cast<uint32_t>(variant.Bindings.size())),
                                    allUsedBindingIndices,
                                    std::back_inserter(unusedBindingIndices));

        for (const auto& unusedIndex : unusedBindingIndices)
        {
            std::println(
                stderr,
                "[shader_cooker] unreferenced binding in [{}]: {} is declared but no entrypoint reads it",
                variant.VariantDescription,
                DescribeBinding(variant.Bindings[unusedIndex]));
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
    uint32_t ValidateResolvedLibrary(const TargetProfile& target, const CompiledVariant& variant)
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
                std::println(stderr,
                             "[shader_cooker] REFLECTION MISMATCH in {}{} ({}) for target {}:\n{}",
                             entryPoint.Name,
                             entryPoint.VariantSuffix,
                             variant.VariantDescription,
                             target.Name,
                             comparison.Report);
            }
        }

        return mismatchCount;
    }

    /** Replays every variant through the finished tables and compares the result against the text the
     * compiler produced. This is the one check that makes a wrong shader impossible to ship: an index
     * mistake, a table hole, or a bad collapse all show up here, and all of them fail the cook. */
    const CompiledVariant* FindCompiledVariant(std::span<const CompiledVariant> compiled,
                                               uint32_t variant_index) noexcept
    {
        // `compiled` is sorted in ascending order already: we can use lower_bound to find variant idx in
        // log2(n)
        auto candidateIter = std::ranges::lower_bound(
            compiled, variant_index, std::less<uint32_t>{}, &CompiledVariant::VariantIndex);
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
    CookResult<void> VerifyLayoutRoundTrip(const CookedModule& module,
                                           std::span<const CompiledVariant> compiled)
    {
        uint32_t mismatches = 0u;

        for (const LibraryVariant& variant : module.Variants)
        {
            const CompiledVariant* origin = FindCompiledVariant(compiled, variant.Index);
            if (origin == nullptr)
            {
                continue;
            }

            for (size_t i = 0u; i < origin->EntryPoints.size(); ++i)
            {
                if (ResolveLayoutView(module, variant, i) == BuildEntryPointLayoutView(*origin, i))
                {
                    continue;
                }

                std::println(stderr,
                             "[shader_cooker] LAYOUT ROUND TRIP FAILED for {} [{}]: the tables return "
                             "different bindings than the compiler produced",
                             origin->EntryPoints[i].Name,
                             variant.Description);
                ++mismatches;
            }
        }

        if (mismatches != 0u)
        {
            return std::unexpected(CookError::LibraryRoundTripFailed);
        }

        return {};
    }

    CookResult<void> VerifyLibraryRoundTrip(const CookedModule& module,
                                            std::span<const CompiledVariant> compiled)
    {
        if (module.Variants.size() != compiled.size())
        {
            std::println(stderr,
                         "[shader_cooker] module {} holds {} variants but the cook produced {}",
                         module.Name,
                         module.Variants.size(),
                         compiled.size());
            return std::unexpected(CookError::LibraryRoundTripFailed);
        }

        uint32_t mismatches = 0u;
        for (const LibraryVariant& variant : module.Variants)
        {
            const CompiledVariant* origin = FindCompiledVariant(compiled, variant.Index);
            if (origin == nullptr)
            {
                std::println(stderr,
                             "[shader_cooker] variant index {} is in the library but not in the cook",
                             variant.Index);
                ++mismatches;
                continue;
            }

            for (size_t i = 0u; i < origin->EntryPoints.size(); ++i)
            {
                if (ResolveSource(module, variant, i) != origin->EntryPoints[i].Code)
                {
                    std::println(stderr,
                                 "[shader_cooker] ROUND TRIP FAILED for {} [{}]: the table returns "
                                 "different text than the compiler produced",
                                 origin->EntryPoints[i].Name,
                                 variant.Description);
                    ++mismatches;
                }
            }
        }

        if (mismatches != 0u)
        {
            return std::unexpected(CookError::LibraryRoundTripFailed);
        }

        return {};
    }

    CookError EmitLibraryModules(std::string_view header_stem,
                                 std::string_view header_name,
                                 const std::vector<CookedModule>& modules,
                                 OutputSink& sink,
                                 CookStatistics& statistics)
    {
        for (const CookedModule& module : modules)
        {
            const std::string sourceName = MakeModuleSourceFileName(header_stem, module.Name);
            const std::string source = EmitShaderLibraryModuleSource(module, header_name);
            const CookError writeSourceResult = sink.WriteArtifact(sourceName, source);
            if (writeSourceResult != CookError::Success)
            {
                return writeSourceResult;
            }

            statistics.GeneratedSourceBytes += source.size();
            std::println(stderr,
                         "[shader_cooker] wrote {} ({} unique sources, {} resources, {} resource "
                         "lists, {} footprint lists, {} visibility lists, {} KiB)",
                         sourceName,
                         module.Sources.size(),
                         module.Resources.size(),
                         module.ResourceLists.size(),
                         module.FootprintLists.size(),
                         module.VisibilityLists.size(),
                         source.size() / 1024u);

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

    /** Writes the header and one source file for each module. The header name comes from the sink, so
     * the generated source includes exactly the file the user asked for.
     * todo: For writing files, we can accumulate output we want to write into a buffer, and only validate
     * things once. Validate directory when opening the stream, validate write success of coalesced writes
     * (cleans up control flow)*/
    CookError EmitLibraryArtifacts(const CookedLibrary& library, OutputSink& sink, CookStatistics& statistics)
    {
        const std::string headerName{ sink.PrimaryName() };
        const std::filesystem::path headerPath{ headerName };
        const std::string headerStem = headerPath.stem().string();

        const std::string header = EmitShaderLibraryHeader(library);
        CookError result = sink.Write(header);
        if (result != CookError::Success)
        {
            return result;
        }

        result = EmitLibraryModules(headerStem, headerName, library.Modules, sink, statistics);
        if (result != CookError::Success)
        {
            return result;
        }

        const std::string report = GenerateDedupeReport(library);
        result = sink.WriteArtifact("ShaderLibrary.dedupe.txt", report);
        if (result != CookError::Success)
        {
            return result;
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

    /** Builds the compiler for one module, and checks everything that must hold before the first
     * variant compiles. */
    CookResult<void> PrepareModuleCompiler(const CookerOptions& options,
                                           const std::filesystem::path& module_path,
                                           DiagnosticSink& diagnostics,
                                           SlangCompiler& compiler,
                                           const PermutationSpace*& out_space)
    {
        SlangCompilerCreateInfo createInfo;
        createInfo.ModulePath = module_path;
        createInfo.ModuleCacheDirectory = options.ModuleCacheDirectory;
        createInfo.OptimizationLevel = options.OptimizationLevel;
        createInfo.MultithreadVariantBuild = options.MultithreadEntryPointCodegen;

        if (auto initializeResult = compiler.Initialize(createInfo, diagnostics);
            initializeResult != CookError::Success)
        {
            return std::unexpected(initializeResult);
        }

        const std::string_view moduleName = compiler.ModuleName();
        std::println(stderr,
                     "[shader_cooker] module {} declares {} entrypoints",
                     moduleName,
                     compiler.EntryPointCount());

        out_space = FindPermutationSpaceForModule(moduleName);

        const std::vector<std::string_view> sourceViews{ compiler.ModuleSourceStringViews() };

        if (const CookError axisResult = out_space->VerifyAxisNamesAreDeclared(sourceViews, moduleName);
            axisResult != CookError::Success)
        {
            return std::unexpected(axisResult);
        }

        // No error checking needed as ReportUndrivenExternConstants now returns void
        out_space->ReportUndrivenExternConstants(sourceViews, moduleName);

        return {};
    }

    /** Everything the cook measures for one compiled variant, before it reaches the tables. */
    void RecordVariantStatistics(const CompiledVariant& variant, CookStatistics& statistics)
    {
        ++statistics.VariantsCompiled;
        statistics.EntryPointsCompiled += static_cast<uint32_t>(variant.EntryPoints.size());

        for (const CompiledEntryPoint& entryPoint : variant.EntryPoints)
        {
            statistics.TotalWgslBytes += entryPoint.Code.size();
        }
    }

    void ReportVariantIfRequested(const CookerOptions& options, const CompiledVariant& variant)
    {
        if (!options.ReportReflection)
        {
            return;
        }

        std::println(stderr, "[shader_cooker] variant [{}]", variant.VariantDescription);
        for (size_t i = 0u; i < variant.EntryPoints.size(); ++i)
        {
            ReportEntryPointReflection(variant, i);
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
                                                  DiagnosticSink& sink)
    {
        const bool keepRawVariants = IsStageDumpRequested(options, StageDumpKind::Raw);
        auto compileResultsList = compiler.Compile(variant_set.Variants, sink);

        for (auto&& [idx, result] : std::views::enumerate(compileResultsList))
        {
            const auto& currVariant = variant_set.Variants[idx];
            if (!result)
            {
                std::println(stderr,
                             "[shader_cooker] variant [{}] failed: {}",
                             DescribeAssignment(currVariant.Canonical),
                             ToString(result.error()));
                return result.error();
            }

            const ResolveContext context = MakeResolveContext(currVariant.Canonical, raw_module.ExternDefaults);
            CookResult<CompiledVariant> variantResult = ResolveVariant(result.value(), context);
            if (!variantResult)
            {
                std::println(stderr,
                             "[shader_cooker] variant [{}] failed: {}",
                             DescribeAssignment(currVariant.Canonical),
                             ToString(variantResult.error()));
                return variantResult.error();
            }

            if (keepRawVariants)
            {
                raw_module.Variants.emplace_back(std::move(*result));
            }

            const CompiledVariant& variant = variantResult.value();
            RecordVariantStatistics(variant, statistics);
            ReportVariantIfRequested(options, variant);

            if (options.ValidateAgainstEmittedText)
            {
                statistics.ReflectionMismatches += ValidateResolvedLibrary(target, variant);
            }

            if (options.ReportReflection)
            {
                ReportUnreferencedBindings(variant);
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
                                            std::span<const CompiledVariant> module_variants)
    {
        CookedModule cookedModule = FreezeModuleTables(std::move(interned_module));

        if (CookResult<void> roundTripResult = VerifyLibraryRoundTrip(cookedModule, module_variants);
            !roundTripResult)
        {
            return std::unexpected(roundTripResult.error());
        }

        if (CookResult<void> layoutResult = VerifyLayoutRoundTrip(cookedModule, module_variants);
            !layoutResult)
        {
            return std::unexpected(layoutResult.error());
        }

        std::println(stderr,
                     "[shader_cooker] module {} round trip verified: {} variants resolve to the text "
                     "the compiler produced",
                     cookedModule.Name,
                     cookedModule.Variants.size());

        const ModuleInfluence influence = ComputeAxisInfluence(cookedModule);
        if (const CookResult<void> policy = EnforceModulePolicy(cookedModule, influence); !policy)
        {
            return std::unexpected(policy.error());
        }

        return cookedModule;
    }

    CookResult<void> CookModule(const CookerOptions& options,
                                const std::filesystem::path& module_path,
                                OutputSink& sink,
                                DiagnosticSink& diagnostics,
                                CookedLibrary& out_library,
                                CookStatistics& statistics)
    {
        // `ParseCommandLine` already rejected a name no profile answers to, so this cannot be null.
        const TargetProfile* target = FindTargetProfile(options.TargetName);
        if (target == nullptr)
        {
            return std::unexpected(CookError::UnknownTargetProfile);
        }

        SlangCompiler compiler;
        const PermutationSpace* space = nullptr;

        if (CookResult<void> prepared =
                PrepareModuleCompiler(options, module_path, diagnostics, compiler, space);
            !prepared)
        {
            return prepared;
        }

        // Said once for each module, because a cook that checked nothing must not look like a cook
        // that checked and agreed.
        std::println(stderr,
                     "[shader_cooker] target {} ({} access), cross-check {}",
                     target->Name,
                     ToString(target->Access),
                     DescribeCrossCheckState(*target, options));

        const CookResult<VariantSet> variantSet = space->EnumerateVariants();
        if (!variantSet)
        {
            return std::unexpected(variantSet.error());
        }

        const std::string_view moduleName = compiler.ModuleName();
        std::println(stderr,
                     "[shader_cooker] module {} expands to {} variants over an index space of {}",
                     moduleName,
                     variantSet.value().Variants.size(),
                     variantSet.value().SpaceSize);

        auto dumpPermutationSpace = [&moduleName, &space]()
        {
            return DumpPermutationSpace(moduleName, *space);
        };
        const CookError spaceDumpResult =
            WriteStageDumpIfRequested(options, sink, moduleName, StageDumpKind::Space, dumpPermutationSpace);
        if (spaceDumpResult != CookError::Success)
        {
            return std::unexpected(spaceDumpResult);
        }

        auto dumpVariantSet = [&moduleName, &variantSet]()
        {
            return DumpVariantSet(moduleName, variantSet.value());
        };
        const CookError variantDumpResult =
            WriteStageDumpIfRequested(options, sink, moduleName, StageDumpKind::Variants, dumpVariantSet);
        if (variantDumpResult != CookError::Success)
        {
            return std::unexpected(variantDumpResult);
        }

        InternedModule internedModule;
        if (!options.DedupeEnabled)
        {
            DisableDedupe(internedModule);
        }
        internedModule.Name = moduleName;
        internedModule.Space = space;
        internedModule.SpaceSize = static_cast<uint32_t>(variantSet.value().SpaceSize);

        std::vector<CompiledVariant> moduleVariants;
        moduleVariants.reserve(variantSet.value().Variants.size());

        CookResult<RawModule> rawModuleResult = compiler.PrepareRawModule(*space);
        if (!rawModuleResult)
        {
            return std::unexpected(rawModuleResult.error());
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
            return std::unexpected(compileVariantsResult);
        }

        auto dumpRawModule = [&]()
        {
            return DumpRawModule(rawModule);
        };
        const CookError rawDumpResult =
            WriteStageDumpIfRequested(options, sink, moduleName, StageDumpKind::Raw, dumpRawModule);
        if (rawDumpResult != CookError::Success)
        {
            return std::unexpected(rawDumpResult);
        }

        auto dumpResolvedModule = [&]()
        {
            return DumpResolvedModule(moduleName, moduleVariants);
        };
        const CookError resolvedDumpResult =
            WriteStageDumpIfRequested(options, sink, moduleName, StageDumpKind::Resolved, dumpResolvedModule);
        if (resolvedDumpResult != CookError::Success)
        {
            return std::unexpected(resolvedDumpResult);
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
            return std::unexpected(internedDumpResult);
        }

        CookResult<CookedModule> finalized = FinalizeModule(std::move(internedModule), moduleVariants);
        if (!finalized)
        {
            return std::unexpected(finalized.error());
        }

        CookedModule cookedModule = std::move(finalized.value());

        auto dumpCookedModule = [&]()
        {
            return DumpCookedModule(cookedModule);
        };
        const CookError cookedDumpResult =
            WriteStageDumpIfRequested(options, sink, moduleName, StageDumpKind::Cooked, dumpCookedModule);
        if (cookedDumpResult != CookError::Success)
        {
            return std::unexpected(cookedDumpResult);
        }

        out_library.Modules.push_back(std::move(cookedModule));
        ++statistics.ModulesCooked;
        return {};
    }

} // namespace

CookResult<CookStatistics> RunCookOnce(const CookerOptions& options, OutputSink& sink)
{
    const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    const std::expected<std::filesystem::path, std::error_code> cacheDirectoryResult =
        EnsureModuleCacheDirectory(options.ModuleCacheDirectory);

    if (!cacheDirectoryResult)
    {
        return std::unexpected(CookError::FilesystemError);
    }

    CookStatistics statistics;
    CookedLibrary library;
    // One sink for the whole cook, so a failure count spans every module rather than resetting at
    // each one.
    StderrDiagnosticSink diagnostics;

    for (const std::filesystem::path& modulePath : options.ModulePaths)
    {
        std::println(stderr, "[shader_cooker] cooking {}", modulePath.string());
        const CookResult<void> moduleResult =
            CookModule(options, modulePath, sink, diagnostics, library, statistics);
        if (!moduleResult)
        {
            return std::unexpected(moduleResult.error());
        }
    }

    if (statistics.ReflectionMismatches != 0u)
    {
        return std::unexpected(CookError::ReflectionMismatch);
    }

    const CookError emitResult = EmitLibraryArtifacts(library, sink, statistics);
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
    CookResult<CookStatistics> RunCookTwiceAndCompare(const CookerOptions& options, OutputSink& sink)
    {
        std::println(stderr, "[shader_cooker] determinism check: cooking twice into memory");

        // Both memory sinks take the real sink's primary name. The emitter builds every companion
        // artifact name from it, so a different name here would make the check compare a different
        // set of file names than the cook it stands in for.
        MemoryOutputSink first{ sink.PrimaryName() };
        const CookResult<CookStatistics> firstResult = RunCookOnce(options, first);
        if (!firstResult)
        {
            return firstResult;
        }

        MemoryOutputSink second{ sink.PrimaryName() };
        const CookResult<CookStatistics> secondResult = RunCookOnce(options, second);
        if (!secondResult)
        {
            return secondResult;
        }

        if (first.GetContent() != second.GetContent())
        {
            std::println(stderr, "[shader_cooker] DETERMINISM FAILED: the header differs between cooks");
            return std::unexpected(CookError::CookNotDeterministic);
        }

        if (first.GetArtifacts().size() != second.GetArtifacts().size())
        {
            std::println(stderr,
                         "[shader_cooker] DETERMINISM FAILED: {} artifacts, then {}",
                         first.GetArtifacts().size(),
                         second.GetArtifacts().size());
            return std::unexpected(CookError::CookNotDeterministic);
        }

        for (const auto& [name, content] : first.GetArtifacts())
        {
            const auto other = second.GetArtifacts().find(name);
            if (other == second.GetArtifacts().end() || other->second != content)
            {
                std::println(stderr, "[shader_cooker] DETERMINISM FAILED: {} differs between cooks", name);
                return std::unexpected(CookError::CookNotDeterministic);
            }
        }

        std::println(stderr,
                     "[shader_cooker] determinism verified: {} artifacts identical across two cooks",
                     first.GetArtifacts().size() + 1u);

        const CookError writeResult = sink.Write(first.GetContent());
        if (writeResult != CookError::Success)
        {
            return std::unexpected(writeResult);
        }

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
    if (options.VerifyDeterministic)
    {
        return RunCookTwiceAndCompare(options, sink);
    }

    return RunCookOnce(options, sink);
}

} // namespace lodestone
