#include "JsonWriter.hpp"
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace lodestone
{

std::string_view ToString(JsonWriterError error) noexcept
{
    switch (error)
    {
    case JsonWriterError::Invalid:
        return "Invalid";
    case JsonWriterError::Success:
        return "Success";
    case JsonWriterError::UnbalancedContainers:
        return "UnbalancedContainers";
    default:
        return "Invalid Error passed to JsonWriter::ToString()";
    }
}

JsonWriter::JsonWriter(bool _pretty) noexcept : pretty{ _pretty }
{
    buffer.reserve(4096);
    stack.reserve(16);
}

void JsonWriter::BeginObject() noexcept
{
    beginContainer(ContainerKind::Object, '{');
}

void JsonWriter::EndObject() noexcept
{
    endContainer('}');
}

void JsonWriter::BeginArray() noexcept
{
    beginContainer(ContainerKind::Array, '[');
}

void JsonWriter::EndArray() noexcept
{
    endContainer(']');
}

void JsonWriter::Key(std::string_view key) noexcept
{
    if (stack.empty() || stack.back().Kind != ContainerKind::Object)
    {
        return;
    }

    Frame& top = stack.back();
    if (top.ElementCount > 0u)
    {
        buffer.push_back(',');
    }
    if (pretty)
    {
        buffer.push_back('\n');
        writeIndent(stack.size());
    }

    buffer.push_back('"');
    writeEscaped(key);
    buffer.append(pretty ? "\": " : "\":");
    top.ElementCount++;
}

void JsonWriter::String(std::string_view value) noexcept
{
    beforeValue();
    buffer.push_back('"');
    writeEscaped(value);
    buffer.push_back('"');
}

void JsonWriter::Int(int64_t value) noexcept
{
    beforeValue();
    char digits[24];
    const auto result = std::to_chars(digits, digits + sizeof(digits), value);
    buffer.append(digits, result.ptr);
}

void JsonWriter::UInt(uint64_t value) noexcept
{
    beforeValue();
    char digits[24];
    const auto result = std::to_chars(digits, digits + sizeof(digits), value);
    buffer.append(digits, result.ptr);
}

void JsonWriter::Double(double value) noexcept
{
    beforeValue();
    char digits[32];
    const auto result = std::to_chars(digits, digits + sizeof(digits), value);
    buffer.append(digits, result.ptr);
}

void JsonWriter::Bool(bool value) noexcept
{
    beforeValue();
    buffer.append(value ? "true" : "false");
}

void JsonWriter::Null() noexcept
{
    beforeValue();
    buffer.append("null");
}

void JsonWriter::KeyString(std::string_view key, std::string_view value) noexcept
{
    Key(key);
    String(value);
}

void JsonWriter::KeyInt(std::string_view key, int64_t value) noexcept
{
    Key(key);
    Int(value);
}

void JsonWriter::KeyUInt(std::string_view key, uint64_t value) noexcept
{
    Key(key);
    UInt(value);
}

void JsonWriter::KeyDouble(std::string_view key, double value) noexcept
{
    Key(key);
    Double(value);
}

void JsonWriter::KeyBool(std::string_view key, bool value) noexcept
{
    Key(key);
    Bool(value);
}

void JsonWriter::KeyNull(std::string_view key) noexcept
{
    Key(key);
    Null();
}

JsonResult<std::string> JsonWriter::Finish() noexcept
{
    if (!stack.empty())
    {
        return std::unexpected(JsonWriterError::UnbalancedContainers);
    }

    return buffer;
}

void JsonWriter::beginContainer(ContainerKind kind, char opening) noexcept
{
    beforeValue();
    buffer.push_back(opening);
    stack.push_back(Frame{ .Kind = kind, .ElementCount = 0u });
}

void JsonWriter::endContainer(char closing) noexcept
{
    if (stack.empty())
    {
        return;
    }

    const Frame top = stack.back();
    stack.pop_back();
    if (pretty && top.ElementCount > 0u)
    {
        buffer.push_back('\n');
        writeIndent(stack.size());
    }
    buffer.push_back(closing);
}

void JsonWriter::beforeValue() noexcept
{
    if (stack.empty())
    {
        return;
    }

    Frame& top = stack.back();
    if (top.Kind == ContainerKind::Array)
    {
        if (top.ElementCount > 0u)
        {
            buffer.push_back(',');
        }
        if (pretty)
        {
            buffer.push_back('\n');
            writeIndent(stack.size());
        }
        top.ElementCount++;
    }
}

void JsonWriter::writeIndent(size_t depth) noexcept
{
    if (!pretty)
    {
        return;
    }

    buffer.append(depth * 4u, ' ');
}

void JsonWriter::writeEscaped(std::string_view text) noexcept
{
    for (const char character : text)
    {
        switch (character)
        {
        case '"':
            buffer.append("\\\"");
            break;
        case '\\':
            buffer.append("\\\\");
            break;
        case '\n':
            buffer.append("\\n");
            break;
        case '\r':
            buffer.append("\\r");
            break;
        case '\t':
            buffer.append("\\t");
            break;
        case '\b':
            buffer.append("\\b");
            break;
        case '\f':
            buffer.append("\\f");
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20u)
            {
                constexpr std::string_view hexDigits{ "0123456789abcdef" };
                const auto code = static_cast<unsigned char>(character);
                const char escape[6]{ '\\', 'u', '0', '0', hexDigits[code >> 4u], hexDigits[code & 0xFu] };
                buffer.append(escape, sizeof(escape));
            }
            else
            {
                buffer.push_back(character);
            }
            break;
        }
    }
}

} // namespace lodestone
