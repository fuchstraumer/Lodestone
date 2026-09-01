#include "SlangReflector.hpp"
#include "CookerErrors.hpp"
#include "ShaderLibraryTypes.hpp"
#include "SlangCompilerTypes.hpp"
#include "SlangVariantCompiler.hpp"
#include "compile/Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "magic_enum/magic_enum.hpp"
#include "model/ShaderDataSchema.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationSpace.hpp"
#include "slang-com-ptr.h"
#include "slang.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using namespace lodestone;

/** What Slang names the scope it moves each entry point `uniform` parameter into.
 *
 * `slang-ir-entry-point-uniforms.cpp` writes this string as a name hint, and reflection reports it
 * nowhere. It is a Slang convention, so it lives behind the Slang wall and reaches no other
 * folder. Every emitted name of an entry point parameter starts with it. */
constexpr std::string_view k_EntryPointScopeName = "entryPointParams";

constexpr SlangUInt k_WorkgroupAxisCount = 3u;

// The shader-side attribute names live on RawSizeAttributeKind, so one table states the contract
// and this file asks it for the spelling. Slang drops the `Attribute` suffix from the struct name,
// so those strings match the declarations in tests/assets/LodestoneAttributes.slang.
constexpr std::array<RawSizeAttributeKind, 3u> k_SizeAttributeKinds{ RawSizeAttributeKind::ElementCount,
                                                                     RawSizeAttributeKind::Extent2d,
                                                                     RawSizeAttributeKind::Extent3d };

/** Attribute string arguments reflect as a pointer plus a length, and a null return means the
 * argument was not a string literal after all. That is a cook error rather than a skip: the
 * annotation was written, so the author expects it to do something. */
CookResult<std::string> ReadStringArgument(slang::Attribute* attribute,
                                           uint32_t argument_index,
                                           std::string_view binding_name)
{
    size_t length = 0u;
    const char* text = attribute->getArgumentValueString(argument_index, &length);
    if (text == nullptr)
    {
        std::println(stderr,
                     "[shader_cooker] argument {} of [{}] on '{}' is not a string literal",
                     argument_index,
                     attribute->getName(),
                     binding_name);
        return std::unexpected(CookError::AttributeExpressionParseFailed);
    }

    // The reflected span includes the surrounding quotes on some paths; trim them if present.
    std::string_view value{ text, length };
    if (value.size() >= 2u && value.front() == '"' && value.back() == '"')
    {
        value = value.substr(1u, value.size() - 2u);
    }

    return std::string{ value };
}

/** Splits one varying (shader input/output semantic) into a scalar type and a component count. A scalar
 * counts as one component, so a caller never has to test the kind again. */
void ReadVaryingShape(slang::TypeLayoutReflection* type_layout,
                      VertexScalarType& out_scalar_type,
                      uint32_t& out_component_count) noexcept
{
    out_scalar_type = VertexScalarType::Invalid;
    out_component_count = 0u;

    if (type_layout == nullptr)
    {
        return;
    }

    if (type_layout->getKind() == slang::TypeReflection::Kind::Vector)
    {
        slang::TypeLayoutReflection* elementLayout = type_layout->getElementTypeLayout();
        out_component_count = static_cast<uint32_t>(type_layout->getElementCount());
        if (elementLayout != nullptr)
        {
            out_scalar_type = FromSlangVertexScalarType(elementLayout->getType()->getScalarType());
        }
        return;
    }

    if (type_layout->getKind() == slang::TypeReflection::Kind::Scalar)
    {
        out_component_count = 1u;
        out_scalar_type = FromSlangVertexScalarType(type_layout->getType()->getScalarType());
    }
}

/** One leaf of a varying or uniform tree, as the walker found it.
 *
 * `Path` holds the var layout of every field from the root to this leaf. That is what a caller
 * needs to build a qualified name, and a caller that names nothing ignores it. Building the name
 * in the walker would make three of the four callers pay for a string none of them reads. */
struct LeafVisit
{
    slang::VariableLayoutReflection* Var{ nullptr };
    slang::TypeLayoutReflection* Type{ nullptr };
    /** The offset of this leaf in the category the walk asked for, summed down the tree. */
    uint32_t Offset{ 0u };
    std::span<slang::VariableLayoutReflection* const> Path;
};

/** Calls `visit` for each leaf of one varying or uniform tree.
 *
 * Slang states each field's offset against its parent, so the offset accumulates down the tree,
 * and `category` selects which offset that is. A caller that wants no offset ignores the one it
 * gets. */
template<typename LeafVisitor> // NOLINTNEXTLINE(misc-no-recursion)
void VisitLeaves(slang::VariableLayoutReflection* var_layout,
                 SlangParameterCategory category,
                 uint32_t base_offset,
                 std::vector<slang::VariableLayoutReflection*>& path,
                 const LeafVisitor& visit)
{
    if (var_layout == nullptr)
    {
        return;
    }

    slang::TypeLayoutReflection* typeLayout = var_layout->getTypeLayout();
    if (typeLayout == nullptr)
    {
        return;
    }

    const uint32_t offset = base_offset + static_cast<uint32_t>(var_layout->getOffset(category));
    path.push_back(var_layout);

    if (typeLayout->getKind() == slang::TypeReflection::Kind::Struct)
    {
        const unsigned int fieldCount = typeLayout->getFieldCount();
        for (unsigned int i = 0u; i < fieldCount; ++i)
        {
            VisitLeaves(typeLayout->getFieldByIndex(i), category, offset, path, visit);
        }
    }
    else
    {
        visit(LeafVisit{ .Var = var_layout,
                         .Type = typeLayout,
                         .Offset = offset,
                         .Path = std::span{ std::as_const(path) } });
    }

    path.pop_back();
}

