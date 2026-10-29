#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "ShaderManifestIndex.hpp"
#include "VariantKey.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "emit/OutputSink.hpp"
#include "emit/ShaderManifestEmitter.hpp"
#include "target/StorageFormatTable.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace lodestone;

struct CookOutcome
{
    CookError Result{ CookError::Invalid };
    std::vector<std::byte> Bytes;
    std::vector<Diagnostic> Records;
};

CookOutcome Cook(const std::filesystem::path& module_path, std::string_view target)
{
    CookerOptions options;
    options.ModulePaths.push_back(module_path);
    options.TargetNames.emplace_back(target);
    options.OutputPath = "ShimPlacement";
    options.VerifyDeterministic = true;

    MemoryOutputSink sink;
    RecordingDiagnosticSink diagnostics;
    const CookResult<CookStatistics> cooked = RunCook(options, sink, diagnostics);
    CookOutcome outcome{ .Result = cooked.has_value() ? CookError::Success : cooked.error(),
                         .Records = std::move(diagnostics.Records()) };
    const auto manifest = sink.GetArtifacts().find(std::string{ k_ManifestFileName });
    if (manifest != sink.GetArtifacts().end())
    {
        outcome.Bytes.resize(manifest->second.size());
        std::memcpy(outcome.Bytes.data(), manifest->second.data(), outcome.Bytes.size());
    }
    return outcome;
}

void PrintErrors(const CookOutcome& outcome)
{
    for (const Diagnostic& record : outcome.Records)
    {
        if (record.Severity == DiagnosticSeverity::Error || record.Severity == DiagnosticSeverity::Warning)
        {
            std::println(stderr, "      record: {} {}", record.File, record.Message);
        }
    }
}

std::optional<manifest::ResolvedResource> FindResource(const manifest::LayoutRange& layout,
                                                       std::string_view scope,
                                                       std::string_view name)
{
    for (const manifest::ResolvedResource resource : layout)
    {
        if (resource.ScopeName() == scope && resource.Name() == name)
        {
            return resource;
        }
    }
    return std::nullopt;
}

struct ExpectedValue
{
    std::string_view EntryPoint;
    std::string_view Scope;
    std::string_view Name;
    PlacementKind Kind;
    StoredValueContainer Container;
    uint32_t ByteOffset;
};

