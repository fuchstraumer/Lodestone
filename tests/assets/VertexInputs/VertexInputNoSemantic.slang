module VertexInputNoSemantic;

// The field `pad` has no semantic. The code reads it at location 1, and reflection records no vertex input for it.

struct VertexInput
{
    float3 position : POSITION;
    float pad;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float pad : TEXCOORD0;
};

[shader("vertex")]
VertexOutput MainVS(VertexInput input)
{
    VertexOutput output;
    output.position = float4(input.position, 1.0f);
    output.pad = input.pad;
    return output;
}
