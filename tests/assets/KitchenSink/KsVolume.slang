module KsVolume;
import LodestoneAttributes;
import KsTypes;
import KsShading;

// The volume module. It writes a 3D texture whose extent an axis sizes, it sizes a groupshared array
// with a link-time constant, and it carries a capability axis with a dependent wave-size axis. It has
// two entry points, and the capability axes are inert for the second one, so the policy names them
// under InertAxesForEntryPoints.
//
// NOISE is an interface axis over INoiseSource, which KsShading declares. QUALITY is an enum axis over
// QualityTier, which KsTypes declares, and it shares its name with the QUALITY axis in KsMaterial.

// Interface axis, cross-module to KsShading.
[ls_axis_interface]
public extern struct NOISE : INoiseSource = ValueNoise;

// Enum axis, cross-module to KsTypes, shared name with KsMaterial.
[ls_axis_enum]
extern static const QualityTier QUALITY = QualityTier::Low;

// Tuning axis. It sizes the output 3D texture on every side, and it sizes the groupshared scratch, so
// it can only bind at cook time.
[ls_axis_values("32, 64, 128")][ls_axis_kind("tuning")]
extern static const uint GRID_SIZE = 32u;

// Capability axis. A device may or may not support wave operations.
[ls_axis_boolean][ls_axis_kind("capability")]
extern static const bool USE_WAVE_OPS = true;

// Capability axis, gated on wave operations. It states the widest wave the variant is cooked for.
[ls_axis_values("16, 32, 64")][ls_axis_kind("capability")][ls_axis_active_when("USE_WAVE_OPS == 1")]
extern static const uint WAVE_SIZE = 32u;

// Technique axis for a debug channel.
[ls_axis_boolean][ls_axis_kind("technique")]
extern static const bool WRITE_DEBUG = false;

internal static const uint TILE = 64u;

[ls_extent_3d("GRID_SIZE", "GRID_SIZE", "GRID_SIZE")]
RWTexture3D<float> OutVolume;
[ls_extent_3d("GRID_SIZE", "GRID_SIZE", "GRID_SIZE")]
Texture3D<float> InVolume;
SamplerState VolumeSampler;
RWStructuredBuffer<float> DebugStats;

groupshared float g_tile[TILE];

[shader("compute")]
[numthreads(4, 4, 4)]
void GenerateCS(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    NOISE noise = {};
    const float3 p = float3(tid) / float(GRID_SIZE);
    float value = noise.sample(p * float(QUALITY == QualityTier::Ultra ? 4u : 1u));

    if (USE_WAVE_OPS)
    {
        value += WaveActiveSum(value) / float(WAVE_SIZE);
    }

    if (gi < TILE)
    {
        g_tile[gi] = value;
    }
    GroupMemoryBarrierWithGroupSync();

    OutVolume[tid] = value;

    if (WRITE_DEBUG)
    {
        DebugStats[0] = value;
    }
}

// The second entry point reads the volume back and blurs it. It uses neither wave-op axis, so both are
// inert here.
[shader("compute")]
[numthreads(4, 4, 4)]
void BlurCS(uint3 tid : SV_DispatchThreadID)
{
    const float3 uvw = (float3(tid) + 0.5f) / float(GRID_SIZE);
    const float center = InVolume.SampleLevel(VolumeSampler, uvw, 0.0f);
    const float neighbor = InVolume.SampleLevel(VolumeSampler, uvw + 1.0f / float(GRID_SIZE), 0.0f);
    OutVolume[tid] = (center + neighbor) * 0.5f;
}
