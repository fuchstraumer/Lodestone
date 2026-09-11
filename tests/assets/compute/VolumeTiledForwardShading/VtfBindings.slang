implementing VolumeTiledForwardShading;
__include VtfConfig;
__include VtfTypes;

import LodestoneAttributes;
import ddCommon;

// Single source of truth for the resources of each volume tiled forward entry point.
// Each lodestone_element_count expression names constants from VtfConfig.slang. The cooker multiplies the
// count by the reflected element stride, so no byte size is written by hand.

public struct VtfUpdateLightsResources
{
    [lodestone_element_count("VTF_MAX_POINT_LIGHTS")]
    RWStructuredBuffer<PointLight> PointLights;
    [lodestone_element_count("VTF_MAX_SPOT_LIGHTS")]
    RWStructuredBuffer<SpotLight> SpotLights;
    [lodestone_element_count("VTF_MAX_DIRECTIONAL_LIGHTS")]
    RWStructuredBuffer<DirectionalLight> DirectionalLights;
    ConstantBuffer<LightCounts> Counts;
    ConstantBuffer<CommonMatrices> Matrices;
}

public struct VtfReduceLightAABBsResources
{
    ConstantBuffer<LightCounts> Counts;
    [lodestone_element_count("VTF_MAX_POINT_LIGHTS")]
    StructuredBuffer<PointLight> PointLights;
    [lodestone_element_count("VTF_MAX_SPOT_LIGHTS")]
    StructuredBuffer<SpotLight> SpotLights;
    // Pass 0 writes one AABB for each thread group. Pass 1 reads them and writes one AABB at index 0.
    [lodestone_element_count("VTF_MAX_POINT_LIGHTS / VTF_REDUCE_LIGHTS_NUM_THREADS + 1")]
    RWStructuredBuffer<AABB> LightAABBs;
    ConstantBuffer<ReduceLightAABBParams> Params;
}

public struct VtfMortonCodesResources
{
    [lodestone_element_count("VTF_MAX_POINT_LIGHTS")]
    StructuredBuffer<PointLight> PointLights;
    [lodestone_element_count("VTF_MAX_SPOT_LIGHTS")]
    StructuredBuffer<SpotLight> SpotLights;
    [lodestone_element_count("VTF_MAX_POINT_LIGHTS")]
    RWStructuredBuffer<uint> PointLightIndices;
    [lodestone_element_count("VTF_MAX_SPOT_LIGHTS")]
    RWStructuredBuffer<uint> SpotLightIndices;
    [lodestone_element_count("VTF_MAX_POINT_LIGHTS")]
    RWStructuredBuffer<uint> PointLightMortonCodes;
    [lodestone_element_count("VTF_MAX_SPOT_LIGHTS")]
    RWStructuredBuffer<uint> SpotLightMortonCodes;
    ConstantBuffer<LightCounts> Counts;
    [lodestone_element_count("VTF_MAX_POINT_LIGHTS / VTF_REDUCE_LIGHTS_NUM_THREADS + 1")]
    StructuredBuffer<AABB> LightAABBs;
}

// Both sort passes and the radix sort share this block. The keys are Morton codes and the values are
// light indices.
public struct VtfSortResources
{
    [lodestone_element_count("VTF_MAX_POINT_LIGHTS")]
    StructuredBuffer<uint> InputKeys;
    [lodestone_element_count("VTF_MAX_POINT_LIGHTS")]
    StructuredBuffer<uint> InputValues;
    [lodestone_element_count("VTF_MAX_POINT_LIGHTS")]
    RWStructuredBuffer<uint> OutputKeys;
    [lodestone_element_count("VTF_MAX_POINT_LIGHTS")]
    RWStructuredBuffer<uint> OutputValues;
    // One partition for each thread group, plus one end marker for each sort group.
    //
    // fix: this counted against VTF_MERGE_VALUES_PER_GROUP, which is the size of one merge, and gave
    // 10 entries. The first pass needs the most, because it holds the most sort groups: the chunks
    // start at the radix sort chunk size, so the count is bounded by the element count divided by that
    // chunk size. At 8192 lights the first pass needs 32, and the buffer held 10.
    [lodestone_element_count("VTF_MAX_POINT_LIGHTS / VTF_RADIX_SORT_NUM_THREADS + 2")]
    RWStructuredBuffer<uint> MergePathPartitions;
    ConstantBuffer<SortParams> Params;
}

