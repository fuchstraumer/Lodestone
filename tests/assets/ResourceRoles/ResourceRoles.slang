module ResourceRoles;
import LodestoneAttributes;

// Each resource role on a buffer that fits it, and free tags. ResourceRoleTest reads the roles, the tags, and the
// buffer usage back from the manifest. A role with a 4-byte stride needs an element count that is a whole number of
// commands.

struct DispatchArgs
{
    uint x;
    uint y;
    uint z;
};

struct VertexRecord
{
    float4 position;
    float2 uv;
    float2 padding;
};

// the stride of the command: 12 bytes
[ls_resource_role("IndirectDispatch")]
RWStructuredBuffer<DispatchArgs> DispatchArgsBuffer;

// a 4-byte stride: 8 words are two 16-byte draws
[ls_resource_role("IndirectDraw")]
[ls_element_count("8")]
RWStructuredBuffer<uint> DrawWords;

// a raw buffer counts as a 4-byte stride: 10 words are two 20-byte indexed draws
[ls_resource_role("IndirectDrawIndexed")]
[ls_element_count("10")]
RWByteAddressBuffer DrawIndexedRaw;

// two roles, separated by two spaces: 12 words are four dispatches and three draws
[ls_resource_role("IndirectDispatch  IndirectDraw")]
[ls_element_count("12")]
RWStructuredBuffer<uint> SharedArgs;

[ls_resource_role("IndexData")]
RWStructuredBuffer<uint> Indices;

[ls_resource_role("VertexData")]
[ls_resource_tags("Skinned")]
RWStructuredBuffer<VertexRecord> Vertices;

// the same tags in another order, with a repeat: both read back as "ShadowCascade debug.view"
[ls_resource_tags("ShadowCascade debug.view")]
StructuredBuffer<float4> TaggedA;

[ls_resource_tags("debug.view ShadowCascade ShadowCascade")]
StructuredBuffer<float4> TaggedB;

[shader("compute")]
[numthreads(64, 1, 1)]
void RolesCS(uint3 threadId: SV_DispatchThreadID)
{
    const uint i = threadId.x;
    DispatchArgsBuffer[i].x = i;
    DrawWords[i] = i;
    DrawIndexedRaw.Store(i * 4u, i);
    SharedArgs[i] = i;
    Indices[i] = i;
    Vertices[i].position = TaggedA[i] + TaggedB[i];
}
