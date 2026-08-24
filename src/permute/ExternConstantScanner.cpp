#include "permute/ExternConstantScanner.hpp"

#include <cctype>
#include <cstddef>
#include <string_view>
#include <vector>

namespace lodestone
{

namespace
{

    constexpr std::string_view k_ExternKeyword{ "extern" };

    bool IsIdentifierCharacter(char character) noexcept
    {
        return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
    }

    /** The next line of `source`, and where the line after it starts. */
    std::string_view NextLine(std::string_view source, size_t& line_start) noexcept
    {
        size_t lineEnd = source.find('\n', line_start);
        if (lineEnd == std::string_view::npos)
        {
            lineEnd = source.size();
        }

        const std::string_view line = source.substr(line_start, lineEnd - line_start);
        line_start = lineEnd + 1u;
        return line;
    }

    /** The identifier that ends at `end_index`, reading to the left. Empty when there is none. */
    std::string_view IdentifierBefore(std::string_view line, size_t end_index) noexcept
    {
        size_t nameEnd = end_index;
        while (nameEnd > 0u && !IsIdentifierCharacter(line[nameEnd - 1u]))
        {
            --nameEnd;
        }

        size_t nameStart = nameEnd;
        while (nameStart > 0u && IsIdentifierCharacter(line[nameStart - 1u]))
        {
            --nameStart;
        }

        return line.substr(nameStart, nameEnd - nameStart);
    }

    /** The name that one `extern` line declares. Empty when the line declares none.
     *
     * A declaration names its constant immediately to the left of the `=`, so a name that appears in
     * the default of a different constant is a use and not a declaration. */
    std::string_view DeclaredNameOnLine(std::string_view line) noexcept
    {
        const size_t assignIndex = line.find('=');
        return IdentifierBefore(line, assignIndex == std::string_view::npos ? line.size() : assignIndex);
    }

} // namespace

std::vector<ExternConstantDeclaration> ScanExternConstants(std::string_view source)
{
    std::vector<ExternConstantDeclaration> declarations;

    size_t lineStart = 0u;
    while (lineStart < source.size())
    {
        const std::string_view line = NextLine(source, lineStart);
        if (!line.contains(k_ExternKeyword))
        {
            continue;
        }

        const size_t assignIndex = line.find('=');
        if (assignIndex == std::string_view::npos)
        {
            continue;
        }

        const std::string_view name = IdentifierBefore(line, assignIndex);
        if (name.empty())
        {
            continue;
        }

        const size_t valueStart = assignIndex + 1u;
        size_t valueEnd = line.find(';', valueStart);
        if (valueEnd == std::string_view::npos)
        {
            valueEnd = line.size();
        }
        
        declarations.emplace_back(name, line.substr(valueStart, valueEnd - valueStart));
    }

    return declarations;
}

bool DeclaresExternConstantNamed(std::string_view source, std::string_view name)
{
    size_t lineStart = 0u;
    while (lineStart < source.size())
    {
        const std::string_view line = NextLine(source, lineStart);
        if (line.contains(k_ExternKeyword) && DeclaredNameOnLine(line) == name)
        {
            return true;
        }
    }

    return false;
}

std::string_view TrimWhitespace(std::string_view text) noexcept
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
    {
        text.remove_prefix(1u);
    }

    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
    {
        text.remove_suffix(1u);
    }

    return text;
}

} // namespace lodestone
