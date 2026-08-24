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

}
