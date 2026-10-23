#include "target/ShimTable.hpp"
#include "target/TargetProfile.hpp"
#include <algorithm>
#include <array>
#include <span>
#include <string_view>

namespace lodestone
{

namespace
{
    // Pointer and Indexed cells stay empty until phase F step F4.1 adds their implementations
    constexpr std::array<ShimInterface, 5u> k_ShimInterfaces
    {
        ShimInterface
        { 
            .Name = "IReadBuffer",
            .Class = ShimResourceClass::Buffer,
            .TypeParameterCount = 1u,
            .Bound = "BoundReadBuffer",
            .Indexed = {},
            .Pointer = {},
            .ImplementationTakesFormat = false,
        },
        ShimInterface
        { 
            .Name = "IReadWriteBuffer",
            .Class = ShimResourceClass::Buffer,
            .TypeParameterCount = 1u,
            .Bound = "BoundReadWriteBuffer",
            .Indexed = {},
            .Pointer = {},
            .ImplementationTakesFormat = false,
        },
        ShimInterface
        { 
            .Name = "ITexture2DRef",
            .Class = ShimResourceClass::Texture,
            .TypeParameterCount = 0u,
            .Bound = "BoundTexture2D",
            .Indexed = {},
            .Pointer = {},
            .ImplementationTakesFormat = false,
        },
        ShimInterface
        { 
            .Name = "ISamplerRef",
            .Class = ShimResourceClass::Sampler,
            .TypeParameterCount = 0u,
            .Bound = "BoundSampler",
            .Indexed = {},
            .Pointer = {},
            .ImplementationTakesFormat = false,
        },
        ShimInterface
        { 
            .Name = "IWriteStorageTexture2D",
            .Class = ShimResourceClass::StorageTexture,
            .TypeParameterCount = 0u,
            .Bound = "BoundWriteStorageTexture2D",
            .Indexed = {},
            .Pointer = {},
            .ImplementationTakesFormat = true,
        },
    };
}

std::span<const ShimInterface> GetShimInterfaces() noexcept
{
    return k_ShimInterfaces;
}

const ShimInterface* FindShimInterface(std::string_view name) noexcept
{
    const auto found = std::ranges::find(k_ShimInterfaces, name, &ShimInterface::Name);
    return found != k_ShimInterfaces.end() ? &*found : nullptr;
}

std::string_view ShimImplName(const ShimInterface& shim_interface, AccessModel access) noexcept
{
    switch (access)
    {
    case AccessModel::Bound:
        return shim_interface.Bound;
    case AccessModel::Indexed:
        return shim_interface.Indexed;
    case AccessModel::Pointer:
        return shim_interface.Pointer;
    case AccessModel::Invalid:
        break;
    }
    return {};
}

std::string_view ToString(ShimResourceClass resource_class) noexcept
{
    switch (resource_class)
    {
    case ShimResourceClass::Buffer:
        return "buffer";
    case ShimResourceClass::Texture:
        return "texture";
    case ShimResourceClass::Sampler:
        return "sampler";
    case ShimResourceClass::StorageTexture:
        return "storage texture";
    case ShimResourceClass::Invalid:
        break;
    }
    return "invalid";
}

} // namespace lodestone
