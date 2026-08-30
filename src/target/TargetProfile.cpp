#include "target/TargetProfile.hpp"
#include "target/WgslBindingScanner.hpp"
#include "model/ShaderDataSchema.hpp"
#include "ShaderLibraryTypes.hpp"

#include <array>
#include <span>
#include <string_view>
#include <vector>
#include <algorithm>

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

    /**@brief The WGSL second opinion. First uses `ScanWgslBindings` to extract the bindings declared in the
     * WGSL source text, returned in `declared`. CompareBindings then uses the reflected binding information
     * to validate they match on the data values they both store (which isn't everything, to be clear) */
    class WgslReflectionValidator final : public ResolvedLibraryValidator
    {
    public:
        [[nodiscard]] BindingComparison ValidateEntryPoint(
            std::string_view target_text, std::span<const ReflectedBinding*> used) const override
        {
            std::vector<WgslDeclaredBinding> declared = ScanWgslBindings(target_text);
            // this is a WGSL binding validator: we can collapse these to BoundPlacement* pointers
            // Copy "used" to sort, and sort `declared`: We will scan by location otherwise, wasting time
            std::ranges::sort(declared,
                              [](const WgslDeclaredBinding& lhs, const WgslDeclaredBinding& rhs)
                              {
                                // for descriptor layouts, sort first by group then by binding
                                if (lhs.Group != rhs.Group)
                                {
                                    return lhs.Group < rhs.Group;
                                }
                                return lhs.Binding < rhs.Binding;
                              });
            // used comes to us unsorted, as the caller of this code is not supposed to know the 
            // target (and thus, binding model) it is calling for validation.
            std::ranges::sort(used, BoundPlacementLess);
            return CompareBindings(declared, used);
        }
    };

    const WgslReflectionValidator k_WgslValidator;

    constexpr std::string_view k_WgslName = "wgsl";

    const std::array<TargetProfile, 1u> k_TargetProfiles{ TargetProfile{
        .Name = k_WgslName, .Access = AccessModel::Bound, .Validator = &k_WgslValidator } };

    constexpr std::array<std::string_view, 1u> k_TargetProfileNames{ k_WgslName };

} // namespace

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
        [[fallthrough]];
    default:
        return "invalid";
    }
}

const TargetProfile* FindTargetProfile(std::string_view name) noexcept
{
    for (const TargetProfile& profile : k_TargetProfiles)
    {
        if (profile.Name == name)
        {
            return &profile;
        }
    }

    return nullptr;
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
