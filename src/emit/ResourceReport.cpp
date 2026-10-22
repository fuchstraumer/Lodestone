#include "emit/ResourceReport.hpp"
#include "CookerErrors.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "ShaderManifestLayout.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <magic_enum/magic_enum.hpp>

namespace lodestone
{

namespace
{

    using manifest::CreationFields;
    using manifest::CreationSource;

    /** The rank of a source by what it asks of the renderer. */
    uint32_t Demand(CreationSource source) noexcept
    {
        switch (source)
        {
        case CreationSource::NotApplicable:
            return 0u;
        case CreationSource::Shader:
            return 1u;
        case CreationSource::ShaderPerVariant:
            return 2u;
        case CreationSource::Cpu:
            return 3u;
        }
        return 0u;
    }

    void JoinSource(CreationSource& joined, CreationSource source) noexcept
    {
        if (Demand(source) > Demand(joined))
        {
            joined = source;
        }
    }

    void JoinFields(CreationFields& joined, const CreationFields& fields) noexcept
    {
        JoinSource(joined.Size, fields.Size);
        JoinSource(joined.Usage, fields.Usage);
        JoinSource(joined.Dimension, fields.Dimension);
        JoinSource(joined.Format, fields.Format);
        JoinSource(joined.Extent, fields.Extent);
        JoinSource(joined.ArrayLayers, fields.ArrayLayers);
        JoinSource(joined.MipLevels, fields.MipLevels);
        JoinSource(joined.SampleCount, fields.SampleCount);
        JoinSource(joined.SamplerState, fields.SamplerState);
    }

    /** A resource of one environment while the report reads it. `Values` holds every footprint seen, so a count or an
     * extent prints only when all of them agree. */
    struct PendingEntry
    {
        ResourceReportEntry Entry;
        std::vector<uint8_t> ReadByEntryPoint;
        std::vector<uint8_t> FootprintAxis;
        bool HasValue{ false };
        bool ValueVaries{ false };
    };

    void RecordFootprintValue(PendingEntry& pending, const manifest::ResolvedResource& resource) noexcept
    {
        const std::tuple<uint64_t, uint32_t, uint32_t, uint32_t> value{ resource.ElementCount(),
                                                                         resource.ExtentX(),
                                                                         resource.ExtentY(),
                                                                         resource.ExtentZ() };
        ResourceReportEntry& entry = pending.Entry;
        if (!pending.HasValue)
        {
            std::tie(entry.ElementCount, entry.ExtentX, entry.ExtentY, entry.ExtentZ) = value;
            pending.HasValue = true;
        }
        else if (value != std::tie(entry.ElementCount, entry.ExtentX, entry.ExtentY, entry.ExtentZ))
        {
            pending.ValueVaries = true;
        }
    }

