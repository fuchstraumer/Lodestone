module VeloxAttributes;

// Annotations the shader cooker reads back through reflection. They describe sizes that only the
// shader author knows, so the graph stops being told them a second time on the CPU side, where the
// two statements drift apart.
//
// Every size travels as a string, and this is not a style choice. Slang folds an attribute's integer
// argument at compile time, but the permutation constants are `extern static const` and only fold at
// link time, so `[lodestone_element_count(IFFT_SIZE * IFFT_SIZE)]` fails to compile outright. A string
// argument reaches reflection untouched, and the cooker evaluates it once per variant against the
// axis values it already holds.
//
// User-defined attributes also have no optional parameters -- Slang requires the argument count to
// match exactly -- so each arity needs its own name.
//
// todo-ship: add `vx_byte_size` for the cases where an element count is the wrong unit. When it
// lands, the cooker must reject a byte size that is not a whole multiple of the reflected element
// stride, and reject a declaration that carries both attributes.

/** Number of elements in a structured buffer. The cooker multiplies this by the reflected element
 * stride, so the stride is never written by hand and cannot drift. */
[__AttributeUsage(_AttributeTargets.Var)]
public struct lodestone_element_countAttribute
{
    public string expression;
};

/** Width and height of a 2D texture, in texels. */
[__AttributeUsage(_AttributeTargets.Var)]
public struct lodestone_extent_2dAttribute
{
    public string width;
    public string height;
};

/** Width, height, and depth or array layer count of a 3D or array texture. */
[__AttributeUsage(_AttributeTargets.Var)]
public struct lodestone_extent_3dAttribute
{
    public string width;
    public string height;
    public string depth;
};
