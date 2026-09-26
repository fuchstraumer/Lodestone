#include "target/TargetProfile.hpp"
#include "CookerErrors.hpp"
#include "LodestoneConfig.hpp"
#include "model/ShaderDataSchema.hpp"
#include "ShaderLibraryTypes.hpp"
#include <array>
#include <algorithm>
#include <expected>
#include <span>
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

#if LODESTONE_ENABLE_WGSL
    static WgslValidator k_WgslValidator;
    const ResolvedLibraryValidator* const k_WgslProfileValidator = &k_WgslValidator;
#else
    // No Tint, so no second opinion. A wgsl cook in this build fails unless it passes --no-validate.
    const ResolvedLibraryValidator* const k_WgslProfileValidator = nullptr;
#endif

    // Slang has no WGSL profile. It ignores a profile that does not imply the target, so the old
    // `spirv_1_4` changed nothing (measured on KitchenSink, 2026-09-25).
    // Vulkan 1.2 guarantees SPIR-V 1.5 (decision O5). The validator comes in phase F step F1.4, so a spirv
    // cook needs --no-validate until then.
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
                           .Validator = nullptr },
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
    // its just a vector of pointers, so copying is fine (this is the validation path, anyways)
    std::vector<const ReflectedBinding*> sortedUsed(used.begin(), used.end());
    std::ranges::sort(sortedUsed, BoundPlacementLess);
    return validateEntryPoint(target_code, sortedUsed, sink);
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
