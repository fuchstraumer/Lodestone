#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "ShaderLibraryTypes.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SlangCompiler.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationValue.hpp"
#include "target/ShimTable.hpp"
#include "target/StorageFormatTable.hpp"
#include "target/TargetProfile.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace lodestone;

struct ExpectedExtern
{
    std::string_view Name;
    std::string_view Interface;
    std::string_view TypeArguments;
    /** The module that declares the type argument. Empty for a builtin type, and for an interface with no argument. */
    std::string_view ArgumentModule;
    ShimResourceClass Class;
    uint32_t SourceLine;
};

// source order; the commented-out Ghost and the extern of IUnrelated are not shim externs
constexpr std::array<ExpectedExtern, 5u> k_Expected{ {
    { "Points", "IReadBuffer", "PointLight", "ShimExterns", ShimResourceClass::Buffer, 23u },
    { "Tints", "IReadBuffer", "float4", "", ShimResourceClass::Buffer, 24u },
    { "Albedo", "ITexture2DRef", "", "", ShimResourceClass::Texture, 25u },
    { "Linear", "ISamplerRef", "", "", ShimResourceClass::Sampler, 26u },
    { "Scratch", "IReadWriteBuffer", "float4", "", ShimResourceClass::Buffer, 27u },
} };

/** Starts the bootstrap compile of one module, the way the driver does for the `spirv` profile. */
CookError Prepare(const std::filesystem::path& module_path,
                  const std::filesystem::path& cache_directory,
                  RecordingDiagnosticSink& sink,
                  SlangCompiler& compiler)
{
    SlangCompilerCreateInfo createInfo;
    createInfo.ModulePath = module_path;
    createInfo.ModuleCacheDirectory = cache_directory;
    createInfo.MultithreadVariantBuild = false;
    createInfo.ExpectedBatchSize = 1u;
    createInfo.Language = TargetLanguage::Spirv;
    createInfo.SlangProfileName = "spirv_1_5";
    createInfo.AccessModel = PlacementKind::Bound;
    return compiler.Initialize(createInfo, sink);
}

} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    const std::filesystem::path cacheDirectory = std::filesystem::temp_directory_path() / "LodestoneShimExternTest";
    tests::TestRunner runner{ "ShimExtern" };

    runner.BeginSection("an extern of a shim interface is read from the source and confirmed by reflection");
    {
        RecordingDiagnosticSink sink;
        SlangCompiler compiler;
        runner.CheckEqual(Prepare(assetDirectory / "ShimExterns.slang", cacheDirectory, sink, compiler), CookError::Success,
                          "a module with externs and no export loads");
        const PermutationSpace emptySpace{ std::vector<PermutationAxis>{} };
        const CookResult<RawModule> raw = compiler.PrepareRawModule(emptySpace);
        if (runner.CheckOk(raw, "the raw module prepares"))
        {
            runner.CheckEqual(raw->ShimExterns.size(), k_Expected.size(),
                              "five shim externs: not the commented one, and not the extern of another interface");
            for (size_t i = 0u; i < raw->ShimExterns.size() && i < k_Expected.size(); ++i)
            {
                const RawShimExtern& found = raw->ShimExterns[i];
                const ExpectedExtern& expected = k_Expected[i];
                const std::string claim = std::format("{}: each extern has its name, interface, type argument, resource class, and line", expected.Name);
                runner.CheckEqual(found.Name, expected.Name, claim);
                runner.CheckEqual(found.Interface, expected.Interface, claim);
                runner.CheckEqual(found.TypeArguments, expected.TypeArguments, claim);
                runner.CheckEqual(found.Class, expected.Class, claim);
                runner.CheckEqual(found.SourceLine, expected.SourceLine, claim);
                const bool hasArgument = !expected.TypeArguments.empty();
                runner.Check(found.ArgumentModules.size() == (hasArgument ? 1u : 0u) &&
                                 (!hasArgument || found.ArgumentModules[0] == expected.ArgumentModule),
                             "each type argument has the module that declares it, and a builtin type has none");
            }
        }

        const CookResult<std::vector<RawAxisDeclaration>> axesResult = compiler.BuildAxisDeclarations();
        const std::vector<RawAxisDeclaration> axes = axesResult.has_value() ? *axesResult : std::vector<RawAxisDeclaration>{};
        runner.BeginSection("a shim extern is not an axis");
        const char* axisClaim = "the only axis is the one the module declares, so the variant count does not change";
        if (runner.CheckEqual(axes.size(), 1u, axisClaim))
        {
            runner.CheckEqual(axes[0].Name, "FOG", axisClaim);
        }
    }

    runner.BeginSection("a shim extern that reflection does not find fails the cook, naming the line");
    {
        RecordingDiagnosticSink sink;
        SlangCompiler compiler;
        runner.CheckEqual(Prepare(assetDirectory / "ShimExternHidden.slang", cacheDirectory, sink, compiler), CookError::Success,
                          "the module loads");
        const PermutationSpace emptySpace{ std::vector<PermutationAxis>{} };
        const CookResult<RawModule> raw = compiler.PrepareRawModule(emptySpace);
        runner.CheckError(raw, CookError::ShimExternNotReflected,
                          "an extern inside a namespace is seen by the text reader only");

        const std::vector<Diagnostic>& records = sink.Records();
        const auto error = std::ranges::find(records, DiagnosticSeverity::Error, &Diagnostic::Severity);
        if (runner.Check(error != records.end(), "the cook reports an error record"))
        {
            runner.Check(error->File.ends_with("ShimExternHidden.slang"), std::format("the error names the module file: '{}'", error->File));
            runner.CheckEqual(error->Range.StartLine, 9, "the error names the line of the extern");
            runner.Check(error->Message.contains("Samp"), std::format("the error names the extern: '{}'", error->Message));
        }
    }

    runner.BeginSection("reflection reads ls_storage_format on the line before an extern with no default");
    {
        RecordingDiagnosticSink sink;
        SlangCompiler compiler;
        runner.CheckEqual(Prepare(assetDirectory / "ShimStorageAxis.slang", cacheDirectory, sink, compiler), CookError::Success,
                          "the module loads");
        std::vector<RawEnumCase> cases;
        std::vector<PermutationValue> values;
        for (const StorageFormatInfo& format : GetStorageFormats())
        {
            values.push_back(PermutationValue::MakeEnum(static_cast<uint32_t>(cases.size())));
            cases.push_back(RawEnumCase{ .Name = std::string{ format.EnumCase }, .Value = std::ssize(cases) });
        }
        std::vector<PermutationAxis> axes;
        axes.emplace_back("LEVEL_FORMAT",
                          std::move(values),
                          AxisKind::Tuning,
                          EarliestBindingTime::Cook,
                          AxisValueDomain::Enum,
                          "",
                          "StorageFormat",
                          "LodestoneShims",
                          std::move(cases));
        const PermutationSpace formatSpace{ std::move(axes) };
        const CookResult<RawModule> raw = compiler.PrepareRawModule(formatSpace);
        if (runner.CheckOk(raw, "the storage extern and the buffer extern"))
        {
            runner.CheckEqual(raw->ShimExterns.size(), 2u, "the storage extern and the buffer extern");
        }
        if (raw.has_value() && raw->ShimExterns.size() == 2u)
        {
            const char* formatClaim = "the storage extern has the axis of its attribute";
            runner.CheckEqual(raw->ShimExterns[0].Name, "Destination", formatClaim);
            runner.CheckEqual(raw->ShimExterns[0].FormatAxis, "LEVEL_FORMAT", formatClaim);
            runner.Check(raw->ShimExterns[1].Name == "Weights" && raw->ShimExterns[1].FormatAxis.empty(),
                         "an extern with no attribute has no axis");
        }
    }

    std::filesystem::remove_all(cacheDirectory);
    return runner.Report();
}
