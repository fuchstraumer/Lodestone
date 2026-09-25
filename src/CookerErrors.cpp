#include "CookerErrors.hpp"
#include <magic_enum/magic_enum.hpp>
#include <string_view>

// magic_enum reads names only in [-128, 127] by default, so every error at 128 or above printed as an
// empty string. The widest value today is 301.
template<>
struct magic_enum::customize::enum_range<lodestone::CookError>
{
    static constexpr int min = 0;
    static constexpr int max = 511;
};

namespace lodestone
{

std::string_view ToString(CookError error) noexcept
{
    return magic_enum::enum_name(error);
}

} // namespace lodestone
