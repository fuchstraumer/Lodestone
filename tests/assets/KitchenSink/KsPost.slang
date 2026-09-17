module KsPost;
import LodestoneAttributes;
import KsTypes;

// The post-process module. It draws a fullscreen triangle from the vertex id, so it has a vertex stage
// with no vertex input layout, and one color target. It reads a scene color texture and a bloom chain
// array.
//
// TONE_MAP is an enum axis over ToneMapOperator, shared by name with KsGeometry. SAMPLE_COUNT is a
// tuning axis whose values are a subset of the SAMPLE_COUNT axis in KsMaterial, so the two modules
// exercise the per-module value mask against one root axis.

// Enum axis, cross-module to KsTypes, shared name with KsGeometry.
[ls_axis_enum]
extern static const ToneMapOperator TONE_MAP = ToneMapOperator::Linear;

// Technique axis for the bloom pass.
[ls_axis_boolean][ls_axis_kind("technique")]
extern static const bool APPLY_BLOOM = false;

// Technique axis, gated on bloom. A vignette combines with bloom in this pass, so it is inactive when
// bloom is off.
[ls_axis_boolean][ls_axis_kind("technique")][ls_axis_active_when("APPLY_BLOOM == 1")]
extern static const bool APPLY_VIGNETTE = false;

// Tuning axis. Its values are a strict subset of the SAMPLE_COUNT axis in KsMaterial.
[ls_axis_values("1, 2")][ls_axis_kind("tuning")]
extern static const uint SAMPLE_COUNT = 1u;

struct FullscreenVertex
{
    float4 ClipPosition : SV_Position;
    float2 Uv : TEXCOORD0;
};

uniform Texture2D<float4> SceneColor;
[ls_extent_3d("1024", "1024", "5")]
uniform Texture2DArray<float4> BloomChain;
uniform SamplerState LinearSampler;

[shader("vertex")]
FullscreenVertex FullscreenVS(uint vertexId : SV_VertexID)
{
    FullscreenVertex output;
    output.Uv = float2((vertexId << 1u) & 2u, vertexId & 2u);
    output.ClipPosition = float4(output.Uv * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

[shader("fragment")]
float4 ResolveFS(FullscreenVertex input) : SV_Target
{
    float3 color = float3(0.0f, 0.0f, 0.0f);
    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        color += SceneColor.SampleLevel(LinearSampler, input.Uv, float(i)).rgb;
    }
    color /= float(SAMPLE_COUNT);

    if (APPLY_BLOOM)
    {
        for (uint layer = 0u; layer < 5u; ++layer)
        {
            color += BloomChain.SampleLevel(LinearSampler, float3(input.Uv, float(layer)), 0.0f).rgb * 0.1f;
        }

        if (APPLY_VIGNETTE)
        {
            const float2 d = input.Uv - 0.5f;
            color *= 1.0f - dot(d, d);
        }
    }

    if (TONE_MAP == ToneMapOperator::Reinhard)
    {
        color = color / (color + 1.0f);
    }
    else if (TONE_MAP == ToneMapOperator::Aces)
    {
        color = saturate(color * 0.8f);
    }
    else if (TONE_MAP == ToneMapOperator::Filmic)
    {
        color = sqrt(color);
    }

    return float4(color, 1.0f);
}
