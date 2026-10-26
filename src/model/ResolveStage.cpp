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
#include "ResourceFlags.hpp"
#include "ShaderManifestIndex.hpp"

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
#include <expected>
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
                                                DiagnosticSink& sink);
    CookResult<TextureFootprint> ResolveExtent(const RawSizeAttribute& attribute,
                                               std::string_view binding_name,
                                               const ResolveContext& context,
                                               DiagnosticSink& sink);
    CookResult<BufferFootprint> ResolveElementCount(const RawSizeAttribute& attribute,
                                                    std::string_view binding_name,
                                                    const ResolveContext& context,
                                                    DiagnosticSink& sink);
    /** @brief Resources with no annotation are fine: the caller sizes them. */
    CookResult<ResourceFootprint> ResolveFootprint(std::span<const RawSizeAttribute> attributes,
                                                   std::string_view binding_name,
                                                   const ResolveContext& context,
                                                   DiagnosticSink& sink);
    std::vector<std::string_view> SplitWords(std::string_view text);
    std::vector<RawSizeAttribute> AttributesOfBinding(const RawVariant& raw, size_t binding_index);
    CookResult<std::vector<std::string>> ResolveTags(const RawBinding& raw_binding, DiagnosticSink& sink);
    CookResult<ResourceRoleFlags> ResolveRoleNames(const RawBinding& raw_binding, DiagnosticSink& sink);
    /** A raw buffer counts as stride 4. Indirect roles take their command's stride, or stride 4 with whole commands. */
    CookError CheckRoles(const ReflectedBinding& binding, const ResourceFootprint& footprint, DiagnosticSink& sink);


    uint32_t IndirectCommandBytes(ResourceRoleFlags role) noexcept;

    ShaderLocation LocationOf(const RawDerivedConstant& constant) noexcept;

    DiagnosticNote NoteOf(const RawDerivedConstant& constant);

    enum class VisitColor : uint8_t
    {
        White, // unvisited
        Gray, // visiting (on the current path)
        Black, // finished (value is in the symbol table)
    };

    std::optional<uint32_t> FindDerivedConstant(const DerivedConstantTable& table, std::string_view name);

    /** Depth-first walk from the size attribute arguments through the identifiers of each expression. Constants are
     * evaluated in post-order, and black constants are skipped, so diamonds evaluate once. A gray constant is a cycle. */
    // NOLINTBEGIN(misc-no-recursion): the recursion depth is the length of one chain of derived constants
    class DerivedConstantWalk
    {
    public:
        DerivedConstantWalk(const DerivedConstantTable& table, ResolveContext& context, DiagnosticSink& sink)
            : table(table), context(context), sink(sink), colors(table.Constants.size(), VisitColor::White)
        {}

        CookError VisitRoot(std::string_view argument, std::string_view root);

    private:

        [[nodiscard]] CookError visitIdentifiers(std::span<const std::string> identifiers);
        [[nodiscard]] CookError visit(uint32_t index);
        [[nodiscard]] std::optional<uint32_t> findByName(std::string_view name) const;
        [[nodiscard]] std::vector<DiagnosticNote> notesForPath(std::span<const uint32_t> nodes) const;
        [[nodiscard]] CookError reportCycle(uint32_t index) const;
        [[nodiscard]] CookError reportUnreadable(const RawDerivedConstant& constant,
                                                 std::string_view failure) const;

        const DerivedConstantTable& table;
        ResolveContext& context;
        DiagnosticSink& sink;
        std::vector<VisitColor> colors;
        std::vector<uint32_t> path;
        std::string_view rootDescription;
    };
    // NOLINTEND(misc-no-recursion)


    constexpr bool IsTagCharacter(char character) noexcept
    {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') ||
               character == '_' ||
               character == '.' ||
               character == ':' ||
               character == '-';
    }

    constexpr ReflectedBinding ResolveBinding(const RawBinding& raw_binding) noexcept
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

    constexpr CompiledEntryPoint ResolveEntryPoint(const RawEntryPoint& raw_entry_point) noexcept
    {
        CompiledEntryPoint entryPoint;
        entryPoint.Name = raw_entry_point.Name;
        entryPoint.VariantSuffix = raw_entry_point.VariantSuffix;
        entryPoint.Reflection.Name = raw_entry_point.Name;
        entryPoint.Reflection.Stage = raw_entry_point.Stage;
        entryPoint.Reflection.Workgroup = raw_entry_point.Workgroup;
        entryPoint.Reflection.UsedBindingIndices = raw_entry_point.UsedBindingIndices;
        entryPoint.Reflection.Raster = raw_entry_point.Raster;

        const RawEntryPointConstants& rawConstants = raw_entry_point.Constants;
        EntryPointConstants& constants = entryPoint.Reflection.Constants;
        constants.Delivery = rawConstants.Delivery;
        if (constants.Delivery != EntryPointConstantsDelivery::None)
        {
            constants.ByteSize = rawConstants.ByteSize;
            constants.Members = rawConstants.Members;
        }
        if (constants.Delivery == EntryPointConstantsDelivery::UniformBuffer)
        {
            const auto container = std::ranges::find(raw_entry_point.UsedBindingIndices, *rawConstants.ContainerBinding);
            constants.Binding =
                static_cast<uint32_t>(std::distance(raw_entry_point.UsedBindingIndices.begin(), container));
        }

        // each form starts from reflection, and the reader removes what its code does not declare
        auto makeForm = [&entryPoint](const std::vector<std::byte>& code)
        {
            return EntryPointCode{ .Code = code,
                                   .UsedBindingIndices = entryPoint.Reflection.UsedBindingIndices,
                                   .Constants = entryPoint.Reflection.Constants,
                                   .SamplerTexturePairs = {},
                                   .CodeFacts = {} };
        };
        
        entryPoint.Delivered = makeForm(raw_entry_point.TargetCode);
        if (raw_entry_point.BaseTargetCode.has_value())
        {
            entryPoint.Base = makeForm(*raw_entry_point.BaseTargetCode);
        }

        return entryPoint;
    }

} // namespace

