#include "model/CookedLibrary.hpp"
#include "model/ContentHash.hpp"
#include "model/ContentInterner.hpp"
#include "CookerErrors.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationSpace.hpp"
#include "model/ShaderDataSchema.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lodestone
{

ContentHashValue HashIndexList(const std::vector<uint32_t>& indices) noexcept
{
    // this feels a little.... UB
    std::span<const uint32_t> indicesSpan{ indices.data(), indices.size() };
    std::span<const std::byte> indicesBytesSpan = std::as_bytes(indicesSpan);
    return HashBytes(indicesBytesSpan);
}

ContentHashValue HashSourceString(const std::string& source) noexcept
{
    // make string_view from source first, then cast to std::span<const std::byte> for hashing
    auto bytesSpan = std::as_bytes(std::span{ source.data(), source.length() });
    return HashBytes(bytesSpan);
}

ContentHashValue HashResourceList(const ResourceList& resources) noexcept
{
    return HashIndexList(resources);
}

ContentHashValue HashVisibilityList(const std::vector<uint32_t>& visibility) noexcept
{
    return HashIndexList(visibility);
}

ContentHashValue HashFootprintList(const std::vector<ResourceFootprint>& footprints) noexcept
{
    thread_local StreamingHash compositeHasher;
    compositeHasher.Reset(); // originally wanted to use local array
    // reusable array for at most 4 scalar values per footprint
    std::array<uint64_t, 4> footprintScalars;
    // can we construct this using ranges?
    for (const ResourceFootprint& footprint : footprints)
    {
        footprintScalars.fill(0u);

        if (const BufferFootprint* buffer = std::get_if<BufferFootprint>(&footprint))
        {
            footprintScalars[0] = static_cast<uint64_t>(footprint.index());
            footprintScalars[1] = static_cast<uint64_t>(buffer->ElementCount);
            compositeHasher.Append(std::span{ footprintScalars.data(), 2u });
            compositeHasher.Append(std::string_view{ buffer->Expression });
        }
        else if (const TextureFootprint* texture = std::get_if<TextureFootprint>(&footprint))
        {
            footprintScalars[0] = static_cast<uint64_t>(footprint.index());
            footprintScalars[1] = static_cast<uint64_t>(texture->ExtentX);
            footprintScalars[2] = static_cast<uint64_t>(texture->ExtentY);
            footprintScalars[3] = static_cast<uint64_t>(texture->ExtentZ);
            compositeHasher.Append(std::span{ footprintScalars.data(), footprintScalars.size() });
            compositeHasher.Append(std::string_view{ texture->Expression });
        }
        else
        {
            compositeHasher.Append(static_cast<uint64_t>(footprint.index()));
        }
    }

    return compositeHasher.Finalize();
}

void DisableDedupe(InternedModule& module) noexcept
{
    // todo: This is brittle and vulnerable to being missed whenever we add or change interners.
    // Can we do something smarter? (that's rhetorical. but think about it maybe)
    module.SourceInterner.Disable();
    module.ResourceInterner.Disable();
    module.ResourceListInterner.Disable();
    module.FootprintListInterner.Disable();
    module.VisibilityInterner.Disable();
    module.RasterInterner.Disable();
}

CookError AppendVariantToModule(InternedModule& module,
                                const CompiledVariant& variant,
                                const CanonicalAssignment& canonical)
{
    if (variant.EntryPoints.size() != module.EntryPoints.size())
    {
        std::println(stderr,
                     "[shader_cooker] variant [{}] has {} entrypoints, but module {} declares {}",
                     variant.VariantDescription,
                     variant.EntryPoints.size(),
                     module.Name,
                     module.EntryPoints.size());
        return CookError::ReflectionMismatch;
    }

    const ProvenanceRecord variantOrigin{ .EntryPointName = {},
                                          .VariantDescription = variant.VariantDescription,
                                          .VariantIndex = variant.VariantIndex };

    // Resources are per-variant (since that's the granularity we will build resources and bind
    // models at, not entrypoints: this is one of the advantages of our system)
    ResourceList resources;
    resources.reserve(variant.Bindings.size());
    for (const ReflectedBinding& binding : variant.Bindings)
    {
        resources.push_back(module.ResourceInterner.Intern(binding, variantOrigin).Index);
    }

    LibraryVariant record;
    record.Index = variant.VariantIndex;
    record.Suffix = variant.VariantSuffix;
    record.Description = variant.VariantDescription;
    record.Canonical = canonical;
    record.ResourceListIndex = module.ResourceListInterner.Intern(resources, variantOrigin).Index;
    record.FootprintListIndex = module.FootprintListInterner.Intern(variant.Footprints, variantOrigin).Index;
    record.SourceIndices.reserve(variant.EntryPoints.size());
    record.VisibilityIndices.reserve(variant.EntryPoints.size());
    record.RasterIndices.reserve(variant.EntryPoints.size());
    record.Workgroups.reserve(variant.EntryPoints.size());
    // we could probably make this more efficient in the future, it'll be moving a good bit of data
    // around as variant size grows
    // todo-ship: once we have a better logger with levels, it'd be interesting to see how uniqueness count
    // changes and grows per variant
    for (size_t entryPointIndex = 0u; entryPointIndex < variant.EntryPoints.size(); ++entryPointIndex)
    {
        const CompiledEntryPoint& entryPoint = variant.EntryPoints[entryPointIndex];
        const ProvenanceRecord origin{ .EntryPointName = entryPoint.Name,
                                       .VariantDescription = variant.VariantDescription,
                                       .VariantIndex = variant.VariantIndex };

        const InternResult source = module.SourceInterner.Intern(entryPoint.Code, origin);
        record.SourceIndices.push_back(source.Index);

        const InternResult visibility =
            module.VisibilityInterner.Intern(entryPoint.Reflection.UsedBindingIndices, origin);
        record.VisibilityIndices.push_back(visibility.Index);

        const InternResult raster = module.RasterInterner.Intern(entryPoint.Reflection.Raster, origin);
        record.RasterIndices.push_back(raster.Index);

        record.Workgroups.push_back(entryPoint.Reflection.Workgroup);
    }

    module.Variants.emplace_back(std::move(record));

    return CookError::Success;
}

namespace
{

    template<typename PayloadType>
    TableStatistics DescribeTable(const ContentInterner<PayloadType>& interner)
    {
        return TableStatistics{ .HashName = interner.HashName(),
                                .DedupeEnabled = interner.IsEnabled(),
                                .Interning = interner.Statistics() };
    }

} // namespace

CookedModule FreezeModuleTables(InternedModule&& interned)
{
    CookedModule module;
    module.Name = std::move(interned.Name);
    module.Space = interned.Space;
    module.SpaceSize = interned.SpaceSize;
    module.EntryPoints = std::move(interned.EntryPoints);
    module.Variants = std::move(interned.Variants);

    module.Sources = interned.SourceInterner.ConsumeTable();
    module.Resources = interned.ResourceInterner.ConsumeTable();
    module.ResourceLists = interned.ResourceListInterner.ConsumeTable();
    module.FootprintLists = interned.FootprintListInterner.ConsumeTable();
    module.VisibilityLists = interned.VisibilityInterner.ConsumeTable();
    module.RasterStates = interned.RasterInterner.ConsumeTable();

    module.SourceTable = DescribeTable(interned.SourceInterner);
    module.ResourceTable = DescribeTable(interned.ResourceInterner);
    module.ResourceListTable = DescribeTable(interned.ResourceListInterner);
    module.FootprintListTable = DescribeTable(interned.FootprintListInterner);
    module.VisibilityTable = DescribeTable(interned.VisibilityInterner);
    module.RasterTable = DescribeTable(interned.RasterInterner);

    return module;
}

std::string_view ResolveSource(const CookedModule& module,
                               const LibraryVariant& variant,
                               size_t entry_point_index) noexcept
{
    if (entry_point_index >= variant.SourceIndices.size())
    {
        return {};
    }

    const uint32_t sourceIndex = variant.SourceIndices[entry_point_index];
    if (sourceIndex >= module.Sources.size())
    {
        return {};
    }

    return module.Sources[sourceIndex];
}

ShaderLayout ResolveLayout(const CookedModule& module,
                           const LibraryVariant& variant,
                           size_t entry_point_index)
{
    // remember that visiblity indices is per-EP in this data, so if
    // input index is bigger than that list, it's not in range
    if (entry_point_index >= variant.VisibilityIndices.size() ||
        variant.ResourceListIndex >= module.ResourceLists.size() ||
        variant.FootprintListIndex >= module.FootprintLists.size())
    {
        return {};
    }

    // another indirection: visiblity lists are stored separately. EP idx just keys to that.
    // (remember, resource list size can change considerably between EPs)
    const uint32_t visibilityIndex = variant.VisibilityIndices[entry_point_index];
    if (visibilityIndex >= module.VisibilityLists.size())
    {
        return {};
    }

    const ResourceList& resources = module.ResourceLists[variant.ResourceListIndex];
    const FootprintList& footprints = module.FootprintLists[variant.FootprintListIndex];

    ShaderLayout layout;
    layout.reserve(module.VisibilityLists[visibilityIndex].size());
    // localRsrcIndex == binding of the resource in *this* entry point
    for (const uint32_t localRsrcIndex : module.VisibilityLists[visibilityIndex])
    {
        if (localRsrcIndex >= resources.size() || resources[localRsrcIndex] >= module.Resources.size())
        {
            return {};
        }

        ResourceFootprint footprint =
            localRsrcIndex < footprints.size() ? footprints[localRsrcIndex] : ResourceFootprint{};
        layout.emplace_back(module.Resources[resources[localRsrcIndex]], footprint);
    }

    return layout;
}

ShaderLayoutView ResolveLayoutView(const CookedModule& module,
                                   const LibraryVariant& variant,
                                   size_t entry_point_index)
{
    if (entry_point_index >= variant.VisibilityIndices.size() ||
        variant.ResourceListIndex >= module.ResourceLists.size() ||
        variant.FootprintListIndex >= module.FootprintLists.size()) [[unlikely]]
    {
        return {};
    }

    const uint32_t visibilityIndex = variant.VisibilityIndices[entry_point_index];
    if (visibilityIndex >= module.VisibilityLists.size()) [[unlikely]]
    {
        return {};
    }

    const ResourceList& resources = module.ResourceLists[variant.ResourceListIndex];
    const FootprintList& footprints = module.FootprintLists[variant.FootprintListIndex];

    std::vector<ResolvedBindingView> layoutView;
    layoutView.reserve(module.VisibilityLists[visibilityIndex].size());
    for (const uint32_t localRsrcIndex : module.VisibilityLists[visibilityIndex])
    {
        if (localRsrcIndex >= resources.size() || resources[localRsrcIndex] >= module.Resources.size())
            [[unlikely]]
        {
            return {};
        }

        const ResourceFootprint* footprint =
            localRsrcIndex < footprints.size() ? &footprints[localRsrcIndex] : nullptr;
        layoutView.emplace_back(&module.Resources[resources[localRsrcIndex]], footprint);
    }

    return layoutView;
}

} // namespace lodestone
