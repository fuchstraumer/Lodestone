module ShimStorageAxis;
import LodestoneAttributes;
import LodestoneShims;

// A storage shim extern whose format follows an enum axis, beside a fixed shim extern. The code does not read the axis: only
// the attribute names it, so the axis must survive the pruning of unused axes.

[ls_axis_enum]
extern static const StorageFormat LEVEL_FORMAT = StorageFormat::Rgba16Float;

[ls_storage_format("LEVEL_FORMAT")]
extern struct Destination : IWriteStorageTexture2D;
extern struct Weights : IReadBuffer<float4>;

struct Level
{
    Destination target;
    Weights weights;
};

ParameterBlock<Level> level;

[shader("compute")]
[numthreads(8, 8, 1)]
void StoreCS(uint3 id : SV_DispatchThreadID)
{
    level.target.Store(id.xy, level.weights.Load(id.x));
}