/** Walks the tree one var layout roots. The offset of that var layout counts. */
template<typename LeafVisitor>
void WalkVaryingTree(slang::VariableLayoutReflection* var_layout,
                     SlangParameterCategory category,
                     const LeafVisitor& visit)
{
    std::vector<slang::VariableLayoutReflection*> path;
    VisitLeaves(var_layout, category, 0u, path, visit);
}

/** Joins the field names of one path with dots. Empty when a field on the path has no name, so a
 * caller that needs a full name can refuse a partial one. */
std::string JoinFieldNames(std::span<slang::VariableLayoutReflection* const> path)
{
    if (path.empty())
    {
        return std::string{};
    }

    std::string result;

    for (auto* field : path)
    {
        if (!result.empty())
        {
            result.append(".");
        }

        result.append(field->getName());
    }

    return result;
}

std::string_view ReadSemanticName(slang::VariableLayoutReflection* var_layout) noexcept
{
    const char* name = var_layout->getSemanticName();
    return name == nullptr ? std::string_view{} : std::string_view{ name };
}

/** Records every attribute a vertex buffer must supply. */
void CollectVertexInputs(slang::VariableLayoutReflection* var_layout, ReflectedRasterState& raster)
{
    auto vertexInputVisitor = [&raster](const LeafVisit& leaf)
    {
        const std::string_view semantic = ReadSemanticName(leaf.Var);
        if (semantic.empty() || IsSystemSemantic(semantic))
        {
            return;
        }

        ReflectedVertexInput input;
        input.SemanticName = std::string{ semantic };
        input.Data.SemanticIndex = static_cast<uint32_t>(leaf.Var->getSemanticIndex());
        input.Data.Location = leaf.Offset;
        ReadVaryingShape(leaf.Type, input.Data.ScalarType, input.Data.ComponentCount);

        raster.VertexInputs.emplace_back(std::move(input));
    };
    WalkVaryingTree(var_layout, SLANG_PARAMETER_CATEGORY_VARYING_INPUT, vertexInputVisitor);
}

/** Records every color target the fragment result declares, and whether it writes depth itself. */
void CollectColorTargets(slang::VariableLayoutReflection* var_layout, ReflectedRasterState& raster)
{
    auto varyingVisitor = [&raster](const LeafVisit& leaf)
    {
        if (IsDepthSemantic(ReadSemanticName(leaf.Var)))
        {
            raster.WritesFragDepth = true;
            return;
        }

        ReflectedColorTarget target;
        target.Location = leaf.Offset;
        ReadVaryingShape(leaf.Type, target.ScalarType, target.ComponentCount);

        if (target.ComponentCount != 0u)
        {
            raster.ColorTargets.emplace_back(target);
        }
    };
    WalkVaryingTree(var_layout, SLANG_PARAMETER_CATEGORY_VARYING_OUTPUT, varyingVisitor);
}

/** A fragment shader can write depth through an `out` parameter rather than through its result,
 * so that tree needs a walk of its own. */
void CollectDepthWrites(slang::VariableLayoutReflection* var_layout, ReflectedRasterState& raster)
{
    WalkVaryingTree(var_layout,
                    SLANG_PARAMETER_CATEGORY_VARYING_OUTPUT,
                    [&raster](const LeafVisit& leaf)
                    {
                        if (IsDepthSemantic(ReadSemanticName(leaf.Var)))
                        {
                            raster.WritesFragDepth = true;
                        }
                    });
}

/** Names the scope of every binding range of one layout, in place.
 *
 * Slang flattens a scope into one list of binding ranges, and `getFieldBindingRangeOffset` is the
 * only thing that says which field a range came from. Walk the fields to recover the path the
 * emitter writes: a struct field adds its name to the chain, and a resource field does not,
 * because the leaf variable already carries that name. */
/** One reflected number, or nothing.
 *
 * Slang answers `SLANG_UNKNOWN_SIZE` when a value depends on an unresolved generic parameter or
 * on a link-time constant. That answer is not a number, and a cook that carried it would place a
 * binding at a location no shader holds. Every read of a placement number goes through here. */
constexpr std::optional<uint32_t> ResolvedNumber(size_t value) noexcept
{
    if (value == SLANG_UNKNOWN_SIZE)
    {
        return std::nullopt;
    }

    return static_cast<uint32_t>(value);
}

std::optional<uint32_t> ReadOffset(slang::VariableLayoutReflection* var_layout,
                                   SlangParameterCategory category)
{
    return var_layout != nullptr ? ResolvedNumber(var_layout->getOffset(category)) : std::nullopt;
}

/** Says that one range holds a number reflection could not resolve. Both walks report the same
 * fact about different ranges, so both report it in the same words. */
void ReportUnresolvedLocation(std::string_view range_kind, SlangInt range_index)
{
    std::println(stderr,
                 "[shader_cooker] {} range {} has an unresolved location: link-time constants are "
                 "affecting reflection output",
                 range_kind,
                 range_index);
}

/** Adds one name to a scope chain. Slang joins a scope to what it holds with an underscore, and
 * a chain that starts empty takes the name alone. */
std::string JoinScopeName(std::string_view prefix, const char* name)
{
    if (name == nullptr)
    {
        return std::string{ prefix };
    }

    std::string joined{ prefix };
    joined += joined.empty() ? "" : "_";
    joined += name;
    return joined;
}

// NOLINTNEXTLINE(misc-no-recursion)
void CollectScopeNames(slang::TypeLayoutReflection* layout,
                       const std::string& prefix,
                       SlangInt base,
                       std::vector<std::string>& out_names)
{
    const unsigned int fieldCount = layout->getFieldCount();
    for (unsigned int fieldIndex = 0u; fieldIndex < fieldCount; ++fieldIndex)
    {
        slang::VariableLayoutReflection* field = layout->getFieldByIndex(fieldIndex);
        slang::TypeLayoutReflection* fieldLayout = field != nullptr ? field->getTypeLayout() : nullptr;
        if (fieldLayout == nullptr)
        {
            continue;
        }

        const SlangInt fieldBase = base + layout->getFieldBindingRangeOffset(fieldIndex);
        const SlangInt fieldRangeCount = fieldLayout->getBindingRangeCount();
        if (fieldRangeCount <= 0 || fieldBase < 0)
        {
            continue;
        }

        if (fieldLayout->getKind() == slang::TypeReflection::Kind::Struct)
        {
            CollectScopeNames(fieldLayout, JoinScopeName(prefix, field->getName()), fieldBase, out_names);
            continue;
        }

        const SlangInt last =
            std::min<SlangInt>(fieldBase + fieldRangeCount, static_cast<SlangInt>(out_names.size()));
        // std::fill the tail here
        std::fill(out_names.begin() + static_cast<std::ptrdiff_t>(fieldBase),
                  out_names.begin() + static_cast<std::ptrdiff_t>(last),
                  prefix);
    }
}

/**@brief Reads one subobject range as a parameter block, or returns `std::nullopt` if it can't manage that.
 *
 * @param containing_layout The layout of the type containing the sub-object range.
 * @param sub_object_index The index of the sub-object range within the containing layout.
 * @param scope The binding scope in which the parameter block resides.
 * @return An optional `ParameterBlockInfo` describing the parameter block, or `std::nullopt` if it cannot be
 * read.
 *
 * @note Expanding on this, two numbers had to come from places one wouldn't expect. First, the *bindable*
 * contents of every block always start at binding zero because their own range offsets count from the start
 * of the space, and step over the container. A block of two resources gave bindings 0 and 1, but when we
 * added a float4 that became 1 and 2.... and now the root container was at 0, because the raw float4 isn't
 * itself a binding.
 */
std::optional<ParameterBlockInfo> ReadParameterBlock(slang::TypeLayoutReflection* containing_layout,
                                                     SlangInt sub_object_index,
                                                     const BindingScope& scope)
{
    const SlangInt rangeIndex = containing_layout->getSubObjectRangeBindingRangeIndex(sub_object_index);
    if (rangeIndex < 0 || containing_layout->getBindingRangeDescriptorSetIndex(rangeIndex) >= 0)
    {
        return std::nullopt;
    }

    // A sub-object range is not always a block. Keep only the two this walk understands.
    const slang::BindingType bindingType = containing_layout->getBindingRangeType(rangeIndex);
    if (bindingType != slang::BindingType::ParameterBlock &&
        bindingType != slang::BindingType::ConstantBuffer)
    {
        return std::nullopt;
    }

    slang::TypeLayoutReflection* blockLayout = containing_layout->getBindingRangeLeafTypeLayout(rangeIndex);
    slang::VariableLayoutReflection* spaceOffset =
        containing_layout->getSubObjectRangeOffset(sub_object_index);
    if (blockLayout == nullptr || spaceOffset == nullptr || blockLayout->getElementTypeLayout() == nullptr)
    {
        return std::nullopt;
    }

    const std::optional<uint32_t> space =
        ReadOffset(spaceOffset, SLANG_PARAMETER_CATEGORY_SUB_ELEMENT_REGISTER_SPACE);
    if (!space)
    {
        ReportUnresolvedLocation("parameter block", rangeIndex);
        return std::nullopt;
    }

    slang::VariableReflection* blockVariable = containing_layout->getBindingRangeLeafVariable(rangeIndex);
    const char* blockName = blockVariable != nullptr ? blockVariable->getName() : nullptr;

    ParameterBlockInfo block;
    block.ElementLayout = blockLayout->getElementTypeLayout();
    block.Container = blockLayout->getContainerVarLayout();
    block.Name = blockName != nullptr ? std::string_view{ blockName } : std::string_view{};
    block.Scope.Base.Group = scope.SpaceBase + *space;
    // A block inside a block takes a space of its own, and it counts from the space of this one.
    block.Scope.SpaceBase = block.Scope.Base.Group;
    block.Scope.Name = JoinScopeName(scope.Name, blockName);

    // An unresolved size reads as no ordinary data at all, which is the answer that drafts no
    // container. A block whose size a link-time constant decides has no fixed container anyway.
    block.UniformSize =
        ResolvedNumber(block.ElementLayout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM)).value_or(0u);
    return block;
}


/** Moves each draft into the binding list, and keys each annotation against the position its
 * binding takes. The caller settles the order first, because `BindingIndex` is that position. */
