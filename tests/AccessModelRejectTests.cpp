#include "TestFormat.hpp"
#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "driver/CookerSteps.hpp"
#include "emit/OutputSink.hpp"
#include "emit/ShaderManifestEmitter.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using namespace lodestone;

struct RejectionCase
{
    /** File name inside the asset directory the command line names. */
    std::string_view ModuleFile;
    /** Which `TargetProfile` the cook asks for. */
    std::string_view TargetName;
    /** What `RunCook` must return. `Success` means the module must cook. */
    CookError Expected;
    /** What this row proves, for the failure message. */
    std::string_view Claim;
};

constexpr std::array<RejectionCase, 23u> k_Cases
{
    RejectionCase{ .ModuleFile = "PointerMember.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::PointerTypeNotSupported,
                   .Claim = "a pointer member under a bound access model fails the cook" },
    // Slang's metadata reports the unused resource as used (shader-slang/slang#13789), and the workaround removes
    // it from the visibility. Without the workaround, this row fails with ReflectionMismatch.
    RejectionCase{ .ModuleFile = "EntryPointUnusedResource.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::Success,
                   .Claim = "an unread resource beside entry point constants cooks (shader-slang/slang#13789)" },
    RejectionCase{ .ModuleFile = "SpecConstantWorkgroupSize.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::CodeGenerationFailed,
                   .Claim = "a codegen error fails the cook, even when Slang returns a success code" },
    // The control arm. Without it, a check that rejects every module would pass this suite.
    RejectionCase{ .ModuleFile = "EntryPointParams.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::Success,
                   .Claim = "a module with no pointer still cooks" },
    // the rule that a shim extern (phase F step F2) is exempt from: an interface axis is a local, so it holds no resource
    RejectionCase{ .ModuleFile = "InterfaceAxisResourceMember.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::SlangInterfaceHasResourceMember,
                   .Claim = "an implementation of an interface axis that holds a resource fails the cook" },
    // a storage shim extern links the format of one axis value, so the axis must exist and have a value in every variant
    RejectionCase{ .ModuleFile = "Shims/ShimStorageNoAttribute.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::ShimStorageFormatMissing,
                   .Claim = "a storage shim extern with no format axis fails the cook" },
    RejectionCase{ .ModuleFile = "Shims/ShimStorageUnknownAxis.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::ShimStorageFormatAxisUnknown,
                   .Claim = "a format axis that the module does not declare fails the cook" },
    RejectionCase{ .ModuleFile = "Shims/ShimStorageWrongAxis.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::ShimStorageFormatAxisInvalid,
                   .Claim = "a format axis over another enum fails the cook" },
    RejectionCase{ .ModuleFile = "Shims/ShimStorageGatedAxis.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::ShimStorageFormatAxisInvalid,
                   .Claim = "a gated format axis fails the cook" },
    // the code reads each vertex input, so reflection must record it with the same type
    RejectionCase{ .ModuleFile = "VertexInputs/VertexInputsMatch.slang",
                   .TargetName = "spirv",
                   .Expected = CookError::Success,
                   .Claim = "vertex inputs that reflection records cook on spirv" },
    RejectionCase{ .ModuleFile = "VertexInputs/VertexInputsMatch.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::Success,
                   .Claim = "vertex inputs that reflection records cook on wgsl" },
    // Slang's SPIR-V emit crashes on this parameter, so the check runs before code generation on every profile
    RejectionCase{ .ModuleFile = "VertexInputs/VaryingHoldsResource.slang",
                   .TargetName = "spirv",
                   .Expected = CookError::SlangVaryingParameterHoldsResource,
                   .Claim = "a vertex parameter that holds a buffer shim fails the spirv cook, with no crash" },
    RejectionCase{ .ModuleFile = "VertexInputs/VaryingHoldsResource.slang",
                   .TargetName = "spirv-modern-mutable",
                   .Expected = CookError::SlangVaryingParameterHoldsResource,
                   .Claim = "a vertex parameter that holds a pointer shim fails the spirv-modern-mutable cook" },
    RejectionCase{ .ModuleFile = "VertexInputs/VaryingHoldsResource.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::SlangVaryingParameterHoldsResource,
                   .Claim = "a vertex parameter that holds a buffer shim fails the wgsl cook" },
    // the scalar layout of a shim element must equal its std430 layout, so a renderer writes one buffer for every profile
    RejectionCase{ .ModuleFile = "Shims/ShimPadded.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::Success,
                   .Claim = "a shim element with explicit padding cooks on wgsl" },
    RejectionCase{ .ModuleFile = "Shims/ShimPadded.slang",
                   .TargetName = "spirv",
                   .Expected = CookError::Success,
                   .Claim = "a shim element with explicit padding cooks on spirv" },
    RejectionCase{ .ModuleFile = "Shims/ShimPadded.slang",
                   .TargetName = "spirv-modern-mutable",
                   .Expected = CookError::Success,
                   .Claim = "a shim element with explicit padding cooks on spirv-modern-mutable" },
    RejectionCase{ .ModuleFile = "Shims/ShimPaddingUv.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::ShimElementImplicitPadding,
                   .Claim = "a float2 after a float3 in a shim element fails the wgsl cook" },
    RejectionCase{ .ModuleFile = "Shims/ShimPaddingUv.slang",
                   .TargetName = "spirv",
                   .Expected = CookError::ShimElementImplicitPadding,
                   .Claim = "a float2 after a float3 in a shim element fails the spirv cook" },
    RejectionCase{ .ModuleFile = "Shims/ShimPaddingUv.slang",
                   .TargetName = "spirv-modern-mutable",
                   .Expected = CookError::ShimElementImplicitPadding,
                   .Claim = "a float2 after a float3 in a shim element fails the spirv-modern-mutable cook" },
    RejectionCase{ .ModuleFile = "Shims/ShimPaddingTwoFloat3.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::ShimElementImplicitPadding,
                   .Claim = "a float3 after a float3 in a shim element fails the wgsl cook" },
    RejectionCase{ .ModuleFile = "Shims/ShimPaddingTwoFloat3.slang",
                   .TargetName = "spirv",
                   .Expected = CookError::ShimElementImplicitPadding,
                   .Claim = "a float3 after a float3 in a shim element fails the spirv cook" },
    RejectionCase{ .ModuleFile = "Shims/ShimPaddingTwoFloat3.slang",
                   .TargetName = "spirv-modern-mutable",
                   .Expected = CookError::ShimElementImplicitPadding,
                   .Claim = "a float3 after a float3 in a shim element fails the spirv-modern-mutable cook" },
};

