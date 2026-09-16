#include "compile/RawLibrary.hpp"
#include <cstdint>
#include <string_view>

namespace lodestone
{

std::string_view ToString(RawSizeAttributeKind kind) noexcept
{
    switch (kind)
    {
    case RawSizeAttributeKind::ElementCount:
        return "ls_element_count";
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

} // namespace lodestone
