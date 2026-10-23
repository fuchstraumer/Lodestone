module ShimStorageWrongAxis;
import LodestoneAttributes;
import LodestoneShims;

// A storage shim extern whose attribute names an enum axis over another enum.

public enum Quality
{
    Low,
    High,
}

[ls_axis_enum]
extern static const Quality QUALITY = Quality::Low;

[ls_storage_format("QUALITY")]
extern struct Destination : IWriteStorageTexture2D;

ParameterBlock<Destination> target;

[shader("compute")]
[numthreads(8, 8, 1)]
void StoreCS(uint3 id : SV_DispatchThreadID)
{
    target.Store(id.xy, float4(id.xy, 0.0f, 1.0f));
}
