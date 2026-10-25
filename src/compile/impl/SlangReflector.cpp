#include "SlangReflector.hpp"
#include "CookerErrors.hpp"
#include "ShaderLibraryTypes.hpp"
#include "SlangCompilerTypes.hpp"
#include "SlangVariantCompiler.hpp"
#include "Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "magic_enum/magic_enum.hpp"
#include "model/ShaderDataSchema.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationSpace.hpp"
#include "target/ShimTable.hpp"
#include "target/StorageFormatTable.hpp"
#include "slang-com-ptr.h"
#include "slang.h"
#include "target/TargetProfile.hpp"
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
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using namespace lodestone;

/** Slang's name hint for the scope of entry point `uniform` parameters. Reflection never reports it. */
constexpr std::string_view k_EntryPointScopeName = "entryPointParams";
constexpr const char* k_ShimValueAttributeName = "ls_shim_value";

constexpr SlangUInt k_WorkgroupAxisCount = 3u;

constexpr std::array<RawSizeAttributeKind, 4u> k_SizeAttributeKinds{ RawSizeAttributeKind::ElementCount,
                                                                     RawSizeAttributeKind::Extent1d,
                                                                     RawSizeAttributeKind::Extent2d,
                                                                     RawSizeAttributeKind::Extent3d };

/** Returns an error when the argument is not a string literal: an annotation the author wrote must not be skipped. */
CookResult<std::string> ReadStringArgument(DiagnosticSink& sink,
                                           slang::Attribute* attribute,
                                           uint32_t argument_index,
                                           std::string_view binding_name)
{
    size_t length = 0u;
    const char* text = attribute->getArgumentValueString(argument_index, &length);
    if (text == nullptr)
    {
        return std::unexpected(ReportError(sink,
                                           CookError::AttributeExpressionParseFailed,
                                           std::format("argument {} of [{}] on '{}' is not a string literal",
                                                       argument_index,
                                                       attribute->getName(),
                                                       binding_name)));
    }

    // some paths include the quotes
    std::string_view value{ text, length };
    if (value.size() >= 2u && value.front() == '"' && value.back() == '"')
    {
        value = value.substr(1u, value.size() - 2u);
    }

    return std::string{ value };
}

/** Splits one varying into a scalar type and a component count. A scalar counts as one component. */
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

/** One leaf of a varying or uniform tree. `Path` lists the var layouts from the root to the leaf. */
struct LeafVisit
{
    slang::VariableLayoutReflection* Var{ nullptr };
    slang::TypeLayoutReflection* Type{ nullptr };
    /** Summed from the root, in the category the walk asked for. */
    uint32_t Offset{ 0u };
    std::span<slang::VariableLayoutReflection* const> Path;
};

/** Calls `visit` for each leaf of one tree. Slang offsets are relative to the parent, so the walker sums them. */
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

/** Walks the tree under one var layout, including that var layout's offset. */
template<typename LeafVisitor>
void WalkVaryingTree(slang::VariableLayoutReflection* var_layout,
                     SlangParameterCategory category,
                     const LeafVisitor& visit)
{
    std::vector<slang::VariableLayoutReflection*> path;
    VisitLeaves(var_layout, category, 0u, path, visit);
}

/** Joins the field names of a path with dots. Returns an empty string when any field has no name. */
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

/** Depth can also be written through an `out` parameter, so that tree gets its own walk. */
void CollectDepthWrites(slang::VariableLayoutReflection* var_layout, ReflectedRasterState& raster)
{
    auto varyingVisitor = [&raster](const LeafVisit& leaf)
    {
        if (IsDepthSemantic(ReadSemanticName(leaf.Var)))
        {
            raster.WritesFragDepth = true;
        }
    };
    WalkVaryingTree(var_layout,
                    SLANG_PARAMETER_CATEGORY_VARYING_OUTPUT,
                    varyingVisitor);
}

/** Returns no value for `SLANG_UNKNOWN_SIZE` (an unresolved generic or link-time constant). Every placement read
 * goes through this, so that no binding is placed where no shader uses it. */
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

void ReportUnresolvedLocation(DiagnosticSink& sink, std::string_view range_kind, SlangInt range_index)
{
    ReportWarning(sink,
                  std::format("{} range {} has an unresolved location: link-time constants are affecting reflection output",
                              range_kind,
                              range_index));
}

/** Joins a scope and a name with an underscore, as Slang does. */
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
        std::fill(out_names.begin() + static_cast<std::ptrdiff_t>(fieldBase),
                  out_names.begin() + static_cast<std::ptrdiff_t>(last),
                  prefix);
    }
}

/** Reads one sub-object range as a parameter block, or returns no value when the range is not one. */
std::optional<ParameterBlockInfo> ReadParameterBlock(DiagnosticSink& sink,
                                                     slang::TypeLayoutReflection* containing_layout,
                                                     SlangInt sub_object_index,
                                                     const BindingScope& scope)
{
    const SlangInt rangeIndex = containing_layout->getSubObjectRangeBindingRangeIndex(sub_object_index);
    slang::TypeLayoutReflection* leafLayout = containing_layout->getBindingRangeLeafTypeLayout(rangeIndex);
    if (leafLayout->getKind() != slang::TypeReflection::Kind::ParameterBlock)
    {
        return std::nullopt;
    }

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
        ReportUnresolvedLocation(sink, "parameter block", rangeIndex);
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

    // an unresolved size drafts no container, as a block sized by a link-time constant has no fixed one
    block.UniformSize =
        ResolvedNumber(block.ElementLayout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM)).value_or(0u);
    return block;
}


