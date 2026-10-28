#pragma once
#ifndef LODESTONE_UTILITY_ENUM_CLASS_UTILS_HPP
#define LODESTONE_UTILITY_ENUM_CLASS_UTILS_HPP
#include <type_traits>
#include <utility>

/** @brief Bitwise operators for enum class flags, and safe boolean tests on the results. */

template<typename T>
    requires(std::is_enum_v<T> and requires(T e) { enable_bitmask_operator_or(e); })
constexpr auto operator|(const T lhs, const T rhs) noexcept
{
    return static_cast<T>(std::to_underlying(lhs) | std::to_underlying(rhs));
}

template<typename T>
    requires(std::is_enum_v<T> and requires(T e) { enable_bitmask_operator_and(e); })
constexpr auto operator&(const T lhs, const T rhs) noexcept
{
    return static_cast<T>(std::to_underlying(lhs) & std::to_underlying(rhs));
}

template<typename T>
    requires(std::is_enum_v<T> and requires(T e) { enable_bitmask_operator_xor(e); })
constexpr auto operator^(const T lhs, const T rhs) noexcept
{
    return static_cast<T>(std::to_underlying(lhs) ^ std::to_underlying(rhs));
}

template<typename T>
    requires(std::is_enum_v<T> and requires(T e) { enable_bitmask_operator_or_eq(e); })
constexpr T& operator|=(T& lhs, const T rhs) noexcept
{
    lhs = static_cast<T>(std::to_underlying(lhs) | std::to_underlying(rhs));
    return lhs;
}

template<typename T>
    requires(std::is_enum_v<T> and requires(T e) { enable_bitmask_operator_not(e); })
constexpr auto operator~(const T value) noexcept
{
    return static_cast<T>(~std::to_underlying(value));
}

// usage &= ~Flags::MapWrite
template<typename T>
    requires(std::is_enum_v<T> and requires(T e) { enable_bitmask_operator_and(e); })
constexpr T& operator&=(T& lhs, const T rhs) noexcept
{
    lhs = static_cast<T>(std::to_underlying(lhs) & std::to_underlying(rhs));
    return lhs;
}

template<typename T>
    requires(std::is_enum_v<T> and requires(T e) { enable_bitmask_operator_xor(e); })
constexpr T& operator^=(T& lhs, const T rhs) noexcept
{
    lhs = static_cast<T>(std::to_underlying(lhs) ^ std::to_underlying(rhs));
    return lhs;
}

/** @brief Use this, not `value & flag`, in a condition: the operators return the enum type. */
template<typename T>
    requires std::is_enum_v<T>
constexpr bool HasAllFlags(T value, T flags) noexcept
{
    return (std::to_underlying(value) & std::to_underlying(flags)) == std::to_underlying(flags);
}

/** @brief Differs from HasAllFlags only for multi-bit flags, where a wrong guess is silent. */
template<typename T>
    requires std::is_enum_v<T>
constexpr bool HasAnyFlag(T value, T flags) noexcept
{
    return (std::to_underlying(value) & std::to_underlying(flags)) != 0;
}

#ifndef LODESTONE_MAKE_ENUM_FLAGS
#define LODESTONE_MAKE_ENUM_FLAGS(EnumClass) \
    [[maybe_unused]] consteval bool enable_bitmask_operator_or(EnumClass) { return true; } \
    [[maybe_unused]] consteval bool enable_bitmask_operator_and(EnumClass) { return true; } \
    [[maybe_unused]] consteval bool enable_bitmask_operator_xor(EnumClass) { return true; } \
    [[maybe_unused]] consteval bool enable_bitmask_operator_or_eq(EnumClass) { return true; } \
    [[maybe_unused]] consteval bool enable_bitmask_operator_not(EnumClass) { return true; }
#endif

#endif // !LODESTONE_UTILITY_ENUM_CLASS_UTILS_HPP
