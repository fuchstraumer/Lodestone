implementing InterfaceAxisTest;
import LodestoneAttributes;

public interface IShadeMode { float3 apply(float3 c); }

// each tagged impl is one value of the axis; an untagged conformer would NOT join it
[ls_axis_interface_impl("IShadeMode")]
public struct Passthrough : IShadeMode { public float3 apply(float3 c) { return c; } }
[ls_axis_interface_impl("IShadeMode")]
public struct Invert : IShadeMode { public float3 apply(float3 c) { return 1.0 - c; } }
[ls_axis_interface_impl("IShadeMode")]
public struct Desaturate : IShadeMode { public float3 apply(float3 c) { float g = dot(c, float3(0.299, 0.587, 0.114)); return float3(g, g, g); } }
