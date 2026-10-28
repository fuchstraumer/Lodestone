module VaryingHoldsResource;
import LodestoneShims;

// A vertex entry point parameter that holds a buffer shim. Slang gives the buffer no layout there, and its SPIR-V emit crashes
// on the parameter, so the cook must fail before code generation.

public struct Item
{
    public float4 value;
};

extern struct ItemBuffer : IReadBuffer<Item>;

struct VertexIn
{
    ItemBuffer items;
};

[shader("vertex")]
float4 MainVS(VertexIn input, uint vertexIndex : SV_VertexID) : SV_Position
{
    return input.items.Load(vertexIndex).value;
}