public struct VtfBuildBvhResources
{
    // A 32-ary tree of 7 levels holds 34636833 nodes. The count below covers the levels that the light
    // capacity actually needs, which is the sum of the powers of the branching factor.
    [lodestone_element_count("(VTF_MAX_POINT_LIGHTS * VTF_BVH_BRANCHING) / (VTF_BVH_BRANCHING - 1) + 1")]
    RWStructuredBuffer<AABB> PointLightBVH;
    [lodestone_element_count("(VTF_MAX_SPOT_LIGHTS * VTF_BVH_BRANCHING) / (VTF_BVH_BRANCHING - 1) + 1")]
    RWStructuredBuffer<AABB> SpotLightBVH;
    [lodestone_element_count("VTF_MAX_POINT_LIGHTS")]
    StructuredBuffer<PointLight> PointLights;
    [lodestone_element_count("VTF_MAX_SPOT_LIGHTS")]
    StructuredBuffer<SpotLight> SpotLights;
    ConstantBuffer<BVHParams> BvhParams;
    ConstantBuffer<LightCounts> Counts;
    [lodestone_element_count("VTF_MAX_POINT_LIGHTS")]
    StructuredBuffer<uint> PointLightIndices;
    [lodestone_element_count("VTF_MAX_SPOT_LIGHTS")]
    StructuredBuffer<uint> SpotLightIndices;
}

public struct VtfAssignLightsResources
{
    [lodestone_element_count("VTF_CLUSTER_COUNT")]
    StructuredBuffer<uint> UniqueClusters;
    [lodestone_element_count("VTF_CLUSTER_COUNT")]
    StructuredBuffer<AABB> ClusterAABBs;
    ConstantBuffer<BVHParams> BvhParams;
    ConstantBuffer<LightCounts> Counts;
    [lodestone_element_count("VTF_MAX_POINT_LIGHTS")]
    StructuredBuffer<PointLight> PointLights;
    [lodestone_element_count("VTF_MAX_SPOT_LIGHTS")]
    StructuredBuffer<SpotLight> SpotLights;
    [lodestone_element_count("(VTF_MAX_POINT_LIGHTS * VTF_BVH_BRANCHING) / (VTF_BVH_BRANCHING - 1) + 1")]
    StructuredBuffer<AABB> PointLightBVH;
    [lodestone_element_count("(VTF_MAX_SPOT_LIGHTS * VTF_BVH_BRANCHING) / (VTF_BVH_BRANCHING - 1) + 1")]
    StructuredBuffer<AABB> SpotLightBVH;
    [lodestone_element_count("VTF_MAX_POINT_LIGHTS")]
    StructuredBuffer<uint> PointLightIndices;
    [lodestone_element_count("VTF_MAX_SPOT_LIGHTS")]
    StructuredBuffer<uint> SpotLightIndices;
    // Worst case is every cluster holding a full light list.
    [lodestone_element_count("VTF_CLUSTER_COUNT * VTF_MAX_LIGHTS_PER_CLUSTER")]
    RWStructuredBuffer<uint> PointLightIndexList;
    [lodestone_element_count("VTF_CLUSTER_COUNT * VTF_MAX_LIGHTS_PER_CLUSTER")]
    RWStructuredBuffer<uint> SpotLightIndexList;
    // fix: the grid was a uint buffer that the code indexed at [i * 2] and [i * 2 + 1]. A uint2 states
    // the pair, so an offset can never be read as a count.
    [lodestone_element_count("VTF_CLUSTER_COUNT")]
    RWStructuredBuffer<uint2> PointLightGrid;
    [lodestone_element_count("VTF_CLUSTER_COUNT")]
    RWStructuredBuffer<uint2> SpotLightGrid;
    // The element type is Atomic<uint> and not uint. WGSL has no atomic operation on a plain storage
    // element, so a buffer that any shader increments must say so in its type. Slang then emits
    // `array<atomic<u32>>` and the free function InterlockedAdd is not needed.
    [lodestone_element_count("1")]
    RWStructuredBuffer<Atomic<uint>> PointLightIndexCounter;
    [lodestone_element_count("1")]
    RWStructuredBuffer<Atomic<uint>> SpotLightIndexCounter;
}

