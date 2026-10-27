module ShimGlobal;
import LodestoneShims;

// Two buffer shims at global scope. In the Pointer form they are ordinary data, so Slang stores them in the uniform buffer it
// adds for global data, and that buffer's binding range has no leaf variable.

public struct Item
{
    public float4 value;
};

extern struct ItemBuffer : IReadBuffer<Item>;
extern struct ResultBuffer : IReadWriteBuffer<Item>;

ItemBuffer items;
ResultBuffer results;

[shader("compute")]
[numthreads(64, 1, 1)]
void MainCS(uint3 id : SV_DispatchThreadID)
{
    results.Store(id.x, items.Load(id.x));
}
