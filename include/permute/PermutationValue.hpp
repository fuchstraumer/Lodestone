#pragma once
#ifndef LODESTONE_PERMUTATION_VALUE_HPP
#define LODESTONE_PERMUTATION_VALUE_HPP
#include <cstdint>
#include <string>
#include <string_view>

namespace lodestone
{

// We used to use std::variant, but we know that our permutation values have a fixed set of types, so we can
// represent them more efficiently than a variant. mostly, less templates and stdlib includes
struct PermutationValue
{
    enum class Type : uint8_t
    {
        Invalid = 0,
        Bool,
        UInt
    };

    constexpr PermutationValue() noexcept : type(Type::Invalid), uintValue(static_cast<uint32_t>(0)) {}
    constexpr explicit PermutationValue(bool value) noexcept : type(Type::Bool), boolValue(value) {}
    constexpr explicit PermutationValue(uint32_t value) noexcept : type(Type::UInt), uintValue(value) {}

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] Type GetType() const noexcept;
    [[nodiscard]] bool AsBool() const noexcept;
    [[nodiscard]] uint32_t AsUInt() const noexcept;

    [[nodiscard]] bool operator==(const PermutationValue& other) const noexcept;
    [[nodiscard]] bool operator!=(const PermutationValue& other) const noexcept;
    [[nodiscard]] bool operator<(const PermutationValue& other) const noexcept;

private:
    Type type;
    //NOLINTBEGIN(readability-identifier-naming)
    union
    {
        uint32_t uintValue;
        bool boolValue;
    };
    //NOLINTEND(readability-identifier-naming)
};

/** Widens any axis value to the integer type the size-expression evaluator works in. A `bool` axis
 * becomes 0 or 1, which is what a shader comparing it against a constant would see. */
int64_t PermutationValueToInt64(const PermutationValue& value) noexcept;
std::string ValueToPrintableString(const PermutationValue& value) noexcept;
std::string ValueToSlangLiteral(const PermutationValue& value);
std::string ValueToSlangTypeName(const PermutationValue& value);
std::string MakeExportedConstantSource(std::string_view axis_name, const PermutationValue& value);
std::string MakeVariantModuleName(std::string_view axis_name, const PermutationValue& value);
std::string MakeVariantModulePath(std::string_view axis_name, const PermutationValue& value);

}

#endif // !LODESTONE_PERMUTATION_VALUE_HPP