void AppendBindingDrafts(std::vector<RawBindingDraft>& drafts,
                         std::vector<RawBinding>& out_bindings,
                         std::vector<RawSizeAttribute>& out_attributes)
{
    const auto baseIndex = static_cast<uint32_t>(out_bindings.size());
    for (auto&& [offset, draft] : std::views::enumerate(drafts))
    {
        for (RawSizeAttribute& attribute : draft.Attributes)
        {
            attribute.BindingIndex = baseIndex + static_cast<uint32_t>(offset);
        }
    }

    out_bindings.insert_range(out_bindings.end(), drafts | std::views::transform(&RawBindingDraft::Binding));
    // std::views::join will effectively expand to a push_back for each element in the joined ranges, so
    // reserve upfront
    const size_t attributesCount = std::ranges::fold_left(drafts | std::views::transform(
                                                                       [](const RawBindingDraft& draft)
                                                                       {
                                                                           return draft.Attributes.size();
                                                                       }),
                                                          size_t{ 0u },
                                                          std::plus<size_t>{});
    out_attributes.reserve(out_attributes.size() + attributesCount);
    out_attributes.insert_range(out_attributes.end(),
                                drafts | std::views::transform(&RawBindingDraft::Attributes) |
                                    std::views::join | std::views::as_rvalue);
}

/** Asks the metadata of one entry point which global bindings that entry point reads.
 *
 * `global_bindings` holds the global scope alone. Slang generates each entry point as its own
 * artifact, so two entry points can place different resources at one group and binding. A placement
 * query over the entry point rows would then let one entry point claim the parameter of another. */
std::vector<uint32_t> CollectUsedBindingIndices(const LinkedVariant& linked_variant,
                                                SlangInt entry_point_index,
                                                std::span<const RawBinding> global_bindings)
{
    const auto epIndex = static_cast<size_t>(entry_point_index);
    const Slang::ComPtr<slang::IMetadata>& metadata = linked_variant.EntryPointMetadata[epIndex];

    auto isUsedLambda = [&metadata](const RawPlacement& placement) -> bool
    {
        const BoundPlacement* boundPlacement = GetBoundPlacement(placement);
        if (boundPlacement == nullptr)
        {
            return false;
        }
        // calling across the dll boundary means no inlining this :(
        bool isUsed = false;
        metadata->isParameterLocationUsed(SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT,
                                          boundPlacement->Group,
                                          boundPlacement->Binding,
                                          isUsed);
        return isUsed;
    };

    // for each global binding, filter it out if it's not used by the entry point
    // extract just the index from it, and then return the result as a new std::vector<uint32_t>
    // note that we check entry point usage using the metadata here
    return std::views::enumerate(global_bindings) |
           std::views::filter(
               [&](const auto& pair)
               {
                   return isUsedLambda(std::get<1>(pair).Placement);
               }) |
           std::views::transform(
               [](const auto& pair)
               {
                   return std::get<0>(pair);
               }) |
           std::ranges::to<std::vector<uint32_t>>();
}

} // namespace

namespace lodestone
{

SlangReflector::SlangReflector(std::span<const std::string> entry_point_names,
                               DiagnosticSink* _sink,
                               slang::IGlobalSession* global_session,
                               PlacementKind active_placement_kind) noexcept
    : entryPointNames(entry_point_names),
      sink(_sink),
      globalSession(global_session),
      activePlacementKind(active_placement_kind)
{
}

CookResult<RawVariant> SlangReflector::Reflect(LinkedVariant& linked_variant,
                                               const VariantDescriptor& descriptor)
{
    RawVariant rawVariant;
    rawVariant.VariantSuffix = MakeAssignmentSuffix(descriptor.Canonical);
    rawVariant.VariantDescription = DescribeAssignment(descriptor.Canonical);
    rawVariant.VariantIndex = static_cast<uint32_t>(descriptor.Index);

    // this step extracts the global bindings, which are put into the raw variant first
    // the global bindings are sorted by placement ordering before return from this fn
    const CookError bindingExtrResult =
        extractRawBindings(linked_variant.ProgramLayout, rawVariant.Bindings, rawVariant.SizeAttributes);
    if (bindingExtrResult != CookError::Success) [[unlikely]]
    {
        return std::unexpected(bindingExtrResult);
    }

    const size_t globalBindingCount = rawVariant.Bindings.size();
    for (int64_t i = 0; i < std::ssize(linked_variant.EntryPointStrings); ++i)
    {
        std::vector<RawBindingDraft> entryPointDrafts;
        const std::span<const RawBinding> globalBindings =
            std::span{ rawVariant.Bindings }.first(globalBindingCount);
        CookResult<RawEntryPoint> entryPointResult =
            extractRawEntryPoint(linked_variant, i, globalBindings, entryPointDrafts);
        if (!entryPointResult) [[unlikely]]
        {
            return std::unexpected(entryPointResult.error());
        }

        RawEntryPoint& rawEntryPoint = entryPointResult.value();

        const auto ownedBaseSize = static_cast<uint32_t>(rawVariant.Bindings.size());
        AppendBindingDrafts(entryPointDrafts, rawVariant.Bindings, rawVariant.SizeAttributes);

        // now we need to append the indices of the used bindings for *this* entry point
        auto newIndices = std::views::iota(ownedBaseSize, static_cast<uint32_t>(rawVariant.Bindings.size()));
        rawEntryPoint.UsedBindingIndices.append_range(newIndices);
        // set suffix, and copy over the target text
        rawEntryPoint.VariantSuffix = rawVariant.VariantSuffix;
        rawEntryPoint.TargetText = std::move(linked_variant.EntryPointStrings[static_cast<size_t>(i)]);
        // weird quirk: dereferencing a std::expected with * directly returns an rvalue. otherwise
        // we would have to do std::move(entryPointResult).value() lol
        rawVariant.EntryPoints.emplace_back(std::move(*entryPointResult));
    }

    return rawVariant;
}

CookError SlangReflector::applyLeafTypeUniformBufferLayout(slang::TypeLayoutReflection* buffer_leaf_layout,
                                                           RawBinding& binding) const
{
    // A uniform block's size is fully determined. The graph must never take it from the caller.
    binding.ByteSize = static_cast<uint64_t>(buffer_leaf_layout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM));
    slang::TypeLayoutReflection* elementLayout = buffer_leaf_layout->getElementTypeLayout();

