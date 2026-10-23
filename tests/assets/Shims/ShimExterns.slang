module ShimExterns;
import LodestoneAttributes;
import LodestoneShims;

// Externs of the shim interfaces, with and without a default, beside one ordinary axis. The cooker reads the shim externs
// from the source text and confirms each one with reflection. They are not axes. Nothing here links an implementation yet.

interface IUnrelated
{
    float Value();
}

struct UnrelatedImpl : IUnrelated
{
    float Value() { return 1.0f; }
}

struct PointLight
{
    float4 Position;
};

extern struct Points : IReadBuffer<PointLight>;
extern struct Tints : IReadBuffer<float4> = BoundReadBuffer<float4>;
public extern struct Albedo : ITexture2DRef;
extern struct Linear : ISamplerRef;
extern struct Scratch : IReadWriteBuffer<float4>;
// extern struct Ghost : IReadBuffer<float4>;
extern struct Other : IUnrelated = UnrelatedImpl;

[ls_axis_boolean][ls_axis_kind("technique")]
extern static const bool FOG = false;

RWStructuredBuffer<float> Output;

[shader("compute")]
[numthreads(8, 1, 1)]
void MainCS(uint3 id : SV_DispatchThreadID)
{
    Output[id.x] = FOG ? 1.0f : 0.0f;
}
