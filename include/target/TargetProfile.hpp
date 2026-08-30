#pragma once
#include "ShaderLibraryTypes.hpp"
#ifndef LODESTONE_TARGET_PROFILE_HPP
#define LODESTONE_TARGET_PROFILE_HPP
#include "model/ShaderDataSchema.hpp"
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

/** What one output target is, and what it can check about itself.
 *
 * This also selects the "AccessModel" for bound shader resources: a target like WGSL requires
 * bound access modeling. Cutting edge Vulkan can use pointers. DX12 and minspec Vulkan (at this point)
 * can use bindless (indexed). */
namespace lodestone
{

// todo-ship: unify this with "PlacementKind" in ShaderLibraryTypes.hpp eventually
/**@brief: How a shader reaches a resource. */
enum class AccessModel : uint8_t
{
    Invalid = 0,
    /** Classic model: binding group, index within said bind group */
    Bound,
    /** Bindless model: index into a heap of resources. May be heterogenuous */
    Indexed,
    /** Buffer device address model. Currently only applicable to buffers; Textures still use indexed.*/
    Pointer,
};

std::string_view ToString(AccessModel model) noexcept;

/** @brief The answer a validator gives about one entry point. `Matches` being `true` means the bindings and
 * specializations match the expected value. If `Matches` is `false`, `Report` specifies how/where it is
 * false.
 */
struct BindingComparison
{
    bool Matches{ false };
    std::string Report;
};

/**@brief A valuble second opinon about what one entry point really declared. The emitted artifact
 * (per target, and per variance parameters) decides where things go. Reflection decides sizes and types.
 * We pass in the used bindings reflection detected, to see how they compare: ideally, they should match
 * on the axes that they both share in their data. */
class ResolvedLibraryValidator
{
public:
    ResolvedLibraryValidator() = default;
    virtual ~ResolvedLibraryValidator() = default;
    ResolvedLibraryValidator(const ResolvedLibraryValidator&) = delete;
    ResolvedLibraryValidator& operator=(const ResolvedLibraryValidator&) = delete;
    ResolvedLibraryValidator(ResolvedLibraryValidator&&) = delete;
    ResolvedLibraryValidator& operator=(ResolvedLibraryValidator&&) = delete;

    /** `target_text` is what the backend emitted for one entry point. `used` is the subset of the
     * variant's bindings that this entry point references, which is what reflection claims. */
    [[nodiscard]] virtual BindingComparison ValidateEntryPoint(
        std::string_view target_text, std::span<const ReflectedBinding*> used) const = 0;
};

struct TargetProfile
{
    /** @brief Friendly name for target, e.g, `wgsl` or `spirv` or `dxil` etc */
    std::string_view Name;
    AccessModel Access{ AccessModel::Invalid };
    /** @brief Null when this target cannot check its own output. This shoudln't happen,
     *  but will during the intermediate stages of us deploying new target backends */
    const ResolvedLibraryValidator* Validator{ nullptr };
};

/**@brief Finds the profile one `--target` name selects. Null for a name the cooker does not have.
 * This is a compiled in-table, since we have to define a fair bit of target-specific validation code
 * per new target. */
const TargetProfile* FindTargetProfile(std::string_view name) noexcept;

/** Every target name this build accepts, for the usage text and for an error message. */
std::span<const std::string_view> GetTargetProfileNames() noexcept;

PlacementKind PlacementKindFromAccessModel(AccessModel model) noexcept;

} // namespace lodestone

#endif // !LODESTONE_TARGET_PROFILE_HPP
