#include "SlangReflector.hpp"
#include "slang.h"

namespace
{
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
    template<typename LeafVisitor>
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
        std::string joined;
        for (slang::VariableLayoutReflection* field : path)
        {
            const char* name = field->getName();
            if (name == nullptr)
            {
                return {};
            }

            joined += joined.empty() ? "" : ".";
            joined += name;
        }

        return joined;
    }

    /**Flattens one uniform block into rows of name, offset, and size. Nested fields take
     * a dotted name to reflect the hierarchy. Offset is accumulated from the root of the
     * struct, as slang structures offsets from zero based on recursive walks of all fields.
     */
    void CollectUniformMembers(slang::TypeLayoutReflection* struct_layout,
                               std::vector<ReflectedUniformMember>& members)
    {
        if (struct_layout == nullptr || struct_layout->getKind() != slang::TypeReflection::Kind::Struct)
        {
            return;
        }

        const auto visit = [&members](const LeafVisit& leaf)
        {
            std::string name = JoinFieldNames(leaf.Path);
            if (name.empty())
            {
                return;
            }
            const auto memberSize =
                static_cast<uint32_t>(leaf.Type->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM));
            const auto arrayCount = leaf.Type->getKind() == slang::TypeReflection::Kind::Array
                                        ? static_cast<uint32_t>(leaf.Type->getElementCount())
                                        : 1u;

            members.emplace_back(std::move(name), leaf.Offset, memberSize, arrayCount);
        };

        std::vector<slang::VariableLayoutReflection*> path;
        const unsigned int fieldCount = struct_layout->getFieldCount();
        for (unsigned int i = 0u; i < fieldCount; ++i)
        {
            VisitLeaves(struct_layout->getFieldByIndex(i), SLANG_PARAMETER_CATEGORY_UNIFORM, 0u, path, visit);
        }
    }

    std::string_view ReadSemanticName(slang::VariableLayoutReflection* var_layout) noexcept
    {
        const char* name = var_layout->getSemanticName();
        if (name == nullptr)
        {
            return {};
        }

        return std::string_view{ name };
    }

    /** Records every attribute a vertex buffer must supply. */
    void CollectVertexInputs(slang::VariableLayoutReflection* var_layout, ReflectedRasterState& raster)
    {
        WalkVaryingTree(var_layout,
                        SLANG_PARAMETER_CATEGORY_VARYING_INPUT,
                        [&raster](const LeafVisit& leaf)
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
                        });
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

    /** Reads one sub-object range as a parameter block, or refuses it.
     *
     * Two numbers come from places that are not the obvious ones, and probe modules measured both.
     * **The space of the block** is the sub-element space offset of the sub-object range, added to
     * the space base of the scope that holds it. `getSubObjectRangeSpaceOffset` is not that number:
     * it reported 0 for a block that took space 1. **The contents start at binding zero**, because
     * their own descriptor range offsets already count from the start of the space and already step
     * over the container. A block of two resources reported 0 and 1, and the same block with a
     * `float4` added reported 1 and 2, with the container at 0.
     *
     * The two walks partition the ranges on one test, so no range is drafted twice and none is
     * dropped. The range walk keeps every range the parent placed itself. This one keeps the rest,
     * which are the ranges the parent describes with descriptor ranges alone.
     *
     * A global `ConstantBuffer<T>` is the case that proves the test is needed. The parent places it,
     * so it is an ordinary binding, and it also reports a sub-object range. Reading it here as well
     * drafts it a second time at the wrong location, and `OceanFft` failed exactly so. */
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

        slang::TypeLayoutReflection* blockLayout =
            containing_layout->getBindingRangeLeafTypeLayout(rangeIndex);
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

    /** The constant buffer Slang adds to hold the ordinary data of a block. Neither range list holds
     * it, and a client must create it, so it becomes a binding that carries the name of the block.
     *
     * A block of resources alone holds no such data and gets no such slot, so the caller asks only
     * when `UniformSize` is not zero. Drafting one anyway reports a binding the shader has not got. */
    CookResult<RawBindingDraft> ReadBlockContainer(const ParameterBlockInfo& block,
                                                   std::string_view scope_name)
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
        draft.Binding.Placement =
            BoundPlacement{ .Group = block.Scope.Base.Group, .Binding = *containerBinding };
        draft.Binding.Kind = BindingKind::UniformBuffer;
        draft.Binding.ByteSize = static_cast<uint64_t>(block.UniformSize);
        CollectUniformMembers(block.ElementLayout, draft.Binding.UniformMembers);
        return draft;
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

        out_bindings.insert_range(out_bindings.end(),
                                  drafts | std::views::as_rvalue |
                                      std::views::transform(&RawBindingDraft::Binding));
        out_attributes.insert_range(out_attributes.end(),
                                    drafts | std::views::transform(&RawBindingDraft::Attributes) |
                                        std::views::join | std::views::as_rvalue);
    }
}

