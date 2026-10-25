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
    // a buffer device address addresses buffers only, so textures have no Pointer form, and buffers use no Handle
    constexpr std::array<ShimInterface, 5u> k_ShimInterfaces
    {
        ShimInterface
        { 
            .Name = "IReadBuffer",
            .Class = ShimResourceClass::Buffer,
            .TypeParameterCount = 1u,
            .Bound = "BoundReadBuffer",
            .Handle = {},
            .Pointer = "PointerReadBuffer",
            .ImplementationTakesFormat = false,
            .Resource = ShimResource{ .Kind = BindingKind::StorageBuffer, .Shape = ResourceShape::StructuredBuffer, .Access = ResourceAccess::ReadOnly },
        },
        ShimInterface
        { 
            .Name = "IReadWriteBuffer",
            .Class = ShimResourceClass::Buffer,
            .TypeParameterCount = 1u,
            .Bound = "BoundReadWriteBuffer",
            .Handle = {},
            .Pointer = "PointerReadWriteBuffer",
            .ImplementationTakesFormat = false,
            .Resource = ShimResource{ .Kind = BindingKind::StorageBuffer, .Shape = ResourceShape::StructuredBuffer, .Access = ResourceAccess::ReadWrite },
        },
        ShimInterface
        { 
            .Name = "ITexture2DRef",
            .Class = ShimResourceClass::Texture,
            .TypeParameterCount = 0u,
            .Bound = "BoundTexture2D",
            .Handle = "HandleTexture2D",
            .Pointer = {},
            .ImplementationTakesFormat = false,
            .Resource = ShimResource{ .Kind = BindingKind::Texture, .Shape = ResourceShape::Texture2D, .SampleType = TextureSampleType::Float, .Access = ResourceAccess::ReadOnly },
        },
        ShimInterface
        { 
            .Name = "ISamplerRef",
            .Class = ShimResourceClass::Sampler,
            .TypeParameterCount = 0u,
            .Bound = "BoundSampler",
            .Handle = "HandleSampler",
            .Pointer = {},
            .ImplementationTakesFormat = false,
            .Resource = ShimResource{ .Kind = BindingKind::Sampler },
        },
        ShimInterface
        { 
            .Name = "IWriteStorageTexture2D",
            .Class = ShimResourceClass::StorageTexture,
            .TypeParameterCount = 0u,
            .Bound = "BoundWriteStorageTexture2D",
            .Handle = "HandleWriteStorageTexture2D",
            .Pointer = {},
            .ImplementationTakesFormat = true,
            .Resource = ShimResource{ .Kind = BindingKind::StorageTexture, .Shape = ResourceShape::Texture2D, .Access = ResourceAccess::WriteOnly },
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
    case AccessModel::Handle:
        return shim_interface.Handle;
    case AccessModel::Pointer:
        return shim_interface.Pointer;
    case AccessModel::Invalid:
        break;
    }
    return {};
}

const ShimInterface* FindShimInterfaceByImplementation(std::string_view implementation, AccessModel& out_access) noexcept
{
    for (const ShimInterface& row : k_ShimInterfaces)
    {
        for (const AccessModel access : { AccessModel::Handle, AccessModel::Pointer })
        {
            if (!ShimImplName(row, access).empty() && ShimImplName(row, access) == implementation)
            {
                out_access = access;
                return &row;
            }
        }
    }
    return nullptr;
}

AccessModel ShimFormOf(const ShimForms& forms, ShimResourceClass resource_class) noexcept
{
    switch (resource_class)
    {
    case ShimResourceClass::Buffer:
        return forms.Buffer;
    case ShimResourceClass::Texture:
        return forms.Texture;
    case ShimResourceClass::Sampler:
        return forms.Sampler;
    case ShimResourceClass::StorageTexture:
        return forms.StorageTexture;
    case ShimResourceClass::Invalid:
        break;
    }
    return AccessModel::Invalid;
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
