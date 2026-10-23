module ShimStorageNoAttribute;
import LodestoneAttributes;
import LodestoneShims;

// A storage shim extern with no format axis.

extern struct Destination : IWriteStorageTexture2D;

ParameterBlock<Destination> target;

[shader("compute")]
[numthreads(8, 8, 1)]
void StoreCS(uint3 id : SV_DispatchThreadID)
{
    target.Store(id.xy, float4(id.xy, 0.0f, 1.0f));
}