public struct VtfClusterAABBsResources
{
    ConstantBuffer<ClusterData> Clusters;
    ConstantBuffer<CommonMatrices> Matrices;
    [lodestone_element_count("VTF_CLUSTER_COUNT")]
    RWStructuredBuffer<AABB> ClusterAABBs;
}

public struct VtfFindUniqueClustersResources
{
    // fix: the flags were a uint8_t buffer. WGSL has no 8-bit storage type, so one uint holds one flag.
    [lodestone_element_count("VTF_CLUSTER_COUNT")]
    RWStructuredBuffer<uint> ClusterFlags;
    // Atomic<uint>, for the reason given on the light index counters below.
    [lodestone_element_count("1")]
    RWStructuredBuffer<Atomic<uint>> UniqueClustersCounter;
    [lodestone_element_count("VTF_CLUSTER_COUNT")]
    RWStructuredBuffer<uint> UniqueClusters;
}

public struct VtfClusterIndirectArgsResources
{
    [lodestone_element_count("1")]
    StructuredBuffer<uint> UniqueClustersCounter;
    [lodestone_element_count("1")]
    RWStructuredBuffer<ComputeIndirectArgs> IndirectArgs;
    [lodestone_element_count("1")]
    RWStructuredBuffer<DrawIndirectArgs> DebugClustersIndirectArgs;
}

// ---------------------------------------------------------------------------------------------
// Raster passes. The GLSL declared these through a YAML resource group, which has no Slang form, so
// each group becomes a ParameterBlock like the compute groups above.
// ---------------------------------------------------------------------------------------------

// The pass that marks which clusters a visible surface falls into.
//
// The GLSL bound a ClusterColors texel buffer here and in the cluster debug pass, and the CPU filled
// it once with one colour for each cluster. The colour holds nothing that the cluster index does not,
// so ClusterIndexToDebugColor computes it and both passes drop the binding.
public struct VtfClusterSamplesResources
{
    ConstantBuffer<ClusterData> Clusters;
    ConstantBuffer<CommonMatrices> Matrices;
    [lodestone_element_count("VTF_CLUSTER_COUNT")]
    RWStructuredBuffer<uint> ClusterFlags;
}

public struct VtfDebugClustersResources
{
    ConstantBuffer<CommonMatrices> Matrices;
    [lodestone_element_count("VTF_CLUSTER_COUNT")]
    StructuredBuffer<uint> UniqueClusters;
    [lodestone_element_count("VTF_CLUSTER_COUNT")]
    StructuredBuffer<AABB> ClusterAABBs;
}

public struct VtfDebugLightsResources
{
    ConstantBuffer<CommonMatrices> Matrices;
    [lodestone_element_count("VTF_MAX_POINT_LIGHTS")]
    StructuredBuffer<PointLight> PointLights;
    [lodestone_element_count("VTF_MAX_SPOT_LIGHTS")]
    StructuredBuffer<SpotLight> SpotLights;
}

public struct VtfDebugTextureResources
{
    Texture2D<float4> DebugTexture;
    SamplerState DebugSampler;
}
