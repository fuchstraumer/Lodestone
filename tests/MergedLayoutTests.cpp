#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "VariantKey.hpp"
#include "model/CookedLibrary.hpp"
#include "model/ShaderDataSchema.hpp"
#include "emit/ShaderManifestEmitter.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationTypes.hpp"
#include "permute/PermutationValue.hpp"
#include "ManifestFixtures.hpp"
#include "Diagnostics.hpp"
#include "TestHarness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

// three entry points and one integral axis CASE:
// The module has three entry points (MainVS, MainFS, MainCS) and one integral axis CASE with three values,
// so three variants:
//   CASE 0: VS reads Scene(0,0) and Bones(0,1). FS reads Scene(0,0), Albedo(1,0), and Sampler(1,1).
//           CS reads Bones(0,1).
//   CASE 1: VS reads Scene(0,0). FS reads ShadowMap, a texture at (0,0): a kind conflict.
//   CASE 2: VS reads Scene(0,0). FS reads a second Scene at (0,0) with 128 bytes, not 64: a size conflict.

#ifdef __clang__
#pragma clang diagnostic push
// this is test scaffolding, not shipping code
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

using lodestone::AxisKind;
using lodestone::AxisValueDomain;
using lodestone::BindingKind;
using lodestone::BoundPlacement;
using lodestone::EarliestBindingTime;
using lodestone::PermutationAxis;
using lodestone::PermutationSpace;
using lodestone::PermutationValue;
using lodestone::ReflectedBinding;
using lodestone::ResourceShape;
using lodestone::ShaderStageKind;
using lodestone::VariantKey;
using lodestone::manifest::BundleView;
using lodestone::manifest::EntryPointInstanceView;
using lodestone::manifest::EnvironmentView;
using lodestone::manifest::ErrorCode;
using lodestone::manifest::ManifestResult;
using lodestone::manifest::MergedLayoutView;
using lodestone::manifest::MergedResource;
using lodestone::manifest::VariantView;

