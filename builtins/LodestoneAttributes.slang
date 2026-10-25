module LodestoneAttributes;

// Sizes travel as strings: Slang folds attribute integers at compile time, but our constants fold at link time.
// Attributes have no optional parameters, so each arity has its own name.
// todo-ship: `ls_byte_size`, rejected when not a multiple of the stride or beside `ls_element_count`

/** Number of elements in a structured buffer. The cooker multiplies this by the reflected element
 * stride, so the stride is never written by hand and cannot drift. */
[__AttributeUsage(_AttributeTargets.Var)]
public struct ls_element_countAttribute
{
    public string expression;
};

/** Width of a 1D texture, in texels. */
[__AttributeUsage(_AttributeTargets.Var)]
public struct ls_extent_1dAttribute
{
    public string width;
};

/** Width and height of a 2D texture, in texels. */
[__AttributeUsage(_AttributeTargets.Var)]
public struct ls_extent_2dAttribute
{
    public string width;
    public string height;
};

/** Width, height, and depth or array layer count of a 3D or array texture. */
[__AttributeUsage(_AttributeTargets.Var)]
public struct ls_extent_3dAttribute
{
    public string width;
    public string height;
    public string depth;
};

/** Buffer usages outside of its bindings, separated by spaces: IndirectDispatch, IndirectDraw, IndirectDrawIndexed,
 * IndexData, VertexData. */
[__AttributeUsage(_AttributeTargets.Var)]
public struct ls_resource_roleAttribute
{
    public string roles;
};

/** Free tags for a resource, separated by whitespace. The cooker does not interpret a tag: it stores the
 * tags in the manifest, and the renderer reads them. A tag holds only letters, digits, and `_.:-`. */
[__AttributeUsage(_AttributeTargets.Var)]
public struct ls_resource_tagsAttribute
{
    public string tags;
};

/** shortcut to declare a boolean axis, which will only produce true/false values in permutation space */
[__AttributeUsage(_AttributeTargets.Var)]
public struct ls_axis_booleanAttribute
{
};

/** shortcut to declare an enum axis: attached to an enum value `extern static const`, so author
  * knows the possible values for the axis and reflection can extract the very same. keeps it simple */ 
[__AttributeUsage(_AttributeTargets.Var)]
public struct ls_axis_enumAttribute
{
};

/** List of values used for an axis constraint */
[__AttributeUsage(_AttributeTargets.Var)]
public struct ls_axis_valuesAttribute
{
    public string values;
};

/** The axis is active when this expression over earlier axes is true, such as `"MY_AXIS == 1"`. */
[__AttributeUsage(_AttributeTargets.Var)]
public struct ls_axis_active_whenAttribute
{
    public string condition;
};

/** "Kind" of an axis - defaults to Tuning, but can also be things like "Capability" to represent
  * axis that toggle based on hardware or API capabilities. Helps cooker make decisions accordingly. */
[__AttributeUsage(_AttributeTargets.Var)]
public struct ls_axis_kindAttribute
{
    public string kind;
};

/** The enum axis over `StorageFormat` that selects the storage format of a shim extern, such as `"LEVEL_FORMAT"`. */
[__AttributeUsage(_AttributeTargets.Struct)]
public struct ls_storage_formatAttribute
{
    public string axis;
};

/** Marks the member of a Pointer or Handle shim implementation that stores the buffer device address or the descriptor
  * handle, so that reflection can tell it from user data of the same type. Only `LodestoneShims` uses it. */
[__AttributeUsage(_AttributeTargets.Var)]
public struct ls_shim_valueAttribute
{
};

[__AttributeUsage(_AttributeTargets.Struct)]
public struct ls_axis_interfaceAttribute
{
};

[__AttributeUsage(_AttributeTargets.Struct)]
public struct ls_axis_interface_implAttribute
{
    public string interfaceName;
};
