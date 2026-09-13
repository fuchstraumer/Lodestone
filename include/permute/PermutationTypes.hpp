#pragma once
#ifndef LODESTONE_PERMUTATION_TYPES_HPP
#define LODESTONE_PERMUTATION_TYPES_HPP
#include <cstdint>
#include <string>

// File for all POD permutation types that don't need anything beyond standard includes.
// Helps keep the include graph cleaner, especially for cross-module dependencies.
namespace lodestone
{

struct ExternConstantDefault
{
    std::string Name;
    int64_t Value{ 0 };
};

struct RawInterfaceImpl
{
    std::string Module;
    std::string TypeName; // namespace-qualified, not module-qualified
    constexpr bool operator==(const RawInterfaceImpl& other) const noexcept
    {
        return Module == other.Module && TypeName == other.TypeName;
    }

    constexpr bool operator<(const RawInterfaceImpl& other) const noexcept
    {
        if (Module < other.Module)
        {
            return true;
        }
        else
        {
            return TypeName < other.TypeName;
        }
    }
};

}

#endif // !LODESTONE_PERMUTATION_TYPES_HPP
