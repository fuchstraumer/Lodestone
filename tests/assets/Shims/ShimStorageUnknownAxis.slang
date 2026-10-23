module ShimStorageUnknownAxis;
import LodestoneAttributes;
import LodestoneShims;

// A storage shim extern whose attribute names an axis that the module does not declare: a typo of LEVEL_FORMAT. The code
// reads LEVEL_FORMAT, so that the axis stays in the space and the error can suggest it.

[ls_axis_enum]
extern static const StorageFormat LEVEL_FORMAT = StorageFormat::Rgba16Float;

[ls_storage_format("LEVEL_FROMAT")]
extern struct Destination : IWriteStorageTexture2D;

ParameterBlock<Destination> target;

[shader("compute")]
[numthreads(8, 8, 1)]
void StoreCS(uint3 id : SV_DispatchThreadID)
{
    const float alpha = LEVEL_FORMAT == StorageFormat::R32Float ? 0.0f : 1.0f;
    target.Store(id.xy, float4(id.xy, 0.0f, alpha));
}
