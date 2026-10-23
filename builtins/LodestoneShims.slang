module LodestoneShims;

// Shim interfaces for resources, and their Bound implementations. A shader declares `extern struct X : IReadBuffer<T>`
// and calls the interface. The cooker links the implementation that the target profile uses for that resource class.

// The storage texture formats that every target accepts, for a texture that a shader writes. A format axis is an enum axis over
// this enum (`[ls_axis_enum] extern static const StorageFormat LEVEL_FORMAT = StorageFormat::Rgba16Float;`), so a case that
// is not listed here cannot be written. Each case is a float format: the shader stores a `float4`, and a format with fewer
// channels keeps the first ones.
//
//   Case         WGSL            Vulkan format               Slang type
//   Rgba8Unorm   rgba8unorm      R8G8B8A8_UNORM              RGBA8Unorm
//   Rgba8Snorm   rgba8snorm      R8G8B8A8_SNORM              RGBA8Snorm
//   Rgba16Float  rgba16float     R16G16B16A16_SFLOAT         RGBA16F
//   R32Float     r32float        R32_SFLOAT                  R32F
//   Rg32Float    rg32float       R32G32_SFLOAT               RG32F
//   Rgba32Float  rgba32float     R32G32B32A32_SFLOAT         RGBA32F
//
// Not accepted:
//   - The integer formats (rgba8uint, rgba16uint, r32uint, and the sint set). A shader stores a `uint4` or an `int4` to
//     them, which needs its own interface. Nobody has asked for one.
//   - bgra8unorm. WebGPU allows it as a storage format only with a feature, and Vulkan does not guarantee it.
//   - r8, rg8, r16, rg16, rgb10a2, and rg11b10. They are not WebGPU core storage formats.
//
// A storage texture that a shader both reads and writes is `read_write` in WGSL, and core WebGPU allows that for R32Float
// only. Read a texture through ITexture2DRef and write it through a write-only storage shim instead.
//
// A change to this list is a change to `target/StorageFormatTable.cpp` too: StorageFormatTest compares the two.
public enum StorageFormat
{
    Rgba8Unorm,
    Rgba8Snorm,
    Rgba16Float,
    R32Float,
    Rg32Float,
    Rgba32Float,
}

public interface IReadBuffer<T>
{
    T Load(uint index);
}

public interface IReadWriteBuffer<T>
{
    T Load(uint index);
    void Store(uint index, T value);
}

public interface ITexture2DRef
{
    Texture2D<float4> Get();
}

public interface ISamplerRef
{
    SamplerState Get();
}

// Writes a float4 to a storage texture of one format. The format is a type argument of the implementation (RGBA16F, R32F, ...),
// and the texel of a format with fewer channels keeps the first ones.
public interface IWriteStorageTexture2D
{
    void Store(uint2 coordinate, float4 value);
}

// Converts the float4 of IWriteStorageTexture2D to the texel type of a format: float, float2, or float4.
public interface IStorageTexelFromFloat4
{
    static This FromFloat4(float4 value);
}

extension float : IStorageTexelFromFloat4
{
    static float FromFloat4(float4 value) { return value.x; }
}

extension float2 : IStorageTexelFromFloat4
{
    static float2 FromFloat4(float4 value) { return value.xy; }
}

extension float4 : IStorageTexelFromFloat4
{
    static float4 FromFloat4(float4 value) { return value; }
}

public struct BoundReadBuffer<T> : IReadBuffer<T>
{
    StructuredBuffer<T> data;

    public T Load(uint index)
    {
        return data[index];
    }
}

public struct BoundReadWriteBuffer<T> : IReadWriteBuffer<T>
{
    RWStructuredBuffer<T> data;

    public T Load(uint index)
    {
        return data[index];
    }

    public void Store(uint index, T value)
    {
        data[index] = value;
    }
}

public struct BoundTexture2D : ITexture2DRef
{
    Texture2D<float4> texture;

    public Texture2D<float4> Get()
    {
        return texture;
    }
}

public struct BoundSampler : ISamplerRef
{
    SamplerState sampler;

    public SamplerState Get()
    {
        return sampler;
    }
}

// A write-only storage texture: `write` access in WGSL, which every format of StorageFormat allows.
public struct BoundWriteStorageTexture2D<F : ITexelElement> : IWriteStorageTexture2D
    where F.DataType : IStorageTexelFromFloat4
{
    WTexture2D<F> texture;

    public void Store(uint2 coordinate, float4 value)
    {
        texture.Store(coordinate, F.DataType.FromFloat4(value));
    }
}