namespace lodestone
{
/** Reads the size, shape, and type facts off one binding range's leaf type layout.
 *
 * Slang wraps a resource type around the type it carries, so the useful facts sit one level down.
 * A structured buffer reports its element layout. A texture reports the type it returns. */
void SlangReflector::applyLeafTypeLayout(slang::TypeLayoutReflection* containing_layout,
                                              SlangInt range_index,
                                              slang::BindingType binding_type,
                                              RawBinding& binding)
{
    slang::TypeLayoutReflection* leafLayout = containing_layout->getBindingRangeLeafTypeLayout(range_index);
    if (leafLayout == nullptr)
    {
        return;
    }

    if (binding.Kind == BindingKind::StorageTexture)
    {
        binding.StorageFormat =
            FromSlangImageFormat(containing_layout->getBindingRangeImageFormat(range_index));
        binding.StorageAccess = FromSlangBindingTypeAccess(binding_type);
    }

    if (binding.Kind == BindingKind::Sampler)
    {
        binding.Shape = ResourceShape::Invalid;
        binding.SamplerType = SamplerBindingType::Filtering;
        return;
    }

    slang::TypeReflection* leafType = leafLayout->getType();
    if (leafType == nullptr)
    {
        return;
    }

    binding.Shape = FromSlangResourceShape(leafType->getResourceShape());

    if (IsTextureBinding(binding.Kind))
    {
        slang::TypeReflection* resultType = leafType->getResourceResultType();
        if (resultType != nullptr)
        {
            binding.SampleType = FromSlangScalarType(resultType->getScalarType());
        }

        return;
    }

    if (binding.Kind == BindingKind::UniformBuffer)
    {
        // A uniform block's size is fully determined. The graph must never take it from the caller.
        binding.ByteSize = static_cast<uint64_t>(leafLayout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM));
        slang::TypeLayoutReflection* elementLayout = leafLayout->getElementTypeLayout();
        if (elementLayout != nullptr && binding.ByteSize == 0u)
        {
            binding.ByteSize =
                static_cast<uint64_t>(elementLayout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM));
        }

        CollectUniformMembers(elementLayout, binding.UniformMembers);
        return;
    }

    slang::TypeLayoutReflection* elementLayout = leafLayout->getElementTypeLayout();
    if (elementLayout != nullptr)
    {
        binding.ElementStride = static_cast<uint32_t>(elementLayout->getSize());
    }
}

/** Reads one `[vx_*]` annotation into its argument strings. Stage 3 never evaluates one, so a
 * malformed argument is caught here only when it is not a string at all. */
CookError SlangReflector::CollectRawSizeAttributes(slang::VariableReflection* leaf_variable,
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
        slang::Attribute* found =
            leaf_variable->findAttributeByName(globalSession.get(), attributeName.c_str());
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

/** Walks the binding ranges of one scope, and drafts a binding for each one. */
CookError SlangReflector::CollectBindingRangeDrafts(slang::TypeLayoutReflection* containing_layout,
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

        applyLeafTypeLayout(containing_layout, rangeIndex, bindingType, draft.Binding);
        const CookError collectAttributesError =
            collectRawSizeAttributes(leafVariable, draft.Binding.Name, draft.Attributes);
        if (collectAttributesError != CookError::Success)
        {
            return collectAttributesError;
        }

        out_drafts.emplace_back(std::move(draft));
    }

    return collectSubObjectDrafts(containing_layout, scope, out_drafts);
}

/** Walks the parameter blocks of one scope, and drafts the container and the contents of each.
 *
 * Slang gives a parent scope only descriptor ranges for a block, so the binding range of the block
 * carries a descriptor set index of -1 and the range walk drops it. The contents live in a sub-object
 * range instead, and this is the only walk that reaches them. `ReadParameterBlock` reads one range,
 * and `ReadBlockContainer` reads the slot Slang adds to hold the ordinary data. */
