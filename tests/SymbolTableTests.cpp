#include "compile/SymbolTable.hpp"
#include "TestHarness.hpp"
#include <algorithm>
#include <string_view>
#include <vector>

using lodestone::SymbolTable;
using lodestone::ExternConstantDeclaration;
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

bool DeclarationMatches(const std::vector<ExternConstantDeclaration>& externs,
                        std::string_view name,
                        std::string_view value)
{
    auto iter = std::ranges::find_if(externs, [&](const ExternConstantDeclaration& decl)
    {
        return decl.Name == name && decl.Value == value;
    });
    return iter != externs.end();
}

// A whole `extern static const` line is dropped, in any keyword order. A line that carries only some of
// the three keywords is an ordinary declaration, and its name stays a token.
void TestExternStaticConstStripping(TestRunner& runner)
{
    runner.BeginSection("extern static const stripping");

    constexpr std::string_view k_Source = R"(
[ls_axis_values("1, 2")] extern static const uint AAA = 32u;
[ls_axis_values("3, 4")] static extern const uint BBB = 1.0f;
[ls_axis_values("5, 6")] extern const static uint CCC = 0xFFFF;
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
[ls_boolean_axis] extern static const bool USE_WAVE_OPS = false;
[ls_axis_values("128, 256")] extern static const uint FFT_SIZE = 256;
[ls_axis_values("16, 32")] extern static const uint UNUSED_TILE = 16;
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

// Now the symbol table does double duty as the extern const scanner, make sure it matches the expected behavior
// that used to be in the ExternConstScanner.
void TestExternConstExtraction(TestRunner& runner)
{
    constexpr std::string_view k_Source = R"(module OceanFft;

    extern static const uint IFFT_SIZE = 256;
    extern const static bool IFFT_USE_WAVE_OPS = false;
    const static extern uint IFFT_NUM_WAVE_CASCADES = IFFT_SIZE * 4;
    static extern const uint ANOTHER_HEX_CONSTANT = 0x9e3779b9;

    static const uint NOT_EXTERN = 8;
    extern static const uint IFFT_SIZE_LOG2;

    RWStructuredBuffer<float4> OutputSpectrum;
    )";

    SymbolTable table;
    table.AddSource("OceanFft", k_Source);

    const std::vector<ExternConstantDeclaration> externs = table.ExternConstantsForModule("OceanFft");
    runner.Check(externs.size() == 4u, "A line needs extern static const and = to be recognized.");
    runner.Check(DeclarationMatches(externs, "IFFT_SIZE", "256"), "an integer default reads back");
    runner.Check(DeclarationMatches(externs, "IFFT_USE_WAVE_OPS", "false"), "a bool default reads back");
    runner.Check(DeclarationMatches(externs, "IFFT_NUM_WAVE_CASCADES", "IFFT_SIZE * 4"),
                 "a default that names an earlier constant keeps its whole expression");
    runner.Check(DeclarationMatches(externs, "ANOTHER_HEX_CONSTANT", "0x9e3779b9"), "a hex constant reads back correctly");

    constexpr std::string_view k_IdentifierExtractionSrc = "extern static const uint SPACED    =   42 ; \n";
    table.AddSource("Identifiers", k_IdentifierExtractionSrc);
    const std::vector<ExternConstantDeclaration> identifiersExterns = table.ExternConstantsForModule("Identifiers");
    runner.Check(identifiersExterns.size() == 1u, "Should extract the single extern static const declaration.");
    runner.Check(DeclarationMatches(identifiersExterns, "SPACED", "42"), "The extracted declaration should match the source.");
}

} // namespace

int main()
{
    TestRunner runner{ "SymbolTableTests" };
    TestExternStaticConstStripping(runner);
    TestKeywordStripping(runner);
    TestAxisReachability(runner);
    TestAccumulationAndUnknownModule(runner);
    TestExternConstExtraction(runner);
    return runner.Report();
}
