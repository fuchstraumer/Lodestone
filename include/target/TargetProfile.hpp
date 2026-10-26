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
    /** A descriptor handle into a descriptor heap. Textures and samplers. */
    Handle,
    /** A buffer device address. Buffers only. */
    Pointer,
};

std::string_view ToString(AccessModel model) noexcept;

/** @brief The access model of each class of shim resource. A profile has one form for each class. */
struct ShimForms
{
    AccessModel Buffer{ AccessModel::Invalid };
    AccessModel Texture{ AccessModel::Invalid };
    AccessModel Sampler{ AccessModel::Invalid };
    AccessModel StorageTexture{ AccessModel::Invalid };
};

/** @brief The form of every class when all classes agree, and `Invalid` when they do not. */
AccessModel SingleForm(const ShimForms& forms) noexcept;

PlacementForms PlacementFormsOf(const ShimForms& forms) noexcept;

enum class TargetLanguage : uint8_t
{
    Invalid = 0,
    Wgsl,
    Spirv,
};

/** @brief A sampled pair by (group, binding), since the emitted code decides placements. */
/** @brief A side reached through a handle holds its container's group and binding, and the handle's offset. */
struct SamplerTexturePlacement
{
    uint32_t TextureGroup{ 0u };
    uint32_t TextureBinding{ 0u };
    uint32_t SamplerGroup{ 0u };
    uint32_t SamplerBinding{ 0u };
    StoredValueContainer TextureContainer{ StoredValueContainer::None };
    StoredValueContainer SamplerContainer{ StoredValueContainer::None };
    uint32_t TextureOffset{ 0u };
    uint32_t SamplerOffset{ 0u };

    friend bool operator==(const SamplerTexturePlacement&, const SamplerTexturePlacement&) = default;
};

/** @brief Facts only the emitted code can give. Cook output, so `--no-validate` reads them too. */
struct EmittedCodeFacts
{
    std::vector<SamplerTexturePlacement> SamplerTexturePairs;
    /** No value when the code declares no block. Slang removes unread blocks, so this decides whether the entry point
     * constants exist. */
    std::optional<uint32_t> PushConstantExtent;
    /** Every bound placement the code declares. These are the bindings the entry point uses. */
    std::vector<std::pair<uint32_t, uint32_t>> DeclaredPlacements;
    std::string EntryPointName;
    /** Sorted and unique: `OpCapability` and `OpExtension`, or `enable` directives. */
    std::vector<std::string> CapabilityNames;
    /** Each workgroup variable counts `RoundUp(16, RoundUp(align, size))`, std430. */
    uint32_t WorkgroupMemoryBytes{ 0u };
};

/** The part of one binding on which the emitted code and reflection disagree. */
enum class BindingMismatchKind : uint8_t
{
    Invalid = 0,
    /** The code declares a binding at a placement where reflection has none. */
    EmittedOnly,
    /** Reflection has a binding at a placement where the code declares none. */
    ReflectedOnly,
    /** The code declares a binding that this validator cannot compare. `Emitted` holds the reason. */
    Unsupported,
    Name,
    Kind,
    ArrayCount,
    Shape,
    StorageFormat,
    Access,
    /** The code declares a variable in the descriptor heap set that the heap lowering does not allow, or reflection places a
     * resource in that set. `Emitted` holds the reason. */
    DescriptorHeap,
    /** No member of the container starts at a stored value's offset, or that member has another name or type. `Emitted`
     * holds the reason. */
    StoredValue,
};

std::string_view ToString(BindingMismatchKind kind) noexcept;

/** One disagreement at one placement. `Emitted` and `Reflected` hold the value of each side as text, and both are
 * empty for `EmittedOnly` and `ReflectedOnly`. `Name` is the name in the code, or the reflected name when the code
 * declares no binding there. */
struct BindingMismatch
{
    BindingMismatchKind Kind{ BindingMismatchKind::Invalid };
    uint32_t Group{ 0u };
    uint32_t Binding{ 0u };
    std::string Name;
    std::string Emitted;
    std::string Reflected;

    bool operator==(const BindingMismatch&) const = default;
};

/** One line of the reflection mismatch warning, with no line break. */
std::string FormatBindingMismatch(const BindingMismatch& mismatch);

struct BindingComparison
{
    std::vector<BindingMismatch> Mismatches;
    EmittedCodeFacts Emitted;

    [[nodiscard]] bool Matches() const noexcept { return Mismatches.empty(); }
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

    /** @brief Splits `used` into the bound bindings, sorted, and the stored values, then calls `validateEntryPoint`. */
    [[nodiscard]] CookResult<BindingComparison> ValidateEntryPoint(std::span<const std::byte> target_code,
                                                                   std::span<const ReflectedBinding*> used,
                                                                   DiagnosticSink& sink) const;
    /** @brief Reads the facts and compares nothing, for `--no-validate`. */
    [[nodiscard]] CookResult<EmittedCodeFacts> ReadEmittedCodeFacts(std::span<const std::byte> target_code,
                                                                    DiagnosticSink& sink) const;

protected:
    [[nodiscard]] virtual CookResult<BindingComparison> validateEntryPoint(std::span<const std::byte> target_code,
                                                                           std::span<const ReflectedBinding*> bound,
                                                                           std::span<const ReflectedBinding*> stored,
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
    ShimForms Forms;
    /** @brief The descriptor set of the descriptor heap that handles index. No value without a Handle form. */
    std::optional<uint32_t> DescriptorHeapSet;
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

AccessModel AccessModelFromPlacementKind(PlacementKind kind) noexcept;

ShaderCodeFormat CodeFormatFromLanguage(TargetLanguage language) noexcept;

} // namespace lodestone

#endif // !LODESTONE_TARGET_PROFILE_HPP
