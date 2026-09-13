#pragma once
#ifndef LODESTONE_PERMUTATION_VALUE_HPP
#define LODESTONE_PERMUTATION_VALUE_HPP
#include <cstdint>
#include <string>
#include <string_view>

namespace lodestone
{

class PermutationAxis;

// We used to use std::variant, but we know that our permutation values have a fixed set of types, so we can
// represent them more efficiently than a variant. mostly, less templates and stdlib includes
struct PermutationValue
{
    enum class Type : uint8_t
    {
        Invalid = 0,
        Bool,
        UInt,
        Type // Interface type
    };

    constexpr PermutationValue() noexcept : type(Type::Invalid), uintValue(static_cast<uint32_t>(0)) {}
    constexpr explicit PermutationValue(bool value) noexcept : type(Type::Bool), boolValue(value) {}
    constexpr explicit PermutationValue(uint32_t value) noexcept : type(Type::UInt), uintValue(value) {}

    // Interface types store their values as the ordinal/index within the Axis' type list
    static PermutationValue MakeType(uint32_t ordinal) noexcept;

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] Type GetType() const noexcept;
    [[nodiscard]] bool AsBool() const noexcept;
    [[nodiscard]] uint32_t AsUInt() const noexcept;
    /** @brief Returns the type name of this type, as it is stored by the parent axis for this value */
    [[nodiscard]] std::string_view AsType(const PermutationAxis& axis) const noexcept;

    [[nodiscard]] bool operator==(const PermutationValue& other) const noexcept;
    [[nodiscard]] bool operator!=(const PermutationValue& other) const noexcept;
    [[nodiscard]] bool operator<(const PermutationValue& other) const noexcept;

private:
    Type type;
    //NOLINTBEGIN(readability-identifier-naming)
    union
    {
        uint32_t uintValue{ 0u };
        bool boolValue;
    };
    //NOLINTEND(readability-identifier-naming)
};

/** Widens any axis value to the integer type the size-expression evaluator works in. A `bool` axis
 * becomes 0 or 1, which is what a shader comparing it against a constant would see. */
int64_t PermutationValueToInt64(const PermutationValue& value) noexcept;
std::string ValueToPrintableString(const PermutationValue& value) noexcept;
// needs axis as of interface axis changes: have to get from ordinal to axis for type name
std::string ValueToSlangLiteral(const PermutationAxis& axis, const PermutationValue& value);
std::string ValueToSlangTypeName(const PermutationValue& value);
std::string MakeExportedConstantSource(const PermutationAxis& axis, const PermutationValue& value);
std::string MakeVariantModuleName(const PermutationAxis& axis, const PermutationValue& value);
std::string MakeVariantModulePath(const PermutationAxis& axis, const PermutationValue& value);

// Until I think of a better location, this is going here: It's most related to PermutationValues,
// and breaks an include loop that would be a real pain to break any other way
struct ExternConstantDefault
{
    std::string Name;
    int64_t Value{ 0 };
};

} // namespace lodestone

#endif // !LODESTONE_PERMUTATION_VALUE_HPP
