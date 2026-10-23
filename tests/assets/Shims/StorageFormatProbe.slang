module StorageFormatProbe;

// One write-only storage texture for each accepted format, written with the Slang type that names the format. Each variable
// is named for its case in StorageFormat, and the test reads the format of each binding back from the manifest.

WTexture2D<RGBA8Unorm> Rgba8Unorm;
WTexture2D<RGBA8Snorm> Rgba8Snorm;
WTexture2D<RGBA16F> Rgba16Float;
WTexture2D<R32F> R32Float;
WTexture2D<RG32F> Rg32Float;
WTexture2D<RGBA32F> Rgba32Float;

[shader("compute")]
[numthreads(8, 8, 1)]
void MainCS(uint3 id : SV_DispatchThreadID)
{
    Rgba8Unorm.Store(id.xy, float4(1.0f, 0.0f, 0.0f, 1.0f));
    Rgba8Snorm.Store(id.xy, float4(-1.0f, 0.0f, 0.0f, 1.0f));
    Rgba16Float.Store(id.xy, float4(2.0f));
    R32Float.Store(id.xy, 3.0f);
    Rg32Float.Store(id.xy, float2(4.0f, 5.0f));
    Rgba32Float.Store(id.xy, float4(6.0f));
}
