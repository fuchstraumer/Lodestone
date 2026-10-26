#include "permute/PermutationValue.hpp"
#include "permute/PermutationAxis.hpp"
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace lodestone
{

namespace
{
    // `::` is not legal in module names, so enum values join with `_`
    std::string ValueNameToken(const PermutationAxis& axis, const PermutationValue& value);
}

PermutationValue PermutationValue::MakeEnum(uint32_t ordinal) noexcept
{
    PermutationValue value{};
    value.type = Type::Enum;
    value.uintValue = ordinal;
    return value;
}

PermutationValue PermutationValue::MakeType(uint32_t ordinal) noexcept
{
    PermutationValue value{};
    value.type = Type::Type;
    value.uintValue = ordinal;
    return value;
}

bool PermutationValue::IsValid() const noexcept
{
    return type != Type::Invalid;
}

PermutationValue::Type PermutationValue::GetType() const noexcept
{
    return type;
}

// NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)
bool PermutationValue::AsBool() const noexcept
{
    return static_cast<bool>(uintValue);
}

uint32_t PermutationValue::AsUInt() const noexcept
{
    return uintValue;
}

std::string_view PermutationValue::AsType(const PermutationAxis& axis) const noexcept
{
    return axis.InterfaceImplTypeName(uintValue);
}

std::string_view PermutationValue::AsEnumCase(const PermutationAxis& axis) const noexcept
{
    return axis.EnumCaseName(uintValue);
}

std::string PermutationValue::AsQualifiedEnum(const PermutationAxis& axis) const noexcept
{
    return std::format("{}::{}", axis.EnumTypeName(), axis.EnumCaseName(uintValue));
}

bool PermutationValue::operator==(const PermutationValue& other) const noexcept
{
    if (type != other.type)
    {
        return false;
    }

    return uintValue == other.uintValue;
}

bool PermutationValue::operator!=(const PermutationValue& other) const noexcept
{
    return !(*this == other);
}

bool PermutationValue::operator<(const PermutationValue& other) const noexcept
{
    if (type != other.type)
    {
        return type < other.type;
    }

    return uintValue < other.uintValue;
}
// NOLINTEND(cppcoreguidelines-pro-type-union-access)

int64_t PermutationValueToInt64(const PermutationValue& value) noexcept
{
    return static_cast<int64_t>(value.AsUInt());
}

std::string ValueToSlangLiteral(const PermutationAxis& axis, const PermutationValue& value)
{
    switch (value.GetType())
    {
    case PermutationValue::Type::Bool:
        return value.AsBool() ? "true" : "false";
    case PermutationValue::Type::UInt:
        return std::to_string(value.AsUInt());
    case PermutationValue::Type::Enum:
        return std::string{ value.AsQualifiedEnum(axis) };
    case PermutationValue::Type::Type:
        return std::string{ value.AsType(axis) };
    case PermutationValue::Type::Invalid:
        return "invalid";
    }
    return "invalid";
}

std::string ValueToPrintableString(const PermutationValue& value) noexcept
{
    switch (value.GetType())
    {
    case PermutationValue::Type::Bool:
        return value.AsBool() ? "true" : "false";
    case PermutationValue::Type::UInt:
        [[fallthrough]];
    case PermutationValue::Type::Enum:
        [[fallthrough]];
    case PermutationValue::Type::Type:
        return std::format("{}", value.AsUInt());
    case PermutationValue::Type::Invalid:
        return "invalid";
    }
    return "invalid";
}

std::string ValueToSlangTypeName(const PermutationValue& value)
{
    switch (value.GetType())
    {
    case PermutationValue::Type::Bool:
        return "bool";
    case PermutationValue::Type::UInt:
        return "uint";
    case PermutationValue::Type::Enum:
        return "enum";
    case PermutationValue::Type::Type:
        return "type";
    default:
        std::unreachable();
    }
}

std::string MakeExportedConstantSource(const PermutationAxis& axis, const PermutationValue& value)
{
    if (value.GetType() == PermutationValue::Type::Type)
    {
        // the synthetic module is its own TU, so it imports the declaring module
        const RawInterfaceImpl& impl = axis.InterfaceImpl(value.AsUInt());
        return std::format("import {};\nexport struct {} : {} = {};\n",
                           impl.Module,
                           axis.Name,
                           axis.InterfaceName(),
                           impl.TypeName);
    }
    else if (value.GetType() == PermutationValue::Type::Enum)
    {
        return std::format("import {};\nexport static const {} {} = {};\n",
                           axis.Module(),
                           axis.EnumTypeName(),
                           axis.Name,
                           value.AsQualifiedEnum(axis));
    }
    else
    {
        return std::format("export static const {} {} = {};\n",
                           ValueToSlangTypeName(value),
                           axis.Name,
                           ValueToSlangLiteral(axis, value));
    }
}

std::string MakeVariantModuleName(const PermutationAxis& axis, const PermutationValue& value)
{
    return std::format("{}_{}", axis.Name, ValueNameToken(axis, value));
}

std::string MakeVariantModulePath(const PermutationAxis& axis, const PermutationValue& value)
{
    return std::format("{}_{}.slang", axis.Name, ValueNameToken(axis, value));
}

namespace
{
    std::string ValueNameToken(const PermutationAxis& axis, const PermutationValue& value)
    {
        if (value.GetType() == PermutationValue::Type::Enum)
        {
            return std::format("{}_{}", axis.EnumTypeName(), value.AsEnumCase(axis));
        }
        return ValueToSlangLiteral(axis, value);
    }
}

} // namespace lodestone
