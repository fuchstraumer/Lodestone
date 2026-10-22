#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "ResourceFlags.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "ShaderManifestLayout.hpp"
#include "emit/ResourceReport.hpp"
#include "emit/ShaderManifestEmitter.hpp"
#include "emit/ShaderManifestVerify.hpp"
#include "model/AxisInfluence.hpp"
#include "model/CookedLibrary.hpp"
#include "model/ShaderDataSchema.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationTypes.hpp"
#include "permute/PermutationValue.hpp"
#include "ManifestFixtures.hpp"
#include "TestHarness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// SIZE changes one buffer's element count, and DEBUG changes nothing

namespace
{

using namespace lodestone;
using manifest::CreationFields;
using manifest::CreationSource;

constexpr uint32_t k_Counts = 0u;
constexpr uint32_t k_Params = 1u;
constexpr uint32_t k_Lut = 2u;
constexpr uint32_t k_Target = 3u;
constexpr uint32_t k_Sampler = 4u;

PermutationSpace MakeSpace()
{
    const std::vector<PermutationValue> boolValues{ PermutationValue{ false }, PermutationValue{ true } };
    std::vector<PermutationAxis> axes;
    axes.emplace_back("SIZE", boolValues, AxisKind::Tuning, EarliestBindingTime::Cook, AxisValueDomain::Boolean);
    axes.emplace_back("DEBUG", boolValues, AxisKind::Technique, EarliestBindingTime::Cook, AxisValueDomain::Boolean);
    return PermutationSpace{ std::move(axes) };
}

ReflectedBinding MakeBinding(std::string name, uint32_t binding, BindingKind kind, ResourceShape shape)
{
    ReflectedBinding result;
    result.Name = std::move(name);
    result.Placement = BoundPlacement{ .Group = 0u, .Binding = binding };
    result.Kind = kind;
    result.Shape = shape;
    return result;
}

/** One entry point that reads five resources. The footprint list of a variant follows SIZE: 64 or 128 elements. */
struct VariantChoice
{
    bool Size;
    bool Debug;
};

CookedModule BuildModule(const PermutationSpace& space, std::span<const VariantChoice> variants)
{
    CookedModule module;
    module.Name = "ReportModule";
    module.Space = &space;
    module.EntryPoints.push_back(LibraryEntryPoint{ .Name = "CountCS", .Stage = ShaderStageKind::Compute });
    lodestone::tests::AddFixtureSource(module, tests::BytesOf("// one source for every variant"), lodestone::SourceCodeFacts{});

    ReflectedBinding counts = MakeBinding("Counts", 0u, BindingKind::StorageBuffer, ResourceShape::StructuredBuffer);
    counts.ElementStride = 4u;
    counts.Access = ResourceAccess::ReadWrite;
    ReflectedBinding params = MakeBinding("Params", 1u, BindingKind::UniformBuffer, ResourceShape::Invalid);
    params.ByteSize = 16u;
    ReflectedBinding lut = MakeBinding("Lut", 2u, BindingKind::Texture, ResourceShape::Texture2D);
    ReflectedBinding target = MakeBinding("Target", 3u, BindingKind::StorageTexture, ResourceShape::Texture2D);
    target.StorageFormat = TextureFormat::R32Float;
    target.Access = ResourceAccess::WriteOnly;
    ReflectedBinding sampler = MakeBinding("LinearSampler", 4u, BindingKind::Sampler, ResourceShape::Invalid);
    module.Resources = { counts, params, lut, target, sampler };
    module.ResourceLists.push_back(ResourceList{ k_Counts, k_Params, k_Lut, k_Target, k_Sampler });
    module.VisibilityLists.push_back(VisibilityList{ 0u, 1u, 2u, 3u, 4u });

    for (const uint64_t elementCount : { 64u, 128u })
    {
        module.FootprintLists.push_back(FootprintList{ BufferFootprint{ .ElementCount = elementCount, .Expression = "N" },
                                                       std::monostate{},
                                                       TextureFootprint{ .ExtentX = 64u, .Expression = "64" },
                                                       std::monostate{},
                                                       std::monostate{} });
    }
    tests::AddDefaultInstanceTables(module);

    const std::array instances{ tests::ComputeInstance(0u, 0u) };
    for (const VariantChoice& choice : variants)
    {
        LibraryVariant variant = tests::MakeFixtureVariant(
            space,
            module.Variants.size(),
            PermutationAssignment{
                PermutationBinding{ .Axis = &space.Axes()[0], .Value = PermutationValue{ choice.Size } },
                PermutationBinding{ .Axis = &space.Axes()[1], .Value = PermutationValue{ choice.Debug } } },
            instances);
        variant.FootprintListIndex = choice.Size ? 1u : 0u;
        module.VariantKeys.push_back(space.ComputeVariantKey(variant.Canonical));
        module.Variants.push_back(std::move(variant));
    }
    module.SpaceSize = 4u;
    return module;
}

constexpr std::array<VariantChoice, 4u> k_EveryVariant{ { { false, false }, { false, true }, { true, false }, { true, true } } };
// no two variants differ in one axis, so only the closing pass can find the axis that changes the count
constexpr std::array<VariantChoice, 2u> k_Diagonal{ { { false, false }, { true, true } } };

const ResourceFootprintInfluence* FindInfluence(std::span<const ResourceFootprintInfluence> influence, std::string_view name)
{
    for (const ResourceFootprintInfluence& resource : influence)
    {
        if (resource.Name == name)
        {
            return &resource;
        }
    }
    return nullptr;
}

std::vector<std::byte> EmitBytes(const CookedModule& module)
{
    CookedLibrary library;
    library.ModuleNames = { module.Name };
    library.Profiles = { CookedProfile{ .TargetName = "wgsl",
                                        .AccessModel = PlacementKind::Bound,
                                        .CodeFormat = ShaderCodeFormat::Wgsl } };
    library.Environments.emplace_back(module);
    CookResult<std::vector<std::byte>> bytes = EmitShaderManifest(library);
    if (!bytes || VerifyManifestRoundTrip(library, *bytes) != CookError::Success)
    {
        return {};
    }
    return std::move(*bytes);
}

void CheckMeasurement(tests::TestRunner& runner, const PermutationSpace& space)
{
    runner.BeginSection("the footprint axes are measured from the cooked tables");
    const CookedModule every = BuildModule(space, k_EveryVariant);
    const std::vector<ResourceFootprintInfluence> influence = ComputeFootprintInfluence(every);
    const ResourceFootprintInfluence* counts = FindInfluence(influence, "Counts");
    runner.Check(counts != nullptr && counts->Axes[0] == AxisInfluence::Active && counts->Axes[1] == AxisInfluence::Inert,
                 "SIZE changes the count, and DEBUG does not");
    runner.Check(counts != nullptr && counts->PairAxes[0] == AxisInfluence::Active,
                 "a one-axis pair proves SIZE");
    const ResourceFootprintInfluence* lut = FindInfluence(influence, "Lut");
    runner.Check(lut != nullptr && lut->Axes[0] == AxisInfluence::Inert && lut->Axes[1] == AxisInfluence::Inert,
                 "an extent that no axis changes has no axis");

    runner.BeginSection("a sparse cook is closed by the second pass");
    const CookedModule diagonal = BuildModule(space, k_Diagonal);
    const std::vector<ResourceFootprintInfluence> sparse = ComputeFootprintInfluence(diagonal);
    const ResourceFootprintInfluence* sparseCounts = FindInfluence(sparse, "Counts");
    runner.Check(sparseCounts != nullptr && sparseCounts->PairAxes[0] == AxisInfluence::Undetermined &&
                     sparseCounts->PairAxes[1] == AxisInfluence::Undetermined,
                 "no one-axis pair exists, so the first pass decides nothing");
    runner.Check(sparseCounts != nullptr && sparseCounts->Axes[0] == AxisInfluence::Active &&
                     sparseCounts->Axes[1] == AxisInfluence::Active,
                 "the closing pass marks both axes, because the cook cannot separate them");
    const ResourceFootprintInfluence* sparseLut = FindInfluence(sparse, "Lut");
    runner.Check(sparseLut != nullptr && sparseLut->Axes[0] == AxisInfluence::Inert,
                 "an extent that does not change stays without an axis in a sparse cook");
}

void CheckValidator(tests::TestRunner& runner, const PermutationSpace& space)
{
    runner.BeginSection("the validator compares the measured axes with the declared axes");
    RecordingDiagnosticSink sink;
    CookedModule declared = BuildModule(space, k_EveryVariant);
    declared.FootprintDeclarations.push_back(FootprintAxisDeclaration{ .ScopeName = "", .Name = "Counts", .Axes = { 0u } });
    runner.Check(VerifyFootprintAxes(declared, sink) == CookError::Success, "a count that its attribute names passes");

    const CookedModule undeclared = BuildModule(space, k_EveryVariant);
    runner.Check(VerifyFootprintAxes(undeclared, sink) == CookError::FootprintAxisUndeclared,
                 "a count that changes with an axis that no attribute names fails");

    CookedModule wrongAxis = BuildModule(space, k_EveryVariant);
    wrongAxis.FootprintDeclarations.push_back(FootprintAxisDeclaration{ .ScopeName = "", .Name = "Counts", .Axes = { 1u } });
    runner.Check(VerifyFootprintAxes(wrongAxis, sink) == CookError::FootprintAxisUndeclared,
                 "a count whose attribute names another axis fails");

    CookedModule sparse = BuildModule(space, k_Diagonal);
    sparse.FootprintDeclarations.push_back(FootprintAxisDeclaration{ .ScopeName = "", .Name = "Counts", .Axes = { 0u } });
    runner.Check(VerifyFootprintAxes(sparse, sink) == CookError::Success,
                 "a sparse cook passes, because only a one-axis pair proves an axis");
}

void CheckManifest(tests::TestRunner& runner, const PermutationSpace& space)
{
    runner.BeginSection("the footprint axes and the creation fields read back through the manifest");
    const std::vector<std::byte> bytes = EmitBytes(BuildModule(space, k_EveryVariant));
    const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(bytes);
    runner.Check(bundle.has_value(), "the manifest opens, and its round trip passes");
    if (!bundle.has_value())
    {
        return;
    }
    const manifest::ManifestResult<manifest::EnvironmentView> environment = bundle->OpenEnvironment(0u, 0u);
    runner.Check(environment.has_value(), "the environment opens");
    if (!environment.has_value())
    {
        return;
    }

    const manifest::LayoutRange layout = environment->VariantAt(0u).EntryPoint(0u).Layout();
    runner.Check(layout.Size() == 5u, "the entry point reads five resources");
    if (layout.Size() != 5u)
    {
        return;
    }
    runner.Check(layout[k_Counts].IsFootprintAxis(0u) && !layout[k_Counts].IsFootprintAxis(1u),
                 "the count's mask holds SIZE and not DEBUG");
    runner.Check(!layout[k_Lut].IsFootprintAxis(0u) && !layout[k_Lut].IsFootprintAxis(1u), "the extent's mask is empty");

    // a bit past the module's two axes is damage, and the reader rejects it when the environment opens
    manifest::Header header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    manifest::EnvironmentDirectoryEntry entry;
    std::memcpy(&entry, bytes.data() + header.EnvironmentDirectoryOffset, sizeof(entry));
    manifest::EnvironmentHeader environmentHeader;
    std::memcpy(&environmentHeader, bytes.data() + entry.ExtentOffset, sizeof(environmentHeader));
    const size_t wordOffset = entry.ExtentOffset + environmentHeader.FootprintAxisTableOffset;
    std::vector<std::byte> damaged = bytes;
    uint64_t word = 0u;
    std::memcpy(&word, damaged.data() + wordOffset, sizeof(word));
    word |= uint64_t{ 1u } << 63u;
    std::memcpy(damaged.data() + wordOffset, &word, sizeof(word));
    const manifest::ManifestResult<manifest::BundleView> damagedBundle = manifest::BundleView::Open(damaged);
    runner.Check(damagedBundle.has_value(), "the damage is inside one extent, so the header region still opens");
    if (damagedBundle.has_value())
    {
        const manifest::ManifestResult<manifest::EnvironmentView> damagedOpen = damagedBundle->OpenEnvironment(0u, 0u);
        runner.Check(!damagedOpen.has_value() && damagedOpen.error().Code == manifest::ErrorCode::InvalidFootprintAxes,
                     "a footprint axis bit past the module's axes is rejected when the environment opens");
    }

    const CreationFields counts = manifest::DescribeCreation(layout[k_Counts]);
    runner.Check(counts.Size == CreationSource::ShaderPerVariant && counts.Usage == CreationSource::Shader &&
                     counts.Format == CreationSource::NotApplicable && counts.IsComplete(),
                 "a counted storage buffer is complete, with a size per variant");
    const CreationFields params = manifest::DescribeCreation(layout[k_Params]);
    runner.Check(params.Size == CreationSource::Shader && params.IsComplete(), "a uniform buffer is complete");
    const CreationFields lut = manifest::DescribeCreation(layout[k_Lut]);
    runner.Check(lut.Extent == CreationSource::Shader && lut.Format == CreationSource::Cpu &&
                     lut.MipLevels == CreationSource::Cpu && lut.ArrayLayers == CreationSource::Shader && !lut.IsComplete(),
                 "a sampled texture with an extent still needs a format and mip levels");
    const CreationFields target = manifest::DescribeCreation(layout[k_Target]);
    runner.Check(target.Format == CreationSource::Shader && target.Extent == CreationSource::Cpu,
                 "a storage texture states its format, and with no extent attribute the renderer gives the extent");
    const CreationFields sampler = manifest::DescribeCreation(layout[k_Sampler]);
    runner.Check(sampler.SamplerState == CreationSource::Cpu && sampler.Size == CreationSource::NotApplicable,
                 "a sampler needs its state from the renderer");

    runner.BeginSection("the report reads the written manifest");
    const CookResult<ResourceReport> report = BuildResourceReport(bytes);
    runner.Check(report.has_value() && report->Environments.size() == 1u, "the report has one environment");
    if (!report.has_value() || report->Environments.size() != 1u)
    {
        return;
    }
    const ResourceReportEnvironment& reported = report->Environments.front();
    runner.Check(reported.ModuleName == "ReportModule" && reported.TargetName == "wgsl" && reported.Resources.size() == 5u,
                 "the environment names its module and target, and holds each resource once");
    const auto findEntry = [&](std::string_view name) -> const ResourceReportEntry*
    {
        for (const ResourceReportEntry& entry : reported.Resources)
        {
            if (entry.Name == name)
            {
                return &entry;
            }
        }
        return nullptr;
    };
    const ResourceReportEntry* countsEntry = findEntry("Counts");
    runner.Check(countsEntry != nullptr && countsEntry->FootprintAxes == std::vector<std::string>{ "SIZE" } &&
                     countsEntry->ElementCount == 0u && countsEntry->EntryPoints == std::vector<std::string>{ "CountCS" },
                 "a count that varies names its axis and prints no value");
    const ResourceReportEntry* lutEntry = findEntry("Lut");
    runner.Check(lutEntry != nullptr && lutEntry->ExtentX == 64u && lutEntry->FootprintAxes.empty(),
                 "a fixed extent prints its value");

    const std::string text = FormatResourceReport(*report);
    runner.Check(text.contains("module ReportModule, target wgsl: 2 of 2 buffers complete"),
                 "the text counts the complete buffers, and leaves the textures and the sampler out");
    runner.Check(text.contains("size: per-variant (SIZE)"), "the text names the axis of a size that varies");
    runner.Check(text.contains("extent: shader (64x1x1)"), "the text prints a fixed extent");
}

} // namespace

int main()
{
    tests::TestRunner runner{ "ResourceReport" };
    const PermutationSpace space = MakeSpace();
    CheckMeasurement(runner, space);
    CheckValidator(runner, space);
    CheckManifest(runner, space);
    return runner.Report();
}
