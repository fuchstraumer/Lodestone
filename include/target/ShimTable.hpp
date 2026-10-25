#pragma once
#ifndef LODESTONE_SHIM_TABLE_HPP
#define LODESTONE_SHIM_TABLE_HPP
#include "target/TargetProfile.hpp"
#include <cstdint>
#include <span>
#include <string_view>

namespace lodestone
{

enum class ShimResourceClass : uint8_t
{
    Invalid = 0,
    Buffer,
    Texture,
    Sampler,
    StorageTexture,
};

/** @brief One shim interface of `builtins/LodestoneShims.slang` and the type that implements it for each access model. An
 * empty name is a cell with no implementation yet. */
struct ShimInterface
{
    std::string_view Name;
    ShimResourceClass Class{ ShimResourceClass::Invalid };
    /** @brief The number of type arguments of the interface and of each implementation. */
    uint32_t TypeParameterCount{ 0u };
    std::string_view Bound;
    std::string_view Handle;
    std::string_view Pointer;
    /** @brief The implementations take the Slang type of a storage format (`RGBA16F`) as their only type argument, and the
     * interface has none. A format axis chooses it for each variant. */
    bool ImplementationTakesFormat{ false };
};

std::span<const ShimInterface> GetShimInterfaces() noexcept;

/** @brief Null when the name is not a shim interface. */
const ShimInterface* FindShimInterface(std::string_view name) noexcept;

/** @brief Empty when the cell has no implementation. */
std::string_view ShimImplName(const ShimInterface& shim_interface, AccessModel access) noexcept;

/** @brief The form that `forms` gives to `resource_class`. */
AccessModel ShimFormOf(const ShimForms& forms, ShimResourceClass resource_class) noexcept;

std::string_view ToString(ShimResourceClass resource_class) noexcept;

} // namespace lodestone

#endif // !LODESTONE_SHIM_TABLE_HPP
