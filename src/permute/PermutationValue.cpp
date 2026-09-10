#include "permute/PermutationValue.hpp"
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace lodestone
{

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

int32_t PermutationValue::AsSInt() const noexcept
{
    return sintValue;
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
        return uintValue == other.uintValue;
    case Type::SInt:
        return sintValue == other.sintValue;
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
        return uintValue < other.uintValue;
    case Type::SInt:
        return sintValue < other.sintValue;
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
        return static_cast<int64_t>(value.AsUInt());
    case PermutationValue::Type::SInt:
        return static_cast<int64_t>(value.AsSInt());
    case PermutationValue::Type::Invalid:
        return -1;
    }
    return -1;
}

std::string ValueToSlangLiteral(const PermutationValue& value)
{
    switch (value.GetType())
    {
    case PermutationValue::Type::Bool:
        return value.AsBool() ? "true" : "false";
    case PermutationValue::Type::UInt:
        return std::to_string(value.AsUInt());
    case PermutationValue::Type::SInt:
        return std::to_string(value.AsSInt());
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
        return std::format("{}", value.AsUInt());
    case PermutationValue::Type::SInt:
        return std::format("{}", value.AsSInt());
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
    case PermutationValue::Type::SInt:
    case PermutationValue::Type::Invalid:
        return "int";
    }

    return "int";
}

std::string MakeExportedConstantSource(std::string_view axis_name, const PermutationValue& value)
{
    return std::format("export static const {} {} = {};\n",
                       ValueToSlangTypeName(value),
                       axis_name,
                       ValueToSlangLiteral(value));
}

std::string MakeVariantModuleName(std::string_view axis_name, const PermutationValue& value)
{
    return std::format("{}_{}", axis_name, ValueToSlangLiteral(value));
}

std::string MakeVariantModulePath(std::string_view axis_name, const PermutationValue& value)
{
    return std::format("{}_{}.slang", axis_name, ValueToSlangLiteral(value));
}

} // namespace lodestone
