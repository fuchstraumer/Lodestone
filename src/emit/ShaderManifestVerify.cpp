#include "emit/ShaderManifestVerify.hpp"
#include "VariantKey.hpp"
#include "model/AxisInfluence.hpp"
#include "model/CookedLibrary.hpp"
#include "CookerErrors.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "model/ShaderDataSchema.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "permute/PermutationValue.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <numeric>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace lodestone
{

namespace
{

    CookError CheckBundleDirectory(const CookedLibrary& library, const manifest::BundleView& bundle);
    CookError CheckEnvironment(const CookedModule& module,
                               const manifest::EnvironmentView& view,
                               uint32_t& checked_count);
    CookError CheckVariantKeyDecode(const manifest::EnvironmentView& view,
                                    const LibraryVariant& variant,
                                    VariantKey key);
    /** Derives the relevance again from the written environment alone, through the client views. */
    CookError VerifyAxisRelevance(const manifest::EnvironmentView& view);
    CookError CheckFootprintAxes(const CookedModule& module, const manifest::EnvironmentView& view);
    CookError CheckSourceFacts(const CookedModule& module, const manifest::EnvironmentView& view);
    CookError CheckVariantAxisMask(const CookedModule& module,
                                   const LibraryVariant& variant,
                                   manifest::VariantView read_variant);
    bool StoredAxisValueMatches(const manifest::EnvironmentView& view,
                                uint32_t axis_index,
                                uint32_t digit,
                                const PermutationAxis& axis,
                                const PermutationValue& value);
    CookError CheckAxisDefaults(const CookedModule& module, const manifest::EnvironmentView& view);
    CookError CheckManifestSource(const CookedModule& module,
                                  const manifest::ShaderSourceProvider& provider,
                                  const LibraryVariant& variant,
                                  size_t entry_point_index);
    CookError CheckManifestWorkgroup(const CookedModule& module,
                                     const manifest::ShaderSourceProvider& provider,
                                     const LibraryVariant& variant,
                                     size_t entry_point_index);
    bool RecordMatchesFootprint(const manifest::Footprint& record, const ResourceFootprint& footprint) noexcept;
    bool RecordMatchesBinding(const manifest::Binding& record, const ReflectedBinding& binding) noexcept;
    bool ManifestUniformMembersMatch(const manifest::EnvironmentView& view,
                                     std::span<const manifest::UniformMember> read_members,
                                     std::span<const ReflectedUniformMember> expected_members);
    CookError CheckManifestLayout(const CookedModule& module,
                                  const manifest::EnvironmentView& view,
                                  const LibraryVariant& variant,
                                  manifest::VariantView read_variant,
                                  size_t entry_point_index);
    CookError CheckManifestVertexInputs(std::span<const manifest::VertexInput> read_inputs,
                                        const ReflectedRasterState& expected_raster,
                                        const manifest::EnvironmentView& view);
    CookError CheckManifestColorTargets(std::span<const manifest::ColorTarget> read_targets,
                                        const ReflectedRasterState& expected_raster);
    CookError CheckManifestRaster(const CookedModule& module,
                                  const manifest::EnvironmentView& view,
                                  const LibraryVariant& variant,
                                  size_t entry_point_index);
    CookError CheckManifestSlot(const CookedModule& module,
                                const manifest::EnvironmentView& view,
                                const manifest::ShaderSourceProvider& provider,
                                const LibraryVariant& variant,
                                manifest::VariantView read_variant,
                                size_t entry_point_index);

} // namespace

CookError VerifyManifestRoundTrip(const CookedLibrary& library, const std::vector<std::byte>& manifest_bytes)
{
    const std::span<const std::byte> raw{ manifest_bytes.data(), manifest_bytes.size() };

    const manifest::ManifestResult<manifest::BundleView> opened = manifest::BundleView::Open(raw);
    if (!opened)
    {
        std::println(stderr,
                     "[lodestone] manifest bundle does not open: {}",
                     manifest::DescribeShaderManifestError(opened.error()));
        return CookError::LibraryRoundTripFailed;
    }

    const manifest::BundleView& bundle = *opened;
    const CookError directoryError = CheckBundleDirectory(library, bundle);
    if (directoryError != CookError::Success)
    {
        return directoryError;
    }

    const size_t moduleCount = library.ModuleNames.size();
    uint32_t checked = 0u;
    for (size_t profileIndex = 0u; profileIndex < library.Profiles.size(); ++profileIndex)
    {
        for (size_t moduleIndex = 0u; moduleIndex < moduleCount; ++moduleIndex)
        {
            const std::optional<CookedModule>& environment =
                library.Environments[(profileIndex * moduleCount) + moduleIndex];
            const manifest::ManifestResult<manifest::EnvironmentView> view =
                bundle.OpenEnvironment(static_cast<uint32_t>(profileIndex), static_cast<uint32_t>(moduleIndex));

            if (!environment.has_value())
            {
                if (view.has_value() || view.error().Code != manifest::ErrorCode::EnvironmentNotCooked)
                {
                    std::println(stderr,
                                 "[lodestone] manifest holds module {} for target '{}', but the cook did not",
                                 library.ModuleNames[moduleIndex],
                                 library.Profiles[profileIndex].TargetName);
                    return CookError::LibraryRoundTripFailed;
                }
                continue;
            }

            if (!view)
            {
                std::println(stderr,
                             "[lodestone] module {} for target '{}' does not open: {}",
                             library.ModuleNames[moduleIndex],
                             library.Profiles[profileIndex].TargetName,
                             manifest::DescribeShaderManifestError(view.error()));
                return CookError::LibraryRoundTripFailed;
            }

            const CookError environmentError = CheckEnvironment(*environment, *view, checked);
            if (environmentError != CookError::Success)
            {
                return environmentError;
            }
        }
    }

    std::println(stderr,
                 "[lodestone] manifest round trip verified: {} modules x {} profiles, {} entrypoint variants "
                 "read back identical ({} KiB)",
                 moduleCount,
                 library.Profiles.size(),
                 checked,
                 manifest_bytes.size() / 1024u);

    return CookError::Success;
}
namespace
{

    CookError CheckBundleDirectory(const CookedLibrary& library, const manifest::BundleView& bundle)
    {
        if (bundle.ModuleCount() != library.ModuleNames.size() || bundle.Profiles().size() != library.Profiles.size())
        {
            std::println(stderr,
                         "[lodestone] manifest holds {} modules and {} profiles, the cook produced {} and {}",
                         bundle.ModuleCount(),
                         bundle.Profiles().size(),
                         library.ModuleNames.size(),
                         library.Profiles.size());
            return CookError::LibraryRoundTripFailed;
        }

        for (uint32_t moduleIndex = 0u; moduleIndex < bundle.ModuleCount(); ++moduleIndex)
        {
            if (bundle.Module(moduleIndex).Name() != library.ModuleNames[moduleIndex])
            {
                std::println(stderr,
                             "[lodestone] manifest names module '{}', but the cook produced '{}'",
                             bundle.Module(moduleIndex).Name(),
                             library.ModuleNames[moduleIndex]);
                return CookError::LibraryRoundTripFailed;
            }
        }

        for (auto&& [profileIndex, profile] : std::views::enumerate(bundle.Profiles()))
        {
            const CookedProfile& expected = library.Profiles[static_cast<size_t>(profileIndex)];
            if (bundle.String(profile.TargetNameString) != expected.TargetName ||
                profile.AccessModel != expected.AccessModel)
            {
                std::println(stderr,
                             "[lodestone] manifest profile {} names target '{}', but the cook produced '{}'",
                             profileIndex,
                             bundle.String(profile.TargetNameString),
                             expected.TargetName);
                return CookError::LibraryRoundTripFailed;
            }
        }

        return CookError::Success;
    }

    CookError CheckEnvironment(const CookedModule& module,
                               const manifest::EnvironmentView& view,
                               uint32_t& checked_count)
    {
        if (view.Variants().size() != module.Variants.size())
        {
            std::println(stderr,
                         "[lodestone] manifest module {} holds {} variants, the cook produced {}",
                         module.Name,
                         view.Variants().size(),
                         module.Variants.size());
            return CookError::ManifestMissingVariant;
        }

        const CookError defaultsError = CheckAxisDefaults(module, view);
        if (defaultsError != CookError::Success)
        {
            return defaultsError;
        }

        const CookError relevanceError = VerifyAxisRelevance(view);
        if (relevanceError != CookError::Success)
        {
            return relevanceError;
        }

        const CookError footprintAxesError = CheckFootprintAxes(module, view);
        if (footprintAxesError != CookError::Success)
        {
            return footprintAxesError;
        }

        const CookError sourceFactsError = CheckSourceFacts(module, view);
        if (sourceFactsError != CookError::Success)
        {
            return sourceFactsError;
        }

        const manifest::ShaderSourceProvider provider{ view, 0u };
        for (const LibraryVariant& variant : module.Variants)
        {
            const VariantKey key = module.VariantKeys[variant.Index];
            const std::optional<manifest::VariantView> readVariant = view.VariantByKey(key);
            if (!readVariant.has_value())
            {
                std::println(stderr, "[lodestone] manifest holds no variant {} [{}]", variant.Index, variant.Description);
                return CookError::ManifestMissingVariant;
            }

            const CookError keyError = CheckVariantKeyDecode(view, variant, key);
            if (keyError != CookError::Success)
            {
                return keyError;
            }

            const CookError maskError = CheckVariantAxisMask(module, variant, readVariant.value());
            if (maskError != CookError::Success)
            {
                return maskError;
            }

            for (size_t entryPointIndex = 0u; entryPointIndex < module.EntryPoints.size(); ++entryPointIndex)
            {
                const CookError slotError =
                    CheckManifestSlot(module, view, provider, variant, readVariant.value(), entryPointIndex);
                if (slotError != CookError::Success)
                {
                    return slotError;
                }

                ++checked_count;
            }
        }

        return CookError::Success;
    }

    CookError CheckVariantKeyDecode(const manifest::EnvironmentView& view,
                                    const LibraryVariant& variant,
                                    VariantKey key)
    {
        // decode the key as a client does, and compare with the canonical assignment
        const manifest::ModuleView& moduleView = view.Module();
        const uint32_t axisCount = moduleView.AxisCount();
        if (variant.Canonical.size() != axisCount)
        {
            std::println(stderr,
                         "[lodestone] manifest module {} has {} axes, variant [{}] has {}",
                         moduleView.Name(),
                         axisCount,
                         variant.Description,
                         variant.Canonical.size());
            return CookError::ManifestVariantKeyMismatch;
        }

        std::vector<uint32_t> radices(axisCount);
        std::vector<uint32_t> digits(axisCount);
        for (uint32_t axisIndex = 0u; axisIndex < axisCount; ++axisIndex)
        {
            radices[axisIndex] = moduleView.AxisValueCount(axisIndex);
        }
        UnpackVariantKey(key, radices, digits);

        for (uint32_t axisIndex = 0u; axisIndex < axisCount; ++axisIndex)
        {
            const PermutationBinding& binding = variant.Canonical[axisIndex];
            const PermutationAxis& axis = *binding.Axis;
            if (!StoredAxisValueMatches(view, axisIndex, digits[axisIndex], axis, binding.Value))
            {
                std::println(stderr,
                             "[lodestone] manifest key {} of module {} decodes axis '{}' differently than "
                             "variant [{}]",
                             std::to_underlying(key),
                             moduleView.Name(),
                             axis.Name,
                             variant.Description);
                return CookError::ManifestVariantKeyMismatch;
            }
        }

        return CookError::Success;
    }

    bool StoredAxisValueMatches(const manifest::EnvironmentView& view,
                                uint32_t axis_index,
                                uint32_t digit,
                                const PermutationAxis& axis,
                                const PermutationValue& value)
    {
        const manifest::ModuleView& moduleView = view.Module();
        const AxisValueType stored = moduleView.AxisValue(axis_index, digit);
        if (view.String(moduleView.AxisData(axis_index).NameString) != axis.Name)
        {
            return false;
        }

        switch (axis.ValueDomain)
        {
        case AxisValueDomain::Boolean:
            [[fallthrough]];
        case AxisValueDomain::Integral:
            return stored == value.AsUInt();
        case AxisValueDomain::Enum:
            return view.String(stored) == value.AsEnumCase(axis);
        case AxisValueDomain::Type:
            return view.String(stored) == value.AsType(axis);
        case AxisValueDomain::None:
            break;
        }
        std::unreachable();
    }

    CookError CheckAxisDefaults(const CookedModule& module, const manifest::EnvironmentView& view)
    {
        if (module.Space == nullptr)
        {
            return CookError::Success;
        }

        const std::span<const PermutationAxis> axes = module.Space->Axes();
        for (uint32_t axisIndex = 0u; axisIndex < axes.size(); ++axisIndex)
        {
            const PermutationAxis& axis = axes[axisIndex];
            const uint32_t digit = view.Module().AxisDefaultDigit(axisIndex);
            if (!StoredAxisValueMatches(view, axisIndex, digit, axis, axis.GetDeclaredDefault()))
            {
                std::println(stderr,
                             "[lodestone] manifest module {} reads the default of axis '{}' back as digit {}, which "
                             "is not the declared default",
                             module.Name,
                             axis.Name,
                             digit);
                return CookError::ManifestAxisDefaultMismatch;
            }
        }

        return CookError::Success;
    }

    std::span<const std::byte> SourceBytes(const manifest::EnvironmentView& view, uint32_t source_index)
    {
        if (view.CodeFormat() == ShaderCodeFormat::Spirv)
        {
            return std::as_bytes(view.SpirvWords(source_index));
        }
        return std::as_bytes(std::span{ view.Source(source_index) });
    }

    template<typename RecordType>
    bool RecordsEqual(std::span<const RecordType> lhs, std::span<const RecordType> rhs) noexcept
    {
        // records are bags of bytes with zeroed reserved fields, and strings are deduplicated indices
        return lhs.size() == rhs.size() && std::memcmp(lhs.data(), rhs.data(), lhs.size_bytes()) == 0;
    }

    /** `--no-dedupe` makes member indices unique, so the members themselves are compared. */
    bool ResourcesEqual(const manifest::EnvironmentView& view,
                        const manifest::ResolvedResource& lhs,
                        const manifest::ResolvedResource& rhs) noexcept
    {
        manifest::Binding lhsBinding = lhs.Record();
        manifest::Binding rhsBinding = rhs.Record();
        lhsBinding.FirstUniformMember = 0u;
        rhsBinding.FirstUniformMember = 0u;
        lhsBinding.FirstTag = 0u;
        rhsBinding.FirstTag = 0u;
        if (std::memcmp(&lhsBinding, &rhsBinding, sizeof(manifest::Binding)) != 0 ||
            !RecordsEqual(view.UniformMembers(lhs.Record()), view.UniformMembers(rhs.Record())) ||
            !RecordsEqual(view.ResourceTags(lhs.Record()), view.ResourceTags(rhs.Record())))
        {
            return false;
        }

        const manifest::Footprint* lhsFootprint = lhs.FootprintRecord();
        const manifest::Footprint* rhsFootprint = rhs.FootprintRecord();
        if (lhsFootprint == nullptr || rhsFootprint == nullptr)
        {
            return lhsFootprint == rhsFootprint;
        }

        return std::memcmp(lhsFootprint, rhsFootprint, sizeof(manifest::Footprint)) == 0;
    }

    /** By content. Equal record indices are only a shortcut. */
    bool InstancesEqual(const manifest::EnvironmentView& view, uint32_t lhs_variant, uint32_t rhs_variant, uint32_t entry_point)
    {
        const manifest::EntryPointInstanceView lhs = view.VariantAt(lhs_variant).EntryPoint(entry_point);
        const manifest::EntryPointInstanceView rhs = view.VariantAt(rhs_variant).EntryPoint(entry_point);
        const manifest::EntryPointInstance& lhsRecord = lhs.Record();
        const manifest::EntryPointInstance& rhsRecord = rhs.Record();

        if (lhsRecord.WorkgroupX != rhsRecord.WorkgroupX || lhsRecord.WorkgroupY != rhsRecord.WorkgroupY ||
            lhsRecord.WorkgroupZ != rhsRecord.WorkgroupZ)
        {
            return false;
        }

        if (lhsRecord.Raster != rhsRecord.Raster &&
            (!RecordsEqual(view.VertexInputs(lhsRecord.Raster), view.VertexInputs(rhsRecord.Raster)) ||
             !RecordsEqual(view.ColorTargets(lhsRecord.Raster), view.ColorTargets(rhsRecord.Raster)) ||
             view.WritesFragDepth(lhsRecord.Raster) != view.WritesFragDepth(rhsRecord.Raster)))
        {
            return false;
        }

        const manifest::LayoutRange lhsLayout = lhs.Layout();
        const manifest::LayoutRange rhsLayout = rhs.Layout();
        if (lhsLayout.Size() != rhsLayout.Size())
        {
            return false;
        }

        for (uint32_t position = 0u; position < lhsLayout.Size(); ++position)
        {
            if (!ResourcesEqual(view, lhsLayout[position], rhsLayout[position]))
            {
                return false;
            }
        }

        if (!RecordsEqual(lhs.SamplerTexturePairs(), rhs.SamplerTexturePairs()))
        {
            return false;
        }

        const manifest::EntryPointConstants& lhsConstants = lhs.Constants().Record();
        const manifest::EntryPointConstants& rhsConstants = rhs.Constants().Record();
        if (lhsRecord.Constants != rhsRecord.Constants &&
            (lhsConstants.Delivery != rhsConstants.Delivery || lhsConstants.ByteSize != rhsConstants.ByteSize ||
             lhsConstants.Binding != rhsConstants.Binding ||
             !RecordsEqual(view.UniformMembers(lhsConstants), view.UniformMembers(rhsConstants))))
        {
            return false;
        }

        return lhsRecord.Source == rhsRecord.Source ||
               std::ranges::equal(SourceBytes(view, lhsRecord.Source), SourceBytes(view, rhsRecord.Source));
    }

    std::vector<std::span<const uint32_t>> GroupVariants(std::span<const std::vector<uint32_t>> digits,
                                                         std::span<const uint8_t> kept_axes,
                                                         std::vector<uint32_t>& order)
    {
        auto lessOnKept = [&](uint32_t lhs, uint32_t rhs)
        {
            for (size_t axis = 0u; axis < kept_axes.size(); ++axis)
            {
                if (kept_axes[axis] != 0u && digits[lhs][axis] != digits[rhs][axis])
                {
                    return digits[lhs][axis] < digits[rhs][axis];
                }
            }
            return false;
        };

        order.resize(digits.size());
        std::ranges::iota(order, 0u);
        std::ranges::stable_sort(order, lessOnKept);

        std::vector<std::span<const uint32_t>> classes;
        
        size_t begin = 0u;
        while (begin < order.size())
        {
            size_t end = begin + 1u;
            while (end < order.size() && !lessOnKept(order[begin], order[end]))
            {
                ++end;
            }
            classes.emplace_back(order.data() + begin, end - begin);
            begin = end;
        }

        return classes;
    }

    /** Compares names by text, so a wrong bit and a wrong name table both fail. */
    CookError CheckSourceFacts(const CookedModule& module, const manifest::EnvironmentView& view)
    {
        for (uint32_t sourceIndex = 0u; sourceIndex < module.SourceFacts.size(); ++sourceIndex)
        {
            const SourceCodeFacts& expected = module.SourceFacts[sourceIndex];
            std::vector<std::string_view> read;
            for (uint32_t capabilityIndex = 0u; capabilityIndex < view.CapabilityNames().size(); ++capabilityIndex)
            {
                if (view.SourceNeedsCapability(sourceIndex, capabilityIndex))
                {
                    read.push_back(view.CapabilityName(capabilityIndex));
                }
            }

            if (!std::ranges::equal(read, expected.CapabilityNames) ||
                view.SourceWorkgroupMemoryBytes(sourceIndex) != expected.WorkgroupMemoryBytes)
            {
                std::println(stderr,
                             "[lodestone] manifest round trip: module {} source {} reads back capabilities or "
                             "workgroup memory that differ from the code",
                             module.Name,
                             sourceIndex);
                return CookError::ManifestSourceFactsMismatch;
            }
        }

        return CookError::Success;
    }

    CookError CheckFootprintAxes(const CookedModule& module, const manifest::EnvironmentView& view)
    {
        const size_t wordCount = view.Module().AxisMaskWordCount();
        const std::vector<uint64_t> expected = BuildFootprintAxisMasks(module, wordCount);

        for (uint32_t resourceIndex = 0u; resourceIndex < module.Resources.size(); ++resourceIndex)
        {
            const std::span<const uint64_t> written = view.FootprintAxes(resourceIndex);
            const std::span<const uint64_t> measured{ expected.data() + (resourceIndex * wordCount), wordCount };
            if (!std::ranges::equal(written, measured))
            {
                std::println(stderr,
                             "[lodestone] manifest round trip: module {} resource '{}' reads back footprint axes that "
                             "differ from the measured axes",
                             module.Name,
                             module.Resources[resourceIndex].Name);
                return CookError::ManifestFootprintAxesMismatch;
            }
        }

        return CookError::Success;
    }

    CookError VerifyAxisRelevance(const manifest::EnvironmentView& view)
    {
        const manifest::ModuleView& module = view.Module();
        const uint32_t axisCount = module.AxisCount();
        const auto entryPointCount = static_cast<uint32_t>(module.EntryPoints().size());
        const auto variantCount = static_cast<uint32_t>(view.Variants().size());

        std::vector<uint32_t> radices(axisCount);
        for (uint32_t axis = 0u; axis < axisCount; ++axis)
        {
            radices[axis] = module.AxisValueCount(axis);
        }

        std::vector<std::vector<uint32_t>> digits(variantCount, std::vector<uint32_t>(axisCount, 0u));
        for (uint32_t variant = 0u; variant < variantCount; ++variant)
        {
            UnpackVariantKey(view.VariantKeys()[variant], radices, digits[variant]);
        }

        auto describe = [&](uint32_t axis) { return view.String(module.AxisData(axis).NameString); };
        auto entryPointName = [&](uint32_t entry_point)
        {
            return view.String(module.EntryPoints()[entry_point].NameString);
        };

        std::vector<std::vector<uint8_t>> derived(entryPointCount, std::vector<uint8_t>(axisCount, uint8_t{ 0u }));
        std::vector<uint32_t> order;
        std::vector<uint8_t> allButOne(axisCount, uint8_t{ 1u });
        for (uint32_t axis = 0u; axis < axisCount; ++axis)
        {
            allButOne[axis] = 0u;
            const std::vector<std::span<const uint32_t>> classes = GroupVariants(digits, allButOne, order);
            allButOne[axis] = 1u;
            const bool determined =
                std::ranges::any_of(classes, [](std::span<const uint32_t> group) { return group.size() > 1u; });

            for (uint32_t entryPoint = 0u; entryPoint < entryPointCount; ++entryPoint)
            {
                const bool relevant = !determined || std::ranges::any_of(classes, [&](std::span<const uint32_t> group) {
                    return std::ranges::any_of(group.subspan(1u), [&](uint32_t variant) {
                        return !InstancesEqual(view, group.front(), variant, entryPoint);
                    });
                });
                derived[entryPoint][axis] = relevant ? 1u : 0u;
            }
        }

        for (uint32_t entryPoint = 0u; entryPoint < entryPointCount; ++entryPoint)
        {
            std::vector<uint8_t>& relevant = derived[entryPoint];
            while (true)
            {
                std::vector<uint8_t> marked(axisCount, uint8_t{ 0u });
                for (const std::span<const uint32_t> group : GroupVariants(digits, relevant, order))
                {
                    const bool oneInstance = std::ranges::all_of(group.subspan(1u), [&](uint32_t variant) {
                        return InstancesEqual(view, group.front(), variant, entryPoint);
                    });
                    if (oneInstance)
                    {
                        continue;
                    }
                    for (uint32_t axis = 0u; axis < axisCount; ++axis)
                    {
                        const bool varies = std::ranges::any_of(group.subspan(1u), [&](uint32_t variant) {
                            return digits[variant][axis] != digits[group.front()][axis];
                        });
                        if (relevant[axis] == 0u && varies)
                        {
                            marked[axis] = 1u;
                        }
                    }
                }

                if (std::ranges::none_of(marked, [](uint8_t bit) { return bit != 0u; }))
                {
                    break;
                }
                for (uint32_t axis = 0u; axis < axisCount; ++axis)
                {
                    relevant[axis] = static_cast<uint8_t>(relevant[axis] | marked[axis]);
                }
            }
        }

        for (uint32_t entryPoint = 0u; entryPoint < entryPointCount; ++entryPoint)
        {
            for (uint32_t axis = 0u; axis < axisCount; ++axis)
            {
                const bool relevant = derived[entryPoint][axis] != 0u;
                if (view.IsAxisRelevant(entryPoint, axis) != relevant)
                {
                    std::println(stderr,
                                 "[lodestone] AXIS RELEVANCE: the manifest stores axis '{}' as {} for entry point '{}', "
                                 "and the written instances say {}",
                                 describe(axis),
                                 relevant ? "irrelevant" : "relevant",
                                 entryPointName(entryPoint),
                                 relevant ? "relevant" : "irrelevant");
                    return CookError::ManifestAxisRelevanceMismatch;
                }
            }
        }

        return CookError::Success;
    }

    CookError CheckVariantAxisMask(const CookedModule& module,
                                   const LibraryVariant& variant,
                                   manifest::VariantView read_variant)
    {
        if (module.Space == nullptr)
        {
            return CookError::Success;
        }

        const std::span<const PermutationAxis> axes = module.Space->Axes();
        for (uint32_t axisIndex = 0u; axisIndex < axes.size(); ++axisIndex)
        {
            const PermutationAxis* axis = &axes[axisIndex];
            const bool expected = std::ranges::contains(variant.Active, axis, &PermutationBinding::Axis);
            if (read_variant.IsAxisActive(axisIndex) != expected)
            {
                std::println(stderr,
                             "[lodestone] manifest marks axis '{}' {} in variant [{}], the cook did not",
                             axis->Name,
                             expected ? "inactive" : "active",
                             variant.Description);
                return CookError::ManifestVariantAxisMaskMismatch;
            }
        }

        return CookError::Success;
    }

    CookError CheckManifestSource(const CookedModule& module,
                                  const manifest::ShaderSourceProvider& provider,
                                  const LibraryVariant& variant,
                                  size_t entry_point_index)
    {
        const std::span<const std::byte> expectedSource = ResolveSource(module, variant, entry_point_index);
        const VariantKey variantKey = module.VariantKeys[variant.Index];
        const uint32_t entryPoint = static_cast<uint32_t>(entry_point_index);
        const std::span<const std::byte> providerBytes =
            (provider.View().CodeFormat() == ShaderCodeFormat::Spirv)
                ? std::as_bytes(provider.SpirvWords(entryPoint, variantKey))
                : std::as_bytes(std::span{ provider.Source(entryPoint, variantKey) });
        if (std::ranges::equal(providerBytes, expectedSource))
        {
            return CookError::Success;
        }

        std::println(stderr,
                     "[lodestone] manifest returns different text for {} variant {} [{}]",
                     module.EntryPoints[entry_point_index].Name,
                     variant.Index,
                     variant.Description);
        return CookError::ManifestVariantSourceCodeMismatch;
    }

    CookError CheckManifestWorkgroup(const CookedModule& module,
                                     const manifest::ShaderSourceProvider& provider,
                                     const LibraryVariant& variant,
                                     size_t entry_point_index)
    {
        const WorkgroupSize expected = variant.Workgroups[entry_point_index];
        const VariantKey variantKey = module.VariantKeys[variant.Index];
        const WorkgroupSize read = provider.Workgroup(static_cast<uint32_t>(entry_point_index), variantKey);

        if (read.X == expected.X && read.Y == expected.Y && read.Z == expected.Z)
        {
            return CookError::Success;
        }

        std::println(stderr,
                     "[lodestone] manifest returns a different workgroup size for {} variant {}",
                     module.EntryPoints[entry_point_index].Name,
                     variant.Index);
        return CookError::ManifestVariantWorkgroupSizeMismatch;
    }

    bool RecordMatchesFootprint(const manifest::Footprint& record, const ResourceFootprint& footprint) noexcept
    {
        // field by field against the model, never through the writer. Unused fields must read zero
        if (const BufferFootprint* buffer = std::get_if<BufferFootprint>(&footprint))
        {
            return record.Kind == static_cast<uint32_t>(FootprintKind::Buffer) &&
                   record.ElementCount == buffer->ElementCount && record.ExtentX == 0u && record.ExtentY == 0u &&
                   record.ExtentZ == 0u;
        }

        if (const TextureFootprint* texture = std::get_if<TextureFootprint>(&footprint))
        {
            return record.Kind == static_cast<uint32_t>(FootprintKind::Texture) && record.ElementCount == 0u &&
                   record.ExtentX == texture->ExtentX && record.ExtentY == texture->ExtentY &&
                   record.ExtentZ == texture->ExtentZ;
        }

        return record.Kind == static_cast<uint32_t>(FootprintKind::None) && record.ElementCount == 0u &&
               record.ExtentX == 0u && record.ExtentY == 0u && record.ExtentZ == 0u;
    }

    bool RecordMatchesBinding(const manifest::Binding& record, const ReflectedBinding& binding) noexcept
    {
        return record.ByteSize == binding.ByteSize &&
               record.Placement.Word0 == GroupOf(binding) &&
               record.Placement.Word1 == BindingOf(binding) &&
               record.ElementStride == binding.ElementStride &&
               record.ArrayCount == binding.ArrayCount &&
               record.StorageFormat == static_cast<uint32_t>(binding.StorageFormat) &&
               record.UniformMemberCount == static_cast<uint32_t>(binding.UniformMembers.size()) &&
               record.Kind == static_cast<uint8_t>(binding.Kind) &&
               record.Shape == static_cast<uint8_t>(binding.Shape) &&
               record.IsComparisonSampler == static_cast<uint8_t>(binding.IsComparisonSampler) &&
               record.Access == static_cast<uint8_t>(binding.Access) &&
               record.SampleType == static_cast<uint8_t>(binding.SampleType) &&
               record.Roles == std::to_underlying(binding.Roles) &&
               record.TagCount == static_cast<uint32_t>(binding.Tags.size());
    }

    bool ManifestTagsMatch(const manifest::EnvironmentView& view,
                           std::span<const uint32_t> read,
                           std::span<const std::string> expected) noexcept
    {
        return std::ranges::equal(read, expected, [&view](uint32_t string_index, const std::string& tag) {
            return view.String(string_index) == tag;
        });
    }

    bool ManifestUniformMembersMatch(const manifest::EnvironmentView& view,
                                     std::span<const manifest::UniformMember> read_members,
                                     std::span<const ReflectedUniformMember> expected_members)
    {
        if (read_members.size() != expected_members.size())
        {
            return false;
        }

        for (size_t memberIndex = 0u; memberIndex < read_members.size(); ++memberIndex)
        {
            const ReflectedUniformMember& expectedMember = expected_members[memberIndex];
            const manifest::UniformMember& readMember = read_members[memberIndex];

            const bool matches = view.String(readMember.NameString) == expectedMember.Name &&
                                 readMember.Offset == expectedMember.Data.Offset &&
                                 readMember.Size == expectedMember.Data.Size &&
                                 readMember.ArrayCount == expectedMember.Data.ArrayCount &&
                                 readMember.ElementStride == static_cast<uint32_t>(expectedMember.Data.ElementStride) &&
                                 readMember.MatrixLayout == static_cast<uint32_t>(expectedMember.Data.MatrixLayout);
            if (!matches)
            {
                return false;
            }
        }

        return true;
    }

    CookError CheckManifestLayout(const CookedModule& module,
                                  const manifest::EnvironmentView& view,
                                  const LibraryVariant& variant,
                                  manifest::VariantView read_variant,
                                  size_t entry_point_index)
    {
        const CookResult<ShaderLayoutView> expectedLayoutResult = ResolveLayoutView(module, variant, entry_point_index);
        if (!expectedLayoutResult)
        {
            return expectedLayoutResult.error();
        }
        const ShaderLayoutView& expectedLayout = *expectedLayoutResult;

        if (entry_point_index >= read_variant.EntryPointCount())
        {
            std::println(stderr,
                         "[lodestone] manifest variant {} holds no slot {}",
                         variant.Index,
                         entry_point_index);
            return CookError::ManifestVariantMissingEntryPoint;
        }

        const manifest::LayoutRange layout =
            read_variant.EntryPoint(static_cast<uint32_t>(entry_point_index)).Layout();

        if (layout.Size() != expectedLayout.size())
        {
            std::println(stderr,
                         "[lodestone] manifest variant {} entry point {} sees {} resources, the cook "
                         "produced {}",
                         variant.Index,
                         entry_point_index,
                         layout.Size(),
                         expectedLayout.size());
            return CookError::ManifestVariantResourceVisibilityMismatch;
        }

        size_t position = 0u;
        for (const manifest::ResolvedResource resource : layout)
        {
            const ResolvedBindingView expected = expectedLayout[position];
            const manifest::Footprint* footprint = resource.FootprintRecord();

            if (footprint == nullptr)
            {
                std::println(stderr,
                             "[lodestone] manifest variant {} resolves resource {} out of range",
                             variant.Index,
                             position);
                return CookError::ManifestVariantResourceResolveOutOfRange;
            }

            const manifest::Binding& read = resource.Record();

            if (resource.Name() != expected.Resource->Name ||
                resource.ScopeName() != expected.Resource->ScopeName ||
                !RecordMatchesBinding(read, *expected.Resource) ||
                !RecordMatchesFootprint(*footprint, *expected.Footprint) ||
                !ManifestUniformMembersMatch(view, view.UniformMembers(read), expected.Resource->UniformMembers) ||
                !ManifestTagsMatch(view, view.ResourceTags(read), expected.Resource->Tags))
            {
                std::println(stderr,
                             "[lodestone] manifest binding '{}' of variant {} does not match the cook",
                             expected.Resource->Name,
                             variant.Index);
                return CookError::ManifestVariantResourceBindingMismatch;
            }

            ++position;
        }

        const SamplerPairList& expectedPairs =
            module.SamplerPairLists[variant.SamplerPairIndices[entry_point_index]];
        const std::span<const manifest::SamplerTexturePair> readPairs =
            read_variant.EntryPoint(static_cast<uint32_t>(entry_point_index)).SamplerTexturePairs();
        const bool pairsMatch = std::ranges::equal(readPairs, expectedPairs, [](const manifest::SamplerTexturePair& read, const SamplerTexturePair& expected) {
            return read.Texture == expected.Texture && read.Sampler == expected.Sampler;
        });
        if (!pairsMatch)
        {
            std::println(stderr,
                         "[lodestone] manifest variant {} entry point {} reads {} sampler and texture pairs, the cook "
                         "produced {}, or one pair differs",
                         variant.Index,
                         entry_point_index,
                         readPairs.size(),
                         expectedPairs.size());
            return CookError::ManifestSamplerPairMismatch;
        }

        const EntryPointConstants& expectedConstants =
            module.EntryPointConstantsTable[variant.ConstantsIndices[entry_point_index]];
        const manifest::EntryPointConstantsView readConstants =
            read_variant.EntryPoint(static_cast<uint32_t>(entry_point_index)).Constants();
        const manifest::EntryPointConstants& readRecord = readConstants.Record();
        const bool constantsMatch = readConstants.Delivery() == expectedConstants.Delivery &&
                                    readRecord.ByteSize == expectedConstants.ByteSize &&
                                    readRecord.Binding == expectedConstants.Binding &&
                                    ManifestUniformMembersMatch(view,
                                                                view.UniformMembers(readRecord),
                                                                expectedConstants.Members);
        if (!constantsMatch)
        {
            std::println(stderr,
                         "[lodestone] manifest variant {} entry point {} reads entry point constants that differ from "
                         "the cook",
                         variant.Index,
                         entry_point_index);
            return CookError::ManifestEntryPointConstantsMismatch;
        }

        return CookError::Success;
    }

    CookError CheckManifestVertexInputs(std::span<const manifest::VertexInput> read_inputs,
                                        const ReflectedRasterState& expected_raster,
                                        const manifest::EnvironmentView& view)
    {
        for (size_t inputIndex = 0u; inputIndex < read_inputs.size(); ++inputIndex)
        {
            const ReflectedVertexInput& expectedInput = expected_raster.VertexInputs[inputIndex];
            const manifest::VertexInput& readInput = read_inputs[inputIndex];

            if (view.String(readInput.SemanticNameString) != expectedInput.SemanticName ||
                readInput.SemanticIndex != expectedInput.Data.SemanticIndex ||
                readInput.Location != expectedInput.Data.Location ||
                readInput.ScalarType != static_cast<uint32_t>(expectedInput.Data.ScalarType) ||
                readInput.ComponentCount != expectedInput.Data.ComponentCount)
            {
                std::println(stderr,
                             "[lodestone] manifest vertex input '{}' does not match the cook",
                             expectedInput.SemanticName);
                return CookError::ManifestVertexInputMismatch;
            }
        }

        return CookError::Success;
    }

    CookError CheckManifestColorTargets(std::span<const manifest::ColorTarget> read_targets,
                                        const ReflectedRasterState& expected_raster)
    {
        for (size_t targetIndex = 0u; targetIndex < read_targets.size(); ++targetIndex)
        {
            const ReflectedColorTarget& expectedTarget = expected_raster.ColorTargets[targetIndex];
            const manifest::ColorTarget& readTarget = read_targets[targetIndex];

            if (readTarget.Location != expectedTarget.Location ||
                readTarget.ScalarType != static_cast<uint32_t>(expectedTarget.ScalarType) ||
                readTarget.ComponentCount != expectedTarget.ComponentCount)
            {
                std::println(stderr,
                             "[lodestone] manifest color target {} does not match the cook",
                             expectedTarget.Location);
                return CookError::ManifestColorTargetMismatch;
            }
        }

        return CookError::Success;
    }

    CookError CheckManifestRaster(const CookedModule& module,
                                  const manifest::EnvironmentView& view,
                                  const LibraryVariant& variant,
                                  size_t entry_point_index)
    {
        const uint32_t rasterIndex = variant.RasterIndices[entry_point_index];
        const ReflectedRasterState& expectedRaster = module.RasterStates[rasterIndex];

        const std::span<const manifest::VertexInput> readInputs = view.VertexInputs(rasterIndex);
        const std::span<const manifest::ColorTarget> readTargets = view.ColorTargets(rasterIndex);

        if (readInputs.size() != expectedRaster.VertexInputs.size() ||
            readTargets.size() != expectedRaster.ColorTargets.size() ||
            view.WritesFragDepth(rasterIndex) != expectedRaster.WritesFragDepth)
        {
            std::println(stderr,
                         "[lodestone] manifest raster state {} does not match the cook for {}",
                         rasterIndex,
                         module.EntryPoints[entry_point_index].Name);
            return CookError::ManifestRasterStateMismatch;
        }

        const CookError inputsError = CheckManifestVertexInputs(readInputs, expectedRaster, view);
        if (inputsError != CookError::Success)
        {
            return inputsError;
        }

        return CheckManifestColorTargets(readTargets, expectedRaster);
    }

    CookError CheckManifestSlot(const CookedModule& module,
                                const manifest::EnvironmentView& view,
                                const manifest::ShaderSourceProvider& provider,
                                const LibraryVariant& variant,
                                manifest::VariantView read_variant,
                                size_t entry_point_index)
    {
        const CookError sourceError = CheckManifestSource(module, provider, variant, entry_point_index);
        if (sourceError != CookError::Success)
        {
            return sourceError;
        }

        const CookError workgroupError = CheckManifestWorkgroup(module, provider, variant, entry_point_index);
        if (workgroupError != CookError::Success)
        {
            return workgroupError;
        }

        const CookError layoutError = CheckManifestLayout(module, view, variant, read_variant, entry_point_index);
        if (layoutError != CookError::Success)
        {
            return layoutError;
        }

        return CheckManifestRaster(module, view, variant, entry_point_index);
    }

} // namespace

} // namespace lodestone
