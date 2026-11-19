module TexelBuffers;
import LodestoneAttributes;

// A probe for texel buffers. `Buffer<T>` lowers to a uniform texel buffer and `RWBuffer<T>` to a storage texel buffer.
// SPIR-V declares both as an image with the dimension Buffer, so a reader that looks at the sampled mode alone reads
// them as a texture and a storage texture.

Buffer<float4> Source;
RWBuffer<float4> Destination;
RWBuffer<float4> Accumulator;

[shader("compute")]
[numthreads(64, 1, 1)]
void CopyCS(uint3 threadId: SV_DispatchThreadID)
{
    Destination[threadId.x] = Source[threadId.x];
    Accumulator[threadId.x] = Accumulator[threadId.x] + Source[threadId.x];
}
