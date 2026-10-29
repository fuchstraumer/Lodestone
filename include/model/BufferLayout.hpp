#pragma once
#ifndef LODESTONE_BUFFER_LAYOUT_HPP
#define LODESTONE_BUFFER_LAYOUT_HPP
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lodestone
{

enum class LayoutNodeKind : uint8_t
{
    Invalid = 0,
    Scalar,
    Vector,
    Matrix,
    Array,
    Struct,
};

enum class BufferLayoutRule : uint8_t
{
    Scalar,
    Std430,
};

/** @brief The shape of one buffer element type, with no Slang type. A scalar, vector, or matrix stores the byte size of one
 * component. An array has one child, and a struct has one child for each field, in declaration order. */
struct LayoutNode
{
    LayoutNodeKind Kind{ LayoutNodeKind::Invalid };
    uint32_t ScalarSize{ 0u };
    // vector only
    uint32_t Components{ 1u };
    // matrix only
    uint32_t Rows{ 1u };
    uint32_t Columns{ 1u };
    bool RowMajor{ false };
    // array only
    uint32_t ElementCount{ 1u };
    /** @brief The field name when this node is a field of a struct. */
    std::string Name;
    /** @brief The type name of a struct. */
    std::string TypeName;
    std::vector<LayoutNode> Children;
};

struct BufferLayoutSize
{
    uint32_t Size{ 0u };
    uint32_t Alignment{ 1u };
};

enum class LayoutDifferenceKind : uint8_t
{
    MemberOffset,
    StructSize,
    ArrayStride,
};

/** @brief The first place where the scalar layout and the std430 layout disagree. */
struct LayoutDifference
{
    LayoutDifferenceKind Kind{ LayoutDifferenceKind::MemberOffset };
    /** @brief The struct that contains the difference, or empty for an element that is not a struct. */
    std::string StructName;
    /** @brief The member with a different offset or stride. Empty for a struct size or a plain element. */
    std::string MemberName;
    uint32_t ScalarValue{ 0u };
    uint32_t Std430Value{ 0u };
};

/** @brief Returns the size and base alignment of a type under one rule. A struct size is a multiple of its alignment. */
BufferLayoutSize ComputeBufferLayout(const LayoutNode& node, BufferLayoutRule rule);

/** @brief Returns the distance between two elements of an array of the type: the size rounded up to the alignment. */
uint32_t ComputeElementStride(const LayoutNode& node, BufferLayoutRule rule);

/** @brief Returns the first member offset, struct size, or array stride that differs between the two rules, or nothing
 * when they agree. The element stride of a type that is not a struct is compared as well. */
std::optional<LayoutDifference> FindLayoutDifference(const LayoutNode& element);

/** @brief Returns the text of the difference for an error message, including the padding that the author must add. */
std::string DescribeLayoutDifference(const LayoutDifference& difference);

} // namespace lodestone

#endif // !LODESTONE_BUFFER_LAYOUT_HPP