CookResult<DerivedConstantTable> MakeDerivedConstantTable(std::span<const RawDerivedConstant> derived_constants,
                                                          std::span<const PermutationAxis> axes,
                                                          std::span<const ExternConstantDefault> extern_defaults,
                                                          DiagnosticSink& sink)
{
    // stable, so the first declaration of a name is kept
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
            std::vector<DiagnosticNote> declarations;
            for (const uint32_t index : run)
            {
                declarations.push_back(NoteOf(derived_constants[index]));
            }
            return std::unexpected(ReportError(sink,
                                               CookError::DerivedConstantAmbiguous,
                                               LocationOf(first),
                                               std::format("derived constant '{}' is declared with different expressions",
                                                           first.Name),
                                               std::move(declarations)));
        }

        table.ByName.push_back(run.front());
    }

    // shadowing an axis lets Slang and this table pick different declarations
    for (const uint32_t index : table.ByName)
    {
        const RawDerivedConstant& constant = derived_constants[index];

        const bool shadowsAxis =
            std::ranges::any_of(axes, [&](const PermutationAxis& axis) { return axis.Name == constant.Name; });
        const bool shadowsExtern = std::ranges::any_of(
            extern_defaults, [&](const ExternConstantDefault& entry) { return entry.Name == constant.Name; });
        if (shadowsAxis || shadowsExtern)
        {
            const std::string message = std::format("derived constant '{}' = '{}' has the name of {}. Rename one of them",
                                                    constant.Name,
                                                    constant.Expression,
                                                    shadowsAxis ? "an axis" : "an extern constant");
            return std::unexpected(ReportError(sink, CookError::DerivedConstantShadowsSymbol, LocationOf(constant), message));
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
        // todo-ship: map enum case names to values here
        context.Symbols.push_back(
            AttrExprSymbol{ .Name = binding.Axis->Name, .Value = PermutationValueToInt64(binding.Value) });
    }

    if (derived_constants.ByName.empty())
    {
        return context;
    }

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

CookResult<std::vector<uint32_t>> CollectDeclaredFootprintAxes(const RawVariant& raw,
                                                               size_t binding_index,
                                                               std::span<const PermutationAxis> axes,
                                                               const DerivedConstantTable& derived_constants,
                                                               DiagnosticSink& sink)
{
    std::vector<uint32_t> axisIndices;
    // the table and the walk already reject cycles, and `queued` reads diamonds once
    std::vector<uint8_t> queued(derived_constants.Constants.size(), uint8_t{ 0u });
    std::vector<std::string_view> pending;
    for (const RawSizeAttribute& attribute : raw.SizeAttributes)
    {
        if (attribute.BindingIndex == binding_index)
        {
            pending.append_range(attribute.Arguments);
        }
    }

    while (!pending.empty())
    {
        const std::string_view expression = pending.back();
        pending.pop_back();

        const CookResult<std::vector<std::string>> identifiers = CollectExpressionIdentifiers(expression, sink);
        if (!identifiers)
        {
            return std::unexpected(identifiers.error());
        }

        for (const std::string& identifier : identifiers.value())
        {
            const auto axis = std::ranges::find(axes, identifier, &PermutationAxis::Name);
            if (axis != axes.end())
            {
                axisIndices.push_back(static_cast<uint32_t>(axis - axes.begin()));
                continue;
            }

            const std::optional<uint32_t> constant = FindDerivedConstant(derived_constants, identifier);
            if (constant && queued[*constant] == 0u)
            {
                queued[*constant] = 1u;
                pending.push_back(derived_constants.Constants[*constant].Expression);
            }
        }
    }

    std::ranges::sort(axisIndices);
    const auto [uniqueEnd, rangeEnd] = std::ranges::unique(axisIndices);
    axisIndices.erase(uniqueEnd, rangeEnd);
    return axisIndices;
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

        ReflectedBinding binding = ResolveBinding(rawBinding);
        CookResult<ResourceRoleFlags> roles = ResolveRoleNames(rawBinding, sink);
        if (!roles)
        {
            return std::unexpected(roles.error());
        }
        binding.Roles = *roles;
        CookResult<std::vector<std::string>> tags = ResolveTags(rawBinding, sink);
        if (!tags)
        {
            return std::unexpected(tags.error());
        }
        binding.Tags = std::move(*tags);

        const CookError roleError = CheckRoles(binding, *footprint, sink);
        if (roleError != CookError::Success)
        {
            return std::unexpected(roleError);
        }

        variant.Bindings.push_back(std::move(binding));
        variant.Footprints.push_back(std::move(footprint.value()));
    }

    variant.EntryPoints.reserve(raw.EntryPoints.size());
    for (const RawEntryPoint& rawEntryPoint : raw.EntryPoints)
    {
        variant.EntryPoints.push_back(ResolveEntryPoint(rawEntryPoint));
    }

    return variant;
}

namespace
{

    uint32_t IndirectCommandBytes(ResourceRoleFlags role) noexcept
    {
        switch (role)
        {
        case ResourceRoleFlags::IndirectDispatch:
            return 12u;
        case ResourceRoleFlags::IndirectDraw:
            return 16u;
        case ResourceRoleFlags::IndirectDrawIndexed:
            return 20u;
        default:
            return 0u;
        }
    }

    ShaderLocation LocationOf(const RawDerivedConstant& constant) noexcept
    {
        return ShaderLocation{ .File = constant.SourceFile, .Line = static_cast<uint32_t>(constant.SourceLine) };
    }

    DiagnosticNote NoteOf(const RawDerivedConstant& constant)
    {
        return DiagnosticNote{ .File = constant.SourceFile,
                               .Range = ToSourceRange(LocationOf(constant)),
                               .Message = std::format("'{}' = '{}'", constant.Name, constant.Expression) };
    }

    std::optional<uint32_t> FindDerivedConstant(const DerivedConstantTable& table, std::string_view name)
    {
        auto extractNameView = [&](uint32_t index) -> std::string_view
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

    CookError DerivedConstantWalk::VisitRoot(std::string_view argument, std::string_view root)
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

    CookResult<uint32_t> EvaluateExtentArgument(const RawSizeAttribute& attribute,
                                                uint32_t argument_index,
                                                std::string_view binding_name,
                                                const ResolveContext& context,
                                                DiagnosticSink& sink)
    {
        if (argument_index >= attribute.Arguments.size())
        {
            return std::unexpected(ReportError(sink,
                                               CookError::AttributeExpressionParseFailed,
                                               std::format("[{}] on '{}' is missing argument {}",
                                                           ToString(attribute.Kind),
                                                           binding_name,
                                                           argument_index)));
        }

        const CookResult<int64_t> value =
            EvaluateExpression(attribute.Arguments[argument_index], context.Symbols, sink);
        if (!value)
        {
            return std::unexpected(value.error());
        }

        if (value.value() <= 0)
        {
            return std::unexpected(ReportError(sink,
                                               CookError::AttributeExpressionOutOfRange,
                                               std::format("extent argument {} on '{}' evaluated to {}, which is not a "
                                                           "valid texture dimension",
                                                           argument_index,
                                                           binding_name,
                                                           value.value())));
        }

        return static_cast<uint32_t>(value.value());
    }

    CookResult<TextureFootprint> ResolveExtent(const RawSizeAttribute& attribute,
                                               std::string_view binding_name,
                                               const ResolveContext& context,
                                               DiagnosticSink& sink)
    {
        const int32_t argumentCount = ArgumentCountOf(attribute.Kind);
        std::array<uint32_t, 3u> axes{ 1u, 1u, 1u };

        for (int32_t i = 0u; i < argumentCount && i < std::ssize(axes); ++i)
        {
            const CookResult<uint32_t> value = EvaluateExtentArgument(attribute, i, binding_name, context, sink);
            if (!value)
            {
                return std::unexpected(value.error());
            }

            axes[i] = value.value();
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
            return std::unexpected(ReportError(sink,
                                               CookError::AttributeExpressionParseFailed,
                                               std::format("[{}] on '{}' has no argument",
                                                           ToString(attribute.Kind),
                                                           binding_name)));
        }

        const CookResult<int64_t> value =
            EvaluateExpression(attribute.Arguments.front(), context.Symbols, sink);
        if (!value)
        {
            // the evaluator reported why; this record names the resource it was sizing
            return std::unexpected(ReportError(sink,
                                               value.error(),
                                               std::format("[{}] on '{}' did not evaluate",
                                               ToString(attribute.Kind),
                                               binding_name)));
        }

        if (value.value() <= 0)
        {
            return std::unexpected(ReportError(sink,
                                               CookError::AttributeExpressionOutOfRange,
                                               std::format("[{}] on '{}' evaluated to {}, which cannot size a buffer",
                                               ToString(attribute.Kind),
                                               binding_name,
                                               value.value())));
        }

        return BufferFootprint{ .ElementCount = static_cast<uint64_t>(value.value()),
                                .Expression = attribute.Arguments.front() };
    }

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
                return std::unexpected(ReportError(sink,
                                                   CookError::ReflectionSizeUnresolved,
                                                   std::format("'{}' carries [{}] and [{}]; only one may size a resource",
                                                   binding_name,
                                                   ToString(slot->Kind),
                                                   ToString(attribute.Kind))));
            }

            slot = &attribute;
        }

        if (count != nullptr && extent != nullptr)
        {
            return std::unexpected(ReportError(sink,
                                               CookError::ReflectionSizeUnresolved,
                                               std::format("'{}' carries an element count and an extent; a resource is a "
                                               "buffer or a texture",
                                               binding_name)));
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

    std::vector<std::string_view> SplitWords(std::string_view text)
    {
        std::vector<std::string_view> words;
        for (const auto word : std::views::split(text, ' '))
        {
            const std::string_view view{ word.begin(), word.end() };
            if (!view.empty())
            {
                words.push_back(view);
            }
        }
        return words;
    }

    CookResult<std::vector<std::string>> ResolveTags(const RawBinding& raw_binding, DiagnosticSink& sink)
    {
        std::vector<std::string> tags;
        if (raw_binding.RawTags.empty())
        {
            return tags;
        }

        for (const std::string_view word : SplitWords(raw_binding.RawTags))
        {
            if (!std::ranges::all_of(word, IsTagCharacter))
            {
                return std::unexpected(ReportError(sink,
                                                   CookError::ResourceTagInvalid,
                                                   std::format("'{}': the tag '{}' holds a character other than a "
                                                               "letter, a digit, or one of '_.:-'",
                                                               raw_binding.Name,
                                                               word)));
            }
            tags.emplace_back(word);
        }

        if (tags.empty())
        {
            return std::unexpected(ReportError(sink,
                                               CookError::ResourceTagInvalid,
                                               std::format("'{}': [ls_resource_tags] holds no tag", raw_binding.Name)));
        }

        std::ranges::sort(tags);
        const auto repeated = std::ranges::unique(tags);
        tags.erase(repeated.begin(), repeated.end());
        return tags;
    }

    CookResult<ResourceRoleFlags> ResolveRoleNames(const RawBinding& raw_binding, DiagnosticSink& sink)
    {
        ResourceRoleFlags roles = ResourceRoleFlags::None;
        if (raw_binding.RawRoles.empty())
        {
            return roles;
        }

        for (const std::string_view word : SplitWords(raw_binding.RawRoles))
        {
            const auto found =
                std::ranges::find(k_ResourceRoleNames, word, &std::pair<std::string_view, ResourceRoleFlags>::first);
            if (found == k_ResourceRoleNames.end())
            {
                std::array<std::string_view, k_ResourceRoleNames.size()> names{};
                std::ranges::transform(k_ResourceRoleNames, names.begin(), [](const auto& entry) { return entry.first; });
                const std::string_view nearest = NearestName(word, names);
                return std::unexpected(ReportError(
                    sink,
                    CookError::ResourceRoleUnknown,
                    nearest.empty() ? std::format("'{}': '{}' is not a resource role", raw_binding.Name, word)
                                    : std::format("'{}': '{}' is not a resource role. Did you mean '{}'?",
                                                  raw_binding.Name,
                                                  word,
                                                  nearest)));
            }
            roles |= found->second;
        }

        if (roles == ResourceRoleFlags::None)
        {
            return std::unexpected(ReportError(sink,
                                               CookError::ResourceRoleUnknown,
                                               std::format("'{}': [ls_resource_role] holds no role", raw_binding.Name)));
        }
        return roles;
    }

    CookError CheckRoles(const ReflectedBinding& binding, const ResourceFootprint& footprint, DiagnosticSink& sink)
    {
        if (binding.Roles == ResourceRoleFlags::None)
        {
            return CookError::Success;
        }

        const ResourceShape baseShape = GetBaseShape(binding.Shape);
        const bool isBuffer = binding.Kind == BindingKind::StorageBuffer &&
                              (baseShape == ResourceShape::StructuredBuffer || baseShape == ResourceShape::ByteAddressBuffer);
        if (!isBuffer)
        {
            return ReportError(sink,
                               CookError::ResourceRoleKindMismatch,
                               std::format("'{}': a resource role needs a structured or raw storage buffer, not a {}",
                                           binding.Name,
                                           ToString(binding.Kind)));
        }

        const uint32_t stride = baseShape == ResourceShape::ByteAddressBuffer ? 4u : binding.ElementStride;
        const BufferFootprint* counted = std::get_if<BufferFootprint>(&footprint);
        for (const auto& [name, role] : k_ResourceRoleNames)
        {
            if (!HasAnyFlag(binding.Roles, role))
            {
                continue;
            }

            const uint32_t commandBytes = IndirectCommandBytes(role);
            if (commandBytes != 0u)
            {
                if (stride == commandBytes)
                {
                    continue;
                }
                if (stride != 4u)
                {
                    return ReportError(sink,
                                       CookError::ResourceRoleStrideMismatch,
                                       std::format("'{}': the role {} needs a stride of {} or 4 bytes, not {}",
                                                   binding.Name,
                                                   name,
                                                   commandBytes,
                                                   stride));
                }
                if (counted == nullptr)
                {
                    return ReportError(sink,
                                       CookError::ResourceRoleNeedsElementCount,
                                       std::format("'{}': the role {} with a 4-byte stride needs [ls_element_count], so "
                                                   "the cook can check that the buffer holds whole commands",
                                                   binding.Name,
                                                   name));
                }
                const uint32_t commandWords = commandBytes / 4u;
                if (counted->ElementCount % commandWords != 0u)
                {
                    return ReportError(sink,
                                       CookError::ResourceRoleStrideMismatch,
                                       std::format("'{}': the role {} needs a count of 4-byte elements that is a "
                                                   "multiple of {}, not {}",
                                                   binding.Name,
                                                   name,
                                                   commandWords,
                                                   counted->ElementCount));
                }
                continue;
            }

            const bool strideFits = role == ResourceRoleFlags::IndexData ? stride == 4u : stride != 0u && stride % 4u == 0u;
            if (!strideFits)
            {
                return ReportError(sink,
                                   CookError::ResourceRoleStrideMismatch,
                                   std::format("'{}': the role {} needs {}, not a stride of {}",
                                               binding.Name,
                                               name,
                                               role == ResourceRoleFlags::IndexData ? "a stride of 4 bytes (32-bit indices)"
                                                                                    : "a stride that is a multiple of 4 bytes",
                                               stride));
            }
        }

        return CookError::Success;
    }

    CookError DerivedConstantWalk::visitIdentifiers(std::span<const std::string> identifiers)
    {
        for (const std::string& identifier : identifiers)
        {
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

    CookError DerivedConstantWalk::visit(uint32_t index)
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

        const CookResult<int64_t> value = EvaluateExpression(constant.Expression,
                                                             context.Symbols,
                                                             sink);
        if (!value)
        {
            return reportUnreadable(constant, "did not evaluate");
        }

        context.Symbols.push_back(AttrExprSymbol{ .Name = constant.Name, .Value = value.value() });
        colors[index] = VisitColor::Black;
        path.pop_back();

        return CookError::Success;
    }

    std::optional<uint32_t> DerivedConstantWalk::findByName(std::string_view name) const
    {
        return FindDerivedConstant(table, name);
    }

    std::vector<DiagnosticNote> DerivedConstantWalk::notesForPath(std::span<const uint32_t> nodes) const
    {
        std::vector<DiagnosticNote> notes;
        for (const uint32_t node : nodes)
        {
            notes.push_back(NoteOf(table.Constants[node]));
        }
        return notes;
    }

    CookError DerivedConstantWalk::reportCycle(uint32_t index) const
    {
        const auto cycleStart = std::ranges::find(path, index);
        std::vector<uint32_t> cycle(cycleStart, path.end());
        cycle.push_back(index);
        return ReportError(sink,
                           CookError::DerivedConstantCycle,
                           LocationOf(table.Constants[index]),
                           std::format("derived constants form a cycle, reached from {}", rootDescription),
                           notesForPath(cycle));
    }

    CookError DerivedConstantWalk::reportUnreadable(const RawDerivedConstant& constant,
                                                    std::string_view failure) const
    {
        return ReportError(sink,
                           CookError::DerivedConstantUnreadable,
                           LocationOf(constant),
                           std::format("derived constant '{}' {}, reached from {}", constant.Name, failure, rootDescription),
                           notesForPath(path));
    }


} // namespace

} // namespace lodestone
