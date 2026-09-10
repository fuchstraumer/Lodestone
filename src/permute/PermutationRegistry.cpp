#include "permute/PermutationRegistry.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationPolicy.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationValue.hpp"

#include <array>
#include <string_view>

// The compiled-in registry of every module that declares a permutation space.
//
// Phase E step E6 removes this file. An axis declaration moves onto the `extern static const` line in
// the shader, and a policy moves into a data file. Nothing else in `permute/` holds module data, so
// that step deletes one translation unit and adds a reader.

namespace lodestone
{

namespace
{

    // Axis order is the declaration order, and last parameter is now `RequireWhen` string instead
    // of an index and a value separately
    const PermutationSpace k_OceanFftSpace{ "OceanFft",
                                            { PermutationAxis{ "IFFT_SIZE",
                                                               { PermutationValue{ 128u },
                                                                 PermutationValue{ 256u },
                                                                 PermutationValue{ 512u },
                                                                 PermutationValue{ 1024u },
                                                                 PermutationValue{ 2048u },
                                                                 PermutationValue{ 4096u },
                                                                 PermutationValue{ 8192u } },
                                                               AxisKind::Tuning,
                                                               EarliestBindingTime::Cook,
                                                               AxisValueDomain::Integral },
                                              PermutationAxis{
                                                  "IFFT_USE_WAVE_OPS",
                                                  { PermutationValue{ false }, PermutationValue{ true } },
                                                  AxisKind::Capability,
                                                  EarliestBindingTime::Cook,
                                                  AxisValueDomain::Boolean },
                                              PermutationAxis{ "IFFT_WAVE_SIZE",
                                                               { PermutationValue{ 16u },
                                                                 PermutationValue{ 32u },
                                                                 PermutationValue{ 64u },
                                                                 PermutationValue{ 128u } },
                                                               AxisKind::Tuning,
                                                               EarliestBindingTime::Cook,
                                                               AxisValueDomain::Integral,
                                                               "IFFT_USE_WAVE_OPS == 1" } } };

    const PermutationSpace k_EmptySpace{ "", {} };


    const ModulePolicy k_EmptyPolicy{};

    struct ModuleSpaceEntry
    {
        std::string_view ModuleName;
        const PermutationSpace* Space;
        const ModulePolicy* Policy;
    };

    const std::array<ModuleSpaceEntry, 1> k_ModuleSpaces{ ModuleSpaceEntry{
        .ModuleName = "OceanFft", .Space = &k_OceanFftSpace, .Policy = &k_EmptyPolicy } };

} // namespace


const PermutationSpace* FindPermutationSpaceForModule(std::string_view module_name) noexcept
{
    for (const ModuleSpaceEntry& entry : k_ModuleSpaces)
    {
        if (entry.ModuleName == module_name)
        {
            return entry.Space;
        }
    }

    return &k_EmptySpace;
}

} // namespace lodestone
