#include "model/HostTypeTable.hpp"
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <magic_enum/magic_enum.hpp>

namespace lodestone
{

namespace
{

    std::string DescribeDimensions(const std::vector<HostArrayDimension>& dimensions);

    std::string DescribeFieldDifference(const HostTypeTable& lhs_table,
                                        const HostField& lhs,
                                        const HostTypeTable& rhs_table,
                                        const HostField& rhs)
    {
        if (lhs.Name != rhs.Name)
        {
            return std::format(
                "a field is named '{}' in one layout and '{}' in the other", lhs.Name, rhs.Name);
        }

        if (lhs.Offset != rhs.Offset)
        {
            return std::format(
                "field '{}' is at offset {} and at offset {}", lhs.Name, lhs.Offset, rhs.Offset);
        }

        if (lhs.Size != rhs.Size)
        {
            return std::format("field '{}' has size {} and size {}", lhs.Name, lhs.Size, rhs.Size);
        }

        if (lhs.ArrayDimensions != rhs.ArrayDimensions)
        {
            return std::format("field '{}' has array {} and array {}",
                               lhs.Name,
                               DescribeDimensions(lhs.ArrayDimensions),
                               DescribeDimensions(rhs.ArrayDimensions));
        }

        const bool sameShape = lhs.Kind == rhs.Kind && lhs.ScalarType == rhs.ScalarType &&
                               lhs.Rows == rhs.Rows && lhs.Columns == rhs.Columns &&
                               lhs.MatrixLayout == rhs.MatrixLayout && lhs.StoredValue == rhs.StoredValue;
        if (!sameShape)
        {
            return std::format("field '{}' has another type or matrix layout", lhs.Name);
        }

        if (lhs.Kind == HostFieldKind::Struct)
        {
            const HostStruct& lhsNested = lhs_table.Structs[lhs.StructIndex];
            const HostStruct& rhsNested = rhs_table.Structs[rhs.StructIndex];
            if (lhsNested.Name != rhsNested.Name || lhsNested.Rule != rhsNested.Rule)
            {
                return std::format("field '{}' has struct type '{}' and struct type '{}'",
                                   lhs.Name,
                                   lhsNested.Name,
                                   rhsNested.Name);
            }
        }

        return std::string{};
    }

} // namespace

std::optional<uint32_t> FindHostStruct(const HostTypeTable& table,
                                       std::string_view name,
                                       LayoutRule rule) noexcept
{
    for (size_t index = 0u; index < table.Structs.size(); ++index)
    {
        const HostStruct& candidate = table.Structs[index];
        if (candidate.Rule == rule && candidate.Name == name)
        {
            return static_cast<uint32_t>(index);
        }
    }

    return std::nullopt;
}

std::string DescribeHostStructDifference(const HostTypeTable& lhs_table,
                                         uint32_t lhs_index,
                                         const HostTypeTable& rhs_table,
                                         uint32_t rhs_index)
{
    const HostStruct& lhs = lhs_table.Structs[lhs_index];
    const HostStruct& rhs = rhs_table.Structs[rhs_index];

    if (lhs.ByteSize != rhs.ByteSize)
    {
        return std::format("the size is {} bytes and {} bytes", lhs.ByteSize, rhs.ByteSize);
    }

    if (lhs.Fields.size() != rhs.Fields.size())
    {
        return std::format("the field count is {} and {}", lhs.Fields.size(), rhs.Fields.size());
    }

    for (size_t fieldIndex = 0u; fieldIndex < lhs.Fields.size(); ++fieldIndex)
    {
        std::string difference =
            DescribeFieldDifference(lhs_table, lhs.Fields[fieldIndex], rhs_table, rhs.Fields[fieldIndex]);
        if (!difference.empty())
        {
            return difference;
        }
    }

    return std::string{};
}

std::optional<HostTypeConflict> MergeHostTypes(HostTypeTable& destination,
                                               HostTypeIndex& destination_index,
                                               const HostTypeTable& source,
                                               std::vector<uint32_t>& out_remap)
{
    out_remap.clear();
    out_remap.reserve(source.Structs.size());

    for (size_t sourceIndex = 0u; sourceIndex < source.Structs.size(); ++sourceIndex)
    {
        const HostStruct& incoming = source.Structs[sourceIndex];
        const auto existing = destination_index.find(std::pair{ incoming.Name, incoming.Rule });
        if (existing != destination_index.end())
        {
            const uint32_t existingIndex = existing->second;
            std::string difference = DescribeHostStructDifference(
                destination, existingIndex, source, static_cast<uint32_t>(sourceIndex));
            if (!difference.empty())
            {
                return HostTypeConflict{ .Existing = existingIndex,
                                         .Incoming = static_cast<uint32_t>(sourceIndex),
                                         .Difference = std::move(difference) };
            }

            out_remap.push_back(existingIndex);
            continue;
        }

        // the nested structs come first in the table, so their remapped indices exist already
        HostStruct copy = incoming;
        for (HostField& field : copy.Fields)
        {
            if (field.Kind == HostFieldKind::Struct)
            {
                field.StructIndex = out_remap[field.StructIndex];
            }
        }

        const uint32_t newIndex = static_cast<uint32_t>(destination.Structs.size());
        destination_index.emplace(std::pair{ copy.Name, copy.Rule }, newIndex);
        out_remap.push_back(newIndex);
        destination.Structs.push_back(std::move(copy));
    }

    return std::nullopt;
}

std::string_view ToString(LayoutRule rule) noexcept
{
    return magic_enum::enum_name(rule);
}

namespace
{

    std::string DescribeDimensions(const std::vector<HostArrayDimension>& dimensions)
    {
        if (dimensions.empty())
        {
            return "none";
        }

        std::string text;
        for (const HostArrayDimension& dimension : dimensions)
        {
            text += std::format("[{}] (stride {})", dimension.Count, dimension.Stride);
        }

        return text;
    }

} // namespace

} // namespace lodestone
