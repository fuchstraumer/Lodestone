#include "model/CookedLibrary.hpp"
#include "model/ShaderDataSchema.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationValue.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "emit/ShaderManifestEmitter.hpp"
#include "emit/ShaderManifestVerify.hpp"
#include "VariantKey.hpp"
#include "ManifestFixtures.hpp"
#include "Diagnostics.hpp"
#include "TestHarness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <variant>
#include <span>
#include <string>
#include <utility>
#include <vector>

// the valid case comes from the real emitter, since a hand-written header could agree with a hand-written reader

#ifdef __clang__
#pragma clang diagnostic push
// ignoring these because this is not shipping code
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage-in-libc-call"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

using lodestone::CookedLibrary;
using lodestone::CookedModule;
using lodestone::CookedProfile;
using lodestone::EmitShaderManifest;
using lodestone::VariantKey;
using lodestone::manifest::BundleView;
using lodestone::manifest::EnvironmentDirectoryEntry;
using lodestone::manifest::EnvironmentHeader;
using lodestone::manifest::EnvironmentView;
using lodestone::manifest::ErrorCode;
using lodestone::manifest::Header;
using lodestone::manifest::ManifestResult;

namespace
{

// one axis, so the two variants differ in something the relevance validator can see
const lodestone::PermutationSpace k_SmallSpace{ { lodestone::PermutationAxis{
    "VARIANT",
    { lodestone::PermutationValue{ false }, lodestone::PermutationValue{ true } },
    lodestone::AxisKind::Technique,
    lodestone::EarliestBindingTime::Cook,
    lodestone::AxisValueDomain::Boolean } } };

CookedModule MakeSmallModule()
{
    CookedModule module;
    module.Name = "TestModule";
    module.Space = &k_SmallSpace;
    module.SpaceSize = 2u;

    module.EntryPoints.push_back(
        lodestone::LibraryEntryPoint{ .Name="MainCS", .Stage=lodestone::ShaderStageKind::Compute });

    lodestone::tests::AddFixtureSource(module, lodestone::tests::BytesOf("// wgsl for variant zero"), lodestone::SourceCodeFacts{});
    lodestone::tests::AddFixtureSource(module, lodestone::tests::BytesOf("// wgsl for variant one"), lodestone::SourceCodeFacts{});

    lodestone::ReflectedBinding binding;
    binding.Name = "IfftInput";
    binding.Placement = lodestone::BoundPlacement{ .Group = 0u, .Binding = 1u };
    binding.Kind = lodestone::BindingKind::StorageBuffer;
    binding.ElementStride = 16u;
    binding.Shape = lodestone::ResourceShape::StructuredBuffer;

    module.Resources.push_back(binding);
    module.ResourceLists.push_back(lodestone::ResourceList{ 0u });
    module.FootprintLists.push_back(
        lodestone::FootprintList{ lodestone::BufferFootprint{ .ElementCount = 256u } });
    module.VisibilityLists.push_back(lodestone::VisibilityList{ 0u });

    lodestone::tests::AddDefaultInstanceTables(module);

    for (uint32_t i = 0u; i < 2u; ++i)
    {
        const std::array instances{ lodestone::tests::ComputeInstance(i, 0u) };
        lodestone::LibraryVariant variant = lodestone::tests::MakeFixtureVariant(
            k_SmallSpace,
            i,
            lodestone::PermutationAssignment{ lodestone::PermutationBinding{
                .Axis = &k_SmallSpace.Axes()[0], .Value = lodestone::PermutationValue{ i == 1u } } },
            instances);
        variant.Suffix = i == 0u ? "_A" : "_B";
        variant.Description = i == 0u ? "first" : "second";
        module.Variants.push_back(std::move(variant));
    }

    // One variant key per variant, strictly ascending and parallel to the variant table. The key of a one-axis
    // module is its digit.
    module.VariantKeys = { VariantKey{ 0u }, VariantKey{ 1u } };

    return module;
}

/** One module, cooked for two profiles. The second profile did not cook it, so its environment is empty. */
CookedLibrary MakeSmallLibrary()
{
    CookedLibrary library;
    library.ModuleNames = { "TestModule" };
    library.Profiles = { CookedProfile{ .TargetName = "wgsl",
                                        .Forms = lodestone::PlacementForms{ .Buffer = lodestone::PlacementKind::Bound, .Texture = lodestone::PlacementKind::Bound, .Sampler = lodestone::PlacementKind::Bound, .StorageTexture = lodestone::PlacementKind::Bound },
                                        .CodeFormat = lodestone::ShaderCodeFormat::Wgsl },
                         CookedProfile{ .TargetName = "spirv",
                                        .Forms = lodestone::PlacementForms{ .Buffer = lodestone::PlacementKind::Bound, .Texture = lodestone::PlacementKind::Bound, .Sampler = lodestone::PlacementKind::Bound, .StorageTexture = lodestone::PlacementKind::Bound },
                                        .CodeFormat = lodestone::ShaderCodeFormat::Spirv } };
    library.Environments.emplace_back(MakeSmallModule());
    library.Environments.emplace_back(std::nullopt);
    return library;
}

/** The same module, cooked for the SPIR-V profile only. Each source is whole words. The emitter does not
 * read SPIR-V, so the words need not be a real module. */
CookedLibrary MakeSpirvLibrary()
{
    CookedLibrary library = MakeSmallLibrary();
    CookedModule module = MakeSmallModule();
    module.Sources = { std::vector<std::byte>(8u, std::byte{ 0x01 }), std::vector<std::byte>(12u, std::byte{ 0x02 }) };
    library.Environments = {};
    library.Environments.emplace_back(std::nullopt);
    library.Environments.emplace_back(std::move(module));
    return library;
}

/** The small module with a texture and a sampler that its entry point samples together. The pair
 * list names the two visibility positions. */
CookedModule MakeSampledModule(lodestone::SamplerPairList pairs)
{
    CookedModule module = MakeSmallModule();
    lodestone::ReflectedBinding texture;
    texture.Name = "Albedo";
    texture.Placement = lodestone::BoundPlacement{ .Group = 1u, .Binding = 0u };
    texture.Kind = lodestone::BindingKind::Texture;
    texture.Shape = lodestone::ResourceShape::Texture2D;
    texture.SampleType = lodestone::TextureSampleType::Float;
    lodestone::ReflectedBinding sampler;
    sampler.Name = "LinearSampler";
    sampler.Placement = lodestone::BoundPlacement{ .Group = 1u, .Binding = 1u };
    sampler.Kind = lodestone::BindingKind::Sampler;

    module.Resources = { texture, sampler };
    module.ResourceLists = { lodestone::ResourceList{ 0u, 1u } };
    module.FootprintLists = { lodestone::FootprintList{ std::monostate{}, std::monostate{} } };
    module.VisibilityLists = { lodestone::VisibilityList{ 0u, 1u } };
    module.SamplerPairLists = { std::move(pairs) };
    return module;
}

/** The sampled module cooked for both profiles, each with its own pair list, and each profile recording. */
CookedLibrary MakeSampledLibrary(lodestone::SamplerPairList wgsl_pairs, lodestone::SamplerPairList spirv_pairs)
{
    CookedLibrary library = MakeSmallLibrary();
    library.Profiles[0].SamplerTexturePairsRecorded = true;
    library.Profiles[1].SamplerTexturePairsRecorded = true;
    library.Environments = {};
    library.Environments.emplace_back(MakeSampledModule(std::move(wgsl_pairs)));
    CookedModule spirvModule = MakeSampledModule(std::move(spirv_pairs));
    // a SPIR-V source must be whole words. The emitter does not read SPIR-V, so the words need not be a module
    spirvModule.Sources = { std::vector<std::byte>(8u, std::byte{ 0x01 }), std::vector<std::byte>(12u, std::byte{ 0x02 }) };
    library.Environments.emplace_back(std::move(spirvModule));
    return library;
}

/** The small module, cooked for WGSL only, with entry point constants: a 16-byte uniform buffer named
 * `entryPointParams` at visibility position 0. With `with_constants` false, the instances name the empty record. */
CookedLibrary MakeConstantsLibrary(bool with_constants)
{
    CookedModule module = MakeSmallModule();
    lodestone::ReflectedUniformMember tint;
    tint.Name = "tint";
    tint.Data.Offset = 0u;
    tint.Data.Size = 16u;
    lodestone::ReflectedBinding container;
    container.Name = "entryPointParams";
    container.Placement = lodestone::BoundPlacement{ .Group = 0u, .Binding = 1u };
    container.Kind = lodestone::BindingKind::UniformBuffer;
    container.ByteSize = 16u;
    container.UniformMembers = { tint };

    module.Resources = { container };
    module.ResourceLists = { lodestone::ResourceList{ 0u } };
    module.FootprintLists = { lodestone::FootprintList{ std::monostate{} } };
    module.VisibilityLists = { lodestone::VisibilityList{ 0u } };
    module.EntryPointConstantsTable.push_back(
        lodestone::EntryPointConstants{ .Delivery = lodestone::EntryPointConstantsDelivery::UniformBuffer,
                                        .ByteSize = 16u,
                                        .Binding = 0u,
                                        .Members = { tint } });
    for (lodestone::LibraryVariant& variant : module.Variants)
    {
        variant.ConstantsIndices = { with_constants ? 1u : 0u };
    }

    CookedLibrary library = MakeSmallLibrary();
    library.Environments = {};
    library.Environments.emplace_back(std::move(module));
    library.Environments.emplace_back(std::nullopt);
    return library;
}

template<typename ValueType>
void WriteValue(std::vector<std::byte>& bytes, size_t offset, ValueType value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template<typename RecordType>
RecordType ReadRecord(std::span<const std::byte> bytes, size_t offset)
{
    RecordType record;
    std::memcpy(&record, bytes.data() + offset, sizeof(RecordType));
    return record;
}

ErrorCode BundleErrorFrom(std::span<const std::byte> bytes)
{
    const ManifestResult<BundleView> opened = BundleView::Open(bytes);
    return opened.has_value() ? ErrorCode::Success : opened.error().Code;
}

ErrorCode EnvironmentErrorFrom(std::span<const std::byte> bytes, uint32_t profile_index)
{
    const ManifestResult<BundleView> bundle = BundleView::Open(bytes);
    if (!bundle.has_value())
    {
        return bundle.error().Code;
    }

    const ManifestResult<EnvironmentView> environment = bundle->OpenEnvironment(profile_index, 0u);
    return environment.has_value() ? ErrorCode::Success : environment.error().Code;
}

} // namespace

/** The capability names and the workgroup memory of each source (format 14). Variant zero's source needs two names
 * and 4096 bytes of workgroup memory, variant one's needs one name and none. */
void TestSourceFacts(lodestone::tests::TestRunner& runner)
{
    lodestone::RecordingDiagnosticSink manifestDiagnostics;
    runner.BeginSection("the capability names and the workgroup memory of each source read back");
    CookedLibrary library = MakeSmallLibrary();
    CookedModule& module = *library.Environments[0u];
    module.SourceFacts = { lodestone::SourceCodeFacts{ .CapabilityNames = { "Float16", "GroupNonUniform" },
                                                       .WorkgroupMemoryBytes = 4096u },
                           lodestone::SourceCodeFacts{ .CapabilityNames = { "GroupNonUniform" },
                                                       .WorkgroupMemoryBytes = 0u } };
    module.ObservedCapabilities = { "Float16", "GroupNonUniform" };

    const lodestone::CookResult<std::vector<std::byte>> manifest = EmitShaderManifest(library, manifestDiagnostics);
    if (!runner.CheckOk(manifest, "the emitter accepts a module with source facts"))
    {
        return;
    }
    const std::vector<std::byte>& bytes = *manifest;
    runner.CheckEqual(lodestone::VerifyManifestRoundTrip(library, bytes, manifestDiagnostics), lodestone::CookError::Success,
                      "the round trip reads back the facts the cook wrote");

    const ManifestResult<BundleView> bundle = BundleView::Open(bytes);
    const ManifestResult<EnvironmentView> environment =
        bundle.has_value() ? bundle->OpenEnvironment(0u, 0u) : ManifestResult<EnvironmentView>{ std::unexpected(bundle.error()) };
    if (!runner.CheckOk(environment, "the environment with source facts opens"))
    {
        return;
    }
    runner.Check(!environment->HasCodeFacts(), "a profile whose reader did not run reads as not having facts");
    CookedLibrary readLibrary = library;
    readLibrary.Profiles[0].CodeFactsRead = true;
    const lodestone::CookResult<std::vector<std::byte>> readManifest = EmitShaderManifest(readLibrary, manifestDiagnostics);
    const ManifestResult<BundleView> readBundle = BundleView::Open(readManifest.value());
    const ManifestResult<EnvironmentView> readEnvironment =
        readBundle.has_value() ? readBundle->OpenEnvironment(0u, 0u) : ManifestResult<EnvironmentView>{ std::unexpected(readBundle.error()) };
    runner.Check(readEnvironment.has_value() && readEnvironment->HasCodeFacts(),
                 "a profile whose reader ran reads as having facts");
    if (runner.CheckEqual(environment->CapabilityNames().size(), 2u, "the environment holds the name table, sorted"))
    {
        runner.CheckEqual(environment->CapabilityName(0u), "Float16", "the environment holds the name table, sorted");
        runner.CheckEqual(environment->CapabilityName(1u), "GroupNonUniform", "the environment holds the name table, sorted");
    }
    const lodestone::manifest::EntryPointInstanceView first = environment->VariantAt(0u).EntryPoint(0u);
    const lodestone::manifest::EntryPointInstanceView second = environment->VariantAt(1u).EntryPoint(0u);
    runner.Check(first.RequiresCapability(0u) && first.RequiresCapability(1u) && first.WorkgroupMemoryBytes() == 4096u,
                 "an instance reads its source's names and workgroup memory");
    runner.Check(!second.RequiresCapability(0u) && second.RequiresCapability(1u) && second.WorkgroupMemoryBytes() == 0u,
                 "another source keeps its own facts");

    CookedLibrary otherFacts = library;
    otherFacts.Environments[0u]->SourceFacts[1u].WorkgroupMemoryBytes = 16u;
    runner.CheckEqual(lodestone::VerifyManifestRoundTrip(otherFacts, bytes, manifestDiagnostics), lodestone::CookError::ManifestSourceFactsMismatch,
                      "the round trip fails on workgroup memory that differs");
    CookedLibrary otherNames = library;
    otherNames.Environments[0u]->SourceFacts[1u].CapabilityNames = { "Float16" };
    runner.CheckEqual(lodestone::VerifyManifestRoundTrip(otherNames, bytes, manifestDiagnostics), lodestone::CookError::ManifestSourceFactsMismatch,
                      "the round trip fails on a capability name that differs");

    const Header header = ReadRecord<Header>(bytes, 0u);
    const EnvironmentDirectoryEntry entry = ReadRecord<EnvironmentDirectoryEntry>(bytes, header.EnvironmentDirectoryOffset);
    const EnvironmentHeader environmentHeader = ReadRecord<EnvironmentHeader>(bytes, entry.ExtentOffset);
    std::vector<std::byte> bitPastNames = bytes;
    WriteValue(bitPastNames, entry.ExtentOffset + environmentHeader.SourceCapabilityTableOffset, uint64_t{ 1u } << 63u);
    runner.CheckEqual(EnvironmentErrorFrom(bitPastNames, 0u), ErrorCode::InvalidSourceCapabilities,
                      "a capability bit past the name table is rejected");
    std::vector<std::byte> namePastStrings = bytes;
    WriteValue(namePastStrings, entry.ExtentOffset + environmentHeader.CapabilityNames.Offset, 0xFFFFFFu);
    runner.CheckEqual(EnvironmentErrorFrom(namePastStrings, 0u), ErrorCode::InvalidCapabilityNames,
                      "a capability name past the string table is rejected");
}

/** Two profiles of one variant need the same workgroup memory and the same portable capabilities. The names that only
 * one target spells (`StorageInputOutput16`) are not compared. */
void TestSourceFactsAcrossProfiles(lodestone::tests::TestRunner& runner)
{
    lodestone::RecordingDiagnosticSink crossProfileDiagnostics;
    runner.BeginSection("two profiles of one variant must read the same workgroup memory and portable capabilities");
    const auto makeLibrary = [](lodestone::SourceCodeFacts wgsl, lodestone::SourceCodeFacts spirv)
    {
        CookedLibrary library = MakeSampledLibrary({}, {});
        library.Profiles[0].CodeFactsRead = true;
        library.Profiles[1].CodeFactsRead = true;
        library.Environments[0]->SourceFacts = { wgsl, wgsl };
        library.Environments[1]->SourceFacts = { spirv, spirv };
        return library;
    };

    const lodestone::SourceCodeFacts wgslFacts
    {
        .CapabilityNames = { "f16", "subgroups" },
        .WorkgroupMemoryBytes = 144u
    };
    const lodestone::SourceCodeFacts spirvFacts
    {
        .CapabilityNames = { "Float16", "GroupNonUniform", "GroupNonUniformArithmetic", "StorageInputOutput16" },
        .WorkgroupMemoryBytes = 144u
    };
    runner.CheckEqual(lodestone::VerifySourceFactsAcrossProfiles(makeLibrary(wgslFacts, spirvFacts), crossProfileDiagnostics),
                          lodestone::CookError::Success,
                 "f16 is Float16, subgroups is the GroupNonUniform family, and a SPIR-V only name is not compared");

    lodestone::SourceCodeFacts otherBytes = spirvFacts;
    otherBytes.WorkgroupMemoryBytes = 160u;
    runner.CheckEqual(lodestone::VerifySourceFactsAcrossProfiles(makeLibrary(wgslFacts, otherBytes), crossProfileDiagnostics),
                          lodestone::CookError::SourceFactsTargetMismatch,
                 "workgroup memory that differs fails the cook");

    lodestone::SourceCodeFacts noFloat16 = spirvFacts;
    noFloat16.CapabilityNames = { "GroupNonUniform" };
    runner.CheckEqual(lodestone::VerifySourceFactsAcrossProfiles(makeLibrary(wgslFacts, noFloat16), crossProfileDiagnostics),
                          lodestone::CookError::SourceFactsTargetMismatch,
                 "a 16-bit float that only one profile reads fails the cook");

    lodestone::SourceCodeFacts noSubgroups = spirvFacts;
    noSubgroups.CapabilityNames = { "Float16" };
    runner.CheckEqual(lodestone::VerifySourceFactsAcrossProfiles(makeLibrary(wgslFacts, noSubgroups), crossProfileDiagnostics),
                          lodestone::CookError::SourceFactsTargetMismatch,
                 "a subgroup operation that only one profile reads fails the cook");

    CookedLibrary unread = makeLibrary(wgslFacts, otherBytes);
    unread.Profiles[1].CodeFactsRead = false;
    runner.CheckEqual(lodestone::VerifySourceFactsAcrossProfiles(unread, crossProfileDiagnostics), lodestone::CookError::Success,
                 "a profile with no reader is not compared");
}

/** A member is found by its whole path. The names are the ones from the DiamondDogs report: `SwellWindSpeed`-style
 * suffix matching took the wrong member, so `WindSpeed` and `Swell.WindSpeed` must stay apart. */
void TestMemberLookup(lodestone::tests::TestRunner& runner)
{
    lodestone::RecordingDiagnosticSink manifestDiagnostics;
    runner.BeginSection("a uniform member is found by its whole path");
    CookedLibrary library = MakeConstantsLibrary(true);
    CookedModule& module = *library.Environments[0u];
    const auto member = [](std::string name, uint32_t offset)
    {
        lodestone::ReflectedUniformMember result;
        result.Name = std::move(name);
        result.Data.Offset = offset;
        result.Data.Size = 4u;
        return result;
    };
    const std::vector<lodestone::ReflectedUniformMember> members{ member("WindSpeed", 8u),
                                                                  member("Swell.Height", 48u),
                                                                  member("Swell.WindSpeed", 52u) };
    module.Resources[0u].UniformMembers = members;
    module.Resources[0u].ByteSize = 64u;
    module.EntryPointConstantsTable[1u].Members = members;
    module.EntryPointConstantsTable[1u].ByteSize = 64u;

    const lodestone::CookResult<std::vector<std::byte>> manifest = EmitShaderManifest(library, manifestDiagnostics);
    const ManifestResult<BundleView> bundle =
        manifest.has_value() ? BundleView::Open(*manifest) : ManifestResult<BundleView>{ std::unexpected(ErrorCode::TooSmall) };
    const ManifestResult<EnvironmentView> environment =
        bundle.has_value() ? bundle->OpenEnvironment(0u, 0u) : ManifestResult<EnvironmentView>{ std::unexpected(bundle.error()) };
    if (!runner.CheckOk(environment, "the module with nested member names opens"))
    {
        return;
    }

    const lodestone::manifest::EntryPointInstanceView instance = environment->VariantAt(0u).EntryPoint(0u);
    for (const lodestone::manifest::UniformMemberRange range :
         { instance.Constants().Members(), instance.Layout()[0u].Members() })
    {
        const auto wind = range.Find("WindSpeed");
        const auto swellWind = range.Find("Swell.WindSpeed");
        runner.Check(wind.has_value(), "a plain name finds its own member");
        if (wind.has_value())
        {
            runner.CheckEqual(wind->Offset(), 8u, "a plain name finds its own member");
        }
        runner.Check(swellWind.has_value(), "a dotted path finds the nested member");
        if (swellWind.has_value())
        {
            runner.CheckEqual(swellWind->Offset(), 52u, "a dotted path finds the nested member");
        }
        runner.Check(!range.Find("Wind").has_value() && !range.Find("Speed").has_value() && !range.Find("Swell").has_value() &&
                         !range.Find("").has_value(),
                     "a prefix, a suffix, a parent, and an empty path match nothing");
    }
}

int main()
{
    lodestone::RecordingDiagnosticSink manifestDiagnostics;
    lodestone::RecordingDiagnosticSink crossProfileDiagnostics;
    lodestone::tests::TestRunner runner{ "ShaderManifestRejectTests" };

    const CookedLibrary library = MakeSmallLibrary();
    const lodestone::CookResult<std::vector<std::byte>> manifest = EmitShaderManifest(library, manifestDiagnostics);
    if (!runner.CheckOk(manifest, "the emitter accepts the library"))
    {
        return runner.Report();
    }
    const std::vector<std::byte>& valid = *manifest;
    const Header header = ReadRecord<Header>(valid, 0u);

    runner.BeginSection("a manifest the cooker wrote opens and reads back");
    const ManifestResult<BundleView> opened = BundleView::Open(valid);
    if (runner.CheckOk(opened, "the emitter produces a bundle the reader accepts"))
    {
        runner.CheckEqual(opened->ModuleCount(), 1u, "the reader returns the module name");
        runner.CheckEqual(opened->Module(0u).Name(), "TestModule", "the reader returns the module name");
        runner.CheckEqual(opened->Module(0u).EntryPoints().size(), 1u, "the reader returns the entry point");
        runner.CheckEqual(opened->FindProfile("spirv"), 1, "the reader finds a profile by its target name");
        const ManifestResult<EnvironmentView> environment = opened->OpenEnvironment(0u, 0u);
        if (runner.CheckOk(environment, "the reader returns both variants of the cooked environment"))
        {
            runner.CheckEqual(environment->Variants().size(), 2u,
                              "the reader returns both variants of the cooked environment");
        }
    }
    runner.CheckEqual(lodestone::VerifyManifestRoundTrip(library, *manifest, manifestDiagnostics), lodestone::CookError::Success,
                      "the round trip reads back what the cook wrote");

    runner.BeginSection("an environment the cook skipped reads as absent");
    runner.CheckEqual(EnvironmentErrorFrom(valid, 1u), ErrorCode::EnvironmentNotCooked,
                      "the second profile names no extent for the module");
    runner.CheckEqual(EnvironmentErrorFrom(valid, 2u), ErrorCode::IndexOutOfBounds,
                      "a profile index past the table is rejected, not read");

    runner.BeginSection("the header region opens alone");
    const std::span<const std::byte> headerRegion{ valid.data(), static_cast<size_t>(header.HeaderSize) };
    runner.CheckEqual(BundleErrorFrom(headerRegion), ErrorCode::Success,
                      "a span of the header region alone opens as a bundle");
    runner.CheckEqual(EnvironmentErrorFrom(headerRegion, 0u), ErrorCode::EnvironmentExtentOutOfBounds,
                      "but an extent it does not hold cannot open from it");
    const EnvironmentDirectoryEntry entry = ReadRecord<EnvironmentDirectoryEntry>(valid, header.EnvironmentDirectoryOffset);
    const std::span<const std::byte> extent{ valid.data() + entry.ExtentOffset, static_cast<size_t>(entry.ExtentSize) };
    const ManifestResult<BundleView> headerOnly = BundleView::Open(headerRegion);
    if (headerOnly.has_value())
    {
        runner.CheckOk(EnvironmentView::Open(*headerOnly, 0u, 0u, extent),
                       "an extent read on its own opens against that bundle");
    }

    runner.BeginSection("a short file is rejected before any field is read");
    const std::span<const std::byte> truncated{ valid.data(), sizeof(Header) - 1u };
    runner.CheckEqual(BundleErrorFrom(truncated), ErrorCode::TooSmall, "a span smaller than the header is TooSmall");

    runner.BeginSection("a misaligned span is rejected");
    // The reader maps 64-bit fields in place, so it cannot accept a span that starts off an 8-byte
    // boundary. The valid case above proves the buffer itself starts aligned.
    const std::span<const std::byte> misaligned{ valid.data() + 1u, valid.size() - 1u };
    runner.CheckEqual(BundleErrorFrom(misaligned), ErrorCode::Misaligned, "a span that starts one byte in is Misaligned");

    runner.BeginSection("a damaged header field is rejected by name");
    std::vector<std::byte> badMagic = valid;
    WriteValue(badMagic, offsetof(Header, Magic), 0xDEADBEEFu);
    runner.CheckEqual(BundleErrorFrom(badMagic), ErrorCode::BadMagic, "a wrong magic is BadMagic");

    std::vector<std::byte> badVersion = valid;
    WriteValue(badVersion, offsetof(Header, Version), lodestone::manifest::k_ShaderManifestVersion + 1u);
    runner.CheckEqual(BundleErrorFrom(badVersion), ErrorCode::VersionMismatch, "a future version is VersionMismatch");

    std::vector<std::byte> badSize = valid;
    WriteValue(badSize, offsetof(Header, FileSize), static_cast<uint64_t>(valid.size()) - 8u);
    runner.CheckEqual(BundleErrorFrom(badSize), ErrorCode::SizeMismatch,
                      "a span longer than the file the header states is SizeMismatch");

    std::vector<std::byte> badSection = valid;
    WriteValue(badSection, offsetof(Header, Strings) + offsetof(lodestone::manifest::TableRef64, Offset),
               header.HeaderSize - 1u);
    runner.CheckEqual(BundleErrorFrom(badSection), ErrorCode::SectionOutOfBounds,
                      "a table that reaches past the header region is SectionOutOfBounds");

    runner.BeginSection("a damaged directory or extent is rejected by name");
    std::vector<std::byte> badExtent = valid;
    WriteValue(badExtent,
               header.EnvironmentDirectoryOffset + offsetof(EnvironmentDirectoryEntry, ExtentOffset),
               header.FileSize);
    runner.CheckEqual(BundleErrorFrom(badExtent), ErrorCode::EnvironmentExtentOutOfBounds,
                      "an extent that starts at the end of the file is rejected when the bundle opens");

    std::vector<std::byte> badKeys = valid;
    const EnvironmentHeader environment = ReadRecord<EnvironmentHeader>(valid, entry.ExtentOffset);
    const size_t secondKeyOffset = entry.ExtentOffset + environment.VariantKeyTableOffset + sizeof(VariantKey);
    WriteValue(badKeys, secondKeyOffset, VariantKey{ 0u });
    runner.CheckEqual(EnvironmentErrorFrom(badKeys, 0u), ErrorCode::InvalidVariantKeyOrder,
                      "two equal keys are rejected when the environment opens");

    runner.BeginSection("the code format of a profile");
    const size_t firstFormatOffset = header.Profiles.Offset + offsetof(lodestone::manifest::Profile, CodeFormat);
    std::vector<std::byte> badFormat = valid;
    WriteValue(badFormat, firstFormatOffset, lodestone::ShaderCodeFormat::None);
    runner.CheckEqual(BundleErrorFrom(badFormat), ErrorCode::InvalidProfileCodeFormat,
                      "a profile with no code format is rejected when the bundle opens");

    // The WGSL sources of the small module are 24 and 23 bytes, so the second is not whole words.
    std::vector<std::byte> textAsSpirv = valid;
    WriteValue(textAsSpirv, firstFormatOffset, lodestone::ShaderCodeFormat::Spirv);
    runner.CheckEqual(EnvironmentErrorFrom(textAsSpirv, 0u), ErrorCode::SpirvSourceMisaligned,
                      "a SPIR-V environment whose source is not whole words is rejected when it opens");

    const CookedLibrary spirvLibrary = MakeSpirvLibrary();
    const lodestone::CookResult<std::vector<std::byte>> spirvManifest = EmitShaderManifest(spirvLibrary, manifestDiagnostics);
    if (runner.CheckOk(spirvManifest, "the emitter accepts a SPIR-V environment"))
    {
        runner.CheckEqual(lodestone::VerifyManifestRoundTrip(spirvLibrary, *spirvManifest, manifestDiagnostics), lodestone::CookError::Success,
                          "the round trip reads the SPIR-V words back through SpirvWords");
        const std::vector<std::byte>& spirvBytes = *spirvManifest;
        const ManifestResult<BundleView> spirvBundle = BundleView::Open(spirvBytes);
        const ManifestResult<EnvironmentView> spirvEnvironment =
            spirvBundle.has_value() ? spirvBundle->OpenEnvironment(1u, 0u)
                                    : ManifestResult<EnvironmentView>{ std::unexpected(spirvBundle.error()) };
        if (runner.CheckOk(spirvEnvironment, "a 12-byte SPIR-V source reads back as three words"))
        {
            runner.CheckEqual(spirvEnvironment->SpirvWords(1u).size(), 3u,
                              "a 12-byte SPIR-V source reads back as three words");
        }
    }

    runner.BeginSection("the sampler and texture pairs read back, and a damaged pair is rejected");
    const lodestone::SamplerPairList onePair{ lodestone::SamplerTexturePair{ .Texture = 0u, .Sampler = 1u } };
    const CookedLibrary sampledLibrary = MakeSampledLibrary(onePair, onePair);
    const lodestone::CookResult<std::vector<std::byte>> sampledManifest = EmitShaderManifest(sampledLibrary, manifestDiagnostics);
    if (runner.CheckOk(sampledManifest, "the emitter accepts a module with a sampler and texture pair"))
    {
        const std::vector<std::byte>& sampledBytes = *sampledManifest;
        runner.CheckEqual(lodestone::VerifyManifestRoundTrip(sampledLibrary, sampledBytes, manifestDiagnostics), lodestone::CookError::Success,
                          "the round trip reads the pairs back");
        const ManifestResult<BundleView> sampledBundle = BundleView::Open(sampledBytes);
        const ManifestResult<EnvironmentView> sampledEnvironment =
            sampledBundle.has_value() ? sampledBundle->OpenEnvironment(0u, 0u)
                                      : ManifestResult<EnvironmentView>{ std::unexpected(sampledBundle.error()) };
        runner.Check(sampledEnvironment.has_value() && sampledEnvironment->HasSamplerTexturePairs(),
                     "a recording profile reads as recorded");
        if (sampledEnvironment.has_value())
        {
            const lodestone::manifest::EntryPointInstanceView instance = sampledEnvironment->VariantAt(0u).EntryPoint(0u);
            const std::span<const lodestone::manifest::SamplerTexturePair> pairs = instance.SamplerTexturePairs();
            if (runner.CheckEqual(pairs.size(), 1u, "a pair resolves through the layout to the texture and the sampler"))
            {
                runner.CheckEqual(instance.Layout()[pairs[0].Texture].Name(), "Albedo",
                                  "a pair resolves through the layout to the texture and the sampler");
                runner.CheckEqual(instance.Layout()[pairs[0].Sampler].Name(), "LinearSampler",
                                  "a pair resolves through the layout to the texture and the sampler");
            }
        }

        // a round trip against a cook with no pairs proves that the round trip compares the pairs
        const CookedLibrary unpairedLibrary = MakeSampledLibrary({}, {});
        runner.CheckEqual(lodestone::VerifyManifestRoundTrip(unpairedLibrary, sampledBytes, manifestDiagnostics),
                          lodestone::CookError::ManifestSamplerPairMismatch,
                          "a manifest whose pairs differ from the cook fails the round trip");

        const EnvironmentDirectoryEntry sampledEntry =
            ReadRecord<EnvironmentDirectoryEntry>(sampledBytes, ReadRecord<Header>(sampledBytes, 0u).EnvironmentDirectoryOffset);
        const EnvironmentHeader sampledHeader = ReadRecord<EnvironmentHeader>(sampledBytes, sampledEntry.ExtentOffset);
        std::vector<std::byte> swappedPair = sampledBytes;
        WriteValue(swappedPair,
                   sampledEntry.ExtentOffset + sampledHeader.SamplerTexturePairs.Offset,
                   lodestone::manifest::SamplerTexturePair{ .Texture = 1u, .Sampler = 0u });
        runner.CheckEqual(EnvironmentErrorFrom(swappedPair, 0u), ErrorCode::InvalidSamplerTexturePair,
                          "a pair whose texture position names the sampler is rejected when the environment opens");

        std::vector<std::byte> pastVisibility = sampledBytes;
        WriteValue(pastVisibility,
                   sampledEntry.ExtentOffset + sampledHeader.SamplerTexturePairs.Offset,
                   lodestone::manifest::SamplerTexturePair{ .Texture = 0u, .Sampler = 2u });
        runner.CheckEqual(EnvironmentErrorFrom(pastVisibility, 0u), ErrorCode::InvalidSamplerTexturePair,
                          "a pair position past the visibility list is rejected");

        std::vector<std::byte> badPairList = sampledBytes;
        WriteValue(badPairList,
                   sampledEntry.ExtentOffset + sampledHeader.EntryPointInstanceTableOffset +
                       offsetof(lodestone::manifest::EntryPointInstance, SamplerPairs),
                   5u);
        runner.CheckEqual(EnvironmentErrorFrom(badPairList, 0u), ErrorCode::InvalidEntryPointInstanceSamplerPairIndex,
                          "an instance that names a pair list past the table is rejected");
    }

    CookedLibrary unrecordedLibrary = MakeSampledLibrary({}, {});
    unrecordedLibrary.Profiles[0].SamplerTexturePairsRecorded = false;
    const lodestone::CookResult<std::vector<std::byte>> unrecordedManifest = EmitShaderManifest(unrecordedLibrary, manifestDiagnostics);
    if (unrecordedManifest.has_value())
    {
        const std::vector<std::byte>& unrecordedBytes = *unrecordedManifest;
        const ManifestResult<BundleView> unrecordedBundle = BundleView::Open(unrecordedBytes);
        const ManifestResult<EnvironmentView> unrecorded =
            unrecordedBundle.has_value() ? unrecordedBundle->OpenEnvironment(0u, 0u)
                                         : ManifestResult<EnvironmentView>{ std::unexpected(unrecordedBundle.error()) };
        runner.Check(unrecorded.has_value() && !unrecorded->HasSamplerTexturePairs(),
                     "a profile with no reader reads as not recorded, which is not the same fact as no pairs");
    }

    // The cross-target validator: one variant of one module samples the same resources in every profile.
    runner.BeginSection("two profiles of one variant must read the same pairs");
    runner.CheckEqual(lodestone::VerifySamplerPairsAcrossProfiles(sampledLibrary, crossProfileDiagnostics), lodestone::CookError::Success,
                      "two profiles that read the same pairs agree");
    const CookedLibrary disagreeing = MakeSampledLibrary(onePair, {});
    runner.CheckEqual(lodestone::VerifySamplerPairsAcrossProfiles(disagreeing, crossProfileDiagnostics),
                          lodestone::CookError::SamplerPairsTargetMismatch,
                          "a pair that one profile reads and the other does not fails the cook");
    CookedLibrary oneUnrecorded = MakeSampledLibrary(onePair, {});
    oneUnrecorded.Profiles[1].SamplerTexturePairsRecorded = false;
    runner.CheckEqual(lodestone::VerifySamplerPairsAcrossProfiles(oneUnrecorded, crossProfileDiagnostics), lodestone::CookError::Success,
                      "a profile that did not record its pairs is not compared");

    runner.BeginSection("the entry point constants read back, and a damaged record is rejected");
    const CookedLibrary constantsLibrary = MakeConstantsLibrary(true);
    const lodestone::CookResult<std::vector<std::byte>> constantsManifest = EmitShaderManifest(constantsLibrary, manifestDiagnostics);
    if (runner.CheckOk(constantsManifest, "the emitter accepts a module with entry point constants"))
    {
        const std::vector<std::byte>& constantsBytes = *constantsManifest;
        runner.CheckEqual(lodestone::VerifyManifestRoundTrip(constantsLibrary, constantsBytes, manifestDiagnostics), lodestone::CookError::Success,
                          "the round trip reads the constants back");
        runner.CheckEqual(lodestone::VerifyManifestRoundTrip(MakeConstantsLibrary(false), constantsBytes, manifestDiagnostics),
                          lodestone::CookError::ManifestEntryPointConstantsMismatch,
                          "a manifest whose constants differ from the cook fails the round trip");

        const ManifestResult<BundleView> constantsBundle = BundleView::Open(constantsBytes);
        const ManifestResult<EnvironmentView> constantsEnvironment =
            constantsBundle.has_value() ? constantsBundle->OpenEnvironment(0u, 0u)
                                        : ManifestResult<EnvironmentView>{ std::unexpected(constantsBundle.error()) };
        if (runner.CheckOk(constantsEnvironment, "the environment with constants opens"))
        {
            const lodestone::manifest::EntryPointInstanceView instance =
                constantsEnvironment->VariantAt(0u).EntryPoint(0u);
            const lodestone::manifest::EntryPointConstantsView constants = instance.Constants();
            runner.CheckEqual(constants.Delivery(), lodestone::EntryPointConstantsDelivery::UniformBuffer,
                              "the constants resolve through the layout to their uniform buffer");
            runner.CheckEqual(constants.ByteSize(), 16u,
                              "the constants resolve through the layout to their uniform buffer");
            if (runner.CheckEqual(constants.Members().Size(), 1u, "the constants resolve through the layout to their uniform buffer"))
            {
                runner.CheckEqual(constants.Members()[0u].Name(), "tint",
                                  "the constants resolve through the layout to their uniform buffer");
            }
            runner.CheckEqual(instance.Layout()[constants.BindingPosition()].Name(), "entryPointParams",
                              "the constants resolve through the layout to their uniform buffer");
        }

        const EnvironmentDirectoryEntry constantsEntry = ReadRecord<EnvironmentDirectoryEntry>(
            constantsBytes, ReadRecord<Header>(constantsBytes, 0u).EnvironmentDirectoryOffset);
        const EnvironmentHeader constantsHeader =
            ReadRecord<EnvironmentHeader>(constantsBytes, constantsEntry.ExtentOffset);
        // record 1: record 0 is the empty record that `AddDefaultInstanceTables` adds
        const size_t recordOffset = constantsEntry.ExtentOffset + constantsHeader.EntryPointConstants.Offset +
                                    sizeof(lodestone::manifest::EntryPointConstants);
        const auto goodRecord = ReadRecord<lodestone::manifest::EntryPointConstants>(constantsBytes, recordOffset);
        auto rejectRecord = [&](const lodestone::manifest::EntryPointConstants& record) -> ErrorCode
        {
            std::vector<std::byte> damaged = constantsBytes;
            WriteValue(damaged, recordOffset, record);
            return EnvironmentErrorFrom(damaged, 0u);
        };

        lodestone::manifest::EntryPointConstants unknownDelivery = goodRecord;
        unknownDelivery.Delivery = 7u;
        runner.CheckEqual(rejectRecord(unknownDelivery), ErrorCode::InvalidEntryPointConstants,
                          "an unknown delivery is rejected");
        lodestone::manifest::EntryPointConstants wrongSize = goodRecord;
        wrongSize.ByteSize = 32u;
        runner.CheckEqual(rejectRecord(wrongSize), ErrorCode::InvalidEntryPointConstants,
                          "a uniform buffer of a different size than the constants is rejected");
        lodestone::manifest::EntryPointConstants pastVisibility = goodRecord;
        pastVisibility.Binding = 5u;
        runner.CheckEqual(rejectRecord(pastVisibility), ErrorCode::InvalidEntryPointConstants,
                          "a binding position past the visibility list is rejected");
        lodestone::manifest::EntryPointConstants positionedPush = goodRecord;
        positionedPush.Delivery = std::to_underlying(lodestone::EntryPointConstantsDelivery::PushConstant);
        positionedPush.Binding = 1u;
        runner.CheckEqual(rejectRecord(positionedPush), ErrorCode::InvalidEntryPointConstants,
                          "a push constant block with a binding position is rejected");
        lodestone::manifest::EntryPointConstants membersPastTable = goodRecord;
        membersPastTable.FirstUniformMember = 1000u;
        runner.CheckEqual(rejectRecord(membersPastTable), ErrorCode::InvalidEntryPointConstants,
                          "a member run past the member table is rejected");

        std::vector<std::byte> badIndex = constantsBytes;
        WriteValue(badIndex,
                   constantsEntry.ExtentOffset + constantsHeader.EntryPointInstanceTableOffset +
                       offsetof(lodestone::manifest::EntryPointInstance, Constants),
                   9u);
        runner.CheckEqual(EnvironmentErrorFrom(badIndex, 0u), ErrorCode::InvalidEntryPointInstanceConstantsIndex,
                          "an instance that names a constants record past the table is rejected");
    }

    // The stage 4 checks of a role need Slang (ResourceRoleTest). Here only the record matters, so any binding can hold
    // a role and tags.
    runner.BeginSection("the roles and tags of a binding read back, and a damaged binding is rejected");
    CookedLibrary taggedLibrary = MakeConstantsLibrary(true);
    taggedLibrary.Environments[0]->Resources[0].Roles = lodestone::ResourceRoleFlags::IndexData;
    taggedLibrary.Environments[0]->Resources[0].Tags = { "Alpha", "Beta" };
    const lodestone::CookResult<std::vector<std::byte>> taggedManifest = EmitShaderManifest(taggedLibrary, manifestDiagnostics);
    if (runner.CheckOk(taggedManifest, "the emitter accepts a binding with roles and tags"))
    {
        const std::vector<std::byte>& taggedBytes = *taggedManifest;
        runner.CheckEqual(lodestone::VerifyManifestRoundTrip(taggedLibrary, taggedBytes, manifestDiagnostics), lodestone::CookError::Success,
                          "the round trip reads the roles and tags back");
        CookedLibrary otherTags = MakeConstantsLibrary(true);
        otherTags.Environments[0]->Resources[0].Roles = lodestone::ResourceRoleFlags::IndexData;
        otherTags.Environments[0]->Resources[0].Tags = { "Alpha", "Gamma" };
        runner.Check(lodestone::VerifyManifestRoundTrip(otherTags, taggedBytes, manifestDiagnostics) != lodestone::CookError::Success,
                     "a manifest whose tags differ from the cook fails the round trip");

        const EnvironmentDirectoryEntry taggedEntry = ReadRecord<EnvironmentDirectoryEntry>(
            taggedBytes, ReadRecord<Header>(taggedBytes, 0u).EnvironmentDirectoryOffset);
        const EnvironmentHeader taggedHeader = ReadRecord<EnvironmentHeader>(taggedBytes, taggedEntry.ExtentOffset);
        const size_t bindingOffset = taggedEntry.ExtentOffset + taggedHeader.Bindings.Offset;
        const auto goodBinding = ReadRecord<lodestone::manifest::Binding>(taggedBytes, bindingOffset);
        runner.CheckEqual(goodBinding.Roles, std::to_underlying(lodestone::ResourceRoleFlags::IndexData),
                          "the binding record holds the role bits and the tag count");
        runner.CheckEqual(goodBinding.TagCount, 2u,
                          "the binding record holds the role bits and the tag count");
        auto rejectBinding = [&](const lodestone::manifest::Binding& record) -> ErrorCode
        {
            std::vector<std::byte> damaged = taggedBytes;
            WriteValue(damaged, bindingOffset, record);
            return EnvironmentErrorFrom(damaged, 0u);
        };

        lodestone::manifest::Binding unknownRole = goodBinding;
        unknownRole.Roles = 0x8000u;
        runner.CheckEqual(rejectBinding(unknownRole), ErrorCode::InvalidBindingRoles, "an unknown role bit is rejected");
        lodestone::manifest::Binding tagsPastTable = goodBinding;
        tagsPastTable.TagCount = 100u;
        runner.CheckEqual(rejectBinding(tagsPastTable), ErrorCode::InvalidBindingTags,
                          "a tag run past the tag table is rejected");

        // a stored value names its container by group and binding, and the container must be in the same instance
        lodestone::manifest::Binding boundWithOffset = goodBinding;
        boundWithOffset.ValueOffset = 16u;
        runner.CheckEqual(rejectBinding(boundWithOffset), ErrorCode::InvalidStoredValue,
                          "a bound binding with a value offset is rejected");
        lodestone::manifest::Binding unknownContainer = goodBinding;
        unknownContainer.PlacementKind = std::to_underlying(lodestone::PlacementKind::Handle);
        unknownContainer.ValueContainer = 7u;
        runner.CheckEqual(rejectBinding(unknownContainer), ErrorCode::InvalidStoredValue,
                          "a handle with an unknown container is rejected");
        lodestone::manifest::Binding pushWithBlock = goodBinding;
        pushWithBlock.PlacementKind = std::to_underlying(lodestone::PlacementKind::Pointer);
        pushWithBlock.ValueContainer = std::to_underlying(lodestone::StoredValueContainer::PushConstants);
        runner.CheckEqual(rejectBinding(pushWithBlock), ErrorCode::InvalidStoredValue,
                          "a pointer in push constants that names a group and binding is rejected");
        lodestone::manifest::Binding absentBlock = goodBinding;
        absentBlock.PlacementKind = std::to_underlying(lodestone::PlacementKind::Handle);
        absentBlock.ValueContainer = std::to_underlying(lodestone::StoredValueContainer::UniformBuffer);
        absentBlock.Placement = lodestone::manifest::PlacementPayload{ .Word0 = 9u, .Word1 = 9u };
        runner.CheckEqual(rejectBinding(absentBlock), ErrorCode::StoredValueContainerMissing,
                          "a handle whose uniform buffer is not in the instance is rejected");
        lodestone::manifest::Binding absentPushConstants = goodBinding;
        absentPushConstants.PlacementKind = std::to_underlying(lodestone::PlacementKind::Pointer);
        absentPushConstants.ValueContainer = std::to_underlying(lodestone::StoredValueContainer::PushConstants);
        absentPushConstants.Placement = lodestone::manifest::PlacementPayload{};
        runner.CheckEqual(rejectBinding(absentPushConstants), ErrorCode::StoredValueContainerMissing,
                          "a pointer in push constants of an instance with no push constants is rejected");

        const size_t tagOffset = taggedEntry.ExtentOffset + taggedHeader.ResourceTags.Offset;
        std::vector<std::byte> repeatedTag = taggedBytes;
        WriteValue(repeatedTag, tagOffset + sizeof(uint32_t), ReadRecord<uint32_t>(taggedBytes, tagOffset));
        runner.CheckEqual(EnvironmentErrorFrom(repeatedTag, 0u), ErrorCode::InvalidBindingTags,
                          "a repeated tag is rejected");
        std::vector<std::byte> tagPastStrings = taggedBytes;
        WriteValue(tagPastStrings, tagOffset, 0xFFFFFFu);
        runner.CheckEqual(EnvironmentErrorFrom(tagPastStrings, 0u), ErrorCode::InvalidBindingTags,
                          "a tag past the string table is rejected");
    }

    TestSourceFacts(runner);
    TestSourceFactsAcrossProfiles(runner);
    TestMemberLookup(runner);

    return runner.Report();
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif
