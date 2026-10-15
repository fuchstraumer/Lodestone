#include "emit/Depfile.hpp"
#include <span>
#include <string>
#include <string_view>

namespace lodestone
{

namespace
{
    // Make reads a space as a separator and `#` as a comment, so both take a backslash. `$` is a variable
    // reference in Make and in Ninja, and each spells a literal one as `$$`.
    void AppendEscapedPath(std::string& out, std::string_view path)
    {
        for (const char character : path)
        {
            if (character == ' ' || character == '#')
            {
                out += '\\';
            }
            else if (character == '$')
            {
                out += '$';
            }
            out += character;
        }
    }
}

std::string GenerateDepfile(std::string_view target, std::span<const std::string> prerequisites)
{
    std::string result;
    AppendEscapedPath(result, target);
    result += ':';
    for (const std::string& prerequisite : prerequisites)
    {
        // a backslash before the newline continues the rule on the next line
        result += " \\\n  ";
        AppendEscapedPath(result, prerequisite);
    }
    result += '\n';
    return result;
}

} // namespace lodestone