namespace
{
constexpr uint32_t k_VertexEntryPoint = 0u;
constexpr uint32_t k_FragmentEntryPoint = 1u;
constexpr uint32_t k_ComputeEntryPoint = 2u;
constexpr uint32_t k_CaseCount = 3u;

ReflectedBinding MakeBinding(std::string name, uint32_t group, uint32_t binding, BindingKind kind)
{
    ReflectedBinding result;
    result.Name = std::move(name);
    result.Placement = BoundPlacement{ .Group = group, .Binding = binding };
    result.Kind = kind;
    return result;
}

ReflectedBinding MakeUniformBuffer(std::string name, uint32_t group, uint32_t binding, uint64_t byte_size)
{
    ReflectedBinding result = MakeBinding(std::move(name), group, binding, BindingKind::UniformBuffer);
    result.ByteSize = byte_size;
    return result;
}

ReflectedBinding MakeTexture(std::string name, uint32_t group, uint32_t binding)
{
    ReflectedBinding result = MakeBinding(std::move(name), group, binding, BindingKind::Texture);
    result.Shape = ResourceShape::Texture2D;
    result.SampleType = lodestone::TextureSampleType::Float;
    return result;
}

PermutationSpace MakeSpace()
{
    std::vector<PermutationValue> values;
    for (uint32_t value = 0u; value < k_CaseCount; ++value)
    {
        values.emplace_back(value);
    }

    std::vector<PermutationAxis> axes;
    axes.emplace_back("CASE", std::move(values), AxisKind::Technique, EarliestBindingTime::Cook,
                      AxisValueDomain::Integral);
    return PermutationSpace{ std::move(axes) };
}

// `visibility` gives the visibility list of each variant, in the order VS, FS, CS. A test that needs a bad
// order passes its own lists.
lodestone::CookedModule BuildModule(const PermutationSpace& space,
                                    std::vector<lodestone::VisibilityList> visibility_lists,
                                    const std::array<std::array<uint32_t, 3>, k_CaseCount>& visibility)
{
    lodestone::CookedModule module;
    module.Name = "MergeTestModule";
    module.Space = &space;

    module.EntryPoints.push_back(lodestone::LibraryEntryPoint{ .Name = "MainVS", .Stage = ShaderStageKind::Vertex });
    module.EntryPoints.push_back(lodestone::LibraryEntryPoint{ .Name = "MainFS", .Stage = ShaderStageKind::Fragment });
    module.EntryPoints.push_back(lodestone::LibraryEntryPoint{ .Name = "MainCS", .Stage = ShaderStageKind::Compute });
    lodestone::tests::AddFixtureSource(module, lodestone::tests::BytesOf("// wgsl for the merge test module"), lodestone::SourceCodeFacts{});

    ReflectedBinding bones = MakeBinding("Bones", 0u, 1u, BindingKind::StorageBuffer);
    bones.Shape = ResourceShape::StructuredBuffer;
    bones.ElementStride = 64u;

    module.Resources.push_back(MakeUniformBuffer("Scene", 0u, 0u, 64u));                 // 0
    module.Resources.push_back(std::move(bones));                                      // 1
    module.Resources.push_back(MakeTexture("Albedo", 1u, 0u));                          // 2
    module.Resources.push_back(MakeBinding("LinearSampler", 1u, 1u, BindingKind::Sampler)); // 3
    module.Resources.push_back(MakeTexture("ShadowMap", 0u, 0u));                       // 4
    module.Resources.push_back(MakeUniformBuffer("Scene", 0u, 0u, 128u));               // 5

    // one resource list for each case; a visibility entry is a position in its variant's resource list
    module.ResourceLists.push_back(lodestone::ResourceList{ 0u, 1u, 2u, 3u });
    module.ResourceLists.push_back(lodestone::ResourceList{ 0u, 4u });
    module.ResourceLists.push_back(lodestone::ResourceList{ 0u, 5u });
    // the cook writes one footprint for each resource of a list, and the emitter reads them in parallel
    for (const lodestone::ResourceList& resources : module.ResourceLists)
    {
        module.FootprintLists.emplace_back(resources.size());
    }
    module.VisibilityLists = std::move(visibility_lists);
    lodestone::tests::AddDefaultInstanceTables(module);

    const PermutationAxis& axis = space.Axes()[0];
    for (uint32_t caseIndex = 0u; caseIndex < k_CaseCount; ++caseIndex)
    {
        module.VariantKeys.push_back(VariantKey{ caseIndex });

        const std::array<uint32_t, 3>& caseVisibility = visibility[caseIndex];
        std::array<lodestone::tests::FixtureInstance, 3> instances{};
        for (size_t entryPoint = 0u; entryPoint < instances.size(); ++entryPoint)
        {
            instances[entryPoint] = lodestone::tests::ComputeInstance(0u, caseVisibility[entryPoint]);
        }
        lodestone::LibraryVariant variant = lodestone::tests::MakeFixtureVariant(
            space,
            caseIndex,
            lodestone::PermutationAssignment{ lodestone::PermutationBinding{ .Axis = &axis, .Value = axis.GetValues()[caseIndex] } },
            instances);
        variant.ResourceListIndex = caseIndex;
        variant.FootprintListIndex = caseIndex;
        module.Variants.push_back(std::move(variant));
    }

    module.SpaceSize = k_CaseCount;
    return module;
}

// The lists of the good module, in placement order.
std::vector<lodestone::VisibilityList> GoodVisibilityLists()
{
    return {
        lodestone::VisibilityList{ 0u, 1u },     // 0: case 0 VS
        lodestone::VisibilityList{ 0u, 2u, 3u }, // 1: case 0 FS
        lodestone::VisibilityList{ 1u },         // 2: case 0 CS
        lodestone::VisibilityList{ 0u },         // 3: case 1 and 2 VS
        lodestone::VisibilityList{ 1u },         // 4: case 1 and 2 FS
        lodestone::VisibilityList{},             // 5: case 1 and 2 CS
    };
}

constexpr std::array<std::array<uint32_t, 3>, k_CaseCount> k_GoodVisibility
{
    std::array<uint32_t, 3>{ 0u, 1u, 2u },
    std::array<uint32_t, 3>{ 3u, 4u, 5u },
    std::array<uint32_t, 3>{ 3u, 4u, 5u },
};

std::vector<std::byte> EmitBytes(const PermutationSpace& space,
                                 std::vector<lodestone::VisibilityList> visibility_lists,
                                 const std::array<std::array<uint32_t, 3>, k_CaseCount>& visibility)
{
    lodestone::StderrDiagnosticSink manifestDiagnostics;
    lodestone::CookedLibrary library;
    library.ModuleNames = { "MergeTestModule" };
    library.Profiles = { lodestone::CookedProfile{ .TargetName = "wgsl",
                                                   .AccessModel = lodestone::PlacementKind::Bound,
                                                   .CodeFormat = lodestone::ShaderCodeFormat::Wgsl } };
    library.Environments.emplace_back(BuildModule(space, std::move(visibility_lists), visibility));

    lodestone::CookResult<std::vector<std::byte>> manifest = lodestone::EmitShaderManifest(library, manifestDiagnostics);
    if (!manifest.has_value())
    {
        return {};
    }
    return std::move(*manifest);
}

ManifestResult<EnvironmentView> OpenEnvironment(std::span<const std::byte> bytes)
{
    const ManifestResult<BundleView> bundle = BundleView::Open(bytes);
    if (!bundle.has_value())
    {
        return std::unexpected(bundle.error());
    }
    return bundle->OpenEnvironment(0u, 0u);
}

// Copies a merged layout into a vector, so a check can read it by position.
std::vector<MergedResource> Collect(const MergedLayoutView& layout)
{
    std::vector<MergedResource> result;
    for (const MergedResource resource : layout)
    {
        result.push_back(resource);
    }
    return result;
}

bool IsAt(const MergedResource& resource, uint32_t group, uint32_t binding)
{
    const lodestone::manifest::PlacementPayload placement = resource.Resource.PlacementValue();
    return placement.Word0 == group && placement.Word1 == binding;
}

ErrorCode MergeError(std::span<const EntryPointInstanceView> stages)
{
    const ManifestResult<MergedLayoutView> merged = MergedLayoutView::Merge(stages);
    return merged.has_value() ? ErrorCode::Success : merged.error().Code;
}

void TestMerge(lodestone::tests::TestRunner& runner, const EnvironmentView& environment)
{
    runner.BeginSection("merge");

    const VariantView caseZero = environment.VariantAt(0u);
    const EntryPointInstanceView vertex = caseZero.EntryPoint(k_VertexEntryPoint);
    const EntryPointInstanceView fragment = caseZero.EntryPoint(k_FragmentEntryPoint);
    const EntryPointInstanceView compute = caseZero.EntryPoint(k_ComputeEntryPoint);

    runner.CheckEqual(vertex.Stage(), ShaderStageKind::Vertex, "an entry point instance reports the stage of its entry point");
    runner.CheckEqual(fragment.Stage(), ShaderStageKind::Fragment, "an entry point instance reports the stage of its entry point");
    runner.CheckEqual(compute.Stage(), ShaderStageKind::Compute, "an entry point instance reports the stage of its entry point");

    const std::array raster{ vertex, fragment };
    const ManifestResult<MergedLayoutView> merged = MergedLayoutView::Merge(raster);
    if (runner.CheckOk(merged, "a vertex and a fragment instance of one variant merge"))
    {
        const std::vector<MergedResource> resources = Collect(*merged);
        runner.CheckEqual(resources.size(), 4u, "the merge holds each of the four placements once");
        if (resources.size() == 4u)
        {
            const ShaderStageKind both = ShaderStageKind::Vertex | ShaderStageKind::Fragment;
            runner.Check(IsAt(resources[0], 0u, 0u) && resources[0].Stages == both,
                         "a resource that both stages read carries both stage bits");
            runner.Check(IsAt(resources[1], 0u, 1u) && resources[1].Stages == ShaderStageKind::Vertex,
                         "a resource that only the vertex stage reads carries the vertex bit");
            runner.Check(IsAt(resources[2], 1u, 0u) && resources[2].Stages == ShaderStageKind::Fragment &&
                             IsAt(resources[3], 1u, 1u) && resources[3].Stages == ShaderStageKind::Fragment,
                         "the fragment-only resources follow, in group and binding order");
            runner.CheckEqual(resources[0].Resource.Name(), "Scene", "each merged resource is a full view of its binding record");
            runner.CheckEqual(resources[2].Resource.Name(), "Albedo", "each merged resource is a full view of its binding record");
        }
        runner.CheckEqual(merged->StageCount(), 2u, "the merge counts its stages");
    }

    // the order of the instances changes the reported view, not the result
    const std::array reversed{ fragment, vertex };
    const ManifestResult<MergedLayoutView> reversedMerge = MergedLayoutView::Merge(reversed);
    if (runner.CheckOk(reversedMerge, "the merge does not depend on the order of the instances"))
    {
        runner.CheckEqual(Collect(*reversedMerge).size(), 4u, "the merge does not depend on the order of the instances");
    }

    const std::array computeOnly{ compute };
    const ManifestResult<MergedLayoutView> computeMerge = MergedLayoutView::Merge(computeOnly);
    if (runner.CheckOk(computeMerge, "one compute instance merges on its own"))
    {
        const std::vector<MergedResource> resources = Collect(*computeMerge);
        runner.Check(resources.size() == 1u && IsAt(resources[0], 0u, 1u) &&
                         resources[0].Stages == ShaderStageKind::Compute,
                     "a one-stage merge is that stage's layout, with its stage bit");
    }

    const ManifestResult<MergedLayoutView> emptyMerge = MergedLayoutView::Merge({});
    if (runner.CheckOk(emptyMerge, "a merge of no instances is an empty layout"))
    {
        runner.Check(emptyMerge->begin() == emptyMerge->end(), "a merge of no instances is an empty layout");
    }

    // two variants of one module: case 1 VS reads the same Scene record as case 0
    const VariantView caseOne = environment.VariantAt(1u);
    const std::array acrossVariants{ caseOne.EntryPoint(k_VertexEntryPoint), fragment };
    const ManifestResult<MergedLayoutView> acrossMerge = MergedLayoutView::Merge(acrossVariants);
    if (runner.CheckOk(acrossMerge, "instances from two variants merge when their shared placement agrees"))
    {
        runner.CheckEqual(Collect(*acrossMerge).size(), 3u, "instances from two variants merge when their shared placement agrees");
    }
}

void TestRejections(lodestone::tests::TestRunner& runner, const EnvironmentView& environment)
{
    runner.BeginSection("merge rejects");

    const VariantView caseZero = environment.VariantAt(0u);
    const VariantView caseOne = environment.VariantAt(1u);
    const VariantView caseTwo = environment.VariantAt(2u);

    const std::array kindConflict{ caseOne.EntryPoint(k_VertexEntryPoint), caseOne.EntryPoint(k_FragmentEntryPoint) };
    runner.CheckEqual(MergeError(kindConflict), ErrorCode::MergedLayoutConflict,
                      "a uniform buffer and a texture at one placement are a conflict");

    const std::array sizeConflict{ caseTwo.EntryPoint(k_VertexEntryPoint), caseTwo.EntryPoint(k_FragmentEntryPoint) };
    runner.CheckEqual(MergeError(sizeConflict), ErrorCode::MergedLayoutConflict,
                      "two uniform buffers of different sizes at one placement are a conflict");

    const ManifestResult<MergedLayoutView> named = MergedLayoutView::Merge(sizeConflict);
    const EntryPointInstanceView conflictingFragment = caseTwo.EntryPoint(k_FragmentEntryPoint);
    const lodestone::manifest::LayoutRange conflictingLayout = conflictingFragment.Layout();
    const lodestone::manifest::ResolvedResource conflictingResource = conflictingLayout[0];
    runner.Check(!named.has_value() && named.error().Detail == 1u &&
                     named.error().RecordIndex == conflictingResource.Index(),
                 "a conflict names the binding record and the position of the instance");

    const EntryPointInstanceView vertex = caseZero.EntryPoint(k_VertexEntryPoint);
    const std::array duplicate{ vertex, caseOne.EntryPoint(k_VertexEntryPoint) };
    runner.CheckEqual(MergeError(duplicate), ErrorCode::MergedLayoutDuplicateStage,
                      "two instances of one stage are rejected");

    std::array<EntryPointInstanceView, MergedLayoutView::k_MaxPipelineStages + 1u> tooMany{
        vertex, vertex, vertex, vertex, vertex, vertex, vertex, vertex, vertex
    };
    runner.CheckEqual(MergeError(tooMany), ErrorCode::MergedLayoutTooManyStages,
                      "more instances than k_MaxPipelineStages are rejected before anything else");
}

void TestOpenRejectsOrder(lodestone::tests::TestRunner& runner, const PermutationSpace& space)
{
    runner.BeginSection("open rejects order");

    // case 0 FS reads Albedo(1,0) before Scene(0,0)
    std::vector<lodestone::VisibilityList> lists = GoodVisibilityLists();
    lists[1] = lodestone::VisibilityList{ 2u, 0u, 3u };
    const std::vector<std::byte> bytes = EmitBytes(space, std::move(lists), k_GoodVisibility);
    const ManifestResult<EnvironmentView> opened = OpenEnvironment(bytes);
    runner.CheckError(opened, ErrorCode::InvalidVisibilityListOrder,
                      "a visibility list out of placement order fails the open");

    // a placement read twice by one entry point is not strictly ascending
    std::vector<lodestone::VisibilityList> repeated = GoodVisibilityLists();
    repeated[0] = lodestone::VisibilityList{ 0u, 0u };
    const std::vector<std::byte> repeatedBytes = EmitBytes(space, std::move(repeated), k_GoodVisibility);
    const ManifestResult<EnvironmentView> repeatedOpen = OpenEnvironment(repeatedBytes);
    runner.CheckError(repeatedOpen, ErrorCode::InvalidVisibilityListOrder,
                      "one placement twice in one visibility list fails the open");
}

} // namespace

