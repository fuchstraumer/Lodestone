#pragma once
#ifndef LODESTONE_HOST_TYPE_TABLE_HPP
#define LODESTONE_HOST_TYPE_TABLE_HPP
#include "ShaderLibraryTypes.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/** @file The struct types that a host program mirrors: the layout of each struct under one layout rule. This
 * is the data of a type tree, with indices and no pointers, and no Slang type. */

namespace lodestone
{

enum class HostScalarType : uint8_t
{
    Invalid = 0,
    Bool,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float16,
    Float32,
    Float64,
};

enum class HostFieldKind : uint8_t
{
    Invalid = 0,
    Scalar,
    Vector,
    Matrix,
    Struct,
};

/** @brief `StructIndex` of a field that is not a struct. */
inline constexpr uint32_t k_NoHostStruct = UINT32_MAX;

/** @brief One dimension of an array member. `Stride` is the byte distance between two elements of this dimension. */
struct HostArrayDimension
{
    uint32_t Count{ 1u };
    uint32_t Stride{ 0u };

    friend bool operator==(const HostArrayDimension&, const HostArrayDimension&) = default;
};

/** @brief One member of a host struct. `Size` is the size of the whole member, including the array. A vector
 * has `Rows` of 1 and its component count in `Columns`, and a scalar has both at 1. A buffer device address is
 * a `UInt64` scalar. */
struct HostField
{
    std::string Name;
    uint32_t Offset{ 0u };
    uint32_t Size{ 0u };
    HostFieldKind Kind{ HostFieldKind::Invalid };
    HostScalarType ScalarType{ HostScalarType::Invalid };
    uint32_t Rows{ 1u };
    uint32_t Columns{ 1u };
    MatrixLayout MatrixLayout{ MatrixLayout::Invalid };
    /** The dimensions of an array member, outermost first. Empty when the member is not an array. An unbounded
     * array is not allowed here. */
    std::vector<HostArrayDimension> ArrayDimensions;
    /** Index into `HostTypeTable::Structs`, for `HostFieldKind::Struct`. */
    uint32_t StructIndex{ k_NoHostStruct };
    /** True for the member that a Handle or Pointer shim becomes: a `uint2` handle or a `UInt64` address. The
     * layout of a struct with such a member follows the access model of the profile. */
    bool StoredValue{ false };

    friend bool operator==(const HostField&, const HostField&) = default;
};

/** @brief `Name` is the full name that Slang gives the type, so a generic type includes its type arguments.
 */
struct HostStruct
{
    std::string Name;
    LayoutRule Rule{ LayoutRule::Std140 };
    uint32_t ByteSize{ 0u };
    std::vector<HostField> Fields;

    friend bool operator==(const HostStruct&, const HostStruct&) = default;
};

/** @brief The structs of one variant, or the merged structs of a module. A struct comes after the structs
 * that its fields use, so a nested `StructIndex` is always smaller than the index of its parent. */
struct HostTypeTable
{
    std::vector<HostStruct> Structs;

    friend bool operator==(const HostTypeTable&, const HostTypeTable&) = default;
};

enum class HostRootKind : uint8_t
{
    Binding,
    EntryPointConstants,
};

/** @brief The struct that one binding or one entry point's constants store, as an index into the table.
 * `Owner` is the binding index of the variant, or the entry point index. */
struct HostRoot
{
    HostRootKind Kind{ HostRootKind::Binding };
    uint32_t Owner{ 0u };
    uint32_t Struct{ 0u };

    friend bool operator==(const HostRoot&, const HostRoot&) = default;
};

/** @brief Maps a name and a layout rule to the index of that struct in the table that it was built beside. */
using HostTypeIndex = std::map<std::pair<std::string, LayoutRule>, uint32_t>;

/** @brief Returns the index of the struct with this name and rule, or nothing. Searches every struct, so the
 * module build uses a `HostTypeIndex` instead. */
std::optional<uint32_t> FindHostStruct(const HostTypeTable& table,
                                       std::string_view name,
                                       LayoutRule rule) noexcept;

/** @brief Returns a description of the first difference between two layouts: the size, the field count, or
 * the first field that differs. A nested struct compares by its name and rule, because its own entry in the
 * table is checked on its own. Returns an empty string when the two layouts are equal. */
std::string DescribeHostStructDifference(const HostTypeTable& lhs_table,
                                         uint32_t lhs_index,
                                         const HostTypeTable& rhs_table,
                                         uint32_t rhs_index);

struct HostTypeConflict
{
    /** Index in the destination table. */
    uint32_t Existing{ 0u };
    /** Index in the source table. */
    uint32_t Incoming{ 0u };
    std::string Difference;
};

/** @brief Adds each struct of `source` that `destination` lacks, in order. `out_remap[i]` is the destination
 * index of source struct `i`. `destination_index` must index `destination`, and this function keeps it current.
 * Returns the first struct whose name and rule exist in `destination` with another layout. */
std::optional<HostTypeConflict> MergeHostTypes(HostTypeTable& destination,
                                               HostTypeIndex& destination_index,
                                               const HostTypeTable& source,
                                               std::vector<uint32_t>& out_remap);

std::string_view ToString(LayoutRule rule) noexcept;

} // namespace lodestone

#endif // !LODESTONE_HOST_TYPE_TABLE_HPP
