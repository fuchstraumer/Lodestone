#include "TestHarness.hpp"
#include "CookerErrors.hpp"
#include "ShaderManifestFormat.hpp"
#include "ShaderLibraryTypes.hpp"
#include "model/ShaderDataSchema.hpp"

#include <cstddef>
#include <format>
#include <optional>
#include <tuple>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

// Each helper must be able to fail, or every suite built on it proves nothing. An inner runner takes the failures, and
// its output on stderr is expected.

using lodestone::CookError;
using lodestone::CookResult;
using lodestone::tests::FormatForTest;
using lodestone::tests::TestRunner;

int main()
{
    TestRunner runner{ "TestHarness" };

    runner.BeginSection("CheckEqual passes on equal values and fails on different ones");
    {
        TestRunner inner{ "inner" };
        inner.BeginSection("inner");
        inner.CheckEqual(3, 3, "equal integers");
        inner.CheckEqual(std::string{ "a" }, std::string{ "a" }, "equal strings");
        runner.CheckEqual(inner.Failures(), 0, "equal values pass");
        inner.CheckEqual(3, 4, "different integers");
        runner.CheckEqual(inner.Failures(), 1, "different values fail");
        inner.CheckEqual(int32_t{ -1 }, uint32_t{ 0xFFFFFFFFu }, "-1 against the largest uint32");
        runner.CheckEqual(inner.Failures(), 2, "a signed and an unsigned integer compare by value, not by conversion");
    }

    runner.BeginSection("CheckOk and CheckError read a result");
    {
        TestRunner inner{ "inner" };
        inner.BeginSection("inner");
        const CookResult<int32_t> value{ 7 };
        const CookResult<int32_t> error{ std::unexpected(CookError::FileNotFound) };
        inner.CheckOk(value, "a value");
        inner.CheckError(error, CookError::FileNotFound, "the expected error");
        runner.CheckEqual(inner.Failures(), 0, "a value passes CheckOk, and the expected error passes CheckError");
        inner.CheckOk(error, "an error");
        inner.CheckError(error, CookError::ReflectionMismatch, "another error");
        inner.CheckError(value, CookError::FileNotFound, "a value where an error is expected");
        runner.CheckEqual(inner.Failures(), 3, "an error fails CheckOk, and the wrong error or a value fails CheckError");
    }

    runner.BeginSection("a section or a suite that runs no check fails");
    {
        TestRunner inner{ "inner" };
        inner.BeginSection("empty");
        inner.BeginSection("full");
        inner.Check(true, "a check");
        runner.CheckEqual(inner.Failures(), 1, "a section with no check fails when the next section starts");
        {
            const auto scoped = inner.Section("empty and scoped");
        }
        runner.CheckEqual(inner.Failures(), 2, "a scoped section with no check fails when it ends");
        runner.CheckEqual(inner.Report(), 1, "the suite reports the failure");

        TestRunner silent{ "silent" };
        runner.CheckEqual(silent.Report(), 1, "a suite with no check fails");
    }

    runner.BeginSection("a scoped section that is still open at Report ends once");
    {
        TestRunner inner{ "inner" };
        int32_t reported = 0;
        {
            const auto scoped = inner.Section("open at Report");
            inner.Check(true, "a check");
            reported = inner.Report();
        }
        runner.CheckEqual(reported, 0, "Report finishes the open section");
        runner.CheckEqual(inner.Failures(), 0, "the section's destructor after Report changes nothing");
    }

    runner.BeginSection("a failure prints the value in a readable form");
    runner.CheckEqual(FormatForTest(CookError::FileNotFound), std::string{ "FileNotFound" }, "an enum prints its name");
    runner.CheckEqual(FormatForTest(std::vector<int32_t>{ 1, 2 }), std::string{ "[1, 2]" }, "a range prints its elements");
    runner.CheckEqual(FormatForTest(true), std::string{ "true" }, "a bool prints as a word");
    const lodestone::manifest::ErrorState manifestError{ .Code = lodestone::manifest::ErrorCode::BadMagic };
    runner.Check(FormatForTest(manifestError).contains("BadMagic"), "a manifest error prints its code");

    runner.BeginSection("values that std::format cannot print still print their contents");
    runner.CheckEqual(FormatForTest(lodestone::QueryErrorCode::UnknownAxis), std::string{ "UnknownAxis" },
                      "an enum with no ToString prints its name through magic_enum");
    const std::string stages = FormatForTest(lodestone::ShaderStageKind::Vertex | lodestone::ShaderStageKind::Fragment);
    runner.Check(stages.contains("Vertex") && stages.contains("Fragment"), std::format("a flag set prints each bit: {}", stages));
    runner.CheckEqual(FormatForTest(std::byte{ 0x41 }), std::string{ "0x41" }, "an enum with no named value prints in hex");
    runner.CheckEqual(FormatForTest(std::vector<CookError>{ CookError::FileNotFound, CookError::Success }),
                      std::string{ "[FileNotFound, Success]" },
                      "a range of enums prints each name");
    runner.CheckEqual(FormatForTest(std::optional<uint32_t>{ 7u }), std::string{ "7" }, "an optional prints its value");
    runner.CheckEqual(FormatForTest(std::optional<uint32_t>{}), std::string{ "nullopt" }, "an empty optional says so");
    const lodestone::ResourcePlacement placement{ lodestone::BoundPlacement{ .Group = 1u, .Binding = 2u } };
    runner.CheckEqual(FormatForTest(placement), std::string{ "{1, 2}" }, "a variant prints its alternative, and an aggregate its fields");
    runner.CheckEqual(FormatForTest(lodestone::ResourcePlacement{}), std::string{ "monostate" }, "an empty variant says so");
    runner.CheckEqual(FormatForTest(std::tuple{ CookError::Success, 3 }), std::string{ "{Success, 3}" },
                      "a tuple with an enum prints each element");
    const int32_t* nothing = nullptr;
    runner.CheckEqual(FormatForTest(nothing), std::string{ "nullptr" }, "a null pointer says so");
    const std::vector<std::byte> blob(40u, std::byte{ 0 });
    runner.Check(FormatForTest(blob).ends_with("... 24 more]"), "a long range prints its first elements and the count of the rest");

    return runner.Report();
}