// Task L7: the union of the sampler and texture pairs of a pipeline. In case 0 the vertex stage here sees the
// fragment stage's resources, and both sample Albedo with LinearSampler (positions 1 and 2 of each list).
void TestSamplerPairs(lodestone::tests::TestRunner& runner, const PermutationSpace& space)
{
    lodestone::StderrDiagnosticSink manifestDiagnostics;
    runner.BeginSection("sampler and texture pairs");

    lodestone::CookedModule module = BuildModule(space, GoodVisibilityLists(), k_GoodVisibility);
    module.VisibilityLists[0] = lodestone::VisibilityList{ 0u, 2u, 3u };
    module.SamplerPairLists = { lodestone::SamplerPairList{},
                                lodestone::SamplerPairList{ lodestone::SamplerTexturePair{ .Texture = 1u, .Sampler = 2u } } };
    module.Variants[0].SamplerPairIndices = { 1u, 1u, 0u };

    lodestone::CookedLibrary library;
    library.ModuleNames = { "MergeTestModule" };
    library.Profiles = { lodestone::CookedProfile{ .TargetName = "wgsl",
                                                   .AccessModel = lodestone::PlacementKind::Bound,
                                                   .CodeFormat = lodestone::ShaderCodeFormat::Wgsl,
                                                   .SamplerTexturePairsRecorded = true } };
    library.Environments.emplace_back(std::move(module));
    const lodestone::CookResult<std::vector<std::byte>> manifest = lodestone::EmitShaderManifest(library, manifestDiagnostics);
    const ManifestResult<EnvironmentView> opened =
        manifest.has_value() ? OpenEnvironment(*manifest) : ManifestResult<EnvironmentView>{ std::unexpected(lodestone::manifest::ErrorState{}) };
    if (!runner.CheckOk(opened, "a module with pairs in two stages emits and opens"))
    {
        return;
    }

    const VariantView caseZero = opened->VariantAt(0u);
    const std::array raster{ caseZero.EntryPoint(k_VertexEntryPoint), caseZero.EntryPoint(k_FragmentEntryPoint) };
    const ManifestResult<MergedLayoutView> merged = MergedLayoutView::Merge(raster);
    runner.Check(merged.has_value() && merged->HasSamplerTexturePairs(), "the merge of a recording profile is complete");
    if (merged.has_value())
    {
        std::vector<lodestone::manifest::MergedSamplerTexturePair> pairs;
        for (const lodestone::manifest::MergedSamplerTexturePair pair : merged->SamplerTexturePairs())
        {
            pairs.push_back(pair);
        }
        runner.CheckEqual(pairs.size(), 1u, "a pair that two stages sample appears once");
        if (runner.CheckEqual(pairs.size(), 1u, "the pair resolves to the texture and the sampler"))
        {
            runner.CheckEqual(pairs[0].Texture.Name(), "Albedo", "the pair resolves to the texture and the sampler");
            runner.CheckEqual(pairs[0].Sampler.Name(), "LinearSampler", "the pair resolves to the texture and the sampler");
        }
        if (runner.CheckEqual(pairs.size(), 1u, "the pair names both stages that sample it"))
        {
            runner.CheckEqual(pairs[0].Stages, (ShaderStageKind::Vertex | ShaderStageKind::Fragment),
                              "the pair names both stages that sample it");
        }
    }

    const std::array vertexAndCompute{ caseZero.EntryPoint(k_VertexEntryPoint), caseZero.EntryPoint(k_ComputeEntryPoint) };
    const ManifestResult<MergedLayoutView> partial = MergedLayoutView::Merge(vertexAndCompute);
    if (partial.has_value())
    {
        uint32_t count = 0u;
        ShaderStageKind stages = ShaderStageKind::Invalid;
        for (const lodestone::manifest::MergedSamplerTexturePair pair : partial->SamplerTexturePairs())
        {
            ++count;
            stages = pair.Stages;
        }
        runner.CheckEqual(count, 1u, "a stage with no pairs adds nothing, and does not appear in the stage mask");
        runner.CheckEqual(stages, ShaderStageKind::Vertex, "a stage with no pairs adds nothing, and does not appear in the stage mask");
    }

    const std::array computeOnly{ caseZero.EntryPoint(k_ComputeEntryPoint) };
    const ManifestResult<MergedLayoutView> none = MergedLayoutView::Merge(computeOnly);
    if (runner.CheckOk(none, "a pipeline that samples nothing has no pairs"))
    {
        runner.Check(none->SamplerTexturePairs().begin() == none->SamplerTexturePairs().end(),
                     "a pipeline that samples nothing has no pairs");
    }
}

