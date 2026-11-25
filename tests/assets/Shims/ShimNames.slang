module ShimNames;
import LodestoneShims;

// Two shim buffers, a texture, and a sampler in one entry point, and a shim in a plain struct, in a global, and in an
// entry point uniform. ShimNameTest checks that each resource has the name of the field that the author wrote, on every
// profile.

extern struct Left : IReadBuffer<float4>;
extern struct Right : IReadBuffer<float4>;
extern struct Albedo : ITexture2DRef;
extern struct Linear : ISamplerRef;
extern struct Scratch : IReadWriteBuffer<float4>;

struct Inner
{
    Left Nested;
};

struct Params
{
    Left First;
    Right Second;
    Albedo Surface;
    Linear Filter;
    Inner Group;
};

ParameterBlock<Params> params;

Left GlobalBuffer;

RWStructuredBuffer<float4> Output;

[shader("compute")]
[numthreads(8, 1, 1)]
void MainCS(uint3 id : SV_DispatchThreadID, uniform Scratch scratch)
{
    const float4 tint = params.Surface.Get().SampleLevel(params.Filter.Get(), float2(0.5f, 0.5f), 0.0f);
    const float4 sum = params.First.Load(id.x) + params.Second.Load(id.x) + params.Group.Nested.Load(id.x) +
                       GlobalBuffer.Load(id.x);

    scratch.Store(id.x, sum);
    Output[id.x] = sum * tint;
}
