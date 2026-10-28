module VertexInputsMatch;

// A vertex entry point whose inputs all have semantics. The code and reflection agree on both locations.

struct VertexInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput MainVS(VertexInput input)
{
    VertexOutput output;
    output.position = float4(input.position, 1.0f);
    output.uv = input.uv;
    return output;
}
