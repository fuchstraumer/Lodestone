#include "model/ResolveStage.hpp"
#include "CookerErrors.hpp"
#include "ShaderLibraryTypes.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationSpace.hpp"
#include "compile/RawLibrary.hpp"
#include "model/ShaderDataSchema.hpp"
#include "permute/AttributeExpression.hpp"
#include "permute/PermutationTypes.hpp"
#include "permute/PermutationValue.hpp"
#include "Diagnostics.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <iterator>
#include <numeric>
#include <optional>
#include <ranges>
#include <string>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <print>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace lodestone
{

namespace
{

    CookResult<uint32_t> EvaluateExtentArgument(const RawSizeAttribute& attribute,
                                                uint32_t argument_index,
                                                std::string_view binding_name,
                                                const ResolveContext& context,
                                                DiagnosticSink& sink)
    {
        if (argument_index >= attribute.Arguments.size())
        {
            std::println(stderr,
                         "[lodestone] [{}] on '{}' is missing argument {}",
                         ToString(attribute.Kind),
                         binding_name,
                         argument_index);
            return std::unexpected(CookError::AttributeExpressionParseFailed);
        }

        const CookResult<int64_t> value =
            EvaluateExpression(attribute.Arguments[argument_index], context.Symbols, sink);
        if (!value)
        {
            return std::unexpected(value.error());
        }

        if (value.value() <= 0)
        {
            std::println(stderr,
                         "[lodestone] extent argument {} on '{}' evaluated to {}, which is not a "
                         "valid texture dimension",
                         argument_index,
                         binding_name,
                         value.value());
            return std::unexpected(CookError::AttributeExpressionOutOfRange);
        }

        return static_cast<uint32_t>(value.value());
    }

    CookResult<TextureFootprint> ResolveExtent(const RawSizeAttribute& attribute,
                                               std::string_view binding_name,
                                               const ResolveContext& context,
                                               DiagnosticSink& sink)
    {
        const uint32_t argumentCount = ArgumentCountOf(attribute.Kind);
        std::array<uint32_t, 3u> axes{ 1u, 1u, 1u };

        for (uint32_t i = 0u; i < argumentCount && i < axes.size(); ++i)
        {
            const CookResult<uint32_t> value = EvaluateExtentArgument(attribute, i, binding_name, context, sink);
            if (!value)
            {
                return std::unexpected(value.error());
            }

            axes.at(i) = value.value();
        }

        return TextureFootprint{ .ExtentX = axes[0],
                                 .ExtentY = axes[1],
                                 .ExtentZ = axes[2],
                                 .Expression = attribute.Arguments.front() };
    }

    CookResult<BufferFootprint> ResolveElementCount(const RawSizeAttribute& attribute,
                                                    std::string_view binding_name,
                                                    const ResolveContext& context,
                                                    DiagnosticSink& sink)
    {
        if (attribute.Arguments.empty())
        {
            std::println(stderr,
                         "[lodestone] [{}] on '{}' has no argument",
                         ToString(attribute.Kind),
                         binding_name);
            return std::unexpected(CookError::AttributeExpressionParseFailed);
        }

        const CookResult<int64_t> value =
            EvaluateExpression(attribute.Arguments.front(), context.Symbols, sink);
        if (!value)
        {
            std::println(stderr,
                         "[lodestone] [{}] on '{}' did not evaluate",
                         ToString(attribute.Kind),
                         binding_name);
            return std::unexpected(value.error());
        }

        if (value.value() <= 0)
        {
            std::println(stderr,
                         "[lodestone] [{}] on '{}' evaluated to {}, which cannot size a buffer",
                         ToString(attribute.Kind),
                         binding_name,
                         value.value());
            return std::unexpected(CookError::AttributeExpressionOutOfRange);
        }

        return BufferFootprint{ .ElementCount = static_cast<uint64_t>(value.value()),
                                .Expression = attribute.Arguments.front() };
    }


    /**@brief Every annotation on one resource, evaluated. A resource with no annotation is not an error,
     * because many resources can be sized by the caller. The annotation kind tells us if the author is
     * describing a buffer or texture, which is the current extent of our taxonomy here */
    CookResult<ResourceFootprint> ResolveFootprint(std::span<const RawSizeAttribute> attributes,
                                                   std::string_view binding_name,
                                                   const ResolveContext& context,
                                                   DiagnosticSink& sink)
    {
        const RawSizeAttribute* count = nullptr;
        const RawSizeAttribute* extent = nullptr;

        for (const RawSizeAttribute& attribute : attributes)
        {
            const RawSizeAttribute*& slot =
                attribute.Kind == RawSizeAttributeKind::ElementCount ? count : extent;
            if (slot != nullptr)
            {
                std::println(stderr,
                             "[lodestone] '{}' carries [{}] and [{}]; only one may size a resource",
                             binding_name,
                             ToString(slot->Kind),
                             ToString(attribute.Kind));
                return std::unexpected(CookError::ReflectionSizeUnresolved);
            }

            slot = &attribute;
        }

        if (count != nullptr && extent != nullptr)
        {
            std::println(stderr,
                         "[lodestone] '{}' carries an element count and an extent; a resource is a "
                         "buffer or a texture",
                         binding_name);
            return std::unexpected(CookError::ReflectionSizeUnresolved);
        }

        if (count != nullptr)
        {
            CookResult<BufferFootprint> buffer = ResolveElementCount(*count, binding_name, context, sink);
            if (!buffer)
            {
                return std::unexpected(buffer.error());
            }
            return ResourceFootprint{ std::move(buffer.value()) };
        }

        if (extent != nullptr)
        {
            CookResult<TextureFootprint> texture = ResolveExtent(*extent, binding_name, context, sink);
            if (!texture)
            {
                return std::unexpected(texture.error());
            }
            return ResourceFootprint{ std::move(texture.value()) };
        }

        return ResourceFootprint{};
    }

    std::vector<RawSizeAttribute> AttributesOfBinding(const RawVariant& raw, size_t binding_index)
    {
        std::vector<RawSizeAttribute> attributes;

        for (const RawSizeAttribute& attribute : raw.SizeAttributes)
        {
            if (attribute.BindingIndex == binding_index)
            {
                attributes.push_back(attribute);
            }
        }

        return attributes;
    }

    ReflectedBinding ResolveBinding(const RawBinding& raw_binding)
    {
        ReflectedBinding binding;
        binding.Name = raw_binding.Name;
        binding.ScopeName = raw_binding.ScopeName;
        binding.Placement = raw_binding.Placement;
        binding.Kind = raw_binding.Kind;
        binding.ElementStride = raw_binding.ElementStride;
        binding.ByteSize = raw_binding.ByteSize;
        binding.ArrayCount = raw_binding.ArrayCount;
        binding.Shape = raw_binding.Shape;
        binding.SampleType = raw_binding.SampleType;
        binding.StorageFormat = raw_binding.StorageFormat;
        binding.Access = raw_binding.Access;
        binding.IsComparisonSampler = raw_binding.IsComparisonSampler;
        binding.UniformMembers = raw_binding.UniformMembers;
        return binding;
    }

    CompiledEntryPoint ResolveEntryPoint(const RawEntryPoint& raw_entry_point)
    {
        CompiledEntryPoint entryPoint;
        entryPoint.Name = raw_entry_point.Name;
        entryPoint.VariantSuffix = raw_entry_point.VariantSuffix;
        entryPoint.Code = raw_entry_point.TargetCode;
        entryPoint.Reflection.Name = raw_entry_point.Name;
        entryPoint.Reflection.Stage = raw_entry_point.Stage;
        entryPoint.Reflection.Workgroup = raw_entry_point.Workgroup;
        entryPoint.Reflection.UsedBindingIndices = raw_entry_point.UsedBindingIndices;
        entryPoint.Reflection.Raster = raw_entry_point.Raster;
        entryPoint.Reflection.ConstantsElementBindings = raw_entry_point.ConstantsElementBindings;

        const RawEntryPointConstants& rawConstants = raw_entry_point.Constants;
        EntryPointConstants& constants = entryPoint.Reflection.Constants;
        constants.Delivery = rawConstants.Delivery;
        if (constants.Delivery != EntryPointConstantsDelivery::None)
        {
            constants.ByteSize = rawConstants.ByteSize;
            constants.Members = rawConstants.Members;
        }
        // stage 3 sets the container only when the metadata reports it as used, and the same query decides
        // visibility, so the container is always in the visibility list
        if (constants.Delivery == EntryPointConstantsDelivery::UniformBuffer)
        {
            const auto container = std::ranges::find(raw_entry_point.UsedBindingIndices, *rawConstants.ContainerBinding);
            constants.Binding =
                static_cast<uint32_t>(std::distance(raw_entry_point.UsedBindingIndices.begin(), container));
        }
        
        return entryPoint;
    }

    /** Returns `'NAME' (file:line) = 'expression'`, the form every derived constant error uses. */
    std::string DescribeDerivedConstant(const RawDerivedConstant& constant)
    {
        return std::format("'{}' ({}:{}) = '{}'",
                           constant.Name,
                           constant.SourceFile,
                           constant.SourceLine,
                           constant.Expression);
    }

    enum class VisitColor : uint8_t
    {
        White, // unvisited
        Gray, // visiting (on the current path)
        Black, // finished (value is in the symbol table)
    };

    /** The depth-first walk over the derived constants of one variant. Its roots are the size attribute
     * arguments, and the edges of each constant are the identifiers in its expression. Each constant is
     * evaluated when its visit finishes, which is post-order, so every derived constant that it uses is
     * already in the symbol table. Reaching a gray constant means the path has looped back on itself.
     *
     * Each constant is visited once for each variant. The color array is the cache: a black constant is
     * skipped, so a constant that two others use (a diamond) is evaluated once. */
    // NOLINTBEGIN(misc-no-recursion): the recursion depth is the length of one chain of derived constants
    class DerivedConstantWalk
    {
    public:
        DerivedConstantWalk(const DerivedConstantTable& table, ResolveContext& context, DiagnosticSink& sink)
            : table(table), context(context), sink(sink), colors(table.Constants.size(), VisitColor::White)
        {}

        /** Visits every derived constant that one attribute argument uses. `root` describes the attribute, for
         * the error messages. */
        CookError VisitRoot(std::string_view argument, std::string_view root)
        {
            rootDescription = root;
            const CookResult<std::vector<std::string>> identifiers = CollectExpressionIdentifiers(argument, sink);
            if (!identifiers)
            {
                const std::string message = std::format("{}: the argument '{}' does not parse", root, argument);
                return ReportError(sink, identifiers.error(), message);
            }
            return visitIdentifiers(identifiers.value());
        }

    private:

        [[nodiscard]] CookError visitIdentifiers(std::span<const std::string> identifiers)
        {
            for (const std::string& identifier : identifiers)
            {
                // axes, extern constants, and unknown names are leaves. EvaluateExpression reports an
                // unknown name later, with the expression it was found in
                const std::optional<uint32_t> index = findByName(identifier);
                if (!index)
                {
                    continue;
                }

                const CookError visited = visit(*index);
                if (!visited)
                {
                    return visited;
                }
            }

            return CookError::Success;
        }

        [[nodiscard]] CookError visit(uint32_t index)
        {
            if (colors[index] == VisitColor::Black)
            {
                return CookError::Success;
            }

            const RawDerivedConstant& constant = table.Constants[index];
            if (colors[index] == VisitColor::Gray)
            {
                return reportCycle(index);
            }

            colors[index] = VisitColor::Gray;
            path.push_back(index);

            const CookResult<std::vector<std::string>> identifiers =
                CollectExpressionIdentifiers(constant.Expression, sink);
            if (!identifiers)
            {
                return reportUnreadable(constant, "does not parse");
            }

            const CookError dependencies = visitIdentifiers(identifiers.value());
            if (!dependencies)
            {
                return dependencies;
            }

            // post-order: every derived constant this expression uses is black, so its value is in the table
            const CookResult<int64_t> value = EvaluateExpression(constant.Expression, context.Symbols, sink);
            if (!value)
            {
                return reportUnreadable(constant, "did not evaluate");
            }

            context.Symbols.push_back(AttrExprSymbol{ .Name = constant.Name, .Value = value.value() });
            colors[index] = VisitColor::Black;
            path.pop_back();

            return CookError::Success;
        }

        [[nodiscard]] std::optional<uint32_t> findByName(std::string_view name) const
        {
            auto extractNameView = [&](uint32_t index)->std::string_view
            {
                return std::string_view{ table.Constants[index].Name };
            };
            const auto found = std::ranges::lower_bound(table.ByName, name, std::less{}, extractNameView);
            if (found == table.ByName.end() || table.Constants[*found].Name != name)
            {
                return std::nullopt;
            }
            return *found;
        }

        /** Returns the current path as `'A' (file:line) = '...' -> 'B' (file:line) = '...'`. */
        [[nodiscard]] std::string describePath(std::span<const uint32_t> nodes) const
        {
            std::string description;
            for (const uint32_t node : nodes)
            {
                description += std::format("\n    -> {}", DescribeDerivedConstant(table.Constants[node]));
            }
            return description;
        }

        [[nodiscard]] CookError reportCycle(uint32_t index) const
        {
            // the cycle is the part of the path from the first visit of `index` to its end, and back to `index`
            const auto cycleStart = std::ranges::find(path, index);
            std::vector<uint32_t> cycle(cycleStart, path.end());
            cycle.push_back(index);
            const std::string message = std::format("derived constants form a cycle, reached from {}:{}",
                                                    rootDescription,
                                                    describePath(cycle));
            return ReportError(sink, CookError::DerivedConstantCycle, message);
        }

        [[nodiscard]] CookError reportUnreadable(const RawDerivedConstant& constant, std::string_view failure) const
        {
            const std::string message = std::format("derived constant '{}' {}, reached from {}:{}",
                                                    constant.Name,
                                                    failure,
                                                    rootDescription,
                                                    describePath(path));
            return ReportError(sink, CookError::DerivedConstantUnreadable, message);
        }

        const DerivedConstantTable& table;
        ResolveContext& context;
        DiagnosticSink& sink;
        std::vector<VisitColor> colors;
        // the gray constants, in the order their visits started
        std::vector<uint32_t> path;
        std::string_view rootDescription;
    };
    // NOLINTEND(misc-no-recursion)

} // namespace

CookResult<DerivedConstantTable> MakeDerivedConstantTable(std::span<const RawDerivedConstant> derived_constants,
                                                          std::span<const PermutationAxis> axes,
                                                          std::span<const ExternConstantDefault> extern_defaults,
                                                          DiagnosticSink& sink)
{
    // a stable sort keeps the declarations of one name in source order, so the first one is the one we keep
    std::vector<uint32_t> sorted(derived_constants.size());
    std::ranges::iota(sorted, 0u);
    std::ranges::stable_sort(sorted,
                             std::less{},
                             [&](uint32_t index)
                             {
                                return std::string_view{ derived_constants[index].Name };
                             });

    DerivedConstantTable table{ .Constants = derived_constants, .ByName = {} };
    auto chunkFn = [&](uint32_t lhs, uint32_t rhs)
    {
        return derived_constants[lhs].Name == derived_constants[rhs].Name;
    };
    for (auto run : sorted | std::views::chunk_by(chunkFn))
    {
        const RawDerivedConstant& first = derived_constants[run.front()];
        auto expressionCompare = [&](uint32_t index)
        {
            return derived_constants[index].Expression == first.Expression;
        };
        const bool sameExpression = std::ranges::all_of(run, expressionCompare);
        if (!sameExpression)
        {
            std::string message =
                std::format("derived constant '{}' is declared with different expressions:", first.Name);
            for (const uint32_t index : run)
            {
                message += std::format("\n    {}", DescribeDerivedConstant(derived_constants[index]));
            }
            return std::unexpected(ReportError(sink, CookError::DerivedConstantAmbiguous, std::move(message)));
        }
        table.ByName.push_back(run.front());
    }

    // a derived constant with the name of an axis is a shadowed declaration in the shader, and the two
    // readers of that shader (Slang and this table) could each pick a different one
    for (const uint32_t index : table.ByName)
    {
        const RawDerivedConstant& constant = derived_constants[index];
        const bool shadowsAxis =
            std::ranges::any_of(axes, [&](const PermutationAxis& axis) { return axis.Name == constant.Name; });
        const bool shadowsExtern = std::ranges::any_of(
            extern_defaults, [&](const ExternConstantDefault& entry) { return entry.Name == constant.Name; });
        if (shadowsAxis || shadowsExtern)
        {
            const std::string message =
                std::format("derived constant {} has the name of {} '{}'. Rename one of them",
                            DescribeDerivedConstant(constant),
                            shadowsAxis ? "the axis" : "the extern constant",
                            constant.Name);
            return std::unexpected(ReportError(sink, CookError::DerivedConstantShadowsSymbol, message));
        }
    }

    return table;
}

CookResult<ResolveContext> MakeResolveContext(const PermutationAssignment& canonical,
                                              std::span<const ExternConstantDefault> extern_defaults,
                                              const DerivedConstantTable& derived_constants,
                                              const RawVariant& raw,
                                              DiagnosticSink& sink)
{
    ResolveContext context;
    context.Symbols.reserve(canonical.size() + extern_defaults.size() + derived_constants.ByName.size());

    for (const ExternConstantDefault& entry : extern_defaults)
    {
        context.Symbols.push_back(AttrExprSymbol{ .Name = entry.Name, .Value = entry.Value });
    }

    for (const PermutationBinding& binding : canonical)
    {
        // todo-ship: handle enums here (map case name to integral value)
        context.Symbols.push_back(
            AttrExprSymbol{ .Name = binding.Axis->Name, .Value = PermutationValueToInt64(binding.Value) });
    }

    if (derived_constants.ByName.empty())
    {
        return context;
    }

    // every size attribute argument is a root of the walk (for now)
    DerivedConstantWalk walk{ derived_constants, context, sink };
    for (const RawSizeAttribute& attribute : raw.SizeAttributes)
    {
        const std::string root =
            std::format("[{}] on '{}'", ToString(attribute.Kind), raw.Bindings[attribute.BindingIndex].Name);
        for (const std::string& argument : attribute.Arguments)
        {
            const CookError visited = walk.VisitRoot(argument, root);
            if (!visited)
            {
                return std::unexpected(visited);
            }
        }
    }

    return context;
}

CookResult<CompiledVariant> ResolveVariant(const RawVariant& raw, const ResolveContext& context, DiagnosticSink& sink)
{
    CompiledVariant variant;
    variant.VariantSuffix = raw.VariantSuffix;
    variant.VariantDescription = raw.VariantDescription;
    variant.VariantIndex = raw.VariantIndex;
    variant.Bindings.reserve(raw.Bindings.size());

    variant.Footprints.reserve(raw.Bindings.size());

    for (size_t i = 0u; i < raw.Bindings.size(); ++i)
    {
        const RawBinding& rawBinding = raw.Bindings[i];

        CookResult<ResourceFootprint> footprint =
            ResolveFootprint(AttributesOfBinding(raw, i), rawBinding.Name, context, sink);
        if (!footprint)
        {
            return std::unexpected(footprint.error());
        }

        variant.Bindings.push_back(ResolveBinding(rawBinding));
        variant.Footprints.push_back(std::move(footprint.value()));
    }

    variant.EntryPoints.reserve(raw.EntryPoints.size());
    for (const RawEntryPoint& rawEntryPoint : raw.EntryPoints)
    {
        variant.EntryPoints.push_back(ResolveEntryPoint(rawEntryPoint));
    }

    return variant;
}

} // namespace lodestone
