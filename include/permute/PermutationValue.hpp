#pragma once
#ifndef LODESTONE_PERMUTATION_VALUE_HPP
#define LODESTONE_PERMUTATION_VALUE_HPP
#include <cstdint>
#include <string>
#include <string_view>

namespace lodestone
{

struct PermutationAxis;

// a tagged union rather than std::variant, for fewer templates and includes
// todo-ship: use a variant again, now that the types are more complex
struct PermutationValue
{
    enum class Type : uint8_t
    {
        Invalid = 0,
        Bool,
        UInt,
        Enum, // Handled like interface type
        Type // Interface type
    };

    constexpr PermutationValue() noexcept : type(Type::Invalid) {}
    constexpr explicit PermutationValue(bool value) noexcept : type(Type::Bool), uintValue(value ? 1u : 0u) {}
    constexpr explicit PermutationValue(uint32_t value) noexcept : type(Type::UInt), uintValue(value) {}

    // the ordinal in the axis's type list
    static PermutationValue MakeType(uint32_t ordinal) noexcept;
    static PermutationValue MakeEnum(uint32_t ordinal) noexcept;

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] Type GetType() const noexcept;
    [[nodiscard]] bool AsBool() const noexcept;
    [[nodiscard]] uint32_t AsUInt() const noexcept;
    [[nodiscard]] std::string_view AsType(const PermutationAxis& axis) const noexcept;
    [[nodiscard]] std::string_view AsEnumCase(const PermutationAxis& axis) const noexcept;
    [[nodiscard]] std::string AsQualifiedEnum(const PermutationAxis& axis) const noexcept;
    [[nodiscard]] uint32_t AsEnumLiteral(const PermutationAxis& axis) const noexcept;

    [[nodiscard]] bool operator==(const PermutationValue& other) const noexcept;
    [[nodiscard]] bool operator!=(const PermutationValue& other) const noexcept;
    [[nodiscard]] bool operator<(const PermutationValue& other) const noexcept;

private:
    Type type;
    // enum and type values: the ordinal in the axis's value list
    uint32_t uintValue{ 0u };
};

/** Widens a value for the expression evaluator. A `bool` becomes 0 or 1, as the shader sees it. */
int64_t PermutationValueToInt64(const PermutationValue& value) noexcept;
std::string ValueToPrintableString(const PermutationValue& value) noexcept;
std::string ValueToSlangLiteral(const PermutationAxis& axis, const PermutationValue& value);
std::string ValueToSlangTypeName(const PermutationValue& value);
std::string MakeExportedConstantSource(const PermutationAxis& axis, const PermutationValue& value);
std::string MakeVariantModuleName(const PermutationAxis& axis, const PermutationValue& value);
std::string MakeVariantModulePath(const PermutationAxis& axis, const PermutationValue& value);


} // namespace lodestone

#endif // !LODESTONE_PERMUTATION_VALUE_HPP
