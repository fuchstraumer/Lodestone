#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "emit/OutputSink.hpp"
#include "emit/ShaderManifestEmitter.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// sizes and offsets computed by hand from std140 (WGSL uniform buffer) and std430 (SPIR-V push constants)

namespace
{

using namespace lodestone;

struct ExpectedMember
{
    std::string_view Name;
    uint32_t Offset;
    uint32_t Size;
};

struct ExpectedConstants
{
    std::string_view EntryPoint;
    // zero means no constants: the entry point reads no ordinary data
    uint32_t WgslByteSize;
    uint32_t SpirvByteSize;
    std::span<const ExpectedMember> Members;
};

constexpr std::array<ExpectedMember, 2u> k_MixedMembers
{
    ExpectedMember{ .Name = "material.TintIndex", .Offset = 0u, .Size = 4u },
    ExpectedMember{ .Name = "material.Scale", .Offset = 4u, .Size = 4u },
};

constexpr std::array<ExpectedMember, 2u> k_LooseMembers
{
    ExpectedMember{ .Name = "tint", .Offset = 0u, .Size = 16u },
    ExpectedMember{ .Name = "count", .Offset = 16u, .Size = 4u },
};

// std140 rounds a struct up to 16 bytes, and std430 does not: MixedCS is 16 and 8. A float4 aligns both layouts to
// 16, so LooseCS is 32 in both.
constexpr std::array<ExpectedConstants, 4u> k_Expected
{
    ExpectedConstants{ .EntryPoint = "MixedCS", .WgslByteSize = 16u, .SpirvByteSize = 8u, .Members = k_MixedMembers },
    ExpectedConstants{ .EntryPoint = "LooseCS", .WgslByteSize = 32u, .SpirvByteSize = 32u, .Members = k_LooseMembers },
    ExpectedConstants{ .EntryPoint = "UnusedCS", .WgslByteSize = 0u, .SpirvByteSize = 0u, .Members = {} },
    ExpectedConstants{ .EntryPoint = "ResourcesOnlyCS", .WgslByteSize = 0u, .SpirvByteSize = 0u, .Members = {} },
};

CookResult<CookStatistics> CookIntoMemory(const std::filesystem::path& module_path,
                                          std::span<const std::string_view> targets,
                                          MemoryOutputSink& sink)
{
    CookerOptions options;
    options.ModulePaths.push_back(module_path);
    for (const std::string_view target : targets)
    {
        options.TargetNames.emplace_back(target);
    }
    options.OutputPath = "EntryPointConstants";
    return RunCook(options, sink);
}

/** Copies the manifest out of the sink into a buffer that the reader can map in place. */
std::vector<std::byte> ManifestBytes(const MemoryOutputSink& sink)
{
    const std::map<std::string, std::string>& artifacts = sink.GetArtifacts();
    const auto manifest = artifacts.find(std::string{ k_ManifestFileName });
    if (manifest == artifacts.end())
    {
        return {};
    }

    std::vector<std::byte> bytes(manifest->second.size());
    std::memcpy(bytes.data(), manifest->second.data(), bytes.size());
    return bytes;
}

bool MembersMatch(manifest::UniformMemberRange members, std::span<const ExpectedMember> expected)
{
    if (members.Size() != expected.size())
    {
        return false;
    }

    for (uint32_t i = 0u; i < members.Size(); ++i)
    {
        const manifest::UniformMemberView member = members[i];
        if (member.Name() != expected[i].Name || member.Offset() != expected[i].Offset ||
            member.Size() != expected[i].Size)
        {
            return false;
        }
    }
    return true;
}

/** Checks every entry point of one environment against `k_Expected`. The WGSL constants are a uniform buffer in the
 * layout, and the SPIR-V constants are a push constant block that the layout does not contain. */
void CheckEnvironment(tests::TestRunner& runner, const manifest::EnvironmentView& environment, bool is_wgsl)
{
    const manifest::VariantView variant = environment.VariantAt(0u);
    for (const ExpectedConstants& expected : k_Expected)
    {
        const manifest::ManifestResult<uint32_t> entryPointIndex =
            environment.Module().FindEntryPointIndex(expected.EntryPoint);
        runner.Check(entryPointIndex.has_value(), "the entry point is in the module");
        if (!entryPointIndex)
        {
            continue;
        }

        const manifest::EntryPointInstanceView instance = variant.EntryPoint(*entryPointIndex);
        const manifest::EntryPointConstantsView constants = instance.Constants();
        const uint32_t expectedSize = is_wgsl ? expected.WgslByteSize : expected.SpirvByteSize;
        const EntryPointConstantsDelivery expectedDelivery =
            expectedSize == 0u ? EntryPointConstantsDelivery::None
                               : (is_wgsl ? EntryPointConstantsDelivery::UniformBuffer
                                          : EntryPointConstantsDelivery::PushConstant);

        if (constants.Delivery() != expectedDelivery || constants.ByteSize() != expectedSize)
        {
            std::println(stderr,
                         "[EntryPointConstants] {} {}: expected {} bytes, and the manifest has {} bytes",
                         is_wgsl ? "wgsl" : "spirv",
                         expected.EntryPoint,
                         expectedSize,
                         constants.ByteSize());
        }
        runner.Check(constants.Delivery() == expectedDelivery, "the delivery follows the target and the use");
        runner.Check(constants.ByteSize() == expectedSize, "the byte size follows the target's layout rules");
        runner.Check(MembersMatch(constants.Members(), expected.Members), "each member has its name, offset, and size");

        bool containerInLayout = false;
        for (const manifest::ResolvedResource resource : instance.Layout())
        {
            containerInLayout = containerInLayout || resource.Name() == "entryPointParams";
        }
        if (expectedDelivery == EntryPointConstantsDelivery::UniformBuffer)
        {
            const manifest::ResolvedResource container = instance.Layout()[constants.BindingPosition()];
            runner.Check(container.Name() == "entryPointParams" && container.Kind() == BindingKind::UniformBuffer &&
                             container.ByteSize() == expectedSize,
                         "the WGSL constants resolve to the entryPointParams uniform buffer in the layout");
        }
        else
        {
            runner.Check(!containerInLayout, "constants with no uniform buffer leave no binding in the layout");
        }
    }

    // the resource in the struct parameter is a binding of its own on both targets
    const manifest::ManifestResult<uint32_t> mixed = environment.Module().FindEntryPointIndex("MixedCS");
    bool tintsInLayout = false;
    if (mixed)
    {
        for (const manifest::ResolvedResource resource : variant.EntryPoint(*mixed).Layout())
        {
            tintsInLayout =
                tintsInLayout || (resource.Name() == "Tints" && resource.Kind() == BindingKind::StorageBuffer);
        }
    }
    runner.Check(tintsInLayout, "a resource beside the constants in a struct parameter is a binding");
}

} // namespace

