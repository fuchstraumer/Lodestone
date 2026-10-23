#include "compile/SymbolTable.hpp"
#include "TestHarness.hpp"
#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using lodestone::SymbolTable;
using lodestone::ExternConstantDeclaration;
using lodestone::DerivedConstantDeclaration;
using lodestone::tests::TestRunner;

namespace
{

std::vector<std::string_view> Missing(const SymbolTable& table,
                                      std::vector<std::string> modules,
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

constexpr std::string_view k_TestSourcePath = "test.slang";

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
    table.AddSource("decls", k_TestSourcePath, k_Source);

    runner.CheckEqual(Missing(table, { "decls" }, { "AAA", "BBB", "CCC" }).size(), 3u,
                      "every arrangement of extern static const is stripped");
    runner.Check(Missing(table, { "decls" }, { "DDD", "EEE", "FFF" }).empty(),
                 "a declaration with only one of the three keywords keeps its name");
}

// A comment declares nothing: the doc comment of a builtin module shows an axis declaration as an example.
void TestCommentsDeclareNothing(TestRunner& runner)
{
    runner.BeginSection("comments");

    constexpr std::string_view k_Source = R"(
// extern static const uint IN_LINE_COMMENT = 1u;
extern static const uint REAL = 2u; // extern static const uint AFTER_CODE = 3u;
/* extern static const uint IN_ONE_LINE_BLOCK = 4u; */
/** a block over several lines
 *  extern static const uint IN_MULTI_LINE_BLOCK = 5u;
 *  extern struct InBlockStruct : IReadBuffer<float4>;
 */
extern static const uint AFTER_BLOCK = 6u;
[ls_axis_values("a//b")] extern static const uint WITH_SLASHES = 7u;
)";

    SymbolTable table;
    table.AddSource("comments", k_TestSourcePath, k_Source);
    const std::vector<ExternConstantDeclaration> externs = table.ExternConstantsForModule("comments");
    runner.Check(externs.size() == 3u && DeclarationMatches(externs, "REAL", "2u") &&
                     DeclarationMatches(externs, "AFTER_BLOCK", "6u") && DeclarationMatches(externs, "WITH_SLASHES", "7u"),
                 "only the three declarations outside a comment are read, and a string with two slashes is not a comment");
    runner.Check(table.ExternStructsForModule("comments").empty(), "an extern struct in a block comment is not read");
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
    table.AddSource("kernel", k_TestSourcePath, k_Source);

    runner.CheckEqual(Missing(table, { "kernel" }, { "RWStructuredBuffer", "float", "void", "uint", "for" }).size(), 5u,
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
[ls_axis_boolean] extern static const bool USE_WAVE_OPS = false;
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
    table.AddSource("common", k_TestSourcePath, k_Common);
    table.AddSource("main", k_TestSourcePath, k_Main);

    const std::vector<std::string_view> missing =
        Missing(table, { "common", "main" }, { "USE_WAVE_OPS", "FFT_SIZE", "UNUSED_TILE" });
    if (runner.CheckEqual(missing.size(), 1u, "the used axes are found and only the unused axis is missing"))
    {
        runner.CheckEqual(missing[0], "UNUSED_TILE", "the used axes are found and only the unused axis is missing");
    }

    runner.Check(Missing(table, { "main" }, { "USE_WAVE_OPS", "FFT_SIZE" }).empty(),
                 "an axis used in a module is found when that module is in scope");

    // The crux: the declaring module contains only its own tokens, and the declaration line was skipped, so
    // the axis is NOT a use in the module that declares it. Without this, every axis would read as used.
    runner.CheckEqual(Missing(table, { "common" }, { "USE_WAVE_OPS" }).size(), 1u,
                      "a declaration alone does not count as a use in the declaring module");
}

// A module can be fed several sources, which pool under its name (a root plus its __include'd
// fragments). Unknown modules contain no tokens, so every queried token is missing.
void TestAccumulationAndUnknownModule(TestRunner& runner)
{
    runner.BeginSection("accumulation and unknown module");

    SymbolTable table;
    table.AddSource("mod", k_TestSourcePath, "alpha beta");
    table.AddSource("mod", k_TestSourcePath, "gamma delta"); // pools into "mod" alongside the first source

    runner.Check(Missing(table, { "mod" }, { "alpha", "gamma" }).empty(),
                 "a second AddSource for the same module pools its tokens in");

    runner.CheckEqual(Missing(table, { "nonexistent" }, { "alpha" }).size(), 1u,
                      "an unknown module holds no tokens, so the token is missing");
}

void TestDerivedConstantExtraction(TestRunner& runner)
{
    runner.BeginSection("derived constants");

    constexpr std::string_view k_Source = R"(module Lighting;
    extern static const uint GRID_X = 4u;
    static const uint CLUSTER_COUNT = GRID_X * GRID_Y;
    const static uint LIGHT_SLOTS = CLUSTER_COUNT * 16u;
    staticconst_t SUBSTRING_ONLY = 3u;
    static const uint SIZES[2] = { 1, 2 };
    void main() { static const uint LOCAL_ONLY = 7u; }
    )";

