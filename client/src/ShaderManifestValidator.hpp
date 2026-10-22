#pragma once
#ifndef LODESTONE_CLIENT_SHADER_MANIFEST_VALIDATOR_HPP
#define LODESTONE_CLIENT_SHADER_MANIFEST_VALIDATOR_HPP
#include "ShaderManifest.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace lodestone::manifest::detail
{

    struct EnvironmentContext
    {
        //NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
        const EnvironmentHeader& Environment;
        std::span<const std::byte> Extent;
        uint64_t StringCount{ 0u };
        uint64_t EntryPointCount{ 0u };
        uint32_t AxisMaskWordCount{ 0u };
        uint32_t ModuleAxisCount{ 0u };
        ShaderCodeFormat CodeFormat{ ShaderCodeFormat::None };
    };

    ErrorState ValidateBundle(const Header& parsed, std::span<const std::byte> bytes) noexcept;
    ErrorState OpenEnvironmentExtent(const BundleView& bundle,
                                     uint32_t profile_index,
                                     uint32_t module_index,
                                     std::span<const std::byte> extent_bytes) noexcept;
    ErrorState ValidateEnvironment(const EnvironmentContext& context) noexcept;

} // namespace lodestone::manifest::detail

#endif // !LODESTONE_CLIENT_SHADER_MANIFEST_VALIDATOR_HPP
