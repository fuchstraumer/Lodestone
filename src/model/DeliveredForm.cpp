#include "model/DeliveredForm.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "model/ShaderDataSchema.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lodestone
{

namespace
{

    // debug information is the one reason the delivered code may need a name that the base code does not
    constexpr std::array<std::string_view, 1u> k_DebugInformationCapabilities{ "SPV_KHR_non_semantic_info" };

    std::vector<std::pair<uint32_t, uint32_t>> PairsByBinding(const EntryPointCode& form)
    {
        std::vector<std::pair<uint32_t, uint32_t>> pairs;
        pairs.reserve(form.SamplerTexturePairs.size());
        for (const SamplerTexturePair& pair : form.SamplerTexturePairs)
        {
            pairs.emplace_back(form.UsedBindingIndices[pair.Texture], form.UsedBindingIndices[pair.Sampler]);
        }

        std::ranges::sort(pairs);
        return pairs;
    }

    // returns the first fact the delivered form needs and the base form does not, or nothing
    std::string FindExcess(const EntryPointCode& delivered, const EntryPointCode& base)
    {
        std::vector<uint32_t> deliveredBindings = delivered.UsedBindingIndices;
        std::vector<uint32_t> baseBindings = base.UsedBindingIndices;
        std::ranges::sort(deliveredBindings);
        std::ranges::sort(baseBindings);
        if (!std::ranges::includes(baseBindings, deliveredBindings))
        {
            return "a used binding";
        }

        if (!std::ranges::includes(PairsByBinding(base), PairsByBinding(delivered)))
        {
            return "a sampler pair";
        }

        for (const std::string& name : delivered.CodeFacts.CapabilityNames)
        {
            const bool inBase = std::ranges::binary_search(base.CodeFacts.CapabilityNames, name);
            const bool fromDebug = std::ranges::find(k_DebugInformationCapabilities, name) != k_DebugInformationCapabilities.end();
            if (!inBase && !fromDebug)
            {
                return std::format("the capability {}", name);
            }
        }

        if (delivered.CodeFacts.WorkgroupMemoryBytes > base.CodeFacts.WorkgroupMemoryBytes)
        {
            return std::format("{} bytes of workgroup memory, and the base needs {}",
                               delivered.CodeFacts.WorkgroupMemoryBytes,
                               base.CodeFacts.WorkgroupMemoryBytes);
        }

        // `Binding` is a position in each form's own used bindings, so only the delivery and the size compare
        const EntryPointConstants& constants = delivered.Constants;
        if (constants.Delivery != EntryPointConstantsDelivery::None &&
            (constants.Delivery != base.Constants.Delivery || constants.ByteSize > base.Constants.ByteSize))
        {
            return "entry point constants";
        }

        return {};
    }

} // namespace

CookError VerifyDeliveredForm(const CompiledVariant& variant, std::string_view target_name, DiagnosticSink& sink)
{
    for (const CompiledEntryPoint& entryPoint : variant.EntryPoints)
    {
        if (!entryPoint.Base.has_value())
        {
            continue;
        }

        const std::string excess = FindExcess(entryPoint.Delivered, *entryPoint.Base);
        if (!excess.empty())
        {
            return ReportError(sink,
                               CookError::DeliveredFormExceedsBase,
                               std::format("{}{} ({}) for target {}: the delivered code needs {} that the base code "
                                           "(--O0, no debug information) does not",
                                           entryPoint.Name,
                                           entryPoint.VariantSuffix,
                                           variant.VariantDescription,
                                           target_name,
                                           excess));
        }
    }

    return CookError::Success;
}

} // namespace lodestone