/** Moves the drafts into the binding list. The caller sorts them first, because `BindingIndex` is the position. */
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
    // todo-ship: either find a way to get cache_latest in here or just stop doing this
    // its sad but ranges just don't produce the best assembly used like this
    const size_t attributesCount = std::ranges::fold_left(drafts | std::views::transform(
                                                                       [](const RawBindingDraft& draft)
                                                                       {
                                                                           return draft.Attributes.size();
                                                                       }),
                                                          size_t{ 0u },
                                                          std::plus<size_t>{});
    out_attributes.reserve(out_attributes.size() + attributesCount);
    out_attributes.append_range(drafts | std::views::transform(&RawBindingDraft::Attributes) |
                                std::views::join | std::views::as_rvalue);
}

/** Returns the global bindings that one entry point reads. Each entry point is its own artifact, so two can place
 * different resources at one (group, binding), and the query must not cross entry points. */
std::vector<uint32_t> CollectUsedBindingIndices(const LinkedVariant& linked_variant,
                                                SlangInt entry_point_index,
                                                std::span<const RawBinding> global_bindings,
                                                uint32_t index_offset)
{
    const auto epIndex = static_cast<size_t>(entry_point_index);
    const Slang::ComPtr<slang::IMetadata>& metadata = linked_variant.EntryPointMetadata[epIndex];

    // todo-ship: this will need to be modified per-binding-model
    auto isPlacementUsed = [&metadata](const RawPlacement& placement) -> bool
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

    std::vector<uint32_t> usedBindingIndices;
    usedBindingIndices.reserve(global_bindings.size());

    for (const auto&& [index, binding] : std::views::enumerate(global_bindings))
    {
        if (!isPlacementUsed(binding.Placement))
        {
            continue;
        }

        usedBindingIndices.emplace_back(index_offset + static_cast<uint32_t>(index));

    }

    usedBindingIndices.shrink_to_fit();
    return usedBindingIndices;
}

/** Adds the stored values whose container the entry point reads: a used uniform buffer, or the push constants of the
 * entry point itself. A value that the code does not read is visible too, which costs the renderer one write. */
void AddVisibleStoredValues(std::span<const RawBinding> bindings,
                            size_t global_count,
                            size_t first_owned,
                            bool reads_push_constants,
                            std::vector<uint32_t>& used)
{
    // we test against this span during each iteration, and it should only include the elements that were present before this function call,
    // ensuring that we do not consider any newly added used indices within the same iteration loop.
    const std::span<const uint32_t> testSpan{ used };
    for (size_t index = 0u; index < bindings.size(); ++index)
    {
        const StoredValuePlacement* stored = GetStoredValuePlacement(bindings[index].Placement);
        if (stored == nullptr || (index >= global_count && index < first_owned))
        {
            continue;
        }

        bool visible = false;
        if (stored->Container == StoredValueContainer::PushConstants)
        {
            visible = reads_push_constants && index >= first_owned;
        }
        else
        {
            auto placementUsed = [&](uint32_t used_index)
            {
                const BoundPlacement* block = GetBoundPlacement(bindings[used_index].Placement);
                return block != nullptr && *block == stored->Block;
            };
            visible = std::ranges::any_of(testSpan, placementUsed);
        }

        if (visible)
        {
            used.push_back(static_cast<uint32_t>(index));
        }
    }
}

/** The type argument of an implementation's full name, such as `RGBA16F` in `HandleWriteStorageTexture2D<RGBA16F>`. */
std::string_view TypeArgumentOf(std::string_view full_name) noexcept
{
    const size_t open = full_name.find('<');
    const size_t close = full_name.rfind('>');
    return (open == std::string_view::npos || close == std::string_view::npos || close <= open)
               ? std::string_view{}
               : full_name.substr(open + 1u, close - open - 1u);
}

std::string FullNameOf(slang::TypeReflection* type)
{
    Slang::ComPtr<slang::IBlob> fullName;
    if (type == nullptr || SLANG_FAILED(type->getFullName(fullName.writeRef())) || fullName == nullptr)
    {
        return {};
    }
    return std::string{ static_cast<const char*>(fullName->getBufferPointer()) };
}

ResourceAccess FromSlangAccessType(SlangResourceAccess access)
{
    switch (access)
    {
    case SLANG_RESOURCE_ACCESS_NONE:
        return ResourceAccess::Invalid;
    case SLANG_RESOURCE_ACCESS_READ:
        return ResourceAccess::ReadOnly;
    case SLANG_RESOURCE_ACCESS_READ_WRITE:
        return ResourceAccess::ReadWrite;
    case SLANG_RESOURCE_ACCESS_RASTER_ORDERED:
        return ResourceAccess::RasterizerOrdered;
    case SLANG_RESOURCE_ACCESS_APPEND:
        return ResourceAccess::Append;
    case SLANG_RESOURCE_ACCESS_CONSUME:
        return ResourceAccess::Consume;
    case SLANG_RESOURCE_ACCESS_WRITE:
        return ResourceAccess::WriteOnly;
    case SLANG_RESOURCE_ACCESS_FEEDBACK:
        return ResourceAccess::Feedback;
    case SLANG_RESOURCE_ACCESS_UNKNOWN:
        return ResourceAccess::Invalid;
    }
    return ResourceAccess::Invalid;
}

} // namespace

