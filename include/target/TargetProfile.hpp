#pragma once
#ifndef LODESTONE_TARGET_PROFILE_HPP
#define LODESTONE_TARGET_PROFILE_HPP
#include "CookerErrors.hpp"
#include "ShaderLibraryTypes.hpp"
#include <cstdint>
#include <optional>
#include <utility>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

/** @brief One sampler and one texture that the emitted code samples together, by (group, binding). The emitted
 * artifact decides placements, so a target reader gives a pair in this form, and the cook maps it to the
 * reflected bindings. */
struct SamplerTexturePlacement
{
    uint32_t TextureGroup{ 0u };
    uint32_t TextureBinding{ 0u };
    uint32_t SamplerGroup{ 0u };
    uint32_t SamplerBinding{ 0u };

    friend bool operator==(const SamplerTexturePlacement&, const SamplerTexturePlacement&) = default;
};

/** @brief What a target reader reads from the emitted code of one entry point that reflection cannot report. This is
 * cook output, not a comparison, so the cook reads it with `--no-validate` too. */
struct EmittedCodeFacts
{
    /** The sampler and texture pairs the emitted code samples with. Reflection cannot report which sampler a texture
     * is sampled with, so only the emitted code can. */
    std::vector<SamplerTexturePlacement> SamplerTexturePairs;
    /** The bytes that the push constant block of the entry point covers, from offset 0 to the end of its last
     * member. No value when the code declares no push constant block. Slang removes a block the entry point does
     * not read, and reflection cannot tell that, so this decides whether the entry point constants exist. */
    std::optional<uint32_t> PushConstantExtent;
    /** The (group, binding) of each resource the code declares. Only the read path fills it. The cook uses it to
     * settle the visibility that Slang's metadata cannot report (shader-slang/slang#13789). */
    std::vector<std::pair<uint32_t, uint32_t>> DeclaredPlacements;
    /** The entry point name that the code declares. */
    std::string EntryPointName;
};

/** @brief The answer a validator gives about one entry point. `Matches` being `true` means the bindings and
 * specializations match the expected value. If `Matches` is `false`, `Report` specifies how/where it is
 * false.
 */
struct BindingComparison
{
    bool Matches{ false };
    std::string Report;
    /** Capabilities read back by the validator, based on what the shader code requested or declares
      * This happens as a separate pass bc especially with SPIR-V there's a whole tangled nest of 
      * related,nested,dependent etc caps and features that are really easy to miss otherwise */
    std::vector<std::string> Capabilities;
    EmittedCodeFacts Emitted;
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
    [[nodiscard]] CookResult<BindingComparison> ValidateEntryPoint(std::span<const std::byte> target_code,
                                                                   std::span<const ReflectedBinding*> used,
                                                                   DiagnosticSink& sink) const;
    /**@brief Reads the facts of one entry point's emitted code, and compares nothing. The cook calls this when
     * `--no-validate` skips `ValidateEntryPoint`, because the facts are cook output and not a check. */
    [[nodiscard]] CookResult<EmittedCodeFacts> ReadEmittedCodeFacts(std::span<const std::byte> target_code,
                                                                    DiagnosticSink& sink) const;

protected:
    [[nodiscard]] virtual CookResult<BindingComparison> validateEntryPoint(std::span<const std::byte> target_code,
                                                                           std::span<const ReflectedBinding*> used,
                                                                           DiagnosticSink& sink) const = 0;
    [[nodiscard]] virtual CookResult<EmittedCodeFacts> readEmittedCodeFacts(std::span<const std::byte> target_code,
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
    /** @brief The largest push constant block every device of this target supports, in bytes. Vulkan guarantees
     * 128 (`maxPushConstantsSize`). A block that is larger fails the cook, because a device that supports only the
     * minimum cannot create the pipeline. Zero for a target with no push constants. */
    uint32_t MaxPushConstantBytes{ 0u };
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