    ResourceReportEnvironment ReadEnvironment(const manifest::BundleView& bundle,
                                              const manifest::EnvironmentView& view,
                                              uint32_t profile_index)
    {
        const manifest::ModuleView& module = view.Module();
        const auto entryPointCount = static_cast<uint32_t>(module.EntryPoints().size());
        const uint32_t axisCount = module.AxisCount();

        ResourceReportEnvironment environment;
        environment.ModuleName = std::string{ module.Name() };
        environment.TargetName = std::string{ bundle.String(bundle.Profiles()[profile_index].TargetNameString) };

        // sorted by scope and name, so each record finds its resource with a binary search
        std::vector<PendingEntry> pending;
        for (uint32_t variantIndex = 0u; variantIndex < view.Variants().size(); ++variantIndex)
        {
            const manifest::VariantView variant = view.VariantAt(variantIndex);
            for (uint32_t entryPointIndex = 0u; entryPointIndex < entryPointCount; ++entryPointIndex)
            {
                for (const manifest::ResolvedResource resource : variant.EntryPoint(entryPointIndex).Layout())
                {
                    const auto key = std::make_tuple(resource.ScopeName(), resource.Name());
                    auto position = std::ranges::lower_bound(pending,
                                                             key,
                                                             std::less{},
                                                             [](const PendingEntry& entry)
                                                             {
                                                                 return std::make_tuple(
                                                                     std::string_view{ entry.Entry.ScopeName },
                                                                     std::string_view{ entry.Entry.Name });
                                                             });
                    if (position == pending.end() || position->Entry.ScopeName != resource.ScopeName() ||
                        position->Entry.Name != resource.Name())
                    {
                        PendingEntry created;
                        created.Entry.ScopeName = std::string{ resource.ScopeName() };
                        created.Entry.Name = std::string{ resource.Name() };
                        created.Entry.Kind = resource.Kind();
                        created.ReadByEntryPoint.assign(entryPointCount, uint8_t{ 0u });
                        created.FootprintAxis.assign(axisCount, uint8_t{ 0u });
                        position = pending.insert(position, std::move(created));
                    }

                    PendingEntry& entry = *position;
                    JoinFields(entry.Entry.Fields, manifest::DescribeCreation(resource));
                    entry.ReadByEntryPoint[entryPointIndex] = 1u;
                    for (uint32_t axis = 0u; axis < axisCount; ++axis)
                    {
                        if (resource.IsFootprintAxis(axis))
                        {
                            entry.FootprintAxis[axis] = 1u;
                        }
                    }
                    RecordFootprintValue(entry, resource);
                }
            }
        }

        environment.Resources.reserve(pending.size());
        for (PendingEntry& entry : pending)
        {
            for (uint32_t axis = 0u; axis < axisCount; ++axis)
            {
                if (entry.FootprintAxis[axis] != 0u)
                {
                    entry.Entry.FootprintAxes.emplace_back(bundle.String(module.AxisData(axis).NameString));
                }
            }
            for (uint32_t entryPointIndex = 0u; entryPointIndex < entryPointCount; ++entryPointIndex)
            {
                if (entry.ReadByEntryPoint[entryPointIndex] != 0u)
                {
                    entry.Entry.EntryPoints.emplace_back(
                        bundle.String(module.EntryPoints()[entryPointIndex].NameString));
                }
            }
            if (entry.ValueVaries)
            {
                std::tie(entry.Entry.ElementCount, entry.Entry.ExtentX, entry.Entry.ExtentY, entry.Entry.ExtentZ) =
                    std::make_tuple(uint64_t{ 0u }, 0u, 0u, 0u);
            }
            environment.Resources.push_back(std::move(entry.Entry));
        }

        return environment;
    }

    std::string_view SourceName(CreationSource source) noexcept
    {
        switch (source)
        {
        case CreationSource::NotApplicable:
            return "-";
        case CreationSource::Shader:
            return "shader";
        case CreationSource::ShaderPerVariant:
            return "per-variant";
        case CreationSource::Cpu:
            return "cpu";
        }
        return "-";
    }

    std::string JoinNames(std::span<const std::string> names)
    {
        std::string joined;
        for (const std::string& name : names)
        {
            joined += joined.empty() ? name : std::format(", {}", name);
        }
        return joined;
    }