namespace lodestone
{

SlangReflector::SlangReflector(std::span<const std::string> entry_point_names,
                               DiagnosticSink* _sink,
                               slang::IGlobalSession* global_session,
                               AccessModel buffer_form) noexcept
    : entryPointNames(entry_point_names),
      sink(_sink),
      globalSession(global_session),
      bufferForm(buffer_form)
{
}

CookResult<RawVariant> SlangReflector::Reflect(LinkedVariant& linked_variant,
                                               const VariantDescriptor& descriptor)
{
    RawVariant rawVariant;
    rawVariant.VariantSuffix = MakeAssignmentSuffix(descriptor.Canonical);
    rawVariant.VariantDescription = DescribeAssignment(descriptor.Canonical);
    rawVariant.VariantIndex = descriptor.Index;

    const CookError bindingExtrResult =
        extractRawBindings(linked_variant.ProgramLayout, rawVariant.Bindings, rawVariant.SizeAttributes);
    if (bindingExtrResult != CookError::Success) [[unlikely]]
    {
        return std::unexpected(bindingExtrResult);
    }

    const size_t globalBindingCount = rawVariant.Bindings.size();
    std::vector<RawBindingDraft> entryPointDrafts;
    entryPointDrafts.reserve(16u); // rarely more than this

    for (int64_t i = 0; i < std::ssize(linked_variant.EntryPointCode); ++i)
    {
        entryPointDrafts.clear(); // reuse the pre-allocated vector for this entry point

        // made again each time: AppendBindingDrafts below can reallocate the vector
        const std::span<const RawBinding> globalBindings = std::span{ rawVariant.Bindings }.first(globalBindingCount);
        CookResult<RawEntryPoint> entryPointResult = extractRawEntryPoint(linked_variant,
                                                                          i,
                                                                          globalBindings,
                                                                          entryPointDrafts);
        if (!entryPointResult) [[unlikely]]
        {
            return std::unexpected(entryPointResult.error());
        }

        RawEntryPoint& rawEntryPoint = entryPointResult.value();

        const uint32_t ownedBaseSize = static_cast<uint32_t>(rawVariant.Bindings.size());
        AppendBindingDrafts(entryPointDrafts, rawVariant.Bindings, rawVariant.SizeAttributes);

        if (rawEntryPoint.Constants.ContainerBinding)
        {
            *rawEntryPoint.Constants.ContainerBinding += ownedBaseSize;
        }

        std::span<const RawBinding> entryPointBindings = std::span{ rawVariant.Bindings }.subspan(ownedBaseSize);
        std::vector entryPointIndices = CollectUsedBindingIndices(linked_variant, i, entryPointBindings, ownedBaseSize);
        rawEntryPoint.UsedBindingIndices.append_range(std::move(entryPointIndices));
        AddVisibleStoredValues(rawVariant.Bindings,
                               globalBindingCount,
                               ownedBaseSize,
                               rawEntryPoint.Constants.Delivery == EntryPointConstantsDelivery::PushConstant,
                               rawEntryPoint.UsedBindingIndices);

        // the merged layout needs every used binding in placement order
        const std::span<const RawBinding> allBindings = rawVariant.Bindings;
        std::ranges::stable_sort(rawEntryPoint.UsedBindingIndices,
                                 PlacementLess,
                                 [allBindings](uint32_t binding_index) -> const RawPlacement&
                                 {
                                     const RawBinding& binding = allBindings[binding_index];
                                     return binding.Placement;
                                 });

        rawEntryPoint.VariantSuffix = rawVariant.VariantSuffix;
        
        // If there is no delivered entry point code, the base code is used as the target code.
        // Otherwise, the delivered code is used as the target code, and the base code is stored separately.
        std::vector<std::byte>& baseCode = linked_variant.EntryPointCode[static_cast<size_t>(i)];
        if (linked_variant.DeliveredEntryPointCode.empty())
        {
            rawEntryPoint.TargetCode = std::move(baseCode);
        }
        else
        {
            rawEntryPoint.TargetCode = std::move(linked_variant.DeliveredEntryPointCode[static_cast<size_t>(i)]);
            rawEntryPoint.BaseTargetCode = std::move(baseCode);
        }

        rawVariant.EntryPoints.emplace_back(std::move(*entryPointResult));
    }

    return rawVariant;
}

CookError SlangReflector::applyLeafTypeSamplerLayout(slang::TypeReflection* leaf_type,
                                                     RawBinding& binding) const
{
    binding.IsComparisonSampler = false;
    constexpr static std::string_view k_ComparisonSamplerName = "SamplerComparisonState";
    const char* typeName = leaf_type->getName();

    std::string_view typeNameView{ typeName };
    if (typeNameView == k_ComparisonSamplerName)
    {
        binding.IsComparisonSampler = true;
    }
    
    return CookError::Success;
}

CookError SlangReflector::applyLeafTypeTextureLayout(slang::TypeReflection* leaf_type,
                                                     RawBinding& binding) const
{
    // fixed: `getType()` returns the texture itself. The sample type is the resource result type
    if (slang::TypeReflection* resultType = leaf_type->getResourceResultType(); resultType != nullptr)
    {
        binding.SampleType = FromSlangScalarType(resultType->getScalarType());
    }

    // a depth texture reports float until the shape says shadow
    if (binding.SampleType == TextureSampleType::Float && ResourceShapeIsShadow(binding.Shape))
    {
        binding.SampleType = TextureSampleType::Depth;
        if (binding.Kind == BindingKind::CombinedTextureSampler)
        {
            binding.IsComparisonSampler = true;
        }
    }

    return CookError::Success;
}

CookError SlangReflector::applyLeafTypeUniformBufferLayout(slang::TypeLayoutReflection* buffer_leaf_layout,
                                                           RawBinding& binding,
                                                           std::vector<StoredValueLeaf>& out_values) const
{
    binding.ByteSize = static_cast<uint64_t>(buffer_leaf_layout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM));
    slang::TypeLayoutReflection* elementLayout = buffer_leaf_layout->getElementTypeLayout();