/** Cooks one module into a `MemoryOutputSink`, so that rows write no files and cannot collide. */
struct RowOutcome
{
    CookError Result{ CookError::Invalid };
    std::vector<Diagnostic> Records;
};

RowOutcome CookOneModule(const std::filesystem::path& module_path, std::string_view target_name)
{
    CookerOptions options;
    options.ModulePaths.push_back(module_path);
    options.TargetNames.emplace_back(target_name);
    options.OutputPath = "AccessModelReject";

    MemoryOutputSink sink;
    RecordingDiagnosticSink diagnostics;
    const CookResult<CookStatistics> result = RunCook(options, sink, diagnostics);
    return RowOutcome{ .Result = result ? CookError::Success : result.error(), .Records = std::move(diagnostics.Records()) };
}

/** True when an error record points at the module, by its file or in its text, so the author can find the cause. */
bool AnErrorNamesTheModule(const std::vector<Diagnostic>& records, const std::filesystem::path& module_path)
{
    const std::string fileName = module_path.filename().string();
    const std::string stem = module_path.stem().string();
    return std::ranges::any_of(records,
                               [&](const Diagnostic& record)
                               {
                                   return record.Severity == DiagnosticSeverity::Error &&
                                          (record.File.ends_with(fileName) || record.Message.contains(stem));
                               });
}

void PrintRecords(const std::vector<Diagnostic>& records)
{
    for (const Diagnostic& record : records)
    {
        std::println(stderr, "      record: {} {} '{}' {}", tests::FormatForTest(record.Severity), record.File, record.Message, record.Context);
    }
}