// The push constant blocks of a pipeline. Entry 1 is a 64-byte block with one matrix, entry 2 adds a float4
// after it (80 bytes), and entry 3 is a float4 at offset 0, which a matrix at offset 0 cannot share.
void TestPushConstants(lodestone::tests::TestRunner& runner, const PermutationSpace& space)
{
    lodestone::StderrDiagnosticSink manifestDiagnostics;
    runner.BeginSection("push constants");

    const auto member = [](const char* name, uint32_t offset, uint32_t size, lodestone::MatrixLayout layout) {
        lodestone::ReflectedUniformMember result;
        result.Name = name;
        result.Data.Offset = offset;
        result.Data.Size = size;
        result.Data.MatrixLayout = layout;
        return result;
    };
    const lodestone::ReflectedUniformMember transform = member("transform", 0u, 64u, lodestone::MatrixLayout::ColumnMajor);
    // another name at the same layout: names are not compared
    const lodestone::ReflectedUniformMember worldViewProjection =
        member("worldViewProjection", 0u, 64u, lodestone::MatrixLayout::ColumnMajor);
    const lodestone::ReflectedUniformMember tint = member("tint", 64u, 16u, lodestone::MatrixLayout::Invalid);
    const lodestone::ReflectedUniformMember tintAtZero = member("tint", 0u, 16u, lodestone::MatrixLayout::Invalid);
    const auto push = [](uint32_t size, std::vector<lodestone::ReflectedUniformMember> members) {
        return lodestone::EntryPointConstants{ .Delivery = lodestone::EntryPointConstantsDelivery::PushConstant,
                                               .ByteSize = size,
                                               .Binding = 0u,
                                               .Members = std::move(members) };
    };

    lodestone::CookedModule module = BuildModule(space, GoodVisibilityLists(), k_GoodVisibility);
    module.EntryPointConstantsTable.push_back(push(64u, { transform }));
    module.EntryPointConstantsTable.push_back(push(80u, { worldViewProjection, tint }));
    module.EntryPointConstantsTable.push_back(push(16u, { tintAtZero }));
    // case 0: VS 64 bytes, FS 80 bytes, CS none. case 1: VS 64 bytes, FS the float4 at 0. case 2: none
    module.Variants[0].ConstantsIndices = { 1u, 2u, 0u };
    module.Variants[1].ConstantsIndices = { 1u, 3u, 0u };

    lodestone::CookedLibrary library;
    library.ModuleNames = { "MergeTestModule" };
    library.Profiles = { lodestone::CookedProfile{ .TargetName = "wgsl",
                                                   .AccessModel = lodestone::PlacementKind::Bound,
                                                   .CodeFormat = lodestone::ShaderCodeFormat::Wgsl,
                                                   .SamplerTexturePairsRecorded = true } };
    library.Environments.emplace_back(std::move(module));
    const lodestone::CookResult<std::vector<std::byte>> manifest = lodestone::EmitShaderManifest(library, manifestDiagnostics);
    const ManifestResult<EnvironmentView> opened =
        manifest.has_value() ? OpenEnvironment(*manifest) : ManifestResult<EnvironmentView>{ std::unexpected(lodestone::manifest::ErrorState{}) };
    if (!runner.CheckOk(opened, "a module with push constant blocks emits and opens"))
    {
        return;
    }

    const VariantView caseZero = opened->VariantAt(0u);
    const std::array raster{ caseZero.EntryPoint(k_VertexEntryPoint), caseZero.EntryPoint(k_FragmentEntryPoint) };
    const ManifestResult<MergedLayoutView> merged = MergedLayoutView::Merge(raster);
    runner.Check(merged.has_value() && merged->PushConstants().has_value(),
                 "a block that is a prefix of the other block merges");
    if (merged.has_value() && merged->PushConstants().has_value())
    {
        const lodestone::manifest::MergedPushConstants block = *merged->PushConstants();
        runner.CheckEqual(block.Constants.ByteSize(), 80u, "the merged block is the block with the largest byte size");
        runner.CheckEqual(block.Constants.Members().Size(), 2u, "the merged block is the block with the largest byte size");
        runner.CheckEqual(block.Stages, (ShaderStageKind::Vertex | ShaderStageKind::Fragment), "the merged block names both stages");
    }

    const std::array reversed{ caseZero.EntryPoint(k_FragmentEntryPoint), caseZero.EntryPoint(k_VertexEntryPoint) };
    const ManifestResult<MergedLayoutView> reversedMerge = MergedLayoutView::Merge(reversed);
    runner.Check(reversedMerge.has_value() && reversedMerge->PushConstants().has_value() &&
                     reversedMerge->PushConstants()->Constants.ByteSize() == 80u,
                 "the order of the stages does not change the merged block");

    const std::array vertexAndCompute{ caseZero.EntryPoint(k_VertexEntryPoint), caseZero.EntryPoint(k_ComputeEntryPoint) };
    const ManifestResult<MergedLayoutView> partial = MergedLayoutView::Merge(vertexAndCompute);
    runner.Check(partial.has_value() && partial->PushConstants().has_value() &&
                     partial->PushConstants()->Stages == ShaderStageKind::Vertex,
                 "a stage with no block does not appear in the stage mask");

    const std::array computeOnly{ caseZero.EntryPoint(k_ComputeEntryPoint) };
    const ManifestResult<MergedLayoutView> none = MergedLayoutView::Merge(computeOnly);
    runner.Check(none.has_value() && !none->PushConstants().has_value(), "a pipeline with no block has no push constants");

    const VariantView caseOne = opened->VariantAt(1u);
    const std::array conflicting{ caseOne.EntryPoint(k_VertexEntryPoint), caseOne.EntryPoint(k_FragmentEntryPoint) };
    const ManifestResult<MergedLayoutView> conflict = MergedLayoutView::Merge(conflicting);
    runner.Check(!conflict.has_value() && conflict.error().Code == ErrorCode::MergedLayoutPushConstantConflict &&
                     conflict.error().Detail == 1u && conflict.error().RecordIndex == 3u,
                 "two blocks that differ at offset 0 fail, and the error names the second instance and its record");
}