    if (elementLayout != nullptr && binding.ByteSize == 0u)
    {
        binding.ByteSize = static_cast<uint64_t>(elementLayout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM));
    }

    // if bytesize is still 0, we have a problem and should error out
    if (binding.ByteSize == 0u)
    {
        // log a diagnostic so it's clear where this probably came from
        const char* bufferName = buffer_leaf_layout->getName();
        std::string message = std::format("Failed to determine byte size of buffer element {}",
                                          bufferName != nullptr ? bufferName : "<unknown>");
        return ReportError(*sink, CookError::ReflectionCouldNotFindBufferElementSize, std::move(message));
    }

    const CookError uniformWalkResult = collectUniformMembers(elementLayout, binding.UniformMembers);

    if (!uniformWalkResult)
    {
        return uniformWalkResult;
    }

    return CookError::Success;
}

/** Reads the size, shape, and type facts off one binding range's leaf type layout.
 *
 * Slang wraps a resource type around the type it carries, so the useful facts sit one level down.
 * A structured buffer reports its element layout. A texture reports the type it returns. */
CookError SlangReflector::applyLeafTypeLayout(slang::TypeLayoutReflection* containing_layout,
                                              SlangInt range_index,
                                              slang::BindingType binding_type,
                                              RawBinding& binding) const
{
    slang::TypeLayoutReflection* leafLayout = containing_layout->getBindingRangeLeafTypeLayout(range_index);
    if (leafLayout == nullptr)
    {
        // just reached end of leaf
        return CookError::Success;
    }

    slang::TypeReflection* leafType = leafLayout->getType();
    if (leafType == nullptr)
    {
        return CookError::Success;
    }

    // this will just set shape back to invalid for things like samplers, but otherwise most of the
    // other binding kinds will have their shape determined correctly.
    binding.Shape = FromSlangResourceShape(leafType->getResourceShape());

    switch (binding.Kind)
    {
    case BindingKind::Sampler:
        binding.Shape = ResourceShape::Invalid;
        binding.SamplerType = SamplerBindingType::Filtering;
        return CookError::Success;
    case BindingKind::StorageTexture:
        binding.StorageFormat =
            FromSlangImageFormat(containing_layout->getBindingRangeImageFormat(range_index));
        binding.StorageAccess = FromSlangBindingTypeAccess(binding_type);
        [[fallthrough]];
    case BindingKind::CombinedTextureSampler:
        [[fallthrough]];
    case BindingKind::Texture:
        // fixed: we used `getType()` here, but that just gives the texture: the scalar type of a slang
        // texture is invalid, so to get the actual sample type we need to use `getResourceResultType()`
        // makes sense, but an easy mistake to make
        if (slang::TypeReflection* resultType = leafType->getResourceResultType(); resultType != nullptr)
        {
            binding.SampleType = FromSlangScalarType(resultType->getScalarType());
        }
        return CookError::Success;
    case BindingKind::UniformBuffer:
        return applyLeafTypeUniformBufferLayout(leafLayout, binding);
    case BindingKind::ReadOnlyStructuredBuffer:
        [[fallthrough]];
    case BindingKind::ReadOnlyStorageBuffer:
        [[fallthrough]];
    case BindingKind::StructuredBuffer:
        [[fallthrough]];
    case BindingKind::StorageBuffer:
        if (slang::TypeLayoutReflection* elementLayout = leafLayout->getElementTypeLayout();
            elementLayout != nullptr) [[likely]]
        {
            binding.ElementStride = static_cast<uint32_t>(elementLayout->getSize());
            return CookError::Success;
        }
        else
        {
            std::string message = std::format("Could not find element stride for binding: {}", binding.Name);
            return ReportError(*sink, CookError::ReflectionCouldNotFindBufferElementSize, std::move(message));
        }
    case BindingKind::Invalid:
        [[fallthrough]];
    case BindingKind::InlineUniform:
        [[fallthrough]];
    case BindingKind::RayTracingAccelerationStructure:
        [[fallthrough]];
    case BindingKind::InputRenderTarget:
        [[fallthrough]];
    case BindingKind::ParameterBlock:
        {
            std::string message =
                std::format("Unsupported binding kind: {}", magic_enum::enum_name(binding.Kind));
            return ReportError(*sink, CookError::ReflectionUnsupportedBindingKind, std::move(message));
        }
    }

    if (binding.Kind == BindingKind::UniformBuffer)
    {
    }

    slang::TypeLayoutReflection* elementLayout = leafLayout->getElementTypeLayout();
    if (elementLayout != nullptr)
    {
        binding.ElementStride = static_cast<uint32_t>(elementLayout->getSize());
    }
}

/** Reads one `[vx_*]` annotation into its argument strings. Stage 3 never evaluates one, so a
 * malformed argument is caught here only when it is not a string at all. */
CookError SlangReflector::collectRawSizeAttributes(slang::VariableReflection* leaf_variable,
                                                   std::string_view binding_name,
                                                   std::vector<RawSizeAttribute>& out_attributes) const
{
    if (leaf_variable == nullptr)
    {
        // end of leaf/chain
        return CookError::Success;
    }

    for (const RawSizeAttributeKind kind : k_SizeAttributeKinds)
    {
        const std::string attributeName{ ToString(kind) };
        slang::Attribute* found = leaf_variable->findAttributeByName(globalSession, attributeName.c_str());
        if (found == nullptr)
        {
            continue;
        }

        RawSizeAttribute attribute;
        attribute.Kind = kind;
        attribute.Arguments.reserve(ArgumentCountOf(kind));

        for (uint32_t i = 0u; i < ArgumentCountOf(kind); ++i)
        {
            CookResult<std::string> argument = ReadStringArgument(found, i, binding_name);
            if (!argument)
            {
                return argument.error();
            }

            attribute.Arguments.emplace_back(std::move(argument.value()));
        }

        out_attributes.emplace_back(std::move(attribute));
    }

    return CookError::Success;
}

/**Flattens one uniform block into rows of name, offset, and size. Nested fields take
 * a dotted name to reflect the hierarchy. Offset is accumulated from the root of the
 * struct, as slang structures offsets from zero based on recursive walks of all fields.
 */
CookError SlangReflector::collectUniformMembers(slang::TypeLayoutReflection* struct_layout,
                                                std::vector<ReflectedUniformMember>& members) const
{
    if (struct_layout == nullptr || struct_layout->getKind() != slang::TypeReflection::Kind::Struct)
    {
        return CookError::Success;
    }

    CookError result = CookError::Success;

    const auto visit = [&members, this, &result](const LeafVisit& leaf)
    {
        std::string name = JoinFieldNames(leaf.Path);
        if (name.empty())
        {
            return;
        }
        const auto memberSize = static_cast<uint32_t>(leaf.Type->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM));
        const auto arrayCount = leaf.Type->getKind() == slang::TypeReflection::Kind::Array
                                    ? static_cast<uint32_t>(leaf.Type->getElementCount())
                                    : 1u;
        const auto isPointerType = leaf.Type->getKind() == slang::TypeReflection::Kind::Pointer;
        if (isPointerType && (activePlacementKind == PlacementKind::Bound))
        {
            result = ReportError(
                *sink,
                CookError::PointerTypeNotSupported,
                std::format("Bound placement of resource with name {} cannot be a pointer.", name));
        }

        members.emplace_back(std::move(name), leaf.Offset, memberSize, arrayCount);
    };

    std::vector<slang::VariableLayoutReflection*> path;
    const unsigned int fieldCount = struct_layout->getFieldCount();
    for (unsigned int i = 0u; i < fieldCount; ++i)
    {
        VisitLeaves(struct_layout->getFieldByIndex(i), SLANG_PARAMETER_CATEGORY_UNIFORM, 0u, path, visit);
    }

    return result;
}

// clang-tidy whines about no recursion, and normally while I am indeed an anti-recursion zealot, in this
// case recursion is by far the best way to handle this (and slang damn near requires it)
// todo-ship: add a depth counter, just so we can break out if it gets beyond our max depth
// NOLINTBEGIN(misc-no-recursion)