    if (elementLayout != nullptr && binding.ByteSize == 0u)
    {
        binding.ByteSize = static_cast<uint64_t>(elementLayout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM));
    }

    if (buffer_leaf_layout->getKind() == slang::TypeReflection::Kind::ParameterBlock &&
        binding.ByteSize == 0u)
    {
        return CookError::Success;
    }

    // a hybrid block (nested blocks plus data) has a nonzero size, so zero here is a fault
    if (binding.ByteSize == 0u)
    {
        const char* bufferName = buffer_leaf_layout->getName();
        std::string message = std::format("Failed to determine byte size of buffer element {}",
                                          bufferName != nullptr ? bufferName : "<unknown>");
        return ReportError(*sink, CookError::ReflectionCouldNotFindBufferElementLayout, std::move(message));
    }

    const CookError uniformWalkResult = collectStructMembers(elementLayout, binding.UniformMembers, &out_values);

    if (!uniformWalkResult)
    {
        return uniformWalkResult;
    }

    return CookError::Success;
}

CookError SlangReflector::applyLeafTypeStorageBufferLayout(slang::TypeLayoutReflection* leaf_layout,
                                                           RawBinding& binding) const
{
    if (GetBaseShape(binding.Shape) == ResourceShape::StructuredBuffer)
    {

        slang::TypeLayoutReflection* elementLayout = leaf_layout->getElementTypeLayout();
        if (elementLayout != nullptr)
        {
            binding.ElementStride = static_cast<uint32_t>(elementLayout->getSize());

            const CookError structWalkResult = collectStructMembers(elementLayout, binding.UniformMembers, nullptr);
            if (!structWalkResult)
            {
                return structWalkResult;
            }

            return CookError::Success;
        }
        else
        {
            std::string message = std::format("Could not find element layout for structured buffer: {}", binding.Name);
            return ReportError(*sink, CookError::ReflectionCouldNotFindBufferElementLayout, std::move(message));
        }

    }
    else
    {
        // zero tells clients the stride of a byte address buffer is not defined
        binding.ElementStride = 0u;
    }

    return CookError::Success;
}

CookError SlangReflector::applyLeafTypeTexelBufferLayout(slang::TypeLayoutReflection* containing_layout,
                                                         SlangInt range_index,
                                                         slang::TypeReflection* leaf_type,
                                                         RawBinding& binding) const
{
    // a read-only texel buffer reports a sample result type, and RWBuffer<T> a format
    if (binding.Access == ResourceAccess::ReadOnly)
    {

        if (slang::TypeReflection* resultType = leaf_type->getResourceResultType(); resultType != nullptr)
        {
            binding.SampleType = FromSlangScalarType(resultType->getScalarType());
        }

        return CookError::Success;
    }

    binding.StorageFormat =
        FromSlangImageFormat(containing_layout->getBindingRangeImageFormat(range_index));
    return CookError::Success;
}

/** Reads the size, shape, and type of one binding range from its leaf type layout, one level below the resource. */
CookError SlangReflector::applyLeafTypeLayout(slang::TypeLayoutReflection* containing_layout,
                                              SlangInt range_index,
                                              RawBinding& binding,
                                              std::vector<StoredValueLeaf>& out_values) const
{
    slang::TypeLayoutReflection* leafLayout = containing_layout->getBindingRangeLeafTypeLayout(range_index);
    if (leafLayout == nullptr)
    {
        return CookError::Success;
    }

    slang::TypeReflection* leafType = leafLayout->getType();
    if (leafType == nullptr)
    {
        return CookError::Success;
    }

    binding.Shape = FromSlangResourceShape(leafType->getResourceShape());
    binding.Access = FromSlangAccessType(leafType->getResourceAccess());

    // the kind is a coarse filter, and each child branches on shape and access
    switch (binding.Kind)
    {
    case BindingKind::Sampler:
        return applyLeafTypeSamplerLayout(leafType, binding);
    case BindingKind::CombinedTextureSampler:
        [[fallthrough]];
    case BindingKind::Texture:
        return applyLeafTypeTextureLayout(leafType, binding);
    case BindingKind::UniformBuffer:
        return applyLeafTypeUniformBufferLayout(leafLayout, binding, out_values);
    case BindingKind::ParameterBlock:
        std::unreachable(); // we filter out parameter blocks well before we get here
    case BindingKind::StorageBuffer:
        return applyLeafTypeStorageBufferLayout(leafLayout, binding);
    case BindingKind::TexelBuffer:
        return applyLeafTypeTexelBufferLayout(leafLayout, range_index, leafType, binding);
    case BindingKind::StorageTexture:
        binding.StorageFormat =
            FromSlangImageFormat(containing_layout->getBindingRangeImageFormat(range_index));
        return CookError::Success;
    case BindingKind::InputRenderTarget:
    case BindingKind::InlineUniform:
    case BindingKind::RayTracingAccelerationStructure:
    case BindingKind::PushConstant:
        return CookError::ReflectionUnsupportedBindingKind;
    case BindingKind::Invalid:
        std::unreachable(); // we should never encounter an invalid binding kind here
    }

    return CookError::ReflectionUnsupportedBindingKind;
}

