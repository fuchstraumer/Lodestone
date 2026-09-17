module KsGeometry;
import LodestoneAttributes;
import KsTypes;

// The raster module. It carries a vertex input layout, three color targets, and a depth write, so the
// manifest raster, vertex-input, and color-target tables all get real records. The uniform scene block
// is a ConstantBuffer, and the base-color texture carries a fixed 2D extent.
//
// TONE_MAP is an enum axis over ToneMapOperator, which KsTypes declares. KsPost declares an axis of the
// same name over the same enum, so the two modules must agree on one root axis named TONE_MAP.

// Enum axis. The enum lives in KsTypes, so this proves cross-module enum resolution.
[ls_axis_enum]
extern static const ToneMapOperator TONE_MAP = ToneMapOperator::Linear;

// Resource-presence axis. The tangent frame binds only when a variant reads it.
[ls_axis_boolean][ls_axis_kind("technique")]
extern static const bool HAS_TANGENTS = false;

// Resource-presence axis for the per-vertex color channel.
[ls_axis_boolean][ls_axis_kind("technique")]
extern static const bool VERTEX_COLOR = false;

struct SceneUniforms
{
    float4x4 ViewProjection;
    float4 CameraPosition;
    float4 ToneMapParams;
};

struct VertexInput
{
    half4 Position : POSITION;
    float3 Normal : NORMAL;
    float4 QTangent : TANGENT;
    float2 Uv : TEXCOORD0;
    float4 Color : COLOR0;
};

struct VertexOutput
{
    float4 ClipPosition : SV_Position;
    float3 WorldNormal : NORMAL;
    float2 Uv : TEXCOORD0;
    float4 Color : COLOR0;
};

struct GBuffer
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Material : SV_Target2;
    float Depth : SV_Depth;
};

uniform ConstantBuffer<SceneUniforms> Scene;

[ls_extent_2d("2048", "2048")]
uniform Texture2D<float4> BaseColorTexture;
uniform SamplerState BaseColorSampler;

[shader("vertex")]
VertexOutput MainVS(VertexInput input)
{
    VertexOutput output;
    output.ClipPosition = mul(Scene.ViewProjection, float4(float3(input.Position.xyz), 1.0f));
    output.WorldNormal = HAS_TANGENTS ? input.QTangent.xyz : input.Normal;
    output.Uv = input.Uv;
    output.Color = VERTEX_COLOR ? input.Color : float4(1.0f, 1.0f, 1.0f, 1.0f);
    return output;
}

[shader("fragment")]
GBuffer MainFS(VertexOutput input)
{
    const float4 base = BaseColorTexture.Sample(BaseColorSampler, input.Uv) * input.Color;

    float3 mapped = base.rgb;
    if (TONE_MAP == ToneMapOperator::Reinhard)
    {
        mapped = base.rgb / (base.rgb + 1.0f);
    }
    else if (TONE_MAP == ToneMapOperator::Aces)
    {
        mapped = base.rgb * 0.6f;
    }
    else if (TONE_MAP == ToneMapOperator::Filmic)
    {
        mapped = sqrt(base.rgb);
    }

    GBuffer output;
    output.Albedo = float4(mapped, base.a);
    output.Normal = float4(normalize(input.WorldNormal) * 0.5f + 0.5f, 1.0f);
    output.Material = Scene.ToneMapParams;
    output.Depth = input.ClipPosition.z;
    return output;
}
