#pragma once
#ifndef LODESTONE_PERMUTATION_TYPES_HPP
#define LODESTONE_PERMUTATION_TYPES_HPP
#include <cstdint>
#include <string>
#include <vector>

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
    // the modules that the link line imports. a tagged type lists its declaring module, a policy type lists its own
    std::vector<std::string> Imports;
    bool FromPolicy{ false };
    // where the policy entry is written, for errors. empty and 0 when the type is tagged
    std::string PolicyFile;
    uint32_t PolicyLine{ 0u };
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

struct RawEnumCase
{
    std::string Name; // name of *this* enum value
    int64_t Value{ 0 }; // kept as int64_t for use with size expressions

    constexpr bool operator==(const RawEnumCase& other) const noexcept
    {
        return Name == other.Name && Value == other.Value;
    }

    constexpr bool operator<(const RawEnumCase& other) const noexcept
    {
        if (Name < other.Name)
        {
            return true;
        }
        else
        {
            return Value < other.Value;
        }
    }
};

}

#endif // !LODESTONE_PERMUTATION_TYPES_HPP