/** Walks the binding ranges of one scope, and drafts a binding for each one. */
CookError SlangReflector::collectBindingRangeDrafts(slang::TypeLayoutReflection* containing_layout,
                                                    const BindingScope& scope,
                                                    std::vector<RawBindingDraft>& out_drafts) const
{
    if (containing_layout == nullptr)
    {
        // reached end of the binding range chain (hopefully)
        return CookError::Success;
    }

    const SlangInt bindingRangeCount = containing_layout->getBindingRangeCount();
    out_drafts.reserve(out_drafts.size() + static_cast<size_t>(bindingRangeCount));

    std::vector<std::string> scopeNames(static_cast<size_t>(bindingRangeCount), scope.Name);
    CollectScopeNames(containing_layout, scope.Name, 0, scopeNames);

    for (SlangInt rangeIndex = 0; rangeIndex < bindingRangeCount; ++rangeIndex)
    {
        const slang::BindingType bindingType = containing_layout->getBindingRangeType(rangeIndex);
        // Skip input/output attributes, which slang considers to be part of the global params
        if (bindingType == slang::BindingType::Unknown || bindingType == slang::BindingType::VaryingInput ||
            bindingType == slang::BindingType::VaryingOutput)
        {
            continue;
        }

        const SlangInt descriptorSetIndex = containing_layout->getBindingRangeDescriptorSetIndex(rangeIndex);
        const SlangInt descriptorRangeIndex =
            containing_layout->getBindingRangeFirstDescriptorRangeIndex(rangeIndex);
        if (descriptorSetIndex < 0 || descriptorRangeIndex < 0)
        {
            continue;
        }

        const std::optional<uint32_t> spaceOffset = ResolvedNumber(
            static_cast<size_t>(containing_layout->getDescriptorSetSpaceOffset(descriptorSetIndex)));
        const std::optional<uint32_t> indexOffset =
            ResolvedNumber(static_cast<size_t>(containing_layout->getDescriptorSetDescriptorRangeIndexOffset(
                descriptorSetIndex, descriptorRangeIndex)));
        if (!spaceOffset || !indexOffset)
        {
            ReportUnresolvedLocation("binding", rangeIndex);
            continue;
        }

        slang::VariableReflection* leafVariable = containing_layout->getBindingRangeLeafVariable(rangeIndex);
        const char* leafName = leafVariable != nullptr ? leafVariable->getName() : nullptr;

        RawBindingDraft draft;
        draft.Binding.Name = leafName != nullptr ? leafName : std::string{};
        draft.Binding.ScopeName = std::move(scopeNames[static_cast<size_t>(rangeIndex)]);
        draft.Binding.Placement = BoundPlacement{ .Group = scope.Base.Group + *spaceOffset,
                                                  .Binding = scope.Base.Binding + *indexOffset };
        draft.Binding.Kind = FromSlangBindingType(bindingType);
        draft.Binding.ArrayCount =
            static_cast<uint32_t>(containing_layout->getBindingRangeBindingCount(rangeIndex));

        const CookError applyLayoutError = applyLeafTypeLayout(containing_layout, rangeIndex, bindingType, draft.Binding);
        if (!applyLayoutError)
        {
            return applyLayoutError;
        }

        const CookError collectAttributesError =
            collectRawSizeAttributes(leafVariable, draft.Binding.Name, draft.Attributes);
        if (!collectAttributesError)
        {
            return collectAttributesError;
        }

        out_drafts.emplace_back(std::move(draft));
    }

    return collectSubObjectDrafts(containing_layout, scope, out_drafts);
}

/** The constant buffer Slang adds to hold the ordinary data of a block. Neither range list holds
 * it, and a client must create it, so it becomes a binding that carries the name of the block.
 *
 * A block of resources alone holds no such data and gets no such slot, so the caller asks only
 * when `UniformSize` is not zero. Drafting one anyway reports a binding the shader has not got. */
CookResult<RawBindingDraft> SlangReflector::readBlockContainer(const ParameterBlockInfo& block, std::string_view scope_name) const
{
    const std::optional<uint32_t> containerBinding =
        ReadOffset(block.Container, SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT);
    if (!containerBinding)
    {
        std::println(stderr,
                     "[shader_cooker] parameter block '{}' holds {} bytes of data and reports no "
                     "container binding",
                     block.Name,
                     block.UniformSize);
        return std::unexpected(CookError::ReflectionUnavailable);
    }

    RawBindingDraft draft;
    draft.Binding.Name = std::string{ block.Name };
    draft.Binding.ScopeName = std::string{ scope_name };
    draft.Binding.Placement = BoundPlacement{ .Group = block.Scope.Base.Group, .Binding = *containerBinding };
    draft.Binding.Kind = BindingKind::UniformBuffer;
    draft.Binding.ByteSize = static_cast<uint64_t>(block.UniformSize);
    const CookError uniformMembersError = collectUniformMembers(block.ElementLayout, draft.Binding.UniformMembers);
    if (!uniformMembersError)
    {
        return std::unexpected(uniformMembersError);
    }
    return draft;
}

/**@brief Walks the parameter blocks of one scope, and drafts the container and the contents of each.
 *
 * @note Slang gives a parent scope only descriptor ranges for a block, so the binding range of the block
 * carries a descriptor set index of -1 and the range walk drops it. The contents live in a sub-object
 * range instead, and this is the only walk that reaches them. `ReadParameterBlock` reads one range, and
 * `ReadBlockContainer` reads the slot Slang adds to hold the ordinary data. */
CookError SlangReflector::collectSubObjectDrafts(slang::TypeLayoutReflection* containing_layout,
                                                 const BindingScope& scope,
                                                 std::vector<RawBindingDraft>& out_drafts) const
{
    const SlangInt subObjectRangeCount = containing_layout->getSubObjectRangeCount();

    for (SlangInt subObjectIndex = 0; subObjectIndex < subObjectRangeCount; ++subObjectIndex)
    {
        const std::optional<ParameterBlockInfo> block =
            ReadParameterBlock(containing_layout, subObjectIndex, scope);
        if (!block)
        {
            continue;
        }

        if (block->UniformSize != 0u)
        {
            CookResult<RawBindingDraft> container = readBlockContainer(*block, scope.Name);
            if (!container)
            {
                return container.error();
            }

            out_drafts.emplace_back(std::move(container.value()));
        }

        const CookError contentsError =
            collectBindingRangeDrafts(block->ElementLayout, block->Scope, out_drafts);
        if (contentsError != CookError::Success)
        {
            return contentsError;
        }
    }

    return CookError::Success;
}

// NOLINTEND(misc-no-recursion)

/** The parameter scope of one entry point: where it starts, and the name Slang emits around it.
 *
 * Slang reports an unresolved offset as `SLANG_UNKNOWN_SIZE`. That answer cannot be a placement, and
 * a cook that carried it would emit a binding at a location no shader holds.
 *
 * The scope name is the parameter's own name when the entry point declares one parameter of a struct
 * type. Slang collects loose `uniform` parameters into a synthetic struct instead, and names that
 * `entryPointParams`. Both answers come from the same call, so neither is a special case here. */
