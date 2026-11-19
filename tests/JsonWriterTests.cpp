#include "JsonWriter.hpp"
#include "TestHarness.hpp"

#include <string>
#include <string_view>
#include <utility>

using lodestone::JsonWriter;

namespace
{

std::string WriteOneString(std::string_view text);

} // namespace

int main()
{
    lodestone::tests::TestRunner runner{ "JsonWriterTests" };

    runner.BeginSection("a control character with no short escape writes four hex digits");
    runner.CheckEqual(WriteOneString("\x01"), std::string{ "\"\\u0001\"" }, "0x01 is \\u0001");
    runner.CheckEqual(WriteOneString("\x0b"), std::string{ "\"\\u000b\"" }, "0x0b is \\u000b");
    runner.CheckEqual(WriteOneString("\x10"), std::string{ "\"\\u0010\"" }, "0x10 is \\u0010");
    runner.CheckEqual(WriteOneString("\x1f"), std::string{ "\"\\u001f\"" }, "0x1f is \\u001f");

    runner.BeginSection("a short escape stays short");
    runner.CheckEqual(WriteOneString("a\n\"\\"),
                      std::string{ "\"a\\n\\\"\\\\\"" },
                      "newline, quote, and backslash use their short forms");

    return runner.Report();
}

namespace
{

std::string WriteOneString(std::string_view text)
{
    JsonWriter writer{ false };
    writer.String(text);
    return std::move(*writer.Finish());
}

} // namespace
