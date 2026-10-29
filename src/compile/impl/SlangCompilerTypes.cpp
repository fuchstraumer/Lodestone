#include "SlangCompilerTypes.hpp"
#include "Diagnostics.hpp"
#include "compile/SlangDiagnosticParser.hpp"
#include "model/BufferLayout.hpp"
#include "slang.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace lodestone
{

namespace
{

    // the byte size of one component of a scalar, vector, or matrix. A bool takes a word in a buffer
    uint32_t ScalarByteSize(slang::TypeReflection::ScalarType scalar_type) noexcept;
    bool BuildStructNode(slang::TypeLayoutReflection* type_layout, LayoutNode& out_node, std::string& out_unsupported);

} // namespace

slang::CompilerOptionEntry ToOptionEntry(const CompilerOptionRow& row) noexcept
{
    slang::CompilerOptionEntry entry{};
    entry.name = row.Name;
    entry.value.kind = row.Kind;
    entry.value.intValue0 = row.IntValue;
    entry.value.stringValue0 = row.StringValue;
    return entry;
}

std::vector<slang::CompilerOptionEntry> MakeCompilerOptions(uint32_t optimization_level, uint32_t debug_level)
{
    std::vector<slang::CompilerOptionEntry> options = k_CompilerOptionRows |
                                                      std::views::transform(ToOptionEntry) |
                                                      std::ranges::to<std::vector>();
    options.append_range(MakeFlagOptions(optimization_level, debug_level));
    return options;
}

std::array<slang::CompilerOptionEntry, 2u> MakeFlagOptions(uint32_t optimization_level, uint32_t debug_level) noexcept
{
    const CompilerOptionRow optimizationOption
    {
        .Name = slang::CompilerOptionName::Optimization,
        .Kind = slang::CompilerOptionValueKind::Int,
        .IntValue = static_cast<int32_t>(ToSlangOptimizationLevel(optimization_level))
    };

    const CompilerOptionRow debugOption
    {
        .Name = slang::CompilerOptionName::DebugInformation,
        .Kind = slang::CompilerOptionValueKind::Int,
        .IntValue = static_cast<int32_t>(debug_level)
    };

    return { ToOptionEntry(optimizationOption), ToOptionEntry(debugOption) };
}

std::vector<std::byte> BlobToBytes(slang::IBlob* blob)
{
    if (blob == nullptr || blob->getBufferSize() == 0u)
    {
        return {};
    }

    const size_t size = blob->getBufferSize();
    const std::byte* ptr = static_cast<const std::byte*>(blob->getBufferPointer());
    return std::vector<std::byte>{ ptr, ptr + size };
}

std::string BlobToString(slang::IBlob* blob)
{
    // Slang leaves the blob null when it has nothing to say
    if (blob == nullptr)
    {
        return {};
    }

    return std::string{ static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize() };
}

void ReportDiagnostics(DiagnosticSink& sink, std::string_view context, slang::IBlob* blob)
{
    if (blob == nullptr || blob->getBufferSize() == 0u)
    {
        return;
    }

    ParseSlangDiagnostics(BlobToString(blob), context, sink);
}

// NOLINTBEGIN(misc-no-recursion)
bool BuildLayoutNode(slang::TypeLayoutReflection* type_layout, LayoutNode& out_node, std::string& out_unsupported)
{
    if (type_layout == nullptr)
    {
        out_unsupported = "<unknown>";

        return false;
    }

    slang::TypeReflection* type = type_layout->getType();
    switch (type_layout->getKind())
    {
    case slang::TypeReflection::Kind::Scalar:
        out_node.Kind = LayoutNodeKind::Scalar;
        out_node.ScalarSize = ScalarByteSize(type->getScalarType());

        return true;
    case slang::TypeReflection::Kind::Vector:
        out_node.Kind = LayoutNodeKind::Vector;
        out_node.ScalarSize = ScalarByteSize(type->getScalarType());
        out_node.Components = static_cast<uint32_t>(type->getElementCount());

        return true;
    case slang::TypeReflection::Kind::Matrix:
        out_node.Kind = LayoutNodeKind::Matrix;
        out_node.ScalarSize = ScalarByteSize(type->getScalarType());
        out_node.Rows = type->getRowCount();
        out_node.Columns = type->getColumnCount();
        out_node.RowMajor = type_layout->getMatrixLayoutMode() == SLANG_MATRIX_LAYOUT_ROW_MAJOR;

        return true;
    case slang::TypeReflection::Kind::Array:
    {
        const size_t count = type->getElementCount();
        if (count == 0u || count >= static_cast<size_t>(SLANG_UNKNOWN_SIZE))
        {
            out_unsupported = "an array with no fixed length";

            return false;
        }

        out_node.Kind = LayoutNodeKind::Array;
        out_node.ElementCount = static_cast<uint32_t>(count);
        out_node.Children.emplace_back();

        return BuildLayoutNode(type_layout->getElementTypeLayout(), out_node.Children.front(), out_unsupported);
    }
    case slang::TypeReflection::Kind::Struct:
        return BuildStructNode(type_layout, out_node, out_unsupported);
    default:
        break;
    }

    const char* name = type != nullptr ? type->getName() : nullptr;
    out_unsupported = name != nullptr ? name : "<unnamed>";

    return false;
}
// NOLINTEND(misc-no-recursion)

namespace
{

    uint32_t ScalarByteSize(slang::TypeReflection::ScalarType scalar_type) noexcept
    {
        switch (scalar_type)
        {
        case slang::TypeReflection::ScalarType::Int8:
        case slang::TypeReflection::ScalarType::UInt8:
            return 1u;
        case slang::TypeReflection::ScalarType::Int16:
        case slang::TypeReflection::ScalarType::UInt16:
        case slang::TypeReflection::ScalarType::Float16:
            return 2u;
        case slang::TypeReflection::ScalarType::Int64:
        case slang::TypeReflection::ScalarType::UInt64:
        case slang::TypeReflection::ScalarType::Float64:
            return 8u;
        default:
            return 4u;
        }
    }

    bool BuildStructNode(slang::TypeLayoutReflection* type_layout, LayoutNode& out_node, std::string& out_unsupported)
    {
        const char* typeName = type_layout->getName();
        out_node.Kind = LayoutNodeKind::Struct;
        out_node.TypeName = typeName != nullptr ? typeName : std::string{};

        const unsigned int fieldCount = type_layout->getFieldCount();
        out_node.Children.resize(fieldCount);
        for (unsigned int i = 0u; i < fieldCount; ++i)
        {
            slang::VariableLayoutReflection* field = type_layout->getFieldByIndex(i);
            const char* fieldName = field->getName();
            out_node.Children[i].Name = fieldName != nullptr ? fieldName : std::string{};

            if (!BuildLayoutNode(field->getTypeLayout(), out_node.Children[i], out_unsupported))
            {
                out_unsupported = std::format("{} (member '{}')", out_unsupported, out_node.Children[i].Name);

                return false;
            }
        }

        return true;
    }

} // namespace

} // namespace lodestone
