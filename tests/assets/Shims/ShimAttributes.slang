module ShimAttributes;
import LodestoneShims;
import LodestoneAttributes;

// Resource attributes on the fields of shim type. The author writes them on the field, and the cook must read them there
// in every access model. ShimAttributeTest checks the roles, tags, and sizes of each resource on every profile.

extern struct Indices : IReadBuffer<uint>;
extern struct Counters : IAtomicBuffer<uint>;
extern struct Albedo : ITexture2DRef;
extern struct Linear : ISamplerRef;
extern struct Totals : IAtomicBuffer<uint>;

// A plain user struct is not a shim: an attribute on a field of this type does not reach the resource inside.
struct Holder
{
    RWStructuredBuffer<uint> Inner;
};

struct Params
{
    [ls_resource_role("IndexData")]
    [ls_resource_tags("Aliased:Indices")]
    [ls_element_count("16")]
    Indices Items;

    [ls_resource_tags("Aliased:Counters")]
    [ls_element_count("4")]
    Counters Tally;

    [ls_resource_tags("Surface")]
    [ls_extent_2d("64", "32")]
    Albedo Surface;

    [ls_resource_tags("Filter")]
    Linear Filter;

    [ls_resource_tags("OnPlainStruct")]
    Holder Plain;

    Indices Untagged;
};

ParameterBlock<Params> params;

[ls_resource_tags("Global:Totals")]
[ls_element_count("2")]
Totals GlobalTotals;

RWStructuredBuffer<uint> Output;

[shader("compute")]
[numthreads(8, 1, 1)]
void MainCS(uint3 id : SV_DispatchThreadID)
{
    const float4 tint = params.Surface.Get().SampleLevel(params.Filter.Get(), float2(0.5f, 0.5f), 0.0f);
    const uint ticket = params.Tally.Add(0u, 1u) + GlobalTotals.Add(0u, 1u);

    params.Plain.Inner[id.x] = params.Items.Load(id.x) + params.Untagged.Load(id.x) + ticket + uint(tint.x);
    Output[id.x] = params.Plain.Inner[id.x];
}
