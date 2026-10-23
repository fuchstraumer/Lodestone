module ShimStorage;
import LodestoneShims;

// The write-only storage shim used directly, one texture for each accepted format. The module registers no axis.
// ShimStorageAxis.slang selects the format of an extern with an axis.

struct Levels
{
    BoundWriteStorageTexture2D<RGBA8Unorm> Rgba8Unorm;
    BoundWriteStorageTexture2D<RGBA8Snorm> Rgba8Snorm;
    BoundWriteStorageTexture2D<RGBA16F> Rgba16Float;
    BoundWriteStorageTexture2D<R32F> R32Float;
    BoundWriteStorageTexture2D<RG32F> Rg32Float;
    BoundWriteStorageTexture2D<RGBA32F> Rgba32Float;
};

ParameterBlock<Levels> levels;

[shader("compute")]
[numthreads(8, 8, 1)]
void StoreCS(uint3 id : SV_DispatchThreadID)
{
    const float4 value = float4(id.xy, 0.0f, 1.0f);
    levels.Rgba8Unorm.Store(id.xy, value);
    levels.Rgba8Snorm.Store(id.xy, value);
    levels.Rgba16Float.Store(id.xy, value);
    levels.R32Float.Store(id.xy, value);
    levels.Rg32Float.Store(id.xy, value);
    levels.Rgba32Float.Store(id.xy, value);
}
