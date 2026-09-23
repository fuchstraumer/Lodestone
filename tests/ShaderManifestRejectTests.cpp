#include "model/CookedLibrary.hpp"
#include "model/ShaderDataSchema.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "emit/ShaderManifestEmitter.hpp"
#include "VariantKey.hpp"
#include "TestHarness.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
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

CookedModule MakeSmallModule()
{
    CookedModule module;
    module.Name = "TestModule";
    module.Space = nullptr;
    module.SpaceSize = 2u;

    module.EntryPoints.push_back(
        lodestone::LibraryEntryPoint{ .Name="MainCS", .Stage=lodestone::ShaderStageKind::Compute });

    module.Sources.emplace_back("// wgsl for variant zero");
    module.Sources.emplace_back("// wgsl for variant one");

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

    module.RasterStates.emplace_back();

    for (uint32_t i = 0u; i < 2u; ++i)
    {
        lodestone::LibraryVariant variant;
        variant.Index = i;
        variant.Suffix = i == 0u ? "_A" : "_B";
        variant.Description = i == 0u ? "first" : "second";
        variant.SourceIndices.push_back(i);
        variant.VisibilityIndices.push_back(0u);
        variant.RasterIndices.push_back(0u);
        variant.Workgroups.emplace_back(lodestone::WorkgroupSize{ .X=64u, .Y=1u, .Z=1u });
        module.Variants.emplace_back(std::move(variant));
    }

    // One variant key per variant, strictly ascending and parallel to the variant table. This hand-built
    // module has no axis, so give it keys the reader accepts.
    module.VariantKeys = { VariantKey{ 0u }, VariantKey{ 1u } };

    return module;
}

/** One module, cooked for two profiles. The second profile did not cook it, so its environment is empty. */
CookedLibrary MakeSmallLibrary()
{
    CookedLibrary library;
    library.ModuleNames = { "TestModule" };
    library.Profiles = { CookedProfile{ .TargetName = "wgsl", .AccessModel = lodestone::PlacementKind::Bound },
                         CookedProfile{ .TargetName = "spirv", .AccessModel = lodestone::PlacementKind::Bound } };
    library.Environments.emplace_back(MakeSmallModule());
    library.Environments.emplace_back(std::nullopt);
    return library;
}

std::vector<std::byte> ToBytes(const std::string& manifest)
{
    std::vector<std::byte> bytes(manifest.size());
    std::memcpy(bytes.data(), manifest.data(), manifest.size());
    return bytes;
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
    const lodestone::CookResult<std::string> manifest = EmitShaderManifest(library);
    runner.Check(manifest.has_value(), "the emitter accepts the library");
    if (!manifest.has_value())
    {
        return runner.Report();
    }
    const std::vector<std::byte> valid = ToBytes(*manifest);
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
    WriteValue(badSection, offsetof(Header, StringTableOffset), header.HeaderSize - 1u);
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

    return runner.Report();
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif
