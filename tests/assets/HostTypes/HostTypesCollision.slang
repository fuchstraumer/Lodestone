module HostTypesCollision;

// A binding with the name of a generated array of the host header. The cook must fail, and must not shadow it.

RWStructuredBuffer<uint> Bindings;

[shader("compute")]
[numthreads(64, 1, 1)]
void CollideCS(uint3 threadId: SV_DispatchThreadID)
{
    Bindings[threadId.x] = threadId.x;
}