// each shim of a block takes 16 bytes in the std140 layout, and the push constants take 8
constexpr ExpectedValue k_Expected[] = {
    { "BlockCS", "material_Points", "data", PlacementKind::Pointer, StoredValueContainer::UniformBuffer, 0u },
    { "BlockCS", "material_Surface", "texture", PlacementKind::Handle, StoredValueContainer::UniformBuffer, 16u },
    { "BlockCS", "material_Filter", "sampler", PlacementKind::Handle, StoredValueContainer::UniformBuffer, 32u },
    { "EntryCS", "entryPointParams_scratch", "data", PlacementKind::Pointer, StoredValueContainer::PushConstants, 0u },
};

} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    tests::TestRunner runner{ "ShimPlacement" };

    runner.BeginSection("the shim material cooks for spirv-modern-mutable, with every round trip and two identical cooks");
    const CookOutcome modern = Cook(assetDirectory / "ShimMaterial.slang", "spirv-modern-mutable");
    const CookOutcome bound = Cook(assetDirectory / "ShimMaterial.slang", "spirv");
    if (!runner.CheckEqual(modern.Result, CookError::Success, "the spirv-modern-mutable cook succeeds"))
    {
        PrintErrors(modern);
    }
    runner.CheckEqual(bound.Result, CookError::Success, "the spirv cook of the same module succeeds");

    const manifest::ManifestResult<manifest::BundleView> modernBundle = manifest::BundleView::Open(modern.Bytes);
    const manifest::ManifestResult<manifest::BundleView> boundBundle = manifest::BundleView::Open(bound.Bytes);
    const manifest::ManifestResult<manifest::EnvironmentView> modernEnvironment =
        modernBundle.has_value() ? modernBundle->OpenEnvironment(0u, 0u) : std::unexpected(modernBundle.error());
    const manifest::ManifestResult<manifest::EnvironmentView> boundEnvironment =
        boundBundle.has_value() ? boundBundle->OpenEnvironment(0u, 0u) : std::unexpected(boundBundle.error());
    if (!runner.CheckOk(modernEnvironment, "the spirv-modern-mutable environment opens, with every stored value checked") ||
        !runner.CheckOk(boundEnvironment, "the spirv environment opens"))
    {
        return runner.Report();
    }

    runner.BeginSection("the profile records the form of each class");
    const PlacementForms forms = modernBundle->Profiles()[0].Forms;
    runner.CheckEqual(modernBundle->Profiles()[0].HeapLowering, manifest::DescriptorHeapLowering::MutableDescriptor,
                      "spirv-modern-mutable records the mutable descriptor lowering");
    runner.CheckEqual(modernBundle->Profiles()[0].DescriptorHeapSet, 3u, "the heap is in set 3");
    runner.CheckEqual(forms.Buffer, PlacementKind::Pointer, "buffers are Pointer");
    runner.Check(forms.Texture == PlacementKind::Handle && forms.Sampler == PlacementKind::Handle &&
                     forms.StorageTexture == PlacementKind::Handle,
                 "textures, samplers, and storage textures are Handle");

    for (const ExpectedValue& expected : k_Expected)
    {
        runner.BeginSection(std::format("{}: {}.{} is a stored value", expected.EntryPoint, expected.Scope, expected.Name));
        const manifest::ManifestResult<uint32_t> modernEntry = modernBundle->Module(0u).FindEntryPointIndex(expected.EntryPoint);
        const manifest::ManifestResult<uint32_t> boundEntry = boundBundle->Module(0u).FindEntryPointIndex(expected.EntryPoint);
        if (!runner.CheckOk(modernEntry, "the entry point exists") || !runner.CheckOk(boundEntry, "the entry point exists"))
        {
            continue;
        }

        const manifest::EntryPointInstanceView modernInstance = modernEnvironment->VariantAt(0u).EntryPoint(*modernEntry);
        const manifest::LayoutRange modernLayout = modernInstance.Layout();
        const std::optional<manifest::ResolvedResource> value = FindResource(modernLayout, expected.Scope, expected.Name);
        if (!runner.Check(value.has_value(), "the entry point reads it"))
        {
            continue;
        }

        runner.CheckEqual(value->Placement(), expected.Kind, "its form");
        runner.CheckEqual(value->ValueContainer(), expected.Container, "its container");
        runner.CheckEqual(value->ValueOffset(), expected.ByteOffset, "its byte offset in the container");
        if (expected.Container == StoredValueContainer::UniformBuffer)
        {
            bool containerFound = false;
            for (const manifest::ResolvedResource candidate : modernLayout)
            {
                containerFound = containerFound ||
                                 (candidate.Placement() == PlacementKind::Bound && candidate.Kind() == BindingKind::UniformBuffer &&
                                  candidate.PlacementValue().Word0 == value->PlacementValue().Word0 &&
                                  candidate.PlacementValue().Word1 == value->PlacementValue().Word1);
            }
            runner.Check(containerFound, "its uniform buffer is in the same layout");
        }
        else
        {
            runner.CheckEqual(modernInstance.Constants().Delivery(), EntryPointConstantsDelivery::PushConstant,
                              "the entry point has push constants");
        }

        // the row of the shim table states the resource, and reflection of the Bound form is the second opinion
        const std::optional<manifest::ResolvedResource> boundResource =
            FindResource(boundEnvironment->VariantAt(0u).EntryPoint(*boundEntry).Layout(), expected.Scope, expected.Name);
        if (runner.Check(boundResource.has_value(), "the Bound cook has the same shim resource"))
        {
            runner.CheckEqual(boundResource->Placement(), PlacementKind::Bound, "the Bound cook binds it");
            runner.CheckEqual(value->Kind(), boundResource->Kind(), "the same kind as the Bound form");
            runner.CheckEqual(value->Shape(), boundResource->Shape(), "the same shape as the Bound form");
            runner.CheckEqual(value->Access(), boundResource->Access(), "the same access as the Bound form");
            runner.CheckEqual(value->SampleType(), boundResource->SampleType(), "the same sample type as the Bound form");
            runner.CheckEqual(value->ElementStride(), boundResource->ElementStride(), "the same element stride as the Bound form");
        }
    }

    runner.BeginSection("a texture sampled through two handles gives one pair of the two stored values");
    {
        const manifest::ManifestResult<uint32_t> blockEntry = modernBundle->Module(0u).FindEntryPointIndex("BlockCS");
        if (runner.CheckOk(blockEntry, "BlockCS exists"))
        {
            const manifest::EntryPointInstanceView instance = modernEnvironment->VariantAt(0u).EntryPoint(*blockEntry);
            const std::span<const manifest::SamplerTexturePair> pairs = instance.SamplerTexturePairs();
            if (runner.CheckEqual(pairs.size(), 1u, "one pair"))
            {
                const manifest::ResolvedResource texture = instance.Layout()[pairs[0].Texture];
                const manifest::ResolvedResource sampler = instance.Layout()[pairs[0].Sampler];
                runner.CheckEqual(texture.ScopeName(), "material_Surface", "the texture is the handle in material.Surface");
                runner.CheckEqual(texture.Placement(), PlacementKind::Handle, "the texture side is a Handle");
                runner.CheckEqual(sampler.ScopeName(), "material_Filter", "the sampler is the handle in material.Filter");
                runner.CheckEqual(sampler.Placement(), PlacementKind::Handle, "the sampler side is a Handle");
            }
        }
    }

    runner.BeginSection("a storage handle takes the format of each variant");
    const CookOutcome storage = Cook(assetDirectory / "ShimStorageAxis.slang", "spirv-modern-mutable");
    if (!runner.CheckEqual(storage.Result, CookError::Success, "the format axis module cooks for spirv-modern-mutable"))
    {
        PrintErrors(storage);
    }
    const manifest::ManifestResult<manifest::BundleView> storageBundle = manifest::BundleView::Open(storage.Bytes);
    const manifest::ManifestResult<manifest::EnvironmentView> storageEnvironment =
        storageBundle.has_value() ? storageBundle->OpenEnvironment(0u, 0u) : std::unexpected(storageBundle.error());
    if (runner.CheckOk(storageEnvironment, "the environment opens"))
    {
        const ManifestIndex index{ *storageEnvironment };
        for (const StorageFormatInfo& format : GetStorageFormats())
        {
            const std::expected<VariantKey, QueryError> key =
                index.Query().Where("LEVEL_FORMAT", AxisValueDomain::Enum, format.EnumCase).Single();
            const std::optional<uint32_t> variant = key.has_value() ? storageEnvironment->FindVariant(*key) : std::nullopt;
            if (!runner.Check(variant.has_value(), std::format("{}: the variant exists", format.EnumCase)))
            {
                continue;
            }
            const std::optional<manifest::ResolvedResource> target =
                FindResource(storageEnvironment->VariantAt(*variant).EntryPoint(0u).Layout(), "level_target", "texture");
            if (runner.Check(target.has_value(), std::format("{}: the storage handle is in the layout", format.EnumCase)))
            {
                runner.CheckEqual(target->Placement(), PlacementKind::Handle, std::format("{}: a Handle", format.EnumCase));
                runner.CheckEqual(target->StorageFormat(), format.Format, std::format("{}: the format of the variant", format.EnumCase));
                runner.CheckEqual(target->Access(), ResourceAccess::WriteOnly, std::format("{}: write-only", format.EnumCase));
            }
        }
    }

    runner.BeginSection("a pipeline merges each stored value once, apart from the uniform buffer that stores it");
    const CookOutcome stages = Cook(assetDirectory / "ShimStages.slang", "spirv-modern-mutable");
    if (!runner.CheckEqual(stages.Result, CookError::Success, "the vertex and fragment module cooks for spirv-modern-mutable"))
    {
        PrintErrors(stages);
    }
    const manifest::ManifestResult<manifest::BundleView> stagesBundle = manifest::BundleView::Open(stages.Bytes);
    const manifest::ManifestResult<manifest::EnvironmentView> stagesEnvironment =
        stagesBundle.has_value() ? stagesBundle->OpenEnvironment(0u, 0u) : std::unexpected(stagesBundle.error());
    if (runner.CheckOk(stagesEnvironment, "the environment opens"))
    {
        const manifest::ManifestResult<uint32_t> vertex = stagesBundle->Module(0u).FindEntryPointIndex("MainVS");
        const manifest::ManifestResult<uint32_t> fragment = stagesBundle->Module(0u).FindEntryPointIndex("MainFS");
        if (runner.CheckOk(vertex, "the vertex stage exists") && runner.CheckOk(fragment, "the fragment stage exists"))
        {
            const std::array<manifest::EntryPointInstanceView, 2u> pipeline{ stagesEnvironment->VariantAt(0u).EntryPoint(*vertex),
                                                                             stagesEnvironment->VariantAt(0u).EntryPoint(*fragment) };
            const manifest::ManifestResult<manifest::MergedLayoutView> merged = manifest::MergedLayoutView::Merge(pipeline);
            if (runner.CheckOk(merged, "the stages merge, so a handle does not collide with its uniform buffer"))
            {
                uint32_t uniformBuffers = 0u;
                uint32_t handles = 0u;
                for (const manifest::MergedResource resource : *merged)
                {
                    const bool bothStages = resource.Stages == (ShaderStageKind::Vertex | ShaderStageKind::Fragment);
                    uniformBuffers += resource.Resource.Placement() == PlacementKind::Bound && bothStages ? 1u : 0u;
                    handles += resource.Resource.Placement() == PlacementKind::Handle && bothStages ? 1u : 0u;
                }
                runner.CheckEqual(uniformBuffers, 1u, "the uniform buffer appears once, for both stages");
                runner.CheckEqual(handles, 2u, "the texture and the sampler handle appear once each, for both stages");
            }
        }
    }

    runner.BeginSection("a global shim is stored in the uniform buffer of global data, named as the code names it");
    const CookOutcome global = Cook(assetDirectory / "ShimGlobal.slang", "spirv-modern-mutable");
    if (!runner.CheckEqual(global.Result, CookError::Success, "the global shim module cooks for spirv-modern-mutable"))
    {
        PrintErrors(global);
    }
    const manifest::ManifestResult<manifest::BundleView> globalBundle = manifest::BundleView::Open(global.Bytes);
    const manifest::ManifestResult<manifest::EnvironmentView> globalEnvironment =
        globalBundle.has_value() ? globalBundle->OpenEnvironment(0u, 0u) : std::unexpected(globalBundle.error());
    if (runner.CheckOk(globalEnvironment, "the environment opens"))
    {
        uint32_t containers = 0u;
        uint32_t pointers = 0u;
        uint32_t plainBuffers = 0u;
        for (const manifest::ResolvedResource resource : globalEnvironment->VariantAt(0u).EntryPoint(0u).Layout())
        {
            plainBuffers += resource.Kind() == BindingKind::StorageBuffer && resource.Name() == "plain" &&
                                    resource.Placement() == PlacementKind::Bound
                                ? 1u
                                : 0u;
            containers += resource.Kind() == BindingKind::UniformBuffer && resource.Name() == "globalParams" ? 1u : 0u;
            pointers += resource.Placement() == PlacementKind::Pointer &&
                                resource.ValueContainer() == StoredValueContainer::UniformBuffer
                            ? 1u
                            : 0u;
        }

        runner.CheckEqual(containers, 1u, "the container is the uniform buffer globalParams");
        runner.CheckEqual(pointers, 2u, "both buffers are pointers in that uniform buffer");
        runner.CheckEqual(plainBuffers, 1u, "the plain global buffer, in the element of that uniform buffer, is a bound binding");
    }

    runner.BeginSection("a shim in a ConstantBuffer cooks on every profile, as a buffer or as a stored pointer");
    for (const std::string_view target : { "spirv", "wgsl", "spirv-modern-mutable" })
    {
        const CookOutcome constantBuffer = Cook(assetDirectory / "ShimConstantBuffer.slang", target);
        if (!runner.CheckEqual(constantBuffer.Result, CookError::Success, std::format("the module cooks for {}", target)))
        {
            PrintErrors(constantBuffer);
        }
        const manifest::ManifestResult<manifest::BundleView> cbBundle = manifest::BundleView::Open(constantBuffer.Bytes);
        const manifest::ManifestResult<manifest::EnvironmentView> cbEnvironment =
            cbBundle.has_value() ? cbBundle->OpenEnvironment(0u, 0u) : std::unexpected(cbBundle.error());
        if (!runner.CheckOk(cbEnvironment, std::format("the environment opens for {}", target)))
        {
            continue;
        }

        uint32_t uniformBuffers = 0u;
        uint32_t boundStorage = 0u;
        uint32_t pointers = 0u;
        for (const manifest::ResolvedResource resource : cbEnvironment->VariantAt(0u).EntryPoint(0u).Layout())
        {
            uniformBuffers += resource.Placement() == PlacementKind::Bound && resource.Kind() == BindingKind::UniformBuffer ? 1u : 0u;
            boundStorage += resource.Placement() == PlacementKind::Bound && resource.Kind() == BindingKind::StorageBuffer ? 1u : 0u;
            pointers += resource.Placement() == PlacementKind::Pointer &&
                                resource.ValueContainer() == StoredValueContainer::UniformBuffer
                            ? 1u
                            : 0u;
        }

        const bool modern = target == "spirv-modern-mutable";
        runner.CheckEqual(uniformBuffers, 1u, std::format("{}: the constant buffer is one uniform buffer", target));
        runner.CheckEqual(boundStorage, modern ? 1u : 2u,
                          std::format("{}: the output, and the shim as a bound storage buffer unless it is a pointer", target));
        runner.CheckEqual(pointers, modern ? 1u : 0u, std::format("{}: the shim is a pointer stored in the uniform buffer", target));
    }

    runner.BeginSection("a padded shim element has the same 64-byte stride on every profile");
    for (const std::string_view target : { "wgsl", "spirv", "spirv-modern-mutable" })
    {
        const CookOutcome padded = Cook(assetDirectory / "ShimPadded.slang", target);
        if (!runner.CheckEqual(padded.Result, CookError::Success, std::format("the module cooks for {}", target)))
        {
            PrintErrors(padded);
        }
        const manifest::ManifestResult<manifest::BundleView> paddedBundle = manifest::BundleView::Open(padded.Bytes);
        const manifest::ManifestResult<manifest::EnvironmentView> paddedEnvironment =
            paddedBundle.has_value() ? paddedBundle->OpenEnvironment(0u, 0u) : std::unexpected(paddedBundle.error());
        if (!runner.CheckOk(paddedEnvironment, std::format("the environment opens for {}", target)))
        {
            continue;
        }

        const std::optional<manifest::ResolvedResource> vertices =
            FindResource(paddedEnvironment->VariantAt(0u).EntryPoint(0u).Layout(), "geometry_vertices", "data");
        if (runner.Check(vertices.has_value(), std::format("{}: the vertex buffer is in the layout", target)))
        {
            runner.CheckEqual(vertices->ElementStride(), 64u, std::format("{}: the element stride is 64", target));
            runner.CheckEqual(vertices->Placement(),
                              target == "spirv-modern-mutable" ? PlacementKind::Pointer : PlacementKind::Bound,
                              std::format("{}: the form of the profile", target));
        }
    }

    runner.BeginSection("wgsl and spirv-modern-mutable cook together");
    {
        CookerOptions options;
        options.ModulePaths.push_back(assetDirectory / "ShimMaterial.slang");
        options.TargetNames = { "wgsl", "spirv-modern-mutable" };
        options.OutputPath = "ShimPlacementTogether";
        MemoryOutputSink sink;
        RecordingDiagnosticSink diagnostics;
        const CookResult<CookStatistics> together = RunCook(options, sink, diagnostics);
        if (!runner.CheckOk(together, "the cross-profile checks accept a Bound profile beside a Handle profile"))
        {
            PrintErrors(CookOutcome{ .Records = std::move(diagnostics.Records()) });
        }
    }

    runner.BeginSection("every shim asset cooks for spirv-modern-typed, with the same forms and the per-descriptor-type heap");
    for (const std::string_view asset : { "ShimMaterial", "ShimStorageAxis", "ShimStages", "ShimGlobal", "ShimConstantBuffer", "ShimPadded" })
    {
        const CookOutcome typed = Cook(assetDirectory / std::format("{}.slang", asset), "spirv-modern-typed");
        if (!runner.CheckEqual(typed.Result, CookError::Success, std::format("{} cooks for spirv-modern-typed", asset)))
        {
            PrintErrors(typed);
            continue;
        }

        const manifest::ManifestResult<manifest::BundleView> typedBundle = manifest::BundleView::Open(typed.Bytes);
        if (!runner.CheckOk(typedBundle, std::format("{}: the typed bundle opens", asset)))
        {
            continue;
        }

        const manifest::Profile typedProfile = typedBundle->Profiles()[0];
        runner.CheckEqual(typedProfile.HeapLowering, manifest::DescriptorHeapLowering::PerDescriptorType,
                          std::format("{}: the profile records the per-descriptor-type lowering", asset));
        runner.CheckEqual(typedProfile.DescriptorHeapSet, 3u, std::format("{}: the heap is in set 3", asset));
        runner.CheckEqual(typedProfile.Forms.Buffer, PlacementKind::Pointer, std::format("{}: buffers are Pointer", asset));
        runner.CheckEqual(typedProfile.Forms.Texture, PlacementKind::Handle, std::format("{}: textures are Handle", asset));
    }

    runner.BeginSection("the material gives the same stored values and pair under both lowerings");
    {
        const CookOutcome typedMaterial = Cook(assetDirectory / "ShimMaterial.slang", "spirv-modern-typed");
        const manifest::ManifestResult<manifest::BundleView> typedBundle = manifest::BundleView::Open(typedMaterial.Bytes);
        const manifest::ManifestResult<manifest::EnvironmentView> typedEnvironment =
            typedBundle.has_value() ? typedBundle->OpenEnvironment(0u, 0u) : std::unexpected(typedBundle.error());
        if (runner.CheckOk(typedEnvironment, "the typed environment opens, with every stored value checked"))
        {
            for (const ExpectedValue& expected : k_Expected)
            {
                const manifest::ManifestResult<uint32_t> entry = typedBundle->Module(0u).FindEntryPointIndex(expected.EntryPoint);
                const manifest::ManifestResult<uint32_t> modernEntry = modernBundle->Module(0u).FindEntryPointIndex(expected.EntryPoint);
                if (!runner.CheckOk(entry, "the entry point exists") || !runner.CheckOk(modernEntry, "the entry point exists"))
                {
                    continue;
                }

                const std::optional<manifest::ResolvedResource> typedValue =
                    FindResource(typedEnvironment->VariantAt(0u).EntryPoint(*entry).Layout(), expected.Scope, expected.Name);
                const std::optional<manifest::ResolvedResource> mutableValue =
                    FindResource(modernEnvironment->VariantAt(0u).EntryPoint(*modernEntry).Layout(), expected.Scope, expected.Name);
                if (runner.Check(typedValue.has_value() && mutableValue.has_value(), std::format("{}.{} is in both layouts", expected.Scope, expected.Name)))
                {
                    runner.CheckEqual(typedValue->Placement(), mutableValue->Placement(), "the same form");
                    runner.CheckEqual(typedValue->ValueOffset(), mutableValue->ValueOffset(), "the same byte offset");
                    runner.CheckEqual(typedValue->ValueContainer(), mutableValue->ValueContainer(), "the same container");
                }
            }

            const manifest::ManifestResult<uint32_t> blockEntry = typedBundle->Module(0u).FindEntryPointIndex("BlockCS");
            if (runner.CheckOk(blockEntry, "BlockCS exists"))
            {
                runner.CheckEqual(typedEnvironment->VariantAt(0u).EntryPoint(*blockEntry).SamplerTexturePairs().size(), 1u,
                                  "the pair of the texture and the sampler handle is traced under the typed lowering");
            }
        }
    }

    runner.BeginSection("both modern profiles cook together, and the manifest keeps the lowering of each");
    {
        CookerOptions options;
        options.ModulePaths.push_back(assetDirectory / "ShimMaterial.slang");
        options.TargetNames = { "spirv-modern-mutable", "spirv-modern-typed" };
        options.OutputPath = "ShimPlacementModern";
        MemoryOutputSink sink;
        RecordingDiagnosticSink diagnostics;
        const CookResult<CookStatistics> together = RunCook(options, sink, diagnostics);
        if (!runner.CheckOk(together, "the cross-profile checks accept the two lowerings"))
        {
            PrintErrors(CookOutcome{ .Records = std::move(diagnostics.Records()) });
        }

        const auto manifestFile = sink.GetArtifacts().find(std::string{ k_ManifestFileName });
        if (runner.Check(manifestFile != sink.GetArtifacts().end(), "the cook wrote a manifest"))
        {
            std::vector<std::byte> bytes(manifestFile->second.size());
            std::memcpy(bytes.data(), manifestFile->second.data(), bytes.size());
            const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(bytes);
            if (runner.CheckOk(bundle, "the bundle opens") && runner.CheckEqual(bundle->Profiles().size(), 2u, "two profiles"))
            {
                runner.CheckEqual(bundle->Profiles()[0].HeapLowering, manifest::DescriptorHeapLowering::MutableDescriptor,
                                  "the first profile is mutable");
                runner.CheckEqual(bundle->Profiles()[1].HeapLowering, manifest::DescriptorHeapLowering::PerDescriptorType,
                                  "the second profile is typed");
            }
        }
    }

    return runner.Report();
}
