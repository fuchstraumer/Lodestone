#include "permute/PermutationValue.hpp"
#include "permute/PermutationAxis.hpp"
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace lodestone
{

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
    return boolValue;
}

uint32_t PermutationValue::AsUInt() const noexcept
{
    return uintValue;
}

std::string_view PermutationValue::AsType(const PermutationAxis& axis) const noexcept
{
    return axis.InterfaceAxisName(uintValue);
}

bool PermutationValue::operator==(const PermutationValue& other) const noexcept
{
    if (type != other.type)
    {
        return false;
    }

    switch (type)
    {
    case Type::Bool:
        return boolValue == other.boolValue;
    case Type::UInt:
        [[fallthrough]];
    case Type::Type:
        return uintValue == other.uintValue;
    case Type::Invalid:
        return true;
    }

    return false;
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

    switch (type)
    {
    case Type::Bool:
        return static_cast<int>(boolValue) < static_cast<int>(other.boolValue);
    case Type::UInt:
        [[fallthrough]];
    case Type::Type:
        return uintValue < other.uintValue;
    case Type::Invalid:
        return false;
    }

    return false;
}
// NOLINTEND(cppcoreguidelines-pro-type-union-access)

int64_t PermutationValueToInt64(const PermutationValue& value) noexcept
{
    switch (value.GetType())
    {
    case PermutationValue::Type::Bool:
        return value.AsBool() ? 1 : 0;
    case PermutationValue::Type::UInt:
        [[fallthrough]];
    case PermutationValue::Type::Type:
        return static_cast<int64_t>(value.AsUInt());
    case PermutationValue::Type::Invalid:
        return -1;
    }
    return -1;
}

std::string ValueToSlangLiteral(const PermutationAxis& axis, const PermutationValue& value)
{
    switch (value.GetType())
    {
    case PermutationValue::Type::Bool:
        return value.AsBool() ? "true" : "false";
    case PermutationValue::Type::UInt:
        return std::to_string(value.AsUInt());
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
        return std::format("export struct {} : {} = {};\n",
                           axis.Name,
                           axis.InterfaceName(),
                           ValueToSlangLiteral(axis, value));
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
    return std::format("{}_{}", axis.Name, ValueToSlangLiteral(axis, value));
}

std::string MakeVariantModulePath(const PermutationAxis& axis, const PermutationValue& value)
{
    return std::format("{}_{}.slang", axis.Name, ValueToSlangLiteral(axis, value));
}

} // namespace lodestone
