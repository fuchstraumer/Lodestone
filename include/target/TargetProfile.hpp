#pragma once
#ifndef LODESTONE_TARGET_PROFILE_HPP
#define LODESTONE_TARGET_PROFILE_HPP
#include "CookerErrors.hpp"
#include "ShaderLibraryTypes.hpp"
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace lodestone
{

struct ReflectedBinding;
class DiagnosticSink;

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

/** @brief The language the compiler emits for a profile. Two profiles can share one language. */
enum class TargetLanguage : uint8_t
{
    Invalid = 0,
    Wgsl,
    Spirv,
};

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

    /**@brief This outer function exists to sort the input `used` bindings, before passing it to the virtual derived
     * validateEntryPoint method. This is just a shim that ensures the bindings are sorted before validation. */
    [[nodiscard]] CookResult<BindingComparison> ValidateEntryPoint(std::string_view target_text,
                                                                   std::span<const ReflectedBinding*> used,
                                                                   DiagnosticSink& sink) const;

protected:
    [[nodiscard]] virtual CookResult<BindingComparison> validateEntryPoint(std::string_view target_text,
                                                                           std::span<const ReflectedBinding*> used,
                                                                           DiagnosticSink& sink) const = 0;
};

struct TargetProfile
{
    /** @brief Friendly name for target, e.g, `wgsl` or `spirv` or `dxil` etc */
    std::string_view Name;
    TargetLanguage Language{ TargetLanguage::Invalid };
    /** @brief The Slang profile name, such as `spirv_1_5`. Empty sets no profile. */
    std::string_view SlangProfileName;
    AccessModel Access{ AccessModel::Invalid };
    /** @brief Null when this target cannot check its own output. This shoudln't happen,
     *  but will during the intermediate stages of us deploying new target backends */
    const ResolvedLibraryValidator* Validator{ nullptr };
};

/**@brief Finds the profile one `--target` name selects. Returns an unexpected value for a name the
 * cooker does not have.
 * This is a compiled in-table, since we have to define a fair bit of target-specific validation code
 * per new target. This will change in the future, similar to how Permutations became data-driven */
CookResult<TargetProfile> FindTargetProfile(std::string_view name) noexcept;

/** Every target name this build accepts, for the usage text and for an error message. */
std::span<const std::string_view> GetTargetProfileNames() noexcept;

PlacementKind PlacementKindFromAccessModel(AccessModel model) noexcept;

/** The code format the manifest records for a language. */
ShaderCodeFormat CodeFormatFromLanguage(TargetLanguage language) noexcept;

} // namespace lodestone

#endif // !LODESTONE_TARGET_PROFILE_HPP
