module ShimStages;
import LodestoneShims;

// A vertex and a fragment stage that read one block, which stores a texture and a sampler shim beside ordinary data. The
// vertex stage reads only the data, so its handles are visible through their container alone.

extern struct Albedo : ITexture2DRef;
extern struct Linear : ISamplerRef;

struct Material
{
    Albedo surface;
    Linear filter;
    float4 tint;
};

ParameterBlock<Material> material;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput MainVS(uint id : SV_VertexID)
{
    VertexOutput output;
    output.uv = float2(float(id & 1u), float(id >> 1u));
    output.position = float4(output.uv, 0.0f, 1.0f) * material.tint;
    return output;
}

[shader("fragment")]
float4 MainFS(VertexOutput input) : SV_Target
{
    return material.surface.Get().Sample(material.filter.Get(), input.uv) * material.tint;
}
