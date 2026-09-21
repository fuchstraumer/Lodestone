module KsMaterial;
import LodestoneAttributes;
import KsTypes;
import KsShading;

// The material module, and the widest one. It crosses an interface axis, an enum axis, three
// resource-presence booleans, and a tuning axis, so its variant count is the largest in the set. It
// groups its resources into a parameter block, nests a block inside another block, and takes a block
// on an entry point. It also reaches a 3D LUT, a cube map, and a shadow array, so the resource shapes
// beyond the plain 2D texture get records too.
//
// BRDF is an interface axis over IBrdf, which KsShading declares. QUALITY is an enum axis over
// QualityTier, which KsTypes declares. Both cross a module boundary. KsVolume declares an axis named
// QUALITY over the same enum, so the two modules must agree on one root axis named QUALITY.

// Interface axis. The interface and its impls live in KsShading, so this proves cross-module interface
// resolution.
[ls_axis_interface]
public extern struct BRDF : IBrdf = Lambert;

// Enum axis. Shares its name and enum with the QUALITY axis in KsVolume.
[ls_axis_enum]
extern static const QualityTier QUALITY = QualityTier::Low;

// Resource-presence axis. The normal map binds only when a variant reads it.
[ls_axis_boolean][ls_axis_kind("technique")]
extern static const bool USE_NORMAL_MAP = false;

// Resource-presence axis, gated on the normal map. A detail map has no meaning without a normal map,
// so this axis is inactive whenever USE_NORMAL_MAP is false. This is the gated-default case: a query
// for the default value of a gated axis must not also match the variants where the axis is inactive.
[ls_axis_boolean][ls_axis_kind("technique")][ls_axis_active_when("USE_NORMAL_MAP == 1")]
extern static const bool USE_DETAIL_MAP = false;

// Resource-presence axis for the environment cube map.
[ls_axis_boolean][ls_axis_kind("technique")]
extern static const bool USE_ENV = false;

// Tuning axis. Shares its name with the SAMPLE_COUNT axis in KsPost, but the value sets differ:
// KsPost declares a subset. The root axis holds the union, and each module masks its own subset.
[ls_axis_values("1, 4, 8, 16")][ls_axis_kind("tuning")]
extern static const uint SAMPLE_COUNT = 1u;

struct MaterialResources
{
    [ls_extent_2d("2048", "2048")]
    Texture2D<float4> Albedo;
    [ls_extent_2d("2048", "2048")]
    Texture2D<float4> NormalMap;
    [ls_extent_2d("2048", "2048")]
    Texture2D<float4> DetailMap;
    [ls_extent_3d("64", "64", "64")]
    Texture3D<float4> ColorGradeLut;
    TextureCube<float4> Environment;
    SamplerState Sampler;
    [ls_element_count("SAMPLE_COUNT * 16")]
    StructuredBuffer<float4> Palette;
};

struct ShadowResources
{
    [ls_extent_3d("2048", "2048", "4")]
    Texture2DArray<float> Cascades;
    SamplerComparisonState CompareSampler;
};

// A block that holds a block, so the sub-object walk must add the spaces together.
struct SurfaceResources
{
    ParameterBlock<MaterialResources> Material;
    ParameterBlock<ShadowResources> Shadow;
};

ParameterBlock<SurfaceResources> Surface;
RWStructuredBuffer<float4> OutColor;

float3 shadeOne(float3 albedo, float3 normal, float3 uvw)
{
    BRDF brdf = {};
    float3 n = normal;
    if (USE_NORMAL_MAP)
    {
        const float3 tn = Surface.Material.NormalMap.SampleLevel(Surface.Material.Sampler, uvw.xy, 0.0f).xyz;
        n = normalize(n + tn * 2.0f - 1.0f);
        if (USE_DETAIL_MAP)
        {
            const float3 dn = Surface.Material.DetailMap.SampleLevel(Surface.Material.Sampler, uvw.xy * 4.0f, 0.0f).xyz;
            n = normalize(n + dn * 2.0f - 1.0f);
        }
    }

    float3 lit = brdf.shade(albedo, n, float3(0.0f, 1.0f, 0.0f));

    if (USE_ENV)
    {
        lit += Surface.Material.Environment.SampleLevel(Surface.Material.Sampler, n, 0.0f).rgb * 0.25f;
    }

    const float3 graded = Surface.Material.ColorGradeLut.SampleLevel(Surface.Material.Sampler, saturate(lit), 0.0f).rgb;
    return graded;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void EvalMaterialCS(uint3 tid : SV_DispatchThreadID)
{
    const float4 albedo = Surface.Material.Albedo.SampleLevel(Surface.Material.Sampler, float2(float(tid.x) / 2048.0f, 0.0f), 0.0f);

    float3 accum = float3(0.0f, 0.0f, 0.0f);
    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        const float3 uvw = float3(float(tid.x) / 2048.0f, float(i) / float(SAMPLE_COUNT), 0.0f);
        accum += shadeOne(albedo.rgb, float3(0.0f, 0.0f, 1.0f), uvw);
    }
    accum /= float(SAMPLE_COUNT);

    const float4 tint = Surface.Material.Palette[tid.x % (SAMPLE_COUNT * 16u)];
    float shadow = Surface.Shadow.Cascades.SampleCmpLevelZero(Surface.Shadow.CompareSampler, float3(float2(float(tid.x) / 2048.0f, 0.0f), 0.0f), 0.5f);

    OutColor[tid.x] = float4(accum * tint.rgb * shadow, float(QUALITY == QualityTier::Ultra ? 1.0f : 0.5f));
}

// A second entry point that takes its own block, so the per-entry-point block walk gets a record too.
[shader("compute")]
[numthreads(64, 1, 1)]
void PrefilterCS(uint3 tid : SV_DispatchThreadID, uniform ParameterBlock<MaterialResources> probe)
{
    const float4 albedo = probe.Albedo.SampleLevel(probe.Sampler, float2(float(tid.x) / 2048.0f, 0.0f), 0.0f);
    OutColor[tid.x] += albedo;
}