// The tags and roles are part of a resource: case 2's second Scene is changed to 64 bytes, so only its tag differs.
void TestTagConflict(lodestone::tests::TestRunner& runner, const PermutationSpace& space)
{
    lodestone::StderrDiagnosticSink manifestDiagnostics;
    runner.BeginSection("tags and roles");

    lodestone::CookedModule module = BuildModule(space, GoodVisibilityLists(), k_GoodVisibility);
    for (ReflectedBinding& resource : module.Resources)
    {
        if (resource.Name == "Scene" && resource.ByteSize == 128u)
        {
            resource.ByteSize = 64u;
            resource.Tags = { "Other" };
        }
    }

    lodestone::CookedLibrary library;
    library.ModuleNames = { "MergeTestModule" };
    library.Profiles = { lodestone::CookedProfile{ .TargetName = "wgsl",
                                                   .AccessModel = lodestone::PlacementKind::Bound,
                                                   .CodeFormat = lodestone::ShaderCodeFormat::Wgsl,
                                                   .SamplerTexturePairsRecorded = true } };
    library.Environments.emplace_back(std::move(module));
    const lodestone::CookResult<std::vector<std::byte>> manifest = lodestone::EmitShaderManifest(library, manifestDiagnostics);
    const ManifestResult<EnvironmentView> opened =
        manifest.has_value() ? OpenEnvironment(*manifest) : ManifestResult<EnvironmentView>{ std::unexpected(lodestone::manifest::ErrorState{}) };
    if (!runner.CheckOk(opened, "a module with a tagged resource emits and opens"))
    {
        return;
    }

    const VariantView caseTwo = opened->VariantAt(2u);
    const std::array raster{ caseTwo.EntryPoint(k_VertexEntryPoint), caseTwo.EntryPoint(k_FragmentEntryPoint) };
    runner.CheckEqual(MergeError(raster), ErrorCode::MergedLayoutConflict,
                      "two resources at one placement that differ only in their tags conflict");
}

int main()
{
    lodestone::tests::TestRunner runner{ "MergedLayoutTests" };

    const PermutationSpace space = MakeSpace();
    const std::vector<std::byte> bytes = EmitBytes(space, GoodVisibilityLists(), k_GoodVisibility);
    const ManifestResult<EnvironmentView> opened = OpenEnvironment(bytes);
    if (runner.CheckOk(opened, "the emitter produces a manifest the reader accepts"))
    {
        // the views point at this environment, so it stays here, unmoved, for every check
        const EnvironmentView& environment = *opened;
        TestMerge(runner, environment);
        TestRejections(runner, environment);
    }

    TestOpenRejectsOrder(runner, space);
    TestSamplerPairs(runner, space);
    TestPushConstants(runner, space);
    TestTagConflict(runner, space);
    return runner.Report();
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif
