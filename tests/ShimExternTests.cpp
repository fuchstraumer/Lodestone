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

#include <array>
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
        runner.Check(Prepare(assetDirectory / "ShimExterns.slang", cacheDirectory, sink, compiler) == CookError::Success,
                     "a module with externs and no export loads");
        const PermutationSpace emptySpace{ std::vector<PermutationAxis>{} };
        const CookResult<RawModule> raw = compiler.PrepareRawModule(emptySpace);
        runner.Check(raw.has_value(), "the raw module prepares");
        if (raw.has_value())
        {
            runner.Check(raw->ShimExterns.size() == k_Expected.size(),
                         "five shim externs: not the commented one, and not the extern of another interface");
            for (size_t i = 0u; i < raw->ShimExterns.size() && i < k_Expected.size(); ++i)
            {
                const RawShimExtern& found = raw->ShimExterns[i];
                const ExpectedExtern& expected = k_Expected[i];
                runner.Check(found.Name == expected.Name && found.Interface == expected.Interface &&
                                 found.TypeArguments == expected.TypeArguments && found.Class == expected.Class &&
                                 found.SourceLine == expected.SourceLine,
                             "each extern has its name, interface, type argument, resource class, and line");
                const bool hasArgument = !expected.TypeArguments.empty();
                runner.Check(found.ArgumentModules.size() == (hasArgument ? 1u : 0u) &&
                                 (!hasArgument || found.ArgumentModules[0] == expected.ArgumentModule),
                             "each type argument has the module that declares it, and a builtin type has none");
            }
        }

        const CookResult<std::vector<RawAxisDeclaration>> axesResult = compiler.BuildAxisDeclarations();
        const std::vector<RawAxisDeclaration> axes = axesResult.has_value() ? *axesResult : std::vector<RawAxisDeclaration>{};
        runner.BeginSection("a shim extern is not an axis");
        runner.Check(axes.size() == 1u && axes[0].Name == "FOG",
                     "the only axis is the one the module declares, so the variant count does not change");
    }

    runner.BeginSection("a shim extern that reflection does not find fails the cook, naming the line");
    {
        RecordingDiagnosticSink sink;
        SlangCompiler compiler;
        runner.Check(Prepare(assetDirectory / "ShimExternHidden.slang", cacheDirectory, sink, compiler) == CookError::Success,
                     "the module loads");
        const PermutationSpace emptySpace{ std::vector<PermutationAxis>{} };
        const CookResult<RawModule> raw = compiler.PrepareRawModule(emptySpace);
        runner.Check(!raw.has_value() && raw.error() == CookError::ShimExternNotReflected,
                     "an extern inside a namespace is seen by the text reader only");
    }

    runner.BeginSection("reflection reads ls_storage_format on the line before an extern with no default");
    {
        RecordingDiagnosticSink sink;
        SlangCompiler compiler;
        runner.Check(Prepare(assetDirectory / "ShimStorageAxis.slang", cacheDirectory, sink, compiler) == CookError::Success,
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
        runner.Check(raw.has_value() && raw->ShimExterns.size() == 2u, "the storage extern and the buffer extern");
        if (raw.has_value() && raw->ShimExterns.size() == 2u)
        {
            runner.Check(raw->ShimExterns[0].Name == "Destination" && raw->ShimExterns[0].FormatAxis == "LEVEL_FORMAT",
                         "the storage extern has the axis of its attribute");
            runner.Check(raw->ShimExterns[1].Name == "Weights" && raw->ShimExterns[1].FormatAxis.empty(),
                         "an extern with no attribute has no axis");
        }
    }

    std::filesystem::remove_all(cacheDirectory);
    return runner.Report();
}
