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

// The manifest is the one artifact that leaves this repository as bytes. A reader that accepts a
// malformed file reads whatever follows it in memory, so every rejection path matters as much as the
// happy path.
//
// The valid case comes from the real emitter rather than from a hand-written header. A hand-written
// header can agree with a hand-written reader and still not match what the cooker writes.

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

// One boolean axis, so that the two variants of the small module differ in something. A module with no axis has
// one variant, and the axis relevance validator rejects two variants that nothing tells apart. File scope,
// because the module points into the space.
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

    module.Sources.emplace_back(lodestone::tests::BytesOf("// wgsl for variant zero"));
    module.Sources.emplace_back(lodestone::tests::BytesOf("// wgsl for variant one"));

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
                                        .AccessModel = lodestone::PlacementKind::Bound,
                                        .CodeFormat = lodestone::ShaderCodeFormat::Wgsl },
                         CookedProfile{ .TargetName = "spirv",
                                        .AccessModel = lodestone::PlacementKind::Bound,
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

int main()
{
    lodestone::tests::TestRunner runner{ "ShaderManifestRejectTests" };

    const CookedLibrary library = MakeSmallLibrary();
    const lodestone::CookResult<std::vector<std::byte>> manifest = EmitShaderManifest(library);
    runner.Check(manifest.has_value(), "the emitter accepts the library");
    if (!manifest.has_value())
    {
        return runner.Report();
    }
    const std::vector<std::byte>& valid = *manifest;
    const Header header = ReadRecord<Header>(valid, 0u);

    runner.BeginSection("a manifest the cooker wrote opens and reads back");
    const ManifestResult<BundleView> opened = BundleView::Open(valid);
    runner.Check(opened.has_value(), "the emitter produces a bundle the reader accepts");
    if (opened.has_value())
    {
        runner.Check(opened->ModuleCount() == 1u && opened->Module(0u).Name() == "TestModule",
                     "the reader returns the module name");
        runner.Check(opened->Module(0u).EntryPoints().size() == 1u, "the reader returns the entry point");
        runner.Check(opened->FindProfile("spirv") == 1, "the reader finds a profile by its target name");
        const ManifestResult<EnvironmentView> environment = opened->OpenEnvironment(0u, 0u);
        runner.Check(environment.has_value() && environment->Variants().size() == 2u,
                     "the reader returns both variants of the cooked environment");
    }
    runner.Check(lodestone::VerifyManifestRoundTrip(library, *manifest) == lodestone::CookError::Success,
                 "the round trip reads back what the cook wrote");

    runner.BeginSection("an environment the cook skipped reads as absent");
    runner.Check(EnvironmentErrorFrom(valid, 1u) == ErrorCode::EnvironmentNotCooked,
                 "the second profile names no extent for the module");
    runner.Check(EnvironmentErrorFrom(valid, 2u) == ErrorCode::IndexOutOfBounds,
                 "a profile index past the table is rejected, not read");

    runner.BeginSection("the header region opens alone");
    const std::span<const std::byte> headerRegion{ valid.data(), static_cast<size_t>(header.HeaderSize) };
    runner.Check(BundleErrorFrom(headerRegion) == ErrorCode::Success,
                 "a span of the header region alone opens as a bundle");
    runner.Check(EnvironmentErrorFrom(headerRegion, 0u) == ErrorCode::EnvironmentExtentOutOfBounds,
                 "but an extent it does not hold cannot open from it");
    const EnvironmentDirectoryEntry entry = ReadRecord<EnvironmentDirectoryEntry>(valid, header.EnvironmentDirectoryOffset);
    const std::span<const std::byte> extent{ valid.data() + entry.ExtentOffset, static_cast<size_t>(entry.ExtentSize) };
    const ManifestResult<BundleView> headerOnly = BundleView::Open(headerRegion);
    if (headerOnly.has_value())
    {
        runner.Check(EnvironmentView::Open(*headerOnly, 0u, 0u, extent).has_value(),
                     "an extent read on its own opens against that bundle");
    }

    runner.BeginSection("a short file is rejected before any field is read");
    const std::span<const std::byte> truncated{ valid.data(), sizeof(Header) - 1u };
    runner.Check(BundleErrorFrom(truncated) == ErrorCode::TooSmall, "a span smaller than the header is TooSmall");

    runner.BeginSection("a misaligned span is rejected");
    // The reader maps 64-bit fields in place, so it cannot accept a span that starts off an 8-byte
    // boundary. The valid case above proves the buffer itself starts aligned.
    const std::span<const std::byte> misaligned{ valid.data() + 1u, valid.size() - 1u };
    runner.Check(BundleErrorFrom(misaligned) == ErrorCode::Misaligned, "a span that starts one byte in is Misaligned");

    runner.BeginSection("a damaged header field is rejected by name");
    std::vector<std::byte> badMagic = valid;
    WriteValue(badMagic, offsetof(Header, Magic), 0xDEADBEEFu);
    runner.Check(BundleErrorFrom(badMagic) == ErrorCode::BadMagic, "a wrong magic is BadMagic");

    std::vector<std::byte> badVersion = valid;
    WriteValue(badVersion, offsetof(Header, Version), lodestone::manifest::k_ShaderManifestVersion + 1u);
    runner.Check(BundleErrorFrom(badVersion) == ErrorCode::VersionMismatch, "a future version is VersionMismatch");

    std::vector<std::byte> badSize = valid;
    WriteValue(badSize, offsetof(Header, FileSize), static_cast<uint64_t>(valid.size()) - 8u);
    runner.Check(BundleErrorFrom(badSize) == ErrorCode::SizeMismatch,
                 "a span longer than the file the header states is SizeMismatch");

    std::vector<std::byte> badSection = valid;
    WriteValue(badSection, offsetof(Header, Strings) + offsetof(lodestone::manifest::TableRef64, Offset),
               header.HeaderSize - 1u);
    runner.Check(BundleErrorFrom(badSection) == ErrorCode::SectionOutOfBounds,
                 "a table that reaches past the header region is SectionOutOfBounds");

    runner.BeginSection("a damaged directory or extent is rejected by name");
    std::vector<std::byte> badExtent = valid;
    WriteValue(badExtent,
               header.EnvironmentDirectoryOffset + offsetof(EnvironmentDirectoryEntry, ExtentOffset),
               header.FileSize);
    runner.Check(BundleErrorFrom(badExtent) == ErrorCode::EnvironmentExtentOutOfBounds,
                 "an extent that starts at the end of the file is rejected when the bundle opens");

    std::vector<std::byte> badKeys = valid;
    const EnvironmentHeader environment = ReadRecord<EnvironmentHeader>(valid, entry.ExtentOffset);
    const size_t secondKeyOffset = entry.ExtentOffset + environment.VariantKeyTableOffset + sizeof(VariantKey);
    WriteValue(badKeys, secondKeyOffset, VariantKey{ 0u });
    runner.Check(EnvironmentErrorFrom(badKeys, 0u) == ErrorCode::InvalidVariantKeyOrder,
                 "two equal keys are rejected when the environment opens");

    runner.BeginSection("the code format of a profile");
    const size_t firstFormatOffset = header.Profiles.Offset + offsetof(lodestone::manifest::Profile, CodeFormat);
    std::vector<std::byte> badFormat = valid;
    WriteValue(badFormat, firstFormatOffset, lodestone::ShaderCodeFormat::None);
    runner.Check(BundleErrorFrom(badFormat) == ErrorCode::InvalidProfileCodeFormat,
                 "a profile with no code format is rejected when the bundle opens");

    // The WGSL sources of the small module are 24 and 23 bytes, so the second is not whole words.
    std::vector<std::byte> textAsSpirv = valid;
    WriteValue(textAsSpirv, firstFormatOffset, lodestone::ShaderCodeFormat::Spirv);
    runner.Check(EnvironmentErrorFrom(textAsSpirv, 0u) == ErrorCode::SpirvSourceMisaligned,
                 "a SPIR-V environment whose source is not whole words is rejected when it opens");

    const CookedLibrary spirvLibrary = MakeSpirvLibrary();
    const lodestone::CookResult<std::vector<std::byte>> spirvManifest = EmitShaderManifest(spirvLibrary);
    runner.Check(spirvManifest.has_value(), "the emitter accepts a SPIR-V environment");
    if (spirvManifest.has_value())
    {
        runner.Check(lodestone::VerifyManifestRoundTrip(spirvLibrary, *spirvManifest) == lodestone::CookError::Success,
                     "the round trip reads the SPIR-V words back through SpirvWords");
        const std::vector<std::byte>& spirvBytes = *spirvManifest;
        const ManifestResult<BundleView> spirvBundle = BundleView::Open(spirvBytes);
        const ManifestResult<EnvironmentView> spirvEnvironment =
            spirvBundle.has_value() ? spirvBundle->OpenEnvironment(1u, 0u)
                                    : ManifestResult<EnvironmentView>{ std::unexpected(spirvBundle.error()) };
        runner.Check(spirvEnvironment.has_value() && spirvEnvironment->SpirvWords(1u).size() == 3u,
                     "a 12-byte SPIR-V source reads back as three words");
    }

    runner.BeginSection("the sampler and texture pairs read back, and a damaged pair is rejected");
    const lodestone::SamplerPairList onePair{ lodestone::SamplerTexturePair{ .Texture = 0u, .Sampler = 1u } };
    const CookedLibrary sampledLibrary = MakeSampledLibrary(onePair, onePair);
    const lodestone::CookResult<std::vector<std::byte>> sampledManifest = EmitShaderManifest(sampledLibrary);
    runner.Check(sampledManifest.has_value(), "the emitter accepts a module with a sampler and texture pair");
    if (sampledManifest.has_value())
    {
        const std::vector<std::byte>& sampledBytes = *sampledManifest;
        runner.Check(lodestone::VerifyManifestRoundTrip(sampledLibrary, sampledBytes) == lodestone::CookError::Success,
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
            runner.Check(pairs.size() == 1u && instance.Layout()[pairs[0].Texture].Name() == "Albedo" &&
                             instance.Layout()[pairs[0].Sampler].Name() == "LinearSampler",
                         "a pair resolves through the layout to the texture and the sampler");
        }

        // a round trip against a cook with no pairs proves that the round trip compares the pairs
        const CookedLibrary unpairedLibrary = MakeSampledLibrary({}, {});
        runner.Check(lodestone::VerifyManifestRoundTrip(unpairedLibrary, sampledBytes) ==
                         lodestone::CookError::ManifestSamplerPairMismatch,
                     "a manifest whose pairs differ from the cook fails the round trip");

        const EnvironmentDirectoryEntry sampledEntry =
            ReadRecord<EnvironmentDirectoryEntry>(sampledBytes, ReadRecord<Header>(sampledBytes, 0u).EnvironmentDirectoryOffset);
        const EnvironmentHeader sampledHeader = ReadRecord<EnvironmentHeader>(sampledBytes, sampledEntry.ExtentOffset);
        std::vector<std::byte> swappedPair = sampledBytes;
        WriteValue(swappedPair,
                   sampledEntry.ExtentOffset + sampledHeader.SamplerTexturePairs.Offset,
                   lodestone::manifest::SamplerTexturePair{ .Texture = 1u, .Sampler = 0u });
        runner.Check(EnvironmentErrorFrom(swappedPair, 0u) == ErrorCode::InvalidSamplerTexturePair,
                     "a pair whose texture position names the sampler is rejected when the environment opens");

        std::vector<std::byte> pastVisibility = sampledBytes;
        WriteValue(pastVisibility,
                   sampledEntry.ExtentOffset + sampledHeader.SamplerTexturePairs.Offset,
                   lodestone::manifest::SamplerTexturePair{ .Texture = 0u, .Sampler = 2u });
        runner.Check(EnvironmentErrorFrom(pastVisibility, 0u) == ErrorCode::InvalidSamplerTexturePair,
                     "a pair position past the visibility list is rejected");

        std::vector<std::byte> badPairList = sampledBytes;
        WriteValue(badPairList,
                   sampledEntry.ExtentOffset + sampledHeader.EntryPointInstanceTableOffset +
                       offsetof(lodestone::manifest::EntryPointInstance, SamplerPairs),
                   5u);
        runner.Check(EnvironmentErrorFrom(badPairList, 0u) == ErrorCode::InvalidEntryPointInstanceSamplerPairIndex,
                     "an instance that names a pair list past the table is rejected");
    }

    CookedLibrary unrecordedLibrary = MakeSampledLibrary({}, {});
    unrecordedLibrary.Profiles[0].SamplerTexturePairsRecorded = false;
    const lodestone::CookResult<std::vector<std::byte>> unrecordedManifest = EmitShaderManifest(unrecordedLibrary);
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
    runner.Check(lodestone::VerifySamplerPairsAcrossProfiles(sampledLibrary) == lodestone::CookError::Success,
                 "two profiles that read the same pairs agree");
    const CookedLibrary disagreeing = MakeSampledLibrary(onePair, {});
    runner.Check(lodestone::VerifySamplerPairsAcrossProfiles(disagreeing) ==
                     lodestone::CookError::SamplerPairsTargetMismatch,
                 "a pair that one profile reads and the other does not fails the cook");
    CookedLibrary oneUnrecorded = MakeSampledLibrary(onePair, {});
    oneUnrecorded.Profiles[1].SamplerTexturePairsRecorded = false;
    runner.Check(lodestone::VerifySamplerPairsAcrossProfiles(oneUnrecorded) == lodestone::CookError::Success,
                 "a profile that did not record its pairs is not compared");

    runner.BeginSection("the entry point constants read back, and a damaged record is rejected");
    const CookedLibrary constantsLibrary = MakeConstantsLibrary(true);
    const lodestone::CookResult<std::vector<std::byte>> constantsManifest = EmitShaderManifest(constantsLibrary);
    runner.Check(constantsManifest.has_value(), "the emitter accepts a module with entry point constants");
    if (constantsManifest.has_value())
    {
        const std::vector<std::byte>& constantsBytes = *constantsManifest;
        runner.Check(lodestone::VerifyManifestRoundTrip(constantsLibrary, constantsBytes) == lodestone::CookError::Success,
                     "the round trip reads the constants back");
        runner.Check(lodestone::VerifyManifestRoundTrip(MakeConstantsLibrary(false), constantsBytes) ==
                         lodestone::CookError::ManifestEntryPointConstantsMismatch,
                     "a manifest whose constants differ from the cook fails the round trip");

        const ManifestResult<BundleView> constantsBundle = BundleView::Open(constantsBytes);
        const ManifestResult<EnvironmentView> constantsEnvironment =
            constantsBundle.has_value() ? constantsBundle->OpenEnvironment(0u, 0u)
                                        : ManifestResult<EnvironmentView>{ std::unexpected(constantsBundle.error()) };
        runner.Check(constantsEnvironment.has_value(), "the environment with constants opens");
        if (constantsEnvironment.has_value())
        {
            const lodestone::manifest::EntryPointInstanceView instance =
                constantsEnvironment->VariantAt(0u).EntryPoint(0u);
            const lodestone::manifest::EntryPointConstantsView constants = instance.Constants();
            runner.Check(constants.Delivery() == lodestone::EntryPointConstantsDelivery::UniformBuffer &&
                             constants.ByteSize() == 16u && constants.Members().Size() == 1u &&
                             constants.Members()[0u].Name() == "tint" &&
                             instance.Layout()[constants.BindingPosition()].Name() == "entryPointParams",
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
        runner.Check(rejectRecord(unknownDelivery) == ErrorCode::InvalidEntryPointConstants,
                     "an unknown delivery is rejected");
        lodestone::manifest::EntryPointConstants wrongSize = goodRecord;
        wrongSize.ByteSize = 32u;
        runner.Check(rejectRecord(wrongSize) == ErrorCode::InvalidEntryPointConstants,
                     "a uniform buffer of a different size than the constants is rejected");
        lodestone::manifest::EntryPointConstants pastVisibility = goodRecord;
        pastVisibility.Binding = 5u;
        runner.Check(rejectRecord(pastVisibility) == ErrorCode::InvalidEntryPointConstants,
                     "a binding position past the visibility list is rejected");
        lodestone::manifest::EntryPointConstants positionedPush = goodRecord;
        positionedPush.Delivery = std::to_underlying(lodestone::EntryPointConstantsDelivery::PushConstant);
        positionedPush.Binding = 1u;
        runner.Check(rejectRecord(positionedPush) == ErrorCode::InvalidEntryPointConstants,
                     "a push constant block with a binding position is rejected");
        lodestone::manifest::EntryPointConstants membersPastTable = goodRecord;
        membersPastTable.FirstUniformMember = 1000u;
        runner.Check(rejectRecord(membersPastTable) == ErrorCode::InvalidEntryPointConstants,
                     "a member run past the member table is rejected");

        std::vector<std::byte> badIndex = constantsBytes;
        WriteValue(badIndex,
                   constantsEntry.ExtentOffset + constantsHeader.EntryPointInstanceTableOffset +
                       offsetof(lodestone::manifest::EntryPointInstance, Constants),
                   9u);
        runner.Check(EnvironmentErrorFrom(badIndex, 0u) == ErrorCode::InvalidEntryPointInstanceConstantsIndex,
                     "an instance that names a constants record past the table is rejected");
    }

    return runner.Report();
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif
