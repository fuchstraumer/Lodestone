#include "target/TargetProfile.hpp"
#include "CookerErrors.hpp"
#include "LodestoneConfig.hpp"
#include "model/ShaderDataSchema.hpp"
#include "ShaderLibraryTypes.hpp"
#include "target/SpvValidator.hpp"
#include <array>
#include <algorithm>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#if LODESTONE_ENABLE_WGSL
#include "target/WgslValidator.hpp"
#endif

namespace lodestone
{

namespace
{
    constexpr bool BoundPlacementLess(const ReflectedBinding* lhs, const ReflectedBinding* rhs) noexcept
    {
        if (lhs == nullptr || rhs == nullptr)
        {
            return lhs != nullptr;
        }

        const BoundPlacement* lhsBinding = std::get_if<BoundPlacement>(&lhs->Placement);
        const BoundPlacement* rhsBinding = std::get_if<BoundPlacement>(&rhs->Placement);
        if (lhsBinding->Group != rhsBinding->Group)
        {
            return lhsBinding->Group < rhsBinding->Group;
        }
        return lhsBinding->Binding < rhsBinding->Binding;
    }

    constexpr std::string_view k_WgslName = "wgsl";
    constexpr std::string_view k_SpirvName = "spirv";

    static SpvValidator k_SpvValidator;

#if LODESTONE_ENABLE_WGSL
    static WgslValidator k_WgslValidator;
    const ResolvedLibraryValidator* const k_WgslProfileValidator = &k_WgslValidator;
#else
    const ResolvedLibraryValidator* const k_WgslProfileValidator = nullptr;
#endif

    // Slang has no WGSL profile, and ignores one that does not imply the target. Vulkan 1.2 guarantees SPIR-V 1.5
    const std::array<TargetProfile, 2u> k_TargetProfiles
    {
        TargetProfile{ .Name = k_WgslName,
                           .Language = TargetLanguage::Wgsl,
                           .SlangProfileName = "",
                           .Access = AccessModel::Bound,
                           .Validator = k_WgslProfileValidator },
        TargetProfile{ .Name = k_SpirvName,
                           .Language = TargetLanguage::Spirv,
                           .SlangProfileName = "spirv_1_5",
                           .Access = AccessModel::Bound,
                           .Validator = &k_SpvValidator,
                           .MaxPushConstantBytes = 128u,
                           .VulkanApiVersion = (1u << 22u) | (2u << 12u) },
    };

    constexpr std::array<std::string_view, 2u> k_TargetProfileNames
    {
        k_WgslName,
        k_SpirvName,
    };

} // namespace

CookResult<BindingComparison> ResolvedLibraryValidator::ValidateEntryPoint(std::span<const std::byte> target_code,
                                                                           std::span<const ReflectedBinding*> used,
                                                                           DiagnosticSink& sink) const
{
    std::vector<const ReflectedBinding*> sortedUsed(used.begin(), used.end());
    std::ranges::sort(sortedUsed, BoundPlacementLess);
    return validateEntryPoint(target_code, sortedUsed, sink);
}

CookResult<EmittedCodeFacts> ResolvedLibraryValidator::ReadEmittedCodeFacts(std::span<const std::byte> target_code,
                                                                          DiagnosticSink& sink) const
{
    return readEmittedCodeFacts(target_code, sink);
}

std::string_view ToString(BindingMismatchKind kind) noexcept
{
    switch (kind)
    {
    case BindingMismatchKind::EmittedOnly:
        return "emitted only";
    case BindingMismatchKind::ReflectedOnly:
        return "reflected only";
    case BindingMismatchKind::Unsupported:
        return "unsupported";
    case BindingMismatchKind::Name:
        return "name";
    case BindingMismatchKind::Kind:
        return "kind";
    case BindingMismatchKind::ArrayCount:
        return "array count";
    case BindingMismatchKind::Shape:
        return "shape";
    case BindingMismatchKind::StorageFormat:
        return "storage format";
    case BindingMismatchKind::Access:
        return "access";
    default:
        return "Invalid";
    }
}

std::string FormatBindingMismatch(const BindingMismatch& mismatch)
{
    switch (mismatch.Kind)
    {
    case BindingMismatchKind::EmittedOnly:
        return std::format("  group {} binding {} {}: the code declares it, and reflection has no binding there",
                           mismatch.Group,
                           mismatch.Binding,
                           mismatch.Name);
    case BindingMismatchKind::ReflectedOnly:
        return std::format("  group {} binding {} {}: reflection has it, and the code declares no binding there",
                           mismatch.Group,
                           mismatch.Binding,
                           mismatch.Name);
    case BindingMismatchKind::Unsupported:
        return std::format("  group {} binding {} {}: {}", mismatch.Group, mismatch.Binding, mismatch.Name, mismatch.Emitted);
    default:
        return std::format("  group {} binding {} {}: {} differs, the code has {} and reflection has {}",
                           mismatch.Group,
                           mismatch.Binding,
                           mismatch.Name,
                           ToString(mismatch.Kind),
                           mismatch.Emitted,
                           mismatch.Reflected);
    }
}

std::string_view ToString(AccessModel model) noexcept
{
    switch (model)
    {
    case AccessModel::Bound:
        return "bound";
    case AccessModel::Indexed:
        return "indexed";
    case AccessModel::Pointer:
        return "pointer";
    case AccessModel::Invalid:
        return "invalid";
    }

    return "invalid";
}

CookResult<TargetProfile> FindTargetProfile(std::string_view name) noexcept
{
    for (const TargetProfile& profile : k_TargetProfiles)
    {
        if (profile.Name == name)
        {
            return profile;
        }
    }

    return std::unexpected(CookError::TargetProfileNotFound);
}

std::span<const std::string_view> GetTargetProfileNames() noexcept
{
    return k_TargetProfileNames;
}

PlacementKind PlacementKindFromAccessModel(AccessModel model) noexcept
{
    switch (model)
    {
    case AccessModel::Bound:
        return PlacementKind::Bound;
    case AccessModel::Indexed:
        return PlacementKind::Indexed;
    case AccessModel::Pointer:
        return PlacementKind::Pointer;
    case AccessModel::Invalid:
        [[fallthrough]];
    default:
        return PlacementKind::None;
    }
}

AccessModel AccessModelFromPlacementKind(PlacementKind kind) noexcept
{
    switch (kind)
    {
    case PlacementKind::Bound:
        return AccessModel::Bound;
    case PlacementKind::Indexed:
        return AccessModel::Indexed;
    case PlacementKind::Pointer:
        return AccessModel::Pointer;
    default:
        return AccessModel::Invalid;
    }
}

ShaderCodeFormat CodeFormatFromLanguage(TargetLanguage language) noexcept
{
    switch (language)
    {
    case TargetLanguage::Wgsl:
        return ShaderCodeFormat::Wgsl;
    case TargetLanguage::Spirv:
        return ShaderCodeFormat::Spirv;
    case TargetLanguage::Invalid:
        [[fallthrough]];
    default:
        return ShaderCodeFormat::None;
    }
}

} // namespace lodestone