    SymbolTable table;
    table.AddSource("Lighting", "Lighting.slang", k_Source);

    const std::span<const DerivedConstantDeclaration> derived = table.DerivedConstantsForModule("Lighting");
    auto find = [&](std::string_view name) -> const DerivedConstantDeclaration*
    {
        const auto iter = std::ranges::find(derived, name, &DerivedConstantDeclaration::Name);
        return iter == derived.end() ? nullptr : &*iter;
    };

    runner.CheckEqual(derived.size(), 3u, "three static const lines with an initializer are captured");
    const DerivedConstantDeclaration* clusterCount = find("CLUSTER_COUNT");
    runner.Check(clusterCount != nullptr && clusterCount->Expression == "GRID_X * GRID_Y",
                 "the expression is kept as written, not evaluated");
    runner.Check(clusterCount != nullptr && clusterCount->SourcePath == "Lighting.slang" && clusterCount->SourceLine == 3u,
                 "the source path and the line number are recorded");
    runner.Check(find("LIGHT_SLOTS") != nullptr, "the keyword order does not matter");
    runner.CheckEqual(find("GRID_X"), nullptr, "an extern constant is not a derived constant");
    runner.CheckEqual(find("SUBSTRING_ONLY"), nullptr, "`static` and `const` must be whole tokens, not parts of a type name");
    runner.Check(find("LOCAL_ONLY") != nullptr, "a function-local constant is captured, for the scope filter to remove");
    runner.CheckEqual(find("SIZES"), nullptr, "an array declaration is skipped, because its name is not before the `=`");
    runner.Check(Missing(table, { "Lighting" }, { "GRID_Y" }).empty(),
                 "the names in a derived expression still count as tokens");
}

// Every declaration is classified by one scan of its keywords as whole tokens. The extern constant check used
// to test substrings of the whole line, so a type name that contained `static` and `const` passed it.
void TestDeclarationKeywordTokens(TestRunner& runner)
{
    runner.BeginSection("declaration keywords are whole tokens");

    constexpr std::string_view k_Source = R"(module Keywords;
    extern static const staticconst_buffer TYPED = 2u;
    extern staticconst_buffer NOT_STATIC_CONST = 3u;
    )";

    SymbolTable table;
    table.AddSource("Keywords", k_TestSourcePath, k_Source);

    const std::vector<ExternConstantDeclaration> externs = table.ExternConstantsForModule("Keywords");
    runner.Check(externs.size() == 1u && DeclarationMatches(externs, "TYPED", "2u"),
                 "an extern static const of a type named staticconst_buffer is read with its own name");
    runner.Check(!DeclarationMatches(externs, "NOT_STATIC_CONST", "3u"),
                 "`static` and `const` inside a type name do not make a line an extern static const");
}

