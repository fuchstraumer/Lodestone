#include "driver/steps/FinalizeModuleStep.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "driver/CookerOptions.hpp"
#include "driver/CookerSteps.hpp"
#include "emit/DedupeReport.hpp"
#include "emit/StageDump.hpp"
#include "model/CookedLibrary.hpp"
#include "model/ShaderDataSchema.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace lodestone
{

namespace
{
    // Uses lower_bound to find the variant as an address, in the dense variant array
    const CompiledVariant* FindCompiledVariant(std::span<const CompiledVariant> compiled,
                                               uint64_t variant_index) noexcept;
    // Verifies library variants round trip against the compiled variants (source code, mostly)
    CookError VerifyLibraryRoundTrip(const CookedModule& module,
                                     std::span<const CompiledVariant> compiled,
                                     DiagnosticSink& diagnostics);
    // Replays the layout of each LibraryVariant to make sure it matches CompiledVariant
    CookError VerifyLayoutRoundTrip(const CookedModule& module,
                                    std::span<const CompiledVariant> compiled,
                                    DiagnosticSink& diagnostics);

}

CookResult<FinalizedModule> FinalizeModuleStep::operator()(const SharedCookState& shared_state,
                                                           InternedModule&& interned_module,
                                                           std::span<const CompiledVariant> module_variants) const
{
    CookedModule cookedModule = FreezeModuleTables(std::move(interned_module));
    const CookError roundTripResult = VerifyLibraryRoundTrip(cookedModule, module_variants, *shared_state.Diagnostics);
    if (!roundTripResult)
    {
        return std::unexpected(roundTripResult);
    }

    const CookError layoutResult = VerifyLayoutRoundTrip(cookedModule, module_variants, *shared_state.Diagnostics);
    if (!layoutResult)
    {
        return std::unexpected(layoutResult);
    }

    const std::string roundTripStr = std::format("module {} round trip verified: {} "
                                                    "variants resolve to the text the compiler produced",
                                                    cookedModule.Name,
                                                    cookedModule.Variants.size());
    ReportInfo(*shared_state.Diagnostics, roundTripStr);
    // only check module policy if there are inert axes specified for entry points

    const ModulePolicyEntry* modulePolicy = shared_state.Policy.FindModule(cookedModule.Name);

    if ((modulePolicy != nullptr) && !modulePolicy->InertAxesForEntryPoints.empty())
    {
        const CookError policyError = EnforceModulePolicy(cookedModule, *modulePolicy, *shared_state.Diagnostics);
        if (!policyError)
        {
            return std::unexpected(policyError);
        }
    }

    std::optional<std::string> dump = std::nullopt;
    if (IsStageDumpRequested(shared_state.Options, StageDumpKind::Cooked))
    {
        dump = DumpCookedModule(cookedModule);
    }

    return FinalizedModule{ std::move(cookedModule), std::move(dump) };
}

namespace
{
    const CompiledVariant* FindCompiledVariant(std::span<const CompiledVariant> compiled,
                                               uint64_t variant_index) noexcept
    {
        // `compiled` is sorted in ascending order already: we can use lower_bound
        auto candidateIter = std::ranges::lower_bound(compiled,
                                                      variant_index,
                                                      std::less<uint64_t>{},
                                                      &CompiledVariant::VariantIndex);
        // iterator has to be valid, but the index also needs to match!
        if (candidateIter != compiled.end() &&
            candidateIter->VariantIndex == variant_index)
        {
            return std::to_address(candidateIter);
        }
        return nullptr;
    }

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
                std::span<const std::byte> resolvedSource{ ResolveSource(module, variant, i) };
                std::span<const std::byte> epSourceSpan{ origin->EntryPoints[i].Code };
                if (!std::ranges::equal(resolvedSource, epSourceSpan))
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
}

}
