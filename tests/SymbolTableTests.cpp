#include "compile/SymbolTable.hpp"
#include "TestHarness.hpp"
#include <string_view>
#include <vector>

using lodestone::SymbolTable;
using lodestone::tests::TestRunner;

namespace
{

// A thin wrapper: MissingTokens takes mutable spans, so the caller must own the vectors. Returning the
// result by value is safe because each token is a view into the caller's string literals, not into the
// argument vectors.
std::vector<std::string_view> Missing(const SymbolTable& table,
                                      std::vector<std::string_view> modules,
                                      std::vector<std::string_view> tokens)
{
    return table.MissingTokens(modules, tokens);
}

// A whole `extern static const` line is dropped, in any keyword order. A line that carries only some of
// the three keywords is an ordinary declaration, and its name stays a token.
void TestExternStaticConstStripping(TestRunner& runner)
{
    runner.BeginSection("extern static const stripping");

    constexpr std::string_view k_Source = R"(
[ls_axis_values("1, 2")] extern static const uint AAA;
[ls_axis_values("3, 4")] static extern const uint BBB;
[ls_axis_values("5, 6")] extern const static uint CCC;
const uint DDD = 5;
static uint EEE = 1;
uint FFF = 2;
)";

    SymbolTable table;
    table.AddSource("decls", k_Source);

    runner.Check(Missing(table, { "decls" }, { "AAA", "BBB", "CCC" }).size() == 3u,
                 "every arrangement of extern static const is stripped");
    runner.Check(Missing(table, { "decls" }, { "DDD", "EEE", "FFF" }).empty(),
                 "a declaration with only one of the three keywords keeps its name");
}

// A reserved word or a built-in type name never enters the table. An ordinary identifier does.
void TestKeywordStripping(TestRunner& runner)
{
    runner.BeginSection("keyword stripping");

    constexpr std::string_view k_Source = R"(
RWStructuredBuffer<float> gBuffer;
void doWork(uint count)
{
    for (uint i = 0; i < count; i = i + 1)
    {
        gBuffer[i] = float(i) * 2.0;
    }
}
)";

    SymbolTable table;
    table.AddSource("kernel", k_Source);

    runner.Check(Missing(table, { "kernel" }, { "RWStructuredBuffer", "float", "void", "uint", "for" }).size() == 5u,
                 "reserved words and built-in type names are stripped");
    runner.Check(Missing(table, { "kernel" }, { "gBuffer", "doWork", "count", "i" }).empty(),
                 "ordinary identifiers stay tokens");
}

// The scenario the table exists for: an axis declared in one module and used in another. A used axis is
// found, an unused axis is reported missing, and a declaration alone is not a use.
void TestAxisReachability(TestRunner& runner)
{
    runner.BeginSection("axis reachability");

    constexpr std::string_view k_Common = R"(
module common;
[ls_boolean_axis] extern static const bool USE_WAVE_OPS;
[ls_axis_values("128, 256")] extern static const uint FFT_SIZE;
[ls_axis_values("16, 32")] extern static const uint UNUSED_TILE;
)";

    constexpr std::string_view k_Main = R"(
import common;
RWStructuredBuffer<float> gData;
[numthreads(64, 1, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    if (USE_WAVE_OPS)
    {
        gData[tid.x] = float(FFT_SIZE);
    }
}
)";

    SymbolTable table;
    table.AddSource("common", k_Common);
    table.AddSource("main", k_Main);

    const std::vector<std::string_view> missing =
        Missing(table, { "common", "main" }, { "USE_WAVE_OPS", "FFT_SIZE", "UNUSED_TILE" });
    runner.Check(missing.size() == 1u && missing[0] == "UNUSED_TILE",
                 "the used axes are found and only the unused axis is missing");

    runner.Check(Missing(table, { "main" }, { "USE_WAVE_OPS", "FFT_SIZE" }).empty(),
                 "an axis used in a module is found when that module is in scope");

    // The crux: the declaring module holds only its own tokens, and the declaration line was skipped, so
    // the axis is NOT a use in the module that declares it. Without this, every axis would read as used.
    runner.Check(Missing(table, { "common" }, { "USE_WAVE_OPS" }).size() == 1u,
                 "a declaration alone does not count as a use in the declaring module");
}

// A module can be fed several sources, which pool under its name (a root plus its __include'd
// fragments). An unknown module holds no tokens, so every queried token is missing.
void TestAccumulationAndUnknownModule(TestRunner& runner)
{
    runner.BeginSection("accumulation and unknown module");

    SymbolTable table;
    table.AddSource("mod", "alpha beta");
    table.AddSource("mod", "gamma delta"); // pools into "mod" alongside the first source

    runner.Check(Missing(table, { "mod" }, { "alpha", "gamma" }).empty(),
                 "a second AddSource for the same module pools its tokens in");

    runner.Check(Missing(table, { "nonexistent" }, { "alpha" }).size() == 1u,
                 "an unknown module holds no tokens, so the token is missing");
}

} // namespace

int main()
{
    TestRunner runner{ "SymbolTableTests" };
    TestExternStaticConstStripping(runner);
    TestKeywordStripping(runner);
    TestAxisReachability(runner);
    TestAccumulationAndUnknownModule(runner);
    return runner.Report();
}
