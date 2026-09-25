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

#if LODESTONE_ENABLE_WGSL
    static WgslValidator k_WgslValidator;
    const ResolvedLibraryValidator* const k_WgslProfileValidator = &k_WgslValidator;
#else
    // No Tint, so no second opinion. A wgsl cook in this build fails unless it passes --no-validate.
    const ResolvedLibraryValidator* const k_WgslProfileValidator = nullptr;
#endif

    // Slang has no WGSL profile. It ignores a profile that does not imply the target, so the old
    // `spirv_1_4` changed nothing (measured on KitchenSink, 2026-09-25).
    const std::array<TargetProfile, 1u> k_TargetProfiles
    {
        TargetProfile{ .Name = k_WgslName,
                           .Language = TargetLanguage::Wgsl,
                           .SlangProfileName = "",
                           .Access = AccessModel::Bound,
                           .Validator = k_WgslProfileValidator },
    };

    constexpr std::array<std::string_view, 1u> k_TargetProfileNames{ k_WgslName };

} // namespace

CookResult<BindingComparison> ResolvedLibraryValidator::ValidateEntryPoint(std::string_view target_text,
                                                                   std::span<const ReflectedBinding*> used,
                                                                   DiagnosticSink& sink) const
{
    // its just a vector of pointers, so copying is fine (this is the validation path, anyways)
    std::vector<const ReflectedBinding*> sortedUsed(used.begin(), used.end());
    std::ranges::sort(sortedUsed, BoundPlacementLess);
    return validateEntryPoint(target_text, sortedUsed, sink);
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

} // namespace lodestone
