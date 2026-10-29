#include "model/BufferLayout.hpp"
#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace lodestone
{

namespace
{

    uint32_t RoundUp(uint32_t value, uint32_t alignment) noexcept;

    BufferLayoutSize VectorLayout(uint32_t scalar_size, uint32_t components, BufferLayoutRule rule) noexcept;

    // a matrix is an array of vectors: the columns, or the rows when it is row-major
    uint32_t MatrixVectorComponents(const LayoutNode& node) noexcept;

    uint32_t MatrixVectorCount(const LayoutNode& node) noexcept;

    uint32_t MatrixStride(const LayoutNode& node, BufferLayoutRule rule) noexcept;

    BufferLayoutSize MatrixLayout(const LayoutNode& node, BufferLayoutRule rule) noexcept;

    BufferLayoutSize StructLayout(const LayoutNode& node, BufferLayoutRule rule);

    // walks the fields of a struct under both rules at once, and stops at the first difference
    std::optional<LayoutDifference> FindStructDifference(const LayoutNode& node);

    // the stride of an array or matrix, and the layout inside a struct or an array of structs
    std::optional<LayoutDifference> FindContentDifference(const LayoutNode& node,
                                                          const std::string& struct_name,
                                                          const std::string& member_name);

} // namespace

BufferLayoutSize ComputeBufferLayout(const LayoutNode& node, BufferLayoutRule rule)
{
    switch (node.Kind)
    {
    case LayoutNodeKind::Scalar:
        return BufferLayoutSize{ .Size = node.ScalarSize, .Alignment = node.ScalarSize };
    case LayoutNodeKind::Vector:
        return VectorLayout(node.ScalarSize, node.Components, rule);
    case LayoutNodeKind::Matrix:
        return MatrixLayout(node, rule);
    case LayoutNodeKind::Array:
    {
        const BufferLayoutSize element = ComputeBufferLayout(node.Children.front(), rule);
        const uint32_t stride = RoundUp(element.Size, element.Alignment);

        return BufferLayoutSize{ .Size = stride * node.ElementCount, .Alignment = element.Alignment };
    }
    case LayoutNodeKind::Struct:
        return StructLayout(node, rule);
    case LayoutNodeKind::Invalid:
        break;
    }

    return BufferLayoutSize{};
}

uint32_t ComputeElementStride(const LayoutNode& node, BufferLayoutRule rule)
{
    const BufferLayoutSize layout = ComputeBufferLayout(node, rule);

    return RoundUp(layout.Size, layout.Alignment);
}

std::optional<LayoutDifference> FindLayoutDifference(const LayoutNode& element)
{
    const std::optional<LayoutDifference> inner = FindContentDifference(element, std::string{}, std::string{});
    if (inner)
    {
        return inner;
    }

    const uint32_t scalarStride = ComputeElementStride(element, BufferLayoutRule::Scalar);
    const uint32_t std430Stride = ComputeElementStride(element, BufferLayoutRule::Std430);
    if (scalarStride == std430Stride)
    {
        return std::nullopt;
    }

    return LayoutDifference{ .Kind = LayoutDifferenceKind::ArrayStride,
                             .StructName = {},
                             .MemberName = {},
                             .ScalarValue = scalarStride,
                             .Std430Value = std430Stride };
}

std::string DescribeLayoutDifference(const LayoutDifference& difference)
{
    const std::string where = difference.StructName.empty() ? std::string{ "the element" }
                                                            : std::format("struct '{}'", difference.StructName);
    switch (difference.Kind)
    {
    case LayoutDifferenceKind::MemberOffset:
        return std::format("{} member '{}' is at offset {} in the scalar layout and at offset {} in the std430 layout. "
                           "Add a padding member before it, so that the two layouts agree.",
                           where,
                           difference.MemberName,
                           difference.ScalarValue,
                           difference.Std430Value);
    case LayoutDifferenceKind::StructSize:
        return std::format("{} is {} bytes in the scalar layout and {} bytes in the std430 layout. Add a padding member at "
                           "the end, so that the two layouts agree.",
                           where,
                           difference.ScalarValue,
                           difference.Std430Value);
    case LayoutDifferenceKind::ArrayStride:
    {
        const std::string member =
            difference.MemberName.empty() ? std::string{} : std::format(" member '{}'", difference.MemberName);

        return std::format("{}{} has a stride of {} bytes in the scalar layout and {} bytes in the std430 layout. Pad each "
                           "item to its alignment, so that the two layouts agree.",
                           where,
                           member,
                           difference.ScalarValue,
                           difference.Std430Value);
    }
    }

    return {};
}

namespace
{

    uint32_t RoundUp(uint32_t value, uint32_t alignment) noexcept
    {
        const uint32_t unit = std::max(alignment, 1u);

        return (value + unit - 1u) / unit * unit;
    }

    BufferLayoutSize VectorLayout(uint32_t scalar_size, uint32_t components, BufferLayoutRule rule) noexcept
    {
        const uint32_t size = scalar_size * components;
        if (rule == BufferLayoutRule::Scalar)
        {
            return BufferLayoutSize{ .Size = size, .Alignment = scalar_size };
        }

        // std430 gives a vec3 the alignment of a vec4
        const uint32_t alignedComponents = components == 3u ? 4u : components;

        return BufferLayoutSize{ .Size = size, .Alignment = scalar_size * alignedComponents };
    }

    uint32_t MatrixVectorComponents(const LayoutNode& node) noexcept
    {
        return node.RowMajor ? node.Columns : node.Rows;
    }

    uint32_t MatrixVectorCount(const LayoutNode& node) noexcept
    {
        return node.RowMajor ? node.Rows : node.Columns;
    }

    uint32_t MatrixStride(const LayoutNode& node, BufferLayoutRule rule) noexcept
    {
        const BufferLayoutSize vector = VectorLayout(node.ScalarSize, MatrixVectorComponents(node), rule);

        return RoundUp(vector.Size, vector.Alignment);
    }

    BufferLayoutSize MatrixLayout(const LayoutNode& node, BufferLayoutRule rule) noexcept
    {
        const BufferLayoutSize vector = VectorLayout(node.ScalarSize, MatrixVectorComponents(node), rule);

        return BufferLayoutSize{ .Size = MatrixStride(node, rule) * MatrixVectorCount(node), .Alignment = vector.Alignment };
    }

    BufferLayoutSize StructLayout(const LayoutNode& node, BufferLayoutRule rule)
    {
        uint32_t offset = 0u;
        uint32_t alignment = 1u;
        for (const LayoutNode& field : node.Children)
        {
            const BufferLayoutSize layout = ComputeBufferLayout(field, rule);
            offset = RoundUp(offset, layout.Alignment) + layout.Size;
            alignment = std::max(alignment, layout.Alignment);
        }

        return BufferLayoutSize{ .Size = RoundUp(offset, alignment), .Alignment = alignment };
    }

    std::optional<LayoutDifference> FindStructDifference(const LayoutNode& node)
    {
        uint32_t scalarEnd = 0u;
        uint32_t std430End = 0u;
        for (const LayoutNode& field : node.Children)
        {
            const BufferLayoutSize scalar = ComputeBufferLayout(field, BufferLayoutRule::Scalar);
            const BufferLayoutSize std430 = ComputeBufferLayout(field, BufferLayoutRule::Std430);
            const uint32_t scalarOffset = RoundUp(scalarEnd, scalar.Alignment);
            const uint32_t std430Offset = RoundUp(std430End, std430.Alignment);
            if (scalarOffset != std430Offset)
            {
                return LayoutDifference{ .Kind = LayoutDifferenceKind::MemberOffset,
                                         .StructName = node.TypeName,
                                         .MemberName = field.Name,
                                         .ScalarValue = scalarOffset,
                                         .Std430Value = std430Offset };
            }

            const std::optional<LayoutDifference> inner = FindContentDifference(field, node.TypeName, field.Name);
            if (inner)
            {
                return inner;
            }

            scalarEnd = scalarOffset + scalar.Size;
            std430End = std430Offset + std430.Size;
        }

        const uint32_t scalarSize = ComputeBufferLayout(node, BufferLayoutRule::Scalar).Size;
        const uint32_t std430Size = ComputeBufferLayout(node, BufferLayoutRule::Std430).Size;
        if (scalarSize == std430Size)
        {
            return std::nullopt;
        }

        return LayoutDifference{ .Kind = LayoutDifferenceKind::StructSize,
                                 .StructName = node.TypeName,
                                 .MemberName = {},
                                 .ScalarValue = scalarSize,
                                 .Std430Value = std430Size };
    }

    std::optional<LayoutDifference> FindContentDifference(const LayoutNode& node,
                                                          const std::string& struct_name,
                                                          const std::string& member_name)
    {
        switch (node.Kind)
        {
        case LayoutNodeKind::Struct:
            return FindStructDifference(node);
        case LayoutNodeKind::Array:
        {
            const LayoutNode& element = node.Children.front();
            const uint32_t scalarStride = ComputeElementStride(element, BufferLayoutRule::Scalar);
            const uint32_t std430Stride = ComputeElementStride(element, BufferLayoutRule::Std430);
            if (scalarStride != std430Stride)
            {
                return LayoutDifference{ .Kind = LayoutDifferenceKind::ArrayStride,
                                         .StructName = struct_name,
                                         .MemberName = member_name,
                                         .ScalarValue = scalarStride,
                                         .Std430Value = std430Stride };
            }

            return FindContentDifference(element, struct_name, member_name);
        }
        case LayoutNodeKind::Matrix:
        {
            const uint32_t scalarStride = MatrixStride(node, BufferLayoutRule::Scalar);
            const uint32_t std430Stride = MatrixStride(node, BufferLayoutRule::Std430);
            if (scalarStride == std430Stride)
            {
                return std::nullopt;
            }

            return LayoutDifference{ .Kind = LayoutDifferenceKind::ArrayStride,
                                     .StructName = struct_name,
                                     .MemberName = member_name,
                                     .ScalarValue = scalarStride,
                                     .Std430Value = std430Stride };
        }
        case LayoutNodeKind::Scalar:
        case LayoutNodeKind::Vector:
        case LayoutNodeKind::Invalid:
            break;
        }

        return std::nullopt;
    }

} // namespace

} // namespace lodestone
