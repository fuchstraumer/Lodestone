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
#include "TestHarness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

// The merged layout of one pipeline: each resource once, with every stage that reads it. This test builds a
// manifest inline through the real emitter, so it exercises emit -> open -> merge with no asset and no
// compiler. It also proves that Open rejects a visibility list that is not in placement order, because the
// merge rests on that order.
//
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
    module.Sources.emplace_back(lodestone::tests::BytesOf("// wgsl for the merge test module"));

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
    module.FootprintLists.emplace_back();
    module.VisibilityLists = std::move(visibility_lists);
    module.RasterStates.emplace_back();

    const PermutationAxis& axis = space.Axes()[0];
    for (uint32_t caseIndex = 0u; caseIndex < k_CaseCount; ++caseIndex)
    {
        module.VariantKeys.push_back(VariantKey{ caseIndex });

        lodestone::LibraryVariant variant;
        variant.Index = caseIndex;
        variant.Active.push_back(lodestone::PermutationBinding{ .Axis = &axis, .Value = axis.GetValues()[caseIndex] });
        variant.ResourceListIndex = caseIndex;
        variant.FootprintListIndex = 0u;
        const std::array<uint32_t, 3>& caseVisibility = visibility[caseIndex];
        for (const uint32_t visibilityIndex : caseVisibility)
        {
            variant.SourceIndices.push_back(0u);
            variant.VisibilityIndices.push_back(visibilityIndex);
            variant.RasterIndices.push_back(0u);
            variant.Workgroups.emplace_back(lodestone::WorkgroupSize{ .X = 64u, .Y = 1u, .Z = 1u });
        }
        module.Variants.emplace_back(std::move(variant));
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
    lodestone::CookedLibrary library;
    library.ModuleNames = { "MergeTestModule" };
    library.Profiles = { lodestone::CookedProfile{ .TargetName = "wgsl",
                                                   .AccessModel = lodestone::PlacementKind::Bound,
                                                   .CodeFormat = lodestone::ShaderCodeFormat::Wgsl } };
    library.Environments.emplace_back(BuildModule(space, std::move(visibility_lists), visibility));

    lodestone::CookResult<std::vector<std::byte>> manifest = lodestone::EmitShaderManifest(library);
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

    runner.Check(vertex.Stage() == ShaderStageKind::Vertex && fragment.Stage() == ShaderStageKind::Fragment &&
                     compute.Stage() == ShaderStageKind::Compute,
                 "an entry point instance reports the stage of its entry point");

    const std::array raster{ vertex, fragment };
    const ManifestResult<MergedLayoutView> merged = MergedLayoutView::Merge(raster);
    runner.Check(merged.has_value(), "a vertex and a fragment instance of one variant merge");
    if (merged.has_value())
    {
        const std::vector<MergedResource> resources = Collect(*merged);
        runner.Check(resources.size() == 4u, "the merge holds each of the four placements once");
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
            runner.Check(resources[0].Resource.Name() == "Scene" && resources[2].Resource.Name() == "Albedo",
                         "each merged resource is a full view of its binding record");
        }
        runner.Check(merged->StageCount() == 2u, "the merge counts its stages");
    }

    // the order of the instances changes the reported view, not the result
    const std::array reversed{ fragment, vertex };
    const ManifestResult<MergedLayoutView> reversedMerge = MergedLayoutView::Merge(reversed);
    runner.Check(reversedMerge.has_value() && Collect(*reversedMerge).size() == 4u,
                 "the merge does not depend on the order of the instances");

    const std::array computeOnly{ compute };
    const ManifestResult<MergedLayoutView> computeMerge = MergedLayoutView::Merge(computeOnly);
    runner.Check(computeMerge.has_value(), "one compute instance merges on its own");
    if (computeMerge.has_value())
    {
        const std::vector<MergedResource> resources = Collect(*computeMerge);
        runner.Check(resources.size() == 1u && IsAt(resources[0], 0u, 1u) &&
                         resources[0].Stages == ShaderStageKind::Compute,
                     "a one-stage merge is that stage's layout, with its stage bit");
    }

    const ManifestResult<MergedLayoutView> emptyMerge = MergedLayoutView::Merge({});
    runner.Check(emptyMerge.has_value() && emptyMerge->begin() == emptyMerge->end(),
                 "a merge of no instances is an empty layout");

    // two variants of one module: case 1 VS reads the same Scene record as case 0
    const VariantView caseOne = environment.VariantAt(1u);
    const std::array acrossVariants{ caseOne.EntryPoint(k_VertexEntryPoint), fragment };
    const ManifestResult<MergedLayoutView> acrossMerge = MergedLayoutView::Merge(acrossVariants);
    runner.Check(acrossMerge.has_value() && Collect(*acrossMerge).size() == 3u,
                 "instances from two variants merge when their shared placement agrees");
}

