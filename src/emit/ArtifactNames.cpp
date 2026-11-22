#include "emit/ArtifactNames.hpp"
#include <string>
#include <string_view>

namespace lodestone
{

bool IsValidOutputName(std::string_view output_name) noexcept
{
    const bool isDotName = output_name == "." || output_name == "..";
    const bool hasSeparator = output_name.find_first_of("/\\:") != std::string_view::npos;
    return !output_name.empty() && !isDotName && !hasSeparator;
}

std::string MakeArtifactName(std::string_view output_name, std::string_view suffix)
{
    std::string name;
    name.reserve(output_name.size() + suffix.size());
    name += output_name;
    name += suffix;
    return name;
}

} // namespace lodestone
