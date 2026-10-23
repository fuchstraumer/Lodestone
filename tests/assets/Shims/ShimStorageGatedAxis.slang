module ShimStorageGatedAxis;
import LodestoneAttributes;
import LodestoneShims;

// A storage shim extern whose format axis is gated: a variant with the gate off has no format.

[ls_axis_boolean]
extern static const bool MIPS = true;

[ls_axis_enum][ls_axis_active_when("MIPS == 1")]
extern static const StorageFormat LEVEL_FORMAT = StorageFormat::Rgba16Float;

[ls_storage_format("LEVEL_FORMAT")]
extern struct Destination : IWriteStorageTexture2D;

ParameterBlock<Destination> target;

[shader("compute")]
[numthreads(8, 8, 1)]
void StoreCS(uint3 id : SV_DispatchThreadID)
{
    if (MIPS)
    {
        target.Store(id.xy, float4(id.xy, 0.0f, 1.0f));
    }
}
