#pragma once
#ifndef LODESTONE_CLIENT_SHADER_MANIFEST_DEVICE_HPP
#define LODESTONE_CLIENT_SHADER_MANIFEST_DEVICE_HPP
#include <cstdint>
#include <string_view>

/** What a renderer states about its device. Nothing here names a graphics API. */
namespace lodestone::manifest
{

/** @brief Returns whether the device supports one capability, in the names of the profile's target: SPIR-V
 * `OpCapability` and `OpExtension` names, or WGSL `enable` names. `user` is the pointer the caller passed along. */
using CapabilitySupportFn = bool (*)(void* user, std::string_view capability_name);

/** @brief The limits of one device. A limit left at 0 rejects every instance that has a size to compare. */
struct DeviceLimits
{
    uint32_t MaxWorkgroupMemoryBytes{ 0u };
    uint32_t MaxInvocationsPerWorkgroup{ 0u };
    uint32_t MaxWorkgroupSizeX{ 0u };
    uint32_t MaxWorkgroupSizeY{ 0u };
    uint32_t MaxWorkgroupSizeZ{ 0u };
    uint32_t MaxPushConstantBytes{ 0u };
};

enum class DeviceMismatch : uint8_t
{
    None = 0,
    MissingCapability, // Detail is the index in `EnvironmentView::CapabilityNames()`
    WorkgroupMemory, // Detail is the bytes the code needs
    Invocations, // Detail is the product of the workgroup size, clamped to 32 bits
    WorkgroupSize, // Detail is the size on the first side above its limit
    PushConstants, // Detail is the bytes of the push constant block
};

/** @brief The answer for one entry point instance: the first reason it does not fit, in the order of the enum. */
struct DeviceFit
{
    DeviceMismatch Reason{ DeviceMismatch::None };
    uint32_t Detail{ 0u };

    [[nodiscard]] bool Fits() const noexcept
    {
        return Reason == DeviceMismatch::None;
    }
};

} // namespace lodestone::manifest

#endif // !LODESTONE_CLIENT_SHADER_MANIFEST_DEVICE_HPP
