module Pyramid;
import LodestoneAttributes;
import LodestoneShims;

// The levels of a mip pyramid as one array of storage shims, with the format of every level from one axis. One entry point
// picks the level with a dynamic index, and one with a constant index.

[ls_axis_enum]
extern static const StorageFormat LEVEL_FORMAT = StorageFormat::Rgba16Float;

[ls_storage_format("LEVEL_FORMAT")]
extern struct Level : IWriteStorageTexture2D;

struct Levels
{
    Level levels[16];
};

ParameterBlock<Levels> pyramid;

[shader("compute")]
[numthreads(8, 8, 1)]
void WriteLevelCS(uint3 id : SV_DispatchThreadID, uniform uint level)
{
    pyramid.levels[level].Store(id.xy, float4(0.25f));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void WriteLevel3CS(uint3 id : SV_DispatchThreadID)
{
    pyramid.levels[3].Store(id.xy, float4(0.25f));
}
