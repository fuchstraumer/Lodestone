#include "CookerErrors.hpp"
#include <magic_enum/magic_enum.hpp>
#include <string_view>

// the value range of CookError comes from magic_enum_define_range_adl in CookerErrors.hpp
namespace lodestone
{

std::string_view ToString(CookError error) noexcept
{
    return magic_enum::enum_name(error);
}

} // namespace lodestone