void TestRejections(lodestone::tests::TestRunner& runner, const EnvironmentView& environment)
{
    runner.BeginSection("merge rejects");

    const VariantView caseZero = environment.VariantAt(0u);
    const VariantView caseOne = environment.VariantAt(1u);
    const VariantView caseTwo = environment.VariantAt(2u);

    const std::array kindConflict{ caseOne.EntryPoint(k_VertexEntryPoint), caseOne.EntryPoint(k_FragmentEntryPoint) };
    runner.Check(MergeError(kindConflict) == ErrorCode::MergedLayoutConflict,
                 "a uniform buffer and a texture at one placement are a conflict");

    const std::array sizeConflict{ caseTwo.EntryPoint(k_VertexEntryPoint), caseTwo.EntryPoint(k_FragmentEntryPoint) };
    runner.Check(MergeError(sizeConflict) == ErrorCode::MergedLayoutConflict,
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
    runner.Check(MergeError(duplicate) == ErrorCode::MergedLayoutDuplicateStage,
                 "two instances of one stage are rejected");

    std::array<EntryPointInstanceView, MergedLayoutView::k_MaxPipelineStages + 1u> tooMany{
        vertex, vertex, vertex, vertex, vertex, vertex, vertex, vertex, vertex
    };
    runner.Check(MergeError(tooMany) == ErrorCode::MergedLayoutTooManyStages,
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
    runner.Check(!opened.has_value() && opened.error().Code == ErrorCode::InvalidVisibilityListOrder,
                 "a visibility list out of placement order fails the open");

    // a placement read twice by one entry point is not strictly ascending
    std::vector<lodestone::VisibilityList> repeated = GoodVisibilityLists();
    repeated[0] = lodestone::VisibilityList{ 0u, 0u };
    const std::vector<std::byte> repeatedBytes = EmitBytes(space, std::move(repeated), k_GoodVisibility);
    const ManifestResult<EnvironmentView> repeatedOpen = OpenEnvironment(repeatedBytes);
    runner.Check(!repeatedOpen.has_value() && repeatedOpen.error().Code == ErrorCode::InvalidVisibilityListOrder,
                 "one placement twice in one visibility list fails the open");
}

} // namespace

int main()
{
    lodestone::tests::TestRunner runner{ "MergedLayoutTests" };

    const PermutationSpace space = MakeSpace();
    const std::vector<std::byte> bytes = EmitBytes(space, GoodVisibilityLists(), k_GoodVisibility);
    const ManifestResult<EnvironmentView> opened = OpenEnvironment(bytes);
    runner.Check(opened.has_value(), "the emitter produces a manifest the reader accepts");
    if (opened.has_value())
    {
        // the views point at this environment, so it stays here, unmoved, for every check
        const EnvironmentView& environment = *opened;
        TestMerge(runner, environment);
        TestRejections(runner, environment);
    }

    TestOpenRejectsOrder(runner, space);
    return runner.Report();
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif
