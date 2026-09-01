#include "model/ResolveStage.hpp"
#include "CookerErrors.hpp"
#include "permute/PermutationSpace.hpp"
#include "compile/RawLibrary.hpp"
#include "model/ShaderDataSchema.hpp"
#include "permute/AttributeExpression.hpp"

#include <array>
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
                                                const ResolveContext& context)
    {
        if (argument_index >= attribute.Arguments.size())
        {
            std::println(stderr,
                         "[shader_cooker] [{}] on '{}' is missing argument {}",
                         ToString(attribute.Kind),
                         binding_name,
                         argument_index);
            return std::unexpected(CookError::AttributeExpressionParseFailed);
        }

        const CookResult<int64_t> value =
            EvaluateExpression(attribute.Arguments[argument_index], context.Symbols);
        if (!value)
        {
            return std::unexpected(value.error());
        }

        if (value.value() <= 0)
        {
            std::println(stderr,
                         "[shader_cooker] extent argument {} on '{}' evaluated to {}, which is not a "
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
                                               const ResolveContext& context)
    {
        const uint32_t argumentCount = ArgumentCountOf(attribute.Kind);
        std::array<uint32_t, 3u> axes{ 1u, 1u, 1u };

        for (uint32_t i = 0u; i < argumentCount && i < axes.size(); ++i)
        {
            const CookResult<uint32_t> value = EvaluateExtentArgument(attribute, i, binding_name, context);
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
                                                    const ResolveContext& context)
    {
        if (attribute.Arguments.empty())
        {
            std::println(stderr,
                         "[shader_cooker] [{}] on '{}' has no argument",
                         ToString(attribute.Kind),
                         binding_name);
            return std::unexpected(CookError::AttributeExpressionParseFailed);
        }

        const CookResult<int64_t> value =
            EvaluateExpression(attribute.Arguments.front(), context.Symbols);
        if (!value)
        {
            std::println(stderr,
                         "[shader_cooker] [{}] on '{}' did not evaluate",
                         ToString(attribute.Kind),
                         binding_name);
            return std::unexpected(value.error());
        }

        if (value.value() <= 0)
        {
            std::println(stderr,
                         "[shader_cooker] [{}] on '{}' evaluated to {}, which cannot size a buffer",
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
                                                   const ResolveContext& context)
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
                             "[shader_cooker] '{}' carries [{}] and [{}]; only one may size a resource",
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
                         "[shader_cooker] '{}' carries an element count and an extent; a resource is a "
                         "buffer or a texture",
                         binding_name);
            return std::unexpected(CookError::ReflectionSizeUnresolved);
        }

        if (count != nullptr)
        {
            CookResult<BufferFootprint> buffer = ResolveElementCount(*count, binding_name, context);
            if (!buffer)
            {
                return std::unexpected(buffer.error());
            }
            return ResourceFootprint{ std::move(buffer.value()) };
        }

        if (extent != nullptr)
        {
            CookResult<TextureFootprint> texture = ResolveExtent(*extent, binding_name, context);
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
        binding.StorageAccess = raw_binding.StorageAccess;
        binding.SamplerType = raw_binding.SamplerType;
        binding.UniformMembers = raw_binding.UniformMembers;
        return binding;
    }

    CompiledEntryPoint ResolveEntryPoint(const RawEntryPoint& raw_entry_point)
    {
        CompiledEntryPoint entryPoint;
        entryPoint.Name = raw_entry_point.Name;
        entryPoint.VariantSuffix = raw_entry_point.VariantSuffix;
        entryPoint.Code = raw_entry_point.TargetText;
        entryPoint.Reflection.Name = raw_entry_point.Name;
        entryPoint.Reflection.Stage = raw_entry_point.Stage;
        entryPoint.Reflection.Workgroup = raw_entry_point.Workgroup;
        entryPoint.Reflection.UsedBindingIndices = raw_entry_point.UsedBindingIndices;
        entryPoint.Reflection.Raster = raw_entry_point.Raster;
        return entryPoint;
    }

} // namespace

ResolveContext MakeResolveContext(const PermutationAssignment& canonical,
                                  std::span<const ExternConstantDefault> extern_defaults)
{
    ResolveContext context;
    context.Symbols.reserve(canonical.size() + extern_defaults.size());

    for (const ExternConstantDefault& entry : extern_defaults)
    {
        context.Symbols.push_back(SizeSymbol{ .Name = entry.Name, .Value = entry.Value });
    }

    for (const PermutationBinding& binding : canonical)
    {
        context.Symbols.push_back(
            SizeSymbol{ .Name = binding.Axis->Name, .Value = PermutationValueToInt64(binding.Value) });
    }

    return context;
}

CookResult<CompiledVariant> ResolveVariant(const RawVariant& raw, const ResolveContext& context)
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
            ResolveFootprint(AttributesOfBinding(raw, i), rawBinding.Name, context);
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