CookResult<BindingScope> SlangReflector::entryPointScope(slang::EntryPointReflection* entry_point_layout,
                                                         std::string_view entry_point_name) const
{
    slang::VariableLayoutReflection* varLayout = entry_point_layout->getVarLayout();
    if (varLayout == nullptr)
    {
        return BindingScope{};
    }

    const std::optional<uint32_t> group =
        ResolvedNumber(varLayout->getBindingSpace(SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT));
    const std::optional<uint32_t> binding =
        ReadOffset(varLayout, SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT);
    const std::optional<uint32_t> spaceBase =
        ReadOffset(varLayout, SLANG_PARAMETER_CATEGORY_SUB_ELEMENT_REGISTER_SPACE);
    if (!group || !binding || !spaceBase)
    {
        Diagnostic report;
        report.Severity = DiagnosticSeverity::Error;
        report.Context = "entryPointScope";
        report.Message =
            std::format("entry point {} has an unresolved parameter scope offset", entry_point_name);
        sink->Report(report);
        return std::unexpected(CookError::ReflectionUnavailable);
    }

    return BindingScope{ .Base = BoundPlacement{ .Group = *group, .Binding = *binding },
                         .SpaceBase = *spaceBase,
                         .Name = std::string{ k_EntryPointScopeName } };
}

CookError SlangReflector::extractRawBindings(slang::ProgramLayout* program_layout,
                                             std::vector<RawBinding>& out_bindings,
                                             std::vector<RawSizeAttribute>& out_attributes) const
{
    std::vector<RawBindingDraft> drafts;
    const CookError walkError =
        collectBindingRangeDrafts(program_layout->getGlobalParamsTypeLayout(), BindingScope{}, drafts);
    if (!walkError)
    {
        return walkError;
    }

    // todo-ship: do we actually need stable_sort? if placements are equal, that's an error: it means they
    // have same group and binding, and that shouldn't be possible.
    std::ranges::stable_sort(drafts,
                             PlacementLess,
                             [](const RawBindingDraft& draft) -> const RawPlacement&
                             {
                                 return draft.Binding.Placement;
                             });

    AppendBindingDrafts(drafts, out_bindings, out_attributes);

    return CookError::Success;
}

CookResult<RawEntryPoint> SlangReflector::extractRawEntryPoint(const LinkedVariant& linked_variant,
                                                               int64_t entry_point_index,
                                                               std::span<const RawBinding> global_bindings,
                                                               std::vector<RawBindingDraft>& out_drafts)
{
    RawEntryPoint rawEntryPoint;
    rawEntryPoint.Name = entryPointNames[static_cast<size_t>(entry_point_index)];

    slang::EntryPointReflection* entryPointLayout =
        linked_variant.ProgramLayout->getEntryPointByIndex(static_cast<SlangUInt>(entry_point_index));
    if (entryPointLayout == nullptr)
    {
        return rawEntryPoint;
    }

    rawEntryPoint.Stage = FromSlangStage(entryPointLayout->getStage());

    if (rawEntryPoint.Stage == ShaderStageKind::Compute)
    {
        std::array<SlangUInt, k_WorkgroupAxisCount> workgroupSizes{ 1u, 1u, 1u };
        entryPointLayout->getComputeThreadGroupSize(k_WorkgroupAxisCount, workgroupSizes.data());
        rawEntryPoint.Workgroup.X = static_cast<uint32_t>(workgroupSizes[0]);
        rawEntryPoint.Workgroup.Y = static_cast<uint32_t>(workgroupSizes[1]);
        rawEntryPoint.Workgroup.Z = static_cast<uint32_t>(workgroupSizes[2]);
    }

    extractRasterState(entryPointLayout, rawEntryPoint.Stage, rawEntryPoint.Raster);

    rawEntryPoint.UsedBindingIndices =
        CollectUsedBindingIndices(linked_variant, entry_point_index, global_bindings);

    // A `uniform` parameter on the entry point takes a placement in the space the global bindings
    // use, and the entry point var layout says where that scope starts.
    CookResult<BindingScope> scope = entryPointScope(entryPointLayout, rawEntryPoint.Name);
    if (!scope)
    {
        return std::unexpected(scope.error());
    }

    const CookError walkError =
        collectBindingRangeDrafts(entryPointLayout->getTypeLayout(), scope.value(), out_drafts);
    if (!walkError)
    {
        return std::unexpected(walkError);
    }

    return rawEntryPoint;
}

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch-enum"
#endif

void SlangReflector::extractRasterState(slang::EntryPointReflection* entry_point_layout,
                                        ShaderStageKind stage,
                                        ReflectedRasterState& raster)
{
    switch (stage)
    {
    case ShaderStageKind::Vertex:
        CollectVertexInputs(entry_point_layout->getVarLayout(), raster);
        break;
    case ShaderStageKind::Fragment:
        CollectColorTargets(entry_point_layout->getResultVarLayout(), raster);
        CollectDepthWrites(entry_point_layout->getVarLayout(), raster);
        break;
    case ShaderStageKind::Compute:
        break;
    default:
        std::println(stderr, "Unsupported shader stage in extractRasterState: {}", static_cast<int>(stage));
        break;
    }
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif

} // namespace lodestone
