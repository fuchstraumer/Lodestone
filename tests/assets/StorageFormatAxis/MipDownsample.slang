module MipDownsample;
import LodestoneAttributes;
import LodestoneShims;

// One step of a mip chain: a 2x2 box filter from the previous level into the next. The storage format of the next level is
// an axis, and only the attribute names it: the code reads the level through the shim and never reads LEVEL_FORMAT.

[ls_axis_enum]
extern static const StorageFormat LEVEL_FORMAT = StorageFormat::Rgba16Float;

extern struct Previous : ITexture2DRef;
[ls_storage_format("LEVEL_FORMAT")]
extern struct Next : IWriteStorageTexture2D;

struct MipLevel
{
    Previous previous;
    Next next;
};

ParameterBlock<MipLevel> level;

[shader("compute")]
[numthreads(8, 8, 1)]
void DownsampleCS(uint3 id : SV_DispatchThreadID)
{
    const int2 texel = int2(id.xy) * 2;
    const float4 sum = level.previous.Get().Load(int3(texel, 0)) + level.previous.Get().Load(int3(texel + int2(1, 0), 0)) +
                       level.previous.Get().Load(int3(texel + int2(0, 1), 0)) + level.previous.Get().Load(int3(texel + int2(1, 1), 0));
    level.next.Store(id.xy, sum * 0.25f);
}