/** Cooks one module into a folder on disk, through the sink the console uses. */
CookError CookOneModuleToDisk(const std::filesystem::path& module_path,
                              std::string_view target_name,
                              const std::filesystem::path& output_directory)
{
    CookerOptions options;
    options.ModulePaths.push_back(module_path);
    options.TargetNames.emplace_back(target_name);
    options.OutputPath = output_directory;

    FileOutputSink sink{ output_directory };
    const CookResult<CookStatistics> result = RunCook(options, sink);
    return result ? CookError::Success : result.error();
}

/** True when a temporary file of an atomic write is left in the folder. */
bool HasTemporaryFile(const std::filesystem::path& directory)
{
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator{ directory })
    {
        if (entry.path().extension() == ".tmp")
        {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    // run-tests.bat doesn't give any arguments to each .exe, but we do have CMake set 
    // `LODESTONE_TEST_ASSET_DIR` so that we can still find the directory for our test shaders
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    lodestone::tests::TestRunner runner{ "AccessModelReject" };

    for (const RejectionCase& testCase : k_Cases)
    {
        runner.BeginSection(testCase.Claim);

        const std::filesystem::path modulePath = assetDirectory / testCase.ModuleFile;
        if (!std::filesystem::exists(modulePath))
        {
            runner.Check(false, "the module named in the row is on disk");
            continue;
        }

        const RowOutcome outcome = CookOneModule(modulePath, testCase.TargetName);
        bool passed = runner.CheckEqual(outcome.Result, testCase.Expected, std::format("{}: {}", testCase.ModuleFile, testCase.Claim));
        if (testCase.Expected != CookError::Success)
        {
            passed = runner.Check(AnErrorNamesTheModule(outcome.Records, modulePath),
                                  std::format("{}: an error record names the module", testCase.ModuleFile)) && passed;
        }
        if (!passed)
        {
            PrintRecords(outcome.Records);
        }
    }

    // the mismatch is a warning on the entry point, and the error does not name the module, so these rows check the warning
    for (const std::string_view targetName : { "spirv", "wgsl" })
    {
        runner.BeginSection(std::format("a vertex input that reflection does not record fails the {} cook", targetName));

        const std::filesystem::path modulePath = assetDirectory / "VertexInputs/VertexInputNoSemantic.slang";
        const RowOutcome outcome = CookOneModule(modulePath, targetName);
        runner.CheckEqual(outcome.Result, CookError::ReflectionMismatch, "the cook fails with ReflectionMismatch");
        runner.Check(std::ranges::any_of(outcome.Records,
                                         [](const Diagnostic& record)
                                         {
                                             return record.Severity == DiagnosticSeverity::Warning &&
                                                    record.Message.contains("vertex input location 1: the code has f32x1 and "
                                                                            "reflection has nothing");
                                         }),
                     "a warning states the vertex input that reflection does not record");
    }

    runner.BeginSection("a failed cook leaves no manifest of an earlier cook");
    // a reader that finds an old manifest takes it for the output of the cook that failed
    const std::filesystem::path outputDirectory = std::filesystem::temp_directory_path() / "LodestoneStaleManifestTest";
    std::filesystem::remove_all(outputDirectory);
    std::filesystem::create_directories(outputDirectory);
    const std::filesystem::path manifestPath = outputDirectory / k_ManifestFileName;
    {
        std::ofstream stale{ manifestPath, std::ios::binary };
        stale << "a manifest of an earlier cook";
    }
    const CookError failed = CookOneModuleToDisk(assetDirectory / k_Cases[0].ModuleFile, k_Cases[0].TargetName, outputDirectory);
    runner.CheckEqual(failed, k_Cases[0].Expected, "the cook fails with the error of its row");
    runner.Check(!std::filesystem::exists(manifestPath), "the failed cook removed the old manifest");

    const CookError cooked = CookOneModuleToDisk(assetDirectory / k_Cases[3].ModuleFile, k_Cases[3].TargetName, outputDirectory);
    runner.Check(cooked == CookError::Success && std::filesystem::exists(manifestPath),
                 "a cook that succeeds writes the manifest again");
    runner.Check(!HasTemporaryFile(outputDirectory), "each atomic write renamed its temporary file into place");
    std::filesystem::remove_all(outputDirectory);

    return runner.Report();
}
