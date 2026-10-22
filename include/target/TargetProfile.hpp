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
enum class AccessModel : uint8_t
{
    Invalid = 0,
    Bound,
    Indexed,
    /** Buffers only. Textures use Indexed. */
    Pointer,
};

std::string_view ToString(AccessModel model) noexcept;

enum class TargetLanguage : uint8_t
{
    Invalid = 0,
    Wgsl,
    Spirv,
};

/** @brief A sampled pair by (group, binding), since the emitted code decides placements. */
struct SamplerTexturePlacement
{
    uint32_t TextureGroup{ 0u };
    uint32_t TextureBinding{ 0u };
    uint32_t SamplerGroup{ 0u };
    uint32_t SamplerBinding{ 0u };

    friend bool operator==(const SamplerTexturePlacement&, const SamplerTexturePlacement&) = default;
};

/** @brief Facts only the emitted code can give. Cook output, so `--no-validate` reads them too. */
struct EmittedCodeFacts
{
    std::vector<SamplerTexturePlacement> SamplerTexturePairs;
    /** No value when the code declares no block. Slang removes unread blocks, so this decides whether the entry point
     * constants exist. */
    std::optional<uint32_t> PushConstantExtent;
    /** Read path only. Settles the visibility of shader-slang/slang#13789. */
    std::vector<std::pair<uint32_t, uint32_t>> DeclaredPlacements;
    std::string EntryPointName;
    /** Sorted and unique: `OpCapability` and `OpExtension`, or `enable` directives. */
    std::vector<std::string> CapabilityNames;
    /** Each workgroup variable counts `RoundUp(16, RoundUp(align, size))`, std430. */
    uint32_t WorkgroupMemoryBytes{ 0u };
};

struct BindingComparison
{
    bool Matches{ false };
    std::string Report;
    EmittedCodeFacts Emitted;
};

/** @brief A second opinion on what an entry point declared. The emitted code decides placements, and reflection
 * decides sizes and types. */
class ResolvedLibraryValidator
{
public:
    ResolvedLibraryValidator() = default;
    virtual ~ResolvedLibraryValidator() = default;
    ResolvedLibraryValidator(const ResolvedLibraryValidator&) = delete;
    ResolvedLibraryValidator& operator=(const ResolvedLibraryValidator&) = delete;
    ResolvedLibraryValidator(ResolvedLibraryValidator&&) = delete;
    ResolvedLibraryValidator& operator=(ResolvedLibraryValidator&&) = delete;

    /** @brief Sorts `used`, then calls `validateEntryPoint`. */
    [[nodiscard]] CookResult<BindingComparison> ValidateEntryPoint(std::span<const std::byte> target_code,
                                                                   std::span<const ReflectedBinding*> used,
                                                                   DiagnosticSink& sink) const;
    /** @brief Reads the facts and compares nothing, for `--no-validate`. */
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
    std::string_view Name;
    TargetLanguage Language{ TargetLanguage::Invalid };
    /** @brief Such as `spirv_1_5`. Empty sets no profile. */
    std::string_view SlangProfileName;
    AccessModel Access{ AccessModel::Invalid };
    /** @brief Null when this target cannot check its own output. */
    const ResolvedLibraryValidator* Validator{ nullptr };
    /** @brief The push constant bytes every device supports: 128 for Vulkan. Zero without push constants. */
    uint32_t MaxPushConstantBytes{ 0u };
    /** @brief The guaranteed Vulkan version, as `VK_MAKE_API_VERSION(0, major, minor, 0)`. Zero if not Vulkan. */
    uint32_t VulkanApiVersion{ 0u };
};

/** @brief A compiled-in table, since each target needs its own validation code. */
CookResult<TargetProfile> FindTargetProfile(std::string_view name) noexcept;

std::span<const std::string_view> GetTargetProfileNames() noexcept;

PlacementKind PlacementKindFromAccessModel(AccessModel model) noexcept;

ShaderCodeFormat CodeFormatFromLanguage(TargetLanguage language) noexcept;

} // namespace lodestone

#endif // !LODESTONE_TARGET_PROFILE_HPP
