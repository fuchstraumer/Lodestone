#pragma once
#ifndef LODESTONE_TESTS_TEST_FORMAT_HPP
#define LODESTONE_TESTS_TEST_FORMAT_HPP
#include "ShaderManifestFormat.hpp"
#include "ShaderManifestIndex.hpp"

#include <magic_enum/magic_enum.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// FormatForTest turns any value a check compares into text for the failure message. It recurses through ranges,
// optionals, variants, tuples, and plain aggregates, so a vector of structs prints its contents.
namespace lodestone::tests
{

/** @brief Returns the best text we can build for a test value. */
template<typename ValueType>
std::string FormatForTest(const ValueType& value);
/** @brief Returns the code, table, record, and detail of a manifest error. */
std::string FormatForTest(const manifest::ErrorState& error);
/** @brief Returns the code, axis name, detail, and suggestion of a query error. */
std::string FormatForTest(const QueryError& error);

namespace detail
{

/** A range prints at most this many elements, so a failure on a SPIR-V blob stays readable. */
inline constexpr size_t k_MaxPrintedElements = 16u;
inline constexpr size_t k_MaxPrintedFields = 12u;

template<typename Type>
struct IsOptional : std::false_type
{
};

template<typename Type>
struct IsOptional<std::optional<Type>> : std::true_type
{
};

template<typename Type>
struct IsVariant : std::false_type
{
};

template<typename... Types>
struct IsVariant<std::variant<Types...>> : std::true_type
{
};

template<typename Type>
concept TupleLike = requires { std::tuple_size_v<Type>; };

/** Converts to any field type, so that an unevaluated aggregate initialization can count the fields. This is the
 * technique Boost.PFR uses. It never runs, so the conversion has no definition. */
struct AnyField
{
    template<typename FieldType>
    operator FieldType&() const noexcept;
};

template<typename Type, size_t... Indices>
consteval bool IsBraceConstructibleFrom(std::index_sequence<Indices...>)
{
    return requires { Type{ (static_cast<void>(Indices), AnyField{})... }; };
}

template<typename Type, size_t Count>
consteval size_t FieldCountAtMost()
{
    if constexpr (Count == 0u)
    {
        return 0u;
    }
    else if constexpr (IsBraceConstructibleFrom<Type>(std::make_index_sequence<Count>{}))
    {
        return Count;
    }
    else
    {
        return FieldCountAtMost<Type, Count - 1u>();
    }
}

// An aggregate with a C array member or a base class with fields counts wrong, and the structured binding below then
// fails to compile. Neither appears in a type the tests compare today.
template<typename Type>
concept PrintableAggregate = std::is_aggregate_v<Type> &&
                             !std::is_array_v<Type> &&
                             !IsBraceConstructibleFrom<Type>(std::make_index_sequence<k_MaxPrintedFields + 1u>{}) &&
                             (FieldCountAtMost<Type, k_MaxPrintedFields>() > 0u);

template<typename... FieldTypes>
std::string JoinFields(const FieldTypes&... fields)
{
    std::string text = "{";
    bool first = true;
    ((text += first ? "" : ", ", text += FormatForTest(fields), first = false), ...);
    return text + "}";
}

/** Prints the fields of an aggregate in declaration order. Without reflection, the names are not available. */
template<typename Type>
std::string FormatFields(const Type& value)
{
    constexpr size_t count = FieldCountAtMost<Type, k_MaxPrintedFields>();
    if constexpr (count == 1u)
    {
        const auto& [a] = value;
        return JoinFields(a);
    }
    else if constexpr (count == 2u)
    {
        const auto& [a, b] = value;
        return JoinFields(a, b);
    }
    else if constexpr (count == 3u)
    {
        const auto& [a, b, c] = value;
        return JoinFields(a, b, c);
    }
    else if constexpr (count == 4u)
    {
        const auto& [a, b, c, d] = value;
        return JoinFields(a, b, c, d);
    }
    else if constexpr (count == 5u)
    {
        const auto& [a, b, c, d, e] = value;
        return JoinFields(a, b, c, d, e);
    }
    else if constexpr (count == 6u)
    {
        const auto& [a, b, c, d, e, f] = value;
        return JoinFields(a, b, c, d, e, f);
    }
    else if constexpr (count == 7u)
    {
        const auto& [a, b, c, d, e, f, g] = value;
        return JoinFields(a, b, c, d, e, f, g);
    }
    else if constexpr (count == 8u)
    {
        const auto& [a, b, c, d, e, f, g, h] = value;
        return JoinFields(a, b, c, d, e, f, g, h);
    }
    else if constexpr (count == 9u)
    {
        const auto& [a, b, c, d, e, f, g, h, i] = value;
        return JoinFields(a, b, c, d, e, f, g, h, i);
    }
    else if constexpr (count == 10u)
    {
        const auto& [a, b, c, d, e, f, g, h, i, j] = value;
        return JoinFields(a, b, c, d, e, f, g, h, i, j);
    }
    else if constexpr (count == 11u)
    {
        const auto& [a, b, c, d, e, f, g, h, i, j, k] = value;
        return JoinFields(a, b, c, d, e, f, g, h, i, j, k);
    }
    else
    {
        static_assert(count == k_MaxPrintedFields, "extend FormatFields when k_MaxPrintedFields grows");
        const auto& [a, b, c, d, e, f, g, h, i, j, k, l] = value;
        return JoinFields(a, b, c, d, e, f, g, h, i, j, k, l);
    }
}

/** Prints `[a, b, ...]`, and stops after `k_MaxPrintedElements` with the count of the rest when the range is sized. */
template<typename RangeType>
std::string FormatRange(const RangeType& range)
{
    const std::vector<std::string> printed = range |
                                             std::views::take(k_MaxPrintedElements) |
                                             std::views::transform([](const auto& element) { return FormatForTest(element); }) |
                                             std::ranges::to<std::vector>();
    std::string text = std::format("[{}", printed | std::views::join_with(std::string_view{ ", " }) | std::ranges::to<std::string>());
    if constexpr (std::ranges::sized_range<const RangeType>)
    {
        const size_t total = static_cast<size_t>(std::ranges::size(range));
        if (total > printed.size())
        {
            text += std::format(", ... {} more", total - printed.size());
        }
    }
    return text + "]";
}

template<typename TupleType>
std::string FormatTuple(const TupleType& value)
{
    return std::apply([](const auto&... elements) { return JoinFields(elements...); }, value);
}

/** Prints the enumerator name, or the names of its bits for a flag set, or the value in hex. */
template<typename EnumType>
std::string_view EnumeratorName(EnumType value)
{
    // magic_enum asserts on an enum with no enumerator in its range, such as std::byte or VariantKey
    if constexpr (magic_enum::enum_count<EnumType>() > 0u)
    {
        return magic_enum::enum_name(value);
    }
    else
    {
        return {};
    }
}

template<typename EnumType>
std::string FormatEnum(EnumType value)
{
    const std::string_view name = EnumeratorName(value);
    if (!name.empty())
    {
        return std::string{ name };
    }

    using UnderlyingType = std::make_unsigned_t<std::underlying_type_t<EnumType>>;
    const UnderlyingType bits = static_cast<UnderlyingType>(std::to_underlying(value));
    std::string flags;
    for (int32_t bit = 0; bit < std::numeric_limits<UnderlyingType>::digits; ++bit)
    {
        const UnderlyingType mask = static_cast<UnderlyingType>(UnderlyingType{ 1 } << bit);
        if ((bits & mask) == 0u)
        {
            continue;
        }

        const std::string_view bitName = EnumeratorName(static_cast<EnumType>(mask));
        if (bitName.empty())
        {
            return std::format("0x{:X}", bits);
        }

        flags += flags.empty() ? std::string{ bitName } : std::format(" | {}", bitName);
    }
    
    return flags.empty() ? std::format("0x{:X}", bits) : flags;
}

} // namespace detail

template<typename ValueType>
std::string FormatForTest(const ValueType& value)
{
    if constexpr (std::same_as<ValueType, bool>)
    {
        return value ? "true" : "false";
    }
    else if constexpr (std::formattable<ValueType, char>)
    {
        return std::format("{}", value);
    }
    else if constexpr (std::is_pointer_v<ValueType>)
    {
        return value == nullptr ? std::string{ "nullptr" } : std::format("{}", static_cast<const void*>(value));
    }
    else if constexpr (std::is_enum_v<ValueType>)
    {
        if constexpr (requires { ToString(value); })
        {
            // the repository's ToString returns "Invalid" for a value it has no case for, such as a set of flags
            const std::string text{ ToString(value) };
            if (!text.empty() && (text != "Invalid" || std::to_underlying(value) == 0))
            {
                return text;
            }
        }
        return detail::FormatEnum(value);
    }
    else if constexpr (requires { ToString(value); })
    {
        return std::format("{}", ToString(value));
    }
    else if constexpr (std::same_as<ValueType, std::monostate>)
    {
        return "monostate";
    }
    else if constexpr (detail::IsOptional<ValueType>::value)
    {
        return value.has_value() ? FormatForTest(*value) : std::string{ "nullopt" };
    }
    else if constexpr (detail::IsVariant<ValueType>::value)
    {
        return std::visit([](const auto& alternative) { return FormatForTest(alternative); }, value);
    }
    else if constexpr (std::ranges::input_range<const ValueType>)
    {
        return detail::FormatRange(value);
    }
    else if constexpr (detail::TupleLike<ValueType>)
    {
        return detail::FormatTuple(value);
    }
    else if constexpr (detail::PrintableAggregate<ValueType>)
    {
        return detail::FormatFields(value);
    }
    else
    {
        return "<no formatter>";
    }
}

} // namespace lodestone::tests

#endif // !LODESTONE_TESTS_TEST_FORMAT_HPP