CookError SlangReflector::CollectSubObjectDrafts(slang::TypeLayoutReflection* containing_layout,
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
            CookResult<RawBindingDraft> container = ReadBlockContainer(*block, scope.Name);
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

CookError SlangReflector::ExtractRawBindings(slang::ProgramLayout* program_layout,
                                                  std::vector<RawBinding>& out_bindings,
                                                  std::vector<RawSizeAttribute>& out_attributes) const
{
    std::vector<RawBindingDraft> drafts;
    const CookError walkError =
        collectBindingRangeDrafts(program_layout->getGlobalParamsTypeLayout(), BindingScope{}, drafts);
    if (walkError != CookError::Success)
    {
        return walkError;
    }

    std::ranges::stable_sort(drafts,
                             PlacementLess,
                             [](const RawBindingDraft& draft) -> const RawPlacement&
                             {
                                 return draft.Binding.Placement;
                             });

    AppendBindingDrafts(drafts, out_bindings, out_attributes);

    return CookError::Success;
}

/** Asks the metadata of one entry point which global bindings that entry point reads.
 *
 * `global_bindings` holds the global scope alone. Slang generates each entry point as its own
 * artifact, so two entry points can place different resources at one group and binding. A placement
 * query over the entry point rows would then let one entry point claim the parameter of another. */
void SlangReflector::CollectUsedBindingIndices(slang::IComponentType* linked_program,
                                                    SlangInt entry_point_index,
                                                    std::span<const RawBinding> global_bindings,
                                                    std::vector<uint32_t>& out_used_indices) const
{
    Slang::ComPtr<slang::IMetadata> metadata;
    Slang::ComPtr<slang::IBlob> diagnostics;
    if (SLANG_FAILED(linked_program->getEntryPointMetadata(
            entry_point_index, k_WgslTargetIndex, metadata.writeRef(), diagnostics.writeRef())) ||
        metadata == nullptr)
    {
        ReportDiagnostics(*sink, "getEntryPointMetadata", diagnostics.get());
        return;
    }

    auto isUsedLambda = [&metadata](const RawPlacement& placement) -> bool
    {
        const BoundPlacement* boundPlacement = GetBoundPlacement(placement);
        if (boundPlacement == nullptr)
        {
            return false;
        }

        bool isUsed = false;
        metadata->isParameterLocationUsed(SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT,
                                          boundPlacement->Group,
                                          boundPlacement->Binding,
                                          isUsed);
        return isUsed;
    };

    out_used_indices.append_range(std::views::enumerate(global_bindings) |
                                  std::views::filter(
                                      [&](const auto& pair)
                                      {
                                          return isUsedLambda(std::get<1>(pair).Placement);
                                      }) |
                                  std::views::transform(
                                      [](const auto& pair)
                                      {
                                          return std::get<0>(pair);
                                      }));
}

CookResult<RawEntryPoint> SlangReflector::ExtractRawEntryPoint(
    slang::IComponentType* linked_program,
    slang::ProgramLayout* program_layout,
    SlangInt entry_point_index,
    std::span<const RawBinding> global_bindings,
    std::vector<RawBindingDraft>& out_drafts)
{
    RawEntryPoint rawEntryPoint;
    rawEntryPoint.Name = entryPointNames[static_cast<size_t>(entry_point_index)];

    slang::EntryPointReflection* entryPointLayout =
        program_layout->getEntryPointByIndex(static_cast<SlangUInt>(entry_point_index));
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

    collectUsedBindingIndices(
        linked_program, entry_point_index, global_bindings, rawEntryPoint.UsedBindingIndices);

    // A `uniform` parameter on the entry point takes a placement in the space the global bindings
    // use, and the entry point var layout says where that scope starts.
    CookResult<BindingScope> scope = entryPointScope(entryPointLayout, rawEntryPoint.Name);
    if (!scope)
    {
        return std::unexpected(scope.error());
    }

    const CookError walkError =
        collectBindingRangeDrafts(entryPointLayout->getTypeLayout(), scope.value(), out_drafts);
    if (walkError != CookError::Success)
    {
        return std::unexpected(walkError);
    }

    return rawEntryPoint;
}

void SlangReflector::extractRasterState(slang::EntryPointReflection* entry_point_layout,
                                             ShaderStageKind stage,
                                             ReflectedRasterState& raster)
{
    if (stage == ShaderStageKind::Vertex)
    {
        CollectVertexInputs(entry_point_layout->getVarLayout(), raster);
        return;
    }

    if (stage != ShaderStageKind::Fragment)
    {
        return;
    }

    CollectColorTargets(entry_point_layout->getResultVarLayout(), raster);
    CollectDepthWrites(entry_point_layout->getVarLayout(), raster);
}

}