int main(int argc, char** argv)
{
    // run-tests.bat gives no arguments, so CMake sets LODESTONE_TEST_ASSET_DIR as the fallback
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    tests::TestRunner runner{ "EntryPointConstants" };

    runner.BeginSection("the entry point constants of each target read back with hand-computed layouts");
    constexpr std::array<std::string_view, 2u> k_BothTargets{ "wgsl", "spirv" };
    MemoryOutputSink sink;
    const CookResult<CookStatistics> cooked =
        CookIntoMemory(assetDirectory / "EntryPointConstants.slang", k_BothTargets, sink);
    runner.Check(cooked.has_value(), "the module cooks for both targets");
    const std::vector<std::byte> bytes = ManifestBytes(sink);
    const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(bytes);
    runner.Check(bundle.has_value(), "the manifest opens");
    if (!bundle.has_value())
    {
        return runner.Report();
    }

    for (uint32_t profile = 0u; profile < k_BothTargets.size(); ++profile)
    {
        const manifest::ManifestResult<manifest::EnvironmentView> environment = bundle->OpenEnvironment(profile, 0u);
        runner.Check(environment.has_value(), "the environment opens");
        if (environment.has_value())
        {
            CheckEnvironment(runner, environment.value(), environment->CodeFormat() == ShaderCodeFormat::Wgsl);
        }
    }

    runner.BeginSection("a push constant block above the guaranteed 128 bytes fails the SPIR-V cook");
    constexpr std::array<std::string_view, 1u> k_Spirv{ "spirv" };
    constexpr std::array<std::string_view, 1u> k_Wgsl{ "wgsl" };
    MemoryOutputSink spirvSink;
    const CookResult<CookStatistics> spirvLarge =
        CookIntoMemory(assetDirectory / "EntryPointConstantsTooLarge.slang", k_Spirv, spirvSink);
    runner.Check(!spirvLarge.has_value() && spirvLarge.error() == CookError::EntryPointConstantsTooLarge,
                 "144 bytes of push constants fail with EntryPointConstantsTooLarge");
    MemoryOutputSink wgslSink;
    const CookResult<CookStatistics> wgslLarge =
        CookIntoMemory(assetDirectory / "EntryPointConstantsTooLarge.slang", k_Wgsl, wgslSink);
    runner.Check(wgslLarge.has_value(), "the same data cooks for WGSL, which delivers it in a uniform buffer");

    return runner.Report();
}
