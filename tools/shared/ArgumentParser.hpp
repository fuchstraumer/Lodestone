#pragma once
#ifndef LODESTONE_ARGUMENT_PARSER_HPP
#define LODESTONE_ARGUMENT_PARSER_HPP
#include <string>
#include <string_view>
#include <vector>

namespace lodestone
{

class ArgumentParser
{
public:
    ArgumentParser(int argc, char** argv);
    ArgumentParser(std::string commandLine);
    std::vector<std::string_view> GetArgs() const noexcept { return arguments; }
private:
    void tokenizeArguments();
    // owns the text, and returns views into it
    std::string argumentString;
    std::vector<std::string_view> arguments;
};

ArgumentParser::ArgumentParser(int argc, char** argv)
{
    // copy the arguments into argumentString, untouched
    for (int i = 1; i < argc; ++i)
    {
        argumentString += argv[i];
        if (i < argc - 1)
        {
            argumentString += ' ';
        }
    }
    tokenizeArguments();
}

ArgumentParser::ArgumentParser(std::string commandLine) : argumentString{ std::move(commandLine) }
{
    tokenizeArguments();
}

void ArgumentParser::tokenizeArguments()
{
    std::string_view strView{ argumentString };
    size_t pos = 0;
    while (pos < strView.size())
    {
        // skip whitespace
        while (pos < strView.size() && std::isspace(strView[pos]))
        {
            ++pos;
        }
        
        if (pos >= strView.size())
        {
            break;
        }

        // check for quoted string
        if (strView[pos] == '"')
        {
            size_t endQuote = strView.find('"', pos + 1);
            if (endQuote == std::string_view::npos)
            {
                endQuote = strView.size();
            }
            arguments.emplace_back(strView.substr(pos + 1, endQuote - pos - 1));
            pos = endQuote + 1;
        }
        else
        {
            size_t nextSpace = strView.find(' ', pos);
            if (nextSpace == std::string_view::npos)
            {
                nextSpace = strView.size();
            }
            arguments.emplace_back(strView.substr(pos, nextSpace - pos));
            pos = nextSpace;
        }
    }
}

} // namespace lodestone

#endif // !LODESTONE_ARGUMENT_PARSER_HPP