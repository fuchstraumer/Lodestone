#pragma once
#ifndef LODESTONE_CLIENT_SHADER_MANIFEST_VALIDATOR_HPP
#define LODESTONE_CLIENT_SHADER_MANIFEST_VALIDATOR_HPP
#include "ShaderManifest.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

/** Private to the client library: the checks that `BundleView::Open` and `EnvironmentView::Open` run once, so
 * that every accessor after them runs unchecked. Clients never see this header. */
namespace lodestone::manifest::detail
{

    /** The facts one extent's validators need from the bundle, with the extent itself. */
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

    /** Checks the header, then every whole-cook table, in an order where each check may read what the checks
     * before it proved in bounds. */
    ErrorState ValidateBundle(const Header& parsed, std::span<const std::byte> bytes) noexcept;
    /** Checks the caller's indices and extent bytes against the bundle directory, before the extent is read. */
    ErrorState OpenEnvironmentExtent(const BundleView& bundle,
                                     uint32_t profile_index,
                                     uint32_t module_index,
                                     std::span<const std::byte> extent_bytes) noexcept;
    /** Checks every table of one extent, and every cross-reference between them. */
    ErrorState ValidateEnvironment(const EnvironmentContext& context) noexcept;

} // namespace lodestone::manifest::detail

#endif // !LODESTONE_CLIENT_SHADER_MANIFEST_VALIDATOR_HPP