    /** One field as `name: source`, with the value or the axes where the shader states them. */
    std::string DescribeField(std::string_view name, CreationSource source, const ResourceReportEntry& entry, bool sized)
    {
        std::string text = std::format("{}: {}", name, SourceName(source));
        if (!sized)
        {
            return text;
        }
        if (source == CreationSource::ShaderPerVariant)
        {
            text += std::format(" ({})", JoinNames(entry.FootprintAxes));
        }
        else if (source == CreationSource::Shader && entry.ElementCount != 0u)
        {
            text += std::format(" ({})", entry.ElementCount);
        }
        else if (source == CreationSource::Shader && entry.ExtentX != 0u)
        {
            text += std::format(" ({}x{}x{})", entry.ExtentX, entry.ExtentY, entry.ExtentZ);
        }
        return text;
    }

} // namespace

CookResult<ResourceReport> BuildResourceReport(std::span<const std::byte> manifest_bytes)
{
    const manifest::ManifestResult<manifest::BundleView> opened = manifest::BundleView::Open(manifest_bytes);
    if (!opened)
    {
        std::println(stderr,
                     "[lodestone] the resource report cannot open the manifest: {}",
                     manifest::DescribeShaderManifestError(opened.error()));
        return std::unexpected(CookError::LibraryRoundTripFailed);
    }

    const manifest::BundleView& bundle = *opened;
    ResourceReport report;
    for (uint32_t profileIndex = 0u; profileIndex < bundle.Profiles().size(); ++profileIndex)
    {
        for (uint32_t moduleIndex = 0u; moduleIndex < bundle.ModuleCount(); ++moduleIndex)
        {
            if (bundle.Environment(profileIndex, moduleIndex).ExtentSize == 0u)
            {
                continue;
            }

            const manifest::ManifestResult<manifest::EnvironmentView> view =
                bundle.OpenEnvironment(profileIndex, moduleIndex);
            if (!view)
            {
                std::println(stderr,
                             "[lodestone] the resource report cannot open an environment: {}",
                             manifest::DescribeShaderManifestError(view.error()));
                return std::unexpected(CookError::LibraryRoundTripFailed);
            }
            report.Environments.push_back(ReadEnvironment(bundle, *view, profileIndex));
        }
    }

    return report;
}

std::string FormatResourceReport(const ResourceReport& report)
{
    std::string text;
    text += "Lodestone resource report\n";
    text += "For each resource: where each creation field comes from.\n";
    text += "  shader      the shader fixes the value for every variant\n";
    text += "  per-variant the shader fixes the value for each variant, and the named axes change it\n";
    text += "  cpu         the renderer must supply the value\n";
    text += "A buffer with no cpu field is complete. A texture or a sampler always needs values from the renderer\n";
    text += "(mip levels, sampler state), so the count names buffers only.\n";

    for (const ResourceReportEnvironment& environment : report.Environments)
    {
        // no shader states a texture's mip count, so a texture is never complete, and counting it only adds noise
        const auto isBuffer = [](const ResourceReportEntry& entry)
        { return entry.Kind == BindingKind::UniformBuffer || entry.Kind == BindingKind::StorageBuffer; };
        const auto bufferCount = std::ranges::count_if(environment.Resources, isBuffer);
        const auto completeCount = std::ranges::count_if(environment.Resources,
                                                         [&](const ResourceReportEntry& entry)
                                                         { return isBuffer(entry) && entry.Fields.IsComplete(); });
        text += std::format("\nmodule {}, target {}: {} of {} buffers complete\n",
                            environment.ModuleName,
                            environment.TargetName,
                            completeCount,
                            bufferCount);

        for (const ResourceReportEntry& entry : environment.Resources)
        {
            const std::string name =
                entry.ScopeName.empty() ? entry.Name : std::format("{}.{}", entry.ScopeName, entry.Name);
            text += std::format("  {} : {}, read by {}\n",
                                name,
                                magic_enum::enum_name(entry.Kind),
                                JoinNames(entry.EntryPoints));

            const CreationFields& fields = entry.Fields;
            const std::array<std::tuple<std::string_view, CreationSource, bool>, 9u> rows{ {
                { "size", fields.Size, true },
                { "usage", fields.Usage, false },
                { "dimension", fields.Dimension, false },
                { "format", fields.Format, false },
                { "extent", fields.Extent, true },
                { "layers", fields.ArrayLayers, false },
                { "mips", fields.MipLevels, false },
                { "samples", fields.SampleCount, false },
                { "sampler", fields.SamplerState, false },
            } };

            std::string line;
            for (const auto& [fieldName, source, sized] : rows)
            {
                if (source == CreationSource::NotApplicable)
                {
                    continue;
                }
                line += std::format("{}{}", line.empty() ? "" : ", ", DescribeField(fieldName, source, entry, sized));
            }
            text += std::format("    {}\n", line);
        }
    }

    return text;
}

} // namespace lodestone