/** Reads one size attribute into its argument strings. Stage 4 evaluates them. */
CookError SlangReflector::collectRawSizeAttributes(slang::VariableReflection* leaf_variable,
                                                   std::string_view binding_name,
                                                   std::vector<RawSizeAttribute>& out_attributes) const
{
    if (leaf_variable == nullptr)
    {
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

        for (int32_t i = 0; i < ArgumentCountOf(kind); ++i)
        {
            CookResult<std::string> argument = ReadStringArgument(*sink, found, i, binding_name);
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

/** Leaves `out_value` unchanged when the variable has no such attribute. */
CookError SlangReflector::readSingleStringAttribute(slang::VariableReflection* leaf_variable,
                                                    const char* attribute_name,
                                                    std::string_view binding_name,
                                                    std::string& out_value) const
{
    if (leaf_variable == nullptr)
    {
        return CookError::Success;
    }

    slang::Attribute* found = leaf_variable->findAttributeByName(globalSession, attribute_name);
    if (found == nullptr)
    {
        return CookError::Success;
    }

    CookResult<std::string> argument = ReadStringArgument(*sink, found, 0u, binding_name);
    if (!argument)
    {
        return argument.error();
    }

    out_value = std::move(argument.value());
    
    return CookError::Success;
}

/** Flattens one struct into members with dotted names and offsets from the root. */
CookError SlangReflector::collectStructMembers(slang::TypeLayoutReflection* struct_layout,
                                                std::vector<ReflectedUniformMember>& members,
                                                std::vector<StoredValueLeaf>* out_values) const
{
    if (struct_layout == nullptr || struct_layout->getKind() != slang::TypeReflection::Kind::Struct)
    {
        return CookError::Success;
    }

    CookError result = CookError::Success;

    const auto visit = [&members, out_values, this, &result](const LeafVisit& leaf)
    {
        std::string name = JoinFieldNames(leaf.Path);
        if (name.empty())
        {
            return;
        }

        AccessModel arrayForm{ AccessModel::Invalid };
        slang::TypeLayoutReflection* arrayElement =
            leaf.Type->getKind() == slang::TypeReflection::Kind::Array ? leaf.Type->getElementTypeLayout() : nullptr;
        if (arrayElement != nullptr && arrayElement->getType() != nullptr && arrayElement->getType()->getName() != nullptr &&
            FindShimInterfaceByImplementation(arrayElement->getType()->getName(), arrayForm) != nullptr)
        {
            result = ReportError(*sink,
                                 CookError::ShimStoredValueUnsupported,
                                 std::format("member {} is an array of {} shims, which the cooker does not read yet",
                                             name,
                                             ToString(arrayForm)));
            return;
        }

        slang::VariableReflection* variable = leaf.Var->getVariable();
        if (variable != nullptr && leaf.Path.size() >= 2u &&
            variable->findAttributeByName(globalSession, k_ShimValueAttributeName) != nullptr)
        {
            if (out_values == nullptr)
            {
                result = ReportError(*sink,
                                     CookError::ShimStoredValueUnsupported,
                                     std::format("member {} is a Handle or Pointer shim in a storage buffer element, which "
                                                 "the cooker does not read",
                                                 name));
                return;
            }

            StoredValueLeaf value;
            for (slang::VariableLayoutReflection* field : leaf.Path.first(leaf.Path.size() - 1u))
            {
                value.Scope = JoinScopeName(value.Scope, field->getName());
            }
            value.Name = variable->getName() != nullptr ? variable->getName() : std::string{};
            value.ByteOffset = leaf.Offset;
            value.Member = leaf.Type;
            value.Implementation = leaf.Path[leaf.Path.size() - 2u]->getTypeLayout()->getType();
            out_values->push_back(std::move(value));
        }
        
        uint32_t memberSize = static_cast<uint32_t>(leaf.Type->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM));
        // resources and varyings take no uniform bytes
        if (memberSize == 0u)
        {
            return;
        }
        const bool isArray = leaf.Type->getKind() == slang::TypeReflection::Kind::Array;
        uint32_t arrayCount = 1u;
        uint16_t stride = 0u;
        if (isArray)
        {
            arrayCount = static_cast<uint32_t>(leaf.Type->getElementCount());
            stride = static_cast<uint16_t>(leaf.Type->getElementStride(SLANG_PARAMETER_CATEGORY_UNIFORM));
        }
        const bool isPointerType = leaf.Type->getKind() == slang::TypeReflection::Kind::Pointer;
        if (isPointerType && (bufferForm != AccessModel::Pointer))
        {
            result = ReportError(
                *sink,
                CookError::PointerTypeNotSupported,
                std::format("member {} is a pointer, and the buffers of this profile use the {} form", name, ToString(bufferForm)));
        }
        const bool isMatrix = leaf.Type->getKind() == slang::TypeReflection::Kind::Matrix;
        MatrixLayout layout{ MatrixLayout::Invalid };
        if (isMatrix)
        {
            const SlangMatrixLayoutMode slangLayout = leaf.Type->getMatrixLayoutMode();
            switch (slangLayout)
            {
            case SLANG_MATRIX_LAYOUT_MODE_UNKNOWN:
                layout = MatrixLayout::Invalid;
                break;
            case SLANG_MATRIX_LAYOUT_ROW_MAJOR:
                layout = MatrixLayout::RowMajor;
                break;
            case SLANG_MATRIX_LAYOUT_COLUMN_MAJOR:
                layout = MatrixLayout::ColumnMajor;
                break;
            }
        }

        members.emplace_back(std::move(name),
                             ReflectedUniformMember::Packed{ leaf.Offset,
                                                             memberSize,
                                                             arrayCount,
                                                             stride,
                                                             layout });
    };

    std::vector<slang::VariableLayoutReflection*> path;
    const unsigned int fieldCount = struct_layout->getFieldCount();
    for (unsigned int i = 0u; i < fieldCount; ++i)
    {
        VisitLeaves(struct_layout->getFieldByIndex(i), SLANG_PARAMETER_CATEGORY_UNIFORM, 0u, path, visit);
    }

    return result;
}

// I am an anti-recursion zealot, but Slang damn near requires it here
// todo-ship: add a depth counter, so we can break out past our max depth
// NOLINTBEGIN(misc-no-recursion)

CookError SlangReflector::collectBindingRangeDrafts(slang::TypeLayoutReflection* containing_layout,
                                                    const BindingScope& scope,
                                                    std::vector<RawBindingDraft>& out_drafts) const
{
    if (containing_layout == nullptr)
    {
        return CookError::Success;
    }

    const SlangInt bindingRangeCount = containing_layout->getBindingRangeCount();
    out_drafts.reserve(out_drafts.size() + static_cast<size_t>(bindingRangeCount));

    std::vector<std::string> scopeNames(static_cast<size_t>(bindingRangeCount), scope.Name);
    CollectScopeNames(containing_layout, scope.Name, 0, scopeNames);

    for (SlangInt rangeIndex = 0; rangeIndex < bindingRangeCount; ++rangeIndex)
    {
        const slang::BindingType bindingType = containing_layout->getBindingRangeType(rangeIndex);
        // varyings are part of the global params to Slang
        if (bindingType == slang::BindingType::Unknown ||
            bindingType == slang::BindingType::VaryingInput ||
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
            ReportUnresolvedLocation(*sink, "binding", rangeIndex);
            continue;
        }

        slang::VariableReflection* leafVariable = containing_layout->getBindingRangeLeafVariable(rangeIndex);
        const char* leafName = leafVariable != nullptr ? leafVariable->getName() : nullptr;

        slang::TypeLayoutReflection* leafType = containing_layout->getBindingRangeLeafTypeLayout(rangeIndex);
        if (leafType->getKind() == slang::TypeReflection::Kind::ParameterBlock)
        {
            // collectSubObjectDrafts reads blocks
            continue;
        }

        RawBindingDraft draft;
        draft.Binding.Name = leafName != nullptr ? leafName : std::string{};
        draft.Binding.ScopeName = std::move(scopeNames[static_cast<size_t>(rangeIndex)]);
        draft.Binding.Placement = BoundPlacement{ .Group = scope.Base.Group + *spaceOffset,
                                                  .Binding = scope.Base.Binding + *indexOffset };
        draft.Binding.Kind = FromSlangBindingType(bindingType);
        const SlangInt bindingCount = containing_layout->getBindingRangeBindingCount(rangeIndex);
        draft.Binding.ArrayCount =
            static_cast<size_t>(bindingCount) == SLANG_UNBOUNDED_SIZE ? 0u : static_cast<uint32_t>(bindingCount);

        std::vector<StoredValueLeaf> storedValues;
        const CookError applyLayoutError = applyLeafTypeLayout(containing_layout,
                                                               rangeIndex,
                                                               draft.Binding,
                                                               storedValues);
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

        const CookError rolesError = readSingleStringAttribute(leafVariable, "ls_resource_role", draft.Binding.Name,
                                                               draft.Binding.RawRoles);
        if (!rolesError)
        {
            return rolesError;
        }

        const CookError tagsError = readSingleStringAttribute(leafVariable, "ls_resource_tags", draft.Binding.Name,
                                                              draft.Binding.RawTags);
        if (!tagsError)
        {
            return tagsError;
        }

        const BoundPlacement container = *GetBoundPlacement(draft.Binding.Placement);
        const std::string valueScope = JoinScopeName(draft.Binding.ScopeName, draft.Binding.Name.c_str());
        out_drafts.emplace_back(std::move(draft));
        const CookError storedError = appendStoredValueDrafts(storedValues,
                                                              StoredValueContainer::UniformBuffer,
                                                              container,
                                                              valueScope,
                                                              out_drafts);
        if (!storedError)
        {
            return storedError;
        }
    }

    return collectSubObjectDrafts(containing_layout, scope, out_drafts);
}

/** Drafts the constant buffer Slang adds for a block's ordinary data, named after the block. Only call it when
 * `UniformSize` is not zero: a block of resources alone has no such buffer. */
CookError SlangReflector::readBlockContainer(const ParameterBlockInfo& block,
                                             std::string_view scope_name,
                                             std::vector<RawBindingDraft>& out_drafts) const
{
    const std::optional<uint32_t> containerBinding =
        ReadOffset(block.Container, SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT);
    if (!containerBinding)
    {
        return ReportError(*sink,
                           CookError::ReflectionUnavailable,
                           std::format("parameter block '{}' holds {} bytes of data and reports no container binding",
                                       block.Name,
                                       block.UniformSize));
    }

    RawBindingDraft draft;
    draft.Binding.Name = std::string{ block.Name };
    draft.Binding.ScopeName = std::string{ scope_name };
    draft.Binding.Placement = BoundPlacement{ .Group = block.Scope.Base.Group, .Binding = *containerBinding };
    draft.Binding.Kind = BindingKind::UniformBuffer;
    draft.Binding.ByteSize = static_cast<uint64_t>(block.UniformSize);
    std::vector<StoredValueLeaf> storedValues;
    const CookError uniformMembersError =
        collectStructMembers(block.ElementLayout, draft.Binding.UniformMembers, &storedValues);
    if (!uniformMembersError)
    {
        return uniformMembersError;
    }

    const BoundPlacement container = *GetBoundPlacement(draft.Binding.Placement);
    out_drafts.emplace_back(std::move(draft));
    return appendStoredValueDrafts(storedValues, StoredValueContainer::UniformBuffer, container, block.Scope.Name, out_drafts);
}

/** One binding for each shim value of a container. The interface's row states the resource, because reflection gives a
 * descriptor handle as a `uint2` and a pointer with no resource type. */
CookError SlangReflector::appendStoredValueDrafts(std::span<const StoredValueLeaf> values,
                                                  StoredValueContainer container,
                                                  BoundPlacement block,
                                                  std::string_view scope_name,
                                                  std::vector<RawBindingDraft>& out_drafts) const
{
    for (const StoredValueLeaf& value : values)
    {
        const char* implementationName = value.Implementation != nullptr ? value.Implementation->getName() : nullptr;
        AccessModel form{ AccessModel::Invalid };
        const ShimInterface* row =
            implementationName != nullptr ? FindShimInterfaceByImplementation(implementationName, form) : nullptr;
        if (row == nullptr)
        {
            return ReportError(*sink,
                               CookError::ReflectionUnavailable,
                               std::format("member {} is marked [ls_shim_value] in type '{}', which is not a Handle or "
                                           "Pointer implementation of the shim table",
                                           value.Name,
                                           implementationName != nullptr ? implementationName : ""));
        }

        RawBindingDraft draft;
        draft.Binding.Name = value.Name;
        draft.Binding.ScopeName = JoinScopeName(scope_name, value.Scope.c_str());
        draft.Binding.Placement = StoredValuePlacement{ .Kind = PlacementKindFromAccessModel(form),
                                                        .Container = container,
                                                        .Block = block,
                                                        .ByteOffset = value.ByteOffset };
        draft.Binding.Kind = row->Resource.Kind;
        draft.Binding.Shape = row->Resource.Shape;
        draft.Binding.SampleType = row->Resource.SampleType;
        draft.Binding.Access = row->Resource.Access;

        if (row->ImplementationTakesFormat)
        {
            const std::string fullName = FullNameOf(value.Implementation);
            const std::string_view slangType = TypeArgumentOf(fullName);
            const std::span<const StorageFormatInfo> formats = GetStorageFormats();
            const auto format = std::ranges::find(formats, slangType, &StorageFormatInfo::SlangType);
            if (format == formats.end())
            {
                return ReportError(*sink,
                                   CookError::ReflectionUnavailable,
                                   std::format("{}.{} has the implementation '{}', whose storage format is not one of "
                                               "StorageFormat",
                                               draft.Binding.ScopeName,
                                               value.Name,
                                               fullName));
            }
            draft.Binding.StorageFormat = format->Format;
        }

        if (value.Member->getKind() == slang::TypeReflection::Kind::Pointer)
        {
            // the full name keeps the access of the pointer, so it is compared with the access of the interface
            const std::string pointerName = FullNameOf(value.Member->getType());
            ResourceAccess pointerAccess = ResourceAccess::Invalid;
            if (pointerName.contains("Access.ReadWrite"))
            {
                pointerAccess = ResourceAccess::ReadWrite;
            }
            else if (pointerName.contains("Access.Read"))
            {
                pointerAccess = ResourceAccess::ReadOnly;
            }

            if (pointerAccess != row->Resource.Access)
            {
                return ReportError(*sink,
                                   CookError::ShimPointerAccessMismatch,
                                   std::format("{}.{} is a '{}', and the interface {} grants {} access",
                                               draft.Binding.ScopeName,
                                               value.Name,
                                               pointerName,
                                               row->Name,
                                               magic_enum::enum_name(row->Resource.Access)));
            }

            slang::TypeLayoutReflection* pointee = value.Member->getElementTypeLayout();
            if (pointee != nullptr)
            {
                draft.Binding.ElementStride = static_cast<uint32_t>(pointee->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM));
                const CookError pointeeError = collectStructMembers(pointee, draft.Binding.UniformMembers, nullptr);
                if (!pointeeError)
                {
                    return pointeeError;
                }
            }
        }

        out_drafts.emplace_back(std::move(draft));
    }

    return CookError::Success;
}

/** Drafts the container and the contents of each parameter block of one scope. The range walk skips blocks, so
 * this is the only walk that reads them. */
CookError SlangReflector::collectSubObjectDrafts(slang::TypeLayoutReflection* containing_layout,
                                                 const BindingScope& scope,
                                                 std::vector<RawBindingDraft>& out_drafts) const
{
    const SlangInt subObjectRangeCount = containing_layout->getSubObjectRangeCount();

    for (SlangInt subObjectIndex = 0; subObjectIndex < subObjectRangeCount; ++subObjectIndex)
    {
        const std::optional<ParameterBlockInfo> block =
            ReadParameterBlock(*sink, containing_layout, subObjectIndex, scope);
        if (!block)
        {
            continue;
        }

        if (block->UniformSize != 0u)
        {
            const CookError containerError = readBlockContainer(*block, scope.Name, out_drafts);
            if (!containerError)
            {
                return containerError;
            }
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

/** Returns the parameter scope of one entry point: its offset, and the name Slang emits around it (the struct
 * parameter's name, or `entryPointParams` for loose parameters). */
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

    // todo-ship: equal placements are an error, so stable_sort may be unnecessary
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
        CollectUsedBindingIndices(linked_variant, entry_point_index, global_bindings, 0u);

    CookResult<BindingScope> scope = entryPointScope(entryPointLayout, rawEntryPoint.Name);
    if (!scope)
    {
        return std::unexpected(scope.error());
    }

    // with ordinary data, the parameters' resources are in the element of a constant buffer
    slang::TypeLayoutReflection* parametersLayout = entryPointLayout->getTypeLayout();
    if (parametersLayout != nullptr &&
        parametersLayout->getKind() == slang::TypeReflection::Kind::ConstantBuffer)
    {
        CookResult<slang::TypeLayoutReflection*> elementLayout = readEntryPointConstants(linked_variant,
                                                                                         entry_point_index,
                                                                                         entryPointLayout,
                                                                                         scope.value(),
                                                                                         rawEntryPoint,
                                                                                         out_drafts);
        if (!elementLayout)
        {
            return std::unexpected(elementLayout.error());
        }
        parametersLayout = *elementLayout;
    }

    const CookError walkError = collectBindingRangeDrafts(parametersLayout, scope.value(), out_drafts);
    if (!walkError)
    {
        return std::unexpected(walkError);
    }

    return rawEntryPoint;
}

/** Reads the entry point constants into `out_entry_point.Constants`, and returns the element layout that contains
 * the parameters' resources. */
CookResult<slang::TypeLayoutReflection*> SlangReflector::readEntryPointConstants(
    const LinkedVariant& linked_variant,
    int64_t entry_point_index,
    slang::EntryPointReflection* entry_point_layout,
    const BindingScope& scope,
    RawEntryPoint& out_entry_point,
    std::vector<RawBindingDraft>& out_drafts) const
{
    slang::TypeLayoutReflection* parametersLayout = entry_point_layout->getTypeLayout();
    slang::TypeLayoutReflection* elementLayout = parametersLayout->getElementTypeLayout();
    const std::optional<uint32_t> byteSize =
        elementLayout != nullptr ? ResolvedNumber(elementLayout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM))
                                 : std::nullopt;
    if (!byteSize || parametersLayout->getBindingRangeCount() < 1)
    {
        return std::unexpected(ReportError(*sink,
                                                CookError::ReflectionUnavailable,
                                                std::format("entry point {} has uniform parameters with no readable layout", out_entry_point.Name)));
    }

    RawEntryPointConstants& constants = out_entry_point.Constants;
    constants.ByteSize = *byteSize;
    std::vector<StoredValueLeaf> storedValues;
    const CookError membersError = collectStructMembers(elementLayout, constants.Members, &storedValues);
    if (!membersError)
    {
        return std::unexpected(membersError);
    }

    const slang::BindingType containerType = parametersLayout->getBindingRangeType(0);
    if (containerType == slang::BindingType::PushConstant)
    {
        // the metadata does not track push constants, so the emitted code settles this block later
        constants.Delivery = EntryPointConstantsDelivery::PushConstant;

        const CookError storedError =
            appendStoredValueDrafts(storedValues, StoredValueContainer::PushConstants, BoundPlacement{}, scope.Name, out_drafts);
        if (!storedError)
        {
            return std::unexpected(storedError);
        }

        return elementLayout;
    }

    if (containerType != slang::BindingType::ConstantBuffer)
    {
        return std::unexpected(ReportError(*sink,
                                           CookError::ReflectionUnsupportedBindingKind,
                                           std::format("entry point {} delivers its uniform parameters in a range of type {}",
                                                       out_entry_point.Name,
                                                       static_cast<int>(containerType))));
    }

    const std::optional<uint32_t> containerOffset =
        ReadOffset(parametersLayout->getContainerVarLayout(), SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT);
    if (!containerOffset)
    {
        ReportUnresolvedLocation(*sink, "entry point constants", 0);
        return std::unexpected(CookError::ReflectionUnavailable);
    }

    const BoundPlacement containerPlacement{ .Group = scope.Base.Group,
                                             .Binding = scope.Base.Binding + *containerOffset };
    RawBindingDraft container;
    container.Binding.Name = std::string{ k_EntryPointScopeName };
    container.Binding.Placement = containerPlacement;
    container.Binding.Kind = BindingKind::UniformBuffer;
    container.Binding.ByteSize = constants.ByteSize;
    container.Binding.UniformMembers = constants.Members;

    const Slang::ComPtr<slang::IMetadata>& metadata =
        linked_variant.EntryPointMetadata[static_cast<size_t>(entry_point_index)];
    bool isUsed = false;
    metadata->isParameterLocationUsed(SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT,
                                      containerPlacement.Group,
                                      containerPlacement.Binding,
                                      isUsed);
    if (isUsed)
    {
        constants.Delivery = EntryPointConstantsDelivery::UniformBuffer;
        constants.ContainerBinding = static_cast<uint32_t>(out_drafts.size());
    }
    out_drafts.emplace_back(std::move(container));

    const CookError storedError =
        appendStoredValueDrafts(storedValues, StoredValueContainer::UniformBuffer, containerPlacement, scope.Name, out_drafts);
    if (!storedError)
    {
        return std::unexpected(storedError);
    }

    return elementLayout;
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
    default:
        // only vertex and fragment stages have raster state. The old print here fired for every other stage (compute,
        // ray tracing, tessellation) and told the author nothing they could act on
        break;
    }
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif

} // namespace lodestone
