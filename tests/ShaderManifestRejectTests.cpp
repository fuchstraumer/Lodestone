#include "model/CookedLibrary.hpp"
#include "model/ShaderDataSchema.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "emit/ShaderManifestEmitter.hpp"
#include "TestHarness.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
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

using lodestone::CookedModule;
using lodestone::EmitShaderManifest;
using lodestone::ShaderManifestErrorCode;
using lodestone::ShaderManifestView;

namespace
{

/** Byte offsets of the header fields this file damages. The file format pins them. */
constexpr size_t k_MagicOffset = 0u;
constexpr size_t k_VersionOffset = 4u;
constexpr size_t k_FileSizeOffset = 8u;
constexpr size_t k_StringTableOffsetOffset = 16u;

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
    binding.Shape = lodestone::ResourceShape::Buffer;

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

    // A v2 manifest carries one variant key per variant, strictly ascending and parallel to the
    // variant table. This hand-built module predates that table, so give it keys the reader accepts.
    module.VariantKeys = { 0u, 1u };

    return module;
}

std::vector<std::byte> ToBytes(const std::string& manifest)
{
    std::vector<std::byte> bytes(manifest.size());
    std::memcpy(bytes.data(), manifest.data(), manifest.size());
    return bytes;
}

void WriteUint32(std::vector<std::byte>& bytes, size_t offset, uint32_t value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

ShaderManifestErrorCode ErrorFrom(std::span<const std::byte> bytes)
{
    const lodestone::ManifestResult<ShaderManifestView> opened = ShaderManifestView::Open(bytes);
    if (opened.has_value())
    {
        return ShaderManifestErrorCode::Success;
    }

    return opened.error().Code;
}

} // namespace

int main()
{
    lodestone::tests::TestRunner runner{ "ShaderManifestRejectTests" };

    const CookedModule module = MakeSmallModule();
    const std::string manifest = EmitShaderManifest(module);
    const std::vector<std::byte> valid = ToBytes(manifest);

    runner.BeginSection("a manifest the cooker wrote opens");
    const lodestone::ManifestResult<ShaderManifestView> opened = ShaderManifestView::Open(valid);
    runner.Check(opened.has_value(), "the emitter produces a manifest the reader accepts");
    if (opened.has_value())
    {
        runner.Check(opened.value().ModuleName() == "TestModule", "the reader returns the module name");
        runner.Check(opened.value().EntryPoints().size() == 1u, "the reader returns the entry point");
        runner.Check(opened.value().Variants().size() == 2u, "the reader returns both variants");
    }

    runner.BeginSection("a short file is rejected before any field is read");
    const std::span<const std::byte> truncated{ valid.data(), sizeof(lodestone::ShaderManifestHeader) - 1u };
    runner.Check(ErrorFrom(truncated) == ShaderManifestErrorCode::TooSmall,
                 "a span smaller than the header is TooSmall");

    runner.BeginSection("a misaligned span is rejected");
    // The reader maps 64-bit fields in place, so it cannot accept a span that starts off an 8-byte
    // boundary. The valid case above proves the buffer itself starts aligned.
    const std::span<const std::byte> misaligned{ valid.data() + 1u, valid.size() - 1u };
    runner.Check(ErrorFrom(misaligned) == ShaderManifestErrorCode::Misaligned,
                 "a span that starts one byte in is Misaligned");

    runner.BeginSection("a damaged header field is rejected by name");
    std::vector<std::byte> badMagic = valid;
    WriteUint32(badMagic, k_MagicOffset, 0xDEADBEEFu);
    runner.Check(ErrorFrom(badMagic) == ShaderManifestErrorCode::BadMagic, "a wrong magic is BadMagic");

    std::vector<std::byte> badVersion = valid;
    WriteUint32(badVersion, k_VersionOffset, lodestone::k_ShaderManifestVersion + 1u);
    runner.Check(ErrorFrom(badVersion) == ShaderManifestErrorCode::VersionMismatch,
                 "a future version is VersionMismatch");

    std::vector<std::byte> badSize = valid;
    WriteUint32(badSize, k_FileSizeOffset, static_cast<uint32_t>(valid.size()) + 8u);
    runner.Check(ErrorFrom(badSize) == ShaderManifestErrorCode::SizeMismatch,
                 "a header that claims more bytes than it has is SizeMismatch");

    std::vector<std::byte> badSection = valid;
    WriteUint32(badSection, k_StringTableOffsetOffset, static_cast<uint32_t>(valid.size()) - 1u);
    runner.Check(ErrorFrom(badSection) == ShaderManifestErrorCode::SectionOutOfBounds,
                 "a section that reaches past the file is SectionOutOfBounds");

    return runner.Report();
}
