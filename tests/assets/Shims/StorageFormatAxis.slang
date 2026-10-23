module StorageFormatAxis;
import LodestoneAttributes;
import LodestoneShims;

// An enum axis over the storage formats of the shim module. The axis takes every case of the enum.

[ls_axis_enum]
extern static const StorageFormat LEVEL_FORMAT = StorageFormat::Rgba16Float;

RWStructuredBuffer<float> Output;

[shader("compute")]
[numthreads(8, 1, 1)]
void MainCS(uint3 id : SV_DispatchThreadID)
{
    Output[id.x] = LEVEL_FORMAT == StorageFormat::R32Float ? 1.0f : 0.0f;
}
