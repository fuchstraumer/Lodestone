#include "compile/RawLibrary.hpp"
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace lodestone
{

std::string_view ToString(RawSizeAttributeKind kind) noexcept
{
    switch (kind)
    {
    case RawSizeAttributeKind::ElementCount:
        return "ls_element_count";
    case RawSizeAttributeKind::Extent1d:
        return "ls_extent_1d";
    case RawSizeAttributeKind::Extent2d:
        return "ls_extent_2d";
    case RawSizeAttributeKind::Extent3d:
        return "ls_extent_3d";
    case RawSizeAttributeKind::Invalid:
        return "Invalid";
    }

    return "Invalid";
}

uint32_t ArgumentCountOf(RawSizeAttributeKind kind) noexcept
{
    switch (kind)
    {
    case RawSizeAttributeKind::ElementCount:
    case RawSizeAttributeKind::Extent1d:
        return 1u;
    case RawSizeAttributeKind::Extent2d:
        return 2u;
    case RawSizeAttributeKind::Extent3d:
        return 3u;
    case RawSizeAttributeKind::Invalid:
        return 0u;
    }

    return 0u;
}

std::string DescribeShimExtern(const RawShimExtern& shim_extern)
{
    if (shim_extern.TypeArguments.empty())
    {
        return std::format("extern '{}' of '{}'", shim_extern.Name, shim_extern.Interface);
    }
    return std::format("extern '{}' of '{}<{}>'", shim_extern.Name, shim_extern.Interface, shim_extern.TypeArguments);
}

} // namespace lodestone
