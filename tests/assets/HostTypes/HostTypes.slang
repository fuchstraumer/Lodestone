module HostTypes;

// The host types of every block and buffer element, with the layouts that the test computes by hand. The module has no
// axis, so it is one variant.

struct Light
{
    float3 position;
    float range;
};

struct Falloff
{
    float samples[3];
    float bias;
};

struct Scene
{
    float4x4 world;
    float2x3 skew;
    Light key;
    float3 tints[2];
    uint flags;
};

struct Grid
{
    float2 cells[2][3];
    uint tag;
};

ConstantBuffer<Scene> scene;
ConstantBuffer<Grid> gridBlock;
StructuredBuffer<Grid> gridBuffer;
ConstantBuffer<Falloff> falloffBlock;
StructuredBuffer<Falloff> falloffBuffer;
StructuredBuffer<Light> lights;
StructuredBuffer<float4> plain;
RWStructuredBuffer<float4> output;

[shader("compute")]
[numthreads(64, 1, 1)]
void SceneCS(uint3 threadId: SV_DispatchThreadID)
{
    float value = scene.world[0][0] + scene.skew[0][0] + scene.key.range + scene.tints[1].x + float(scene.flags);
    value += falloffBlock.samples[2] + falloffBlock.bias + falloffBuffer[0].samples[1] + falloffBuffer[0].bias;
    value += lights[0].position.x + plain[0].x;
    value += gridBlock.cells[1][2].x + gridBuffer[0].cells[1][2].y + float(gridBlock.tag + gridBuffer[0].tag);
    output[threadId.x] = float4(value, 0.0f, 0.0f, 0.0f);
}

// A push constant block on SPIR-V, and a uniform buffer on WGSL, so the array of floats has stride 4 and stride 16
[shader("compute")]
[numthreads(64, 1, 1)]
void ParamsCS(uint3 threadId: SV_DispatchThreadID, uniform float weights[3], uniform uint count)
{
    output[threadId.x] = float4(weights[0] + weights[2], float(count), 0.0f, 0.0f);
}

// One struct parameter: the root is the wrapper `NamedCSParams`, and the struct is a nested field
[shader("compute")]
[numthreads(64, 1, 1)]
void NamedCS(uint3 threadId: SV_DispatchThreadID, uniform Light light)
{
    output[threadId.x] = float4(light.position, light.range);
}
