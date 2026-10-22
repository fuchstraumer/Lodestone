#include "emit/DepFile.hpp"
#include <span>
#include <string>
#include <string_view>

namespace lodestone
{

namespace
{
    // Make reads spaces as separators and `#` as comments. Make and Ninja spell a literal `$` as `$$`
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
        result += " \\\n  ";
        AppendEscapedPath(result, prerequisite);
    }
    result += '\n';
    return result;
}

} // namespace lodestone