void TestExternTypeDefaultExtraction(TestRunner& runner)
{
    runner.BeginSection("extern struct defaults");

    constexpr std::string_view k_Source = R"(module Shading;

    public extern struct SHADE_MODE : IShadeMode = Passthrough;
    extern struct BRDF : IBrdf = Shading::Lambert; // a qualified default
    extern struct NO_DEFAULT : IBrdf;
    struct NotExtern : IBrdf = Lambert;
    extern static const MyStructType COUNT = 4;
    )";

    SymbolTable table;
    table.AddSource("Shading", k_TestSourcePath, k_Source);

    const std::vector<ExternConstantDeclaration> types = table.ExternTypeDefaultsForModule("Shading");
    runner.CheckEqual(types.size(), 2u, "only extern struct declarations with an initializer are recognized");
    runner.Check(DeclarationMatches(types, "SHADE_MODE", "Passthrough"),
                 "the struct name comes from after `struct`, not from before `=`");
    runner.Check(DeclarationMatches(types, "BRDF", "Shading::Lambert"), "a qualified default keeps its qualifier");

    const std::vector<ExternConstantDeclaration> constants = table.ExternConstantsForModule("Shading");
    runner.Check(constants.size() == 1u && DeclarationMatches(constants, "COUNT", "4"),
                 "a type named like MyStructType does not count as the struct keyword");

    runner.Check(Missing(table, { "Shading" }, { "SHADE_MODE" }).empty(),
                 "an extern struct name still counts as a token, as it did before defaults were read");
}

// Now the symbol table does double duty as the extern const scanner, make sure it matches the expected behavior
// that used to be in the ExternConstScanner.
void TestExternConstExtraction(TestRunner& runner)
{
    runner.BeginSection("extern const extraction");

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
    table.AddSource("OceanFft", k_TestSourcePath, k_Source);

    const std::vector<ExternConstantDeclaration> externs = table.ExternConstantsForModule("OceanFft");
    runner.CheckEqual(externs.size(), 4u, "A line needs extern static const and = to be recognized.");
    runner.Check(DeclarationMatches(externs, "IFFT_SIZE", "256"), "an integer default reads back");
    runner.Check(DeclarationMatches(externs, "IFFT_USE_WAVE_OPS", "false"), "a bool default reads back");
    runner.Check(DeclarationMatches(externs, "IFFT_NUM_WAVE_CASCADES", "IFFT_SIZE * 4"),
                 "a default that names an earlier constant keeps its whole expression");
    runner.Check(DeclarationMatches(externs, "ANOTHER_HEX_CONSTANT", "0x9e3779b9"), "a hex constant reads back correctly");

    constexpr std::string_view k_IdentifierExtractionSrc = "extern static const uint SPACED    =   42 ; \n";
    table.AddSource("Identifiers", k_TestSourcePath, k_IdentifierExtractionSrc);
    const std::vector<ExternConstantDeclaration> identifiersExterns = table.ExternConstantsForModule("Identifiers");
    runner.CheckEqual(identifiersExterns.size(), 1u, "Should extract the single extern static const declaration.");
    runner.Check(DeclarationMatches(identifiersExterns, "SPACED", "42"), "The extracted declaration should match the source.");
}

} // namespace

int main()
{
    TestRunner runner{ "SymbolTableTests" };
    TestExternStaticConstStripping(runner);
    TestKeywordStripping(runner);
    TestCommentsDeclareNothing(runner);
    TestAxisReachability(runner);
    TestAccumulationAndUnknownModule(runner);
    TestExternConstExtraction(runner);
    TestExternTypeDefaultExtraction(runner);
    TestDerivedConstantExtraction(runner);
    TestDeclarationKeywordTokens(runner);
    return runner.Report();
}
