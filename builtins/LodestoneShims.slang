module LodestoneShims;
import LodestoneAttributes;

// Shim interfaces for resources, and their Bound, Pointer, and Handle implementations. A shader declares `extern struct X : IReadBuffer<T>`
// and calls the interface. The cooker links the implementation that the target profile uses for that resource class.
// Each implementation method is [ForceInline] so that a constant index into an array of shims stays a constant, and needs
// no dynamic-indexing device feature.

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

// Marks the scalar types that an atomic buffer accepts: int and uint. IBitAtomicable also admits 64-bit and 8-bit and 16-bit
// integers, which WebGPU lacks, and IArithmeticAtomicable admits float and half, which need a device feature. Slang seals both
// interfaces, so a marker interface is the way to narrow them.
public interface IShimAtomicScalar
{
}

extension int : IShimAtomicScalar
{
}

extension uint : IShimAtomicScalar
{
}

// An atomic buffer. Every read-modify-write method returns the old value, as Atomic<T> does. The methods use Slang's default
// memory order, Relaxed.
public interface IAtomicBuffer<T : IShimAtomicScalar>
{
    T Load(uint index);
    void Store(uint index, T value);
    T Add(uint index, T value);
    T Sub(uint index, T value);
    T Min(uint index, T value);
    T Max(uint index, T value);
    T And(uint index, T value);
    T Or(uint index, T value);
    T Xor(uint index, T value);
    T Increment(uint index);
    T Decrement(uint index);
    T Exchange(uint index, T value);
    T CompareExchange(uint index, T compare_value, T value);
}

int64_t Wide64ToInt64(Wide64 value)
{
    return int64_t((uint64_t(value.high) << 32) | uint64_t(value.low));
}

Wide64 Wide64FromInt64(int64_t value)
{
    const uint64_t bits = uint64_t(value);

    return Wide64.Make(uint(bits & 0xFFFFFFFFull), uint(bits >> 32));
}

// Adds a value for each lane with one atomic for each wave, and returns what Add returns for that lane. The prefix sum
// gives each lane its offset into the wave's share. `index` must be the same on every lane that calls it, because the first
// active lane adds the wave's total there. This needs the subgroup capabilities, and only an entry point that calls it
// records them.
public uint AtomicAddWaveAggregated<B : IAtomicBuffer<uint>>(B buffer, uint index, uint value)
{
    uint waveTotal = WaveActiveSum(value);
    uint laneOffset = WavePrefixSum(value);
    uint firstLaneOld = 0u;

    if (WaveIsFirstLane())
    {
        firstLaneOld = buffer.Add(index, waveTotal);
    }

    uint waveBase = WaveReadLaneFirst(firstLaneOld);

    return waveBase + laneOffset;
}

// A 64-bit integer as two 32-bit words. WGSL has no 64-bit integer type, and Slang emits `i64` into WGSL without an error
// when a shader holds an int64_t, so the interface of a 64-bit shim must not name one. The words are the little-endian halves
// of the two's complement value, which makes one type serve int64_t and uint64_t.
public struct Wide64
{
    public uint low;
    public uint high;

    public static Wide64 Make(uint low_word, uint high_word)
    {
        Wide64 result;
        result.low = low_word;
        result.high = high_word;
        return result;
    }

    // Sign extends, so FromInt(-1) is the 64-bit value -1.
    public static Wide64 FromInt(int value)
    {
        return Make(uint(value), value < 0 ? 0xFFFFFFFFu : 0u);
    }

    public static Wide64 FromUInt(uint value)
    {
        return Make(value, 0u);
    }

    // Two's complement negation. A zero low word carries into the high word.
    public Wide64 Negated()
    {
        return Make(~low + 1u, ~high + (low == 0u ? 1u : 0u));
    }

    // The signed value, rounded to float. A negative value converts as the negation of its magnitude: the sum of two floats of
    // opposite signs would cancel the low word of a small negative value to zero.
    public float ToFloat()
    {
        const bool negative = int(high) < 0;
        const Wide64 magnitude = negative ? Negated() : this;
        const float value = float(magnitude.high) * 4294967296.0f + float(magnitude.low);

        return negative ? -value : value;
    }
}

// A 64-bit integer accumulator in a buffer: many threads add signed values, and a later dispatch reads the sum. Wrap-around
// is modulo 2^64. Add and Sub return nothing, because the emulated form on WGSL cannot give an exact old value. Load and
// Store are exact only while no Add or Sub runs on that element, which means after a barrier or in a later dispatch: the
// emulated form reads and writes the two words with two atomic operations. Store is for clearing. An element is 8 bytes on
// every profile, little-endian, so a host reads one buffer layout everywhere. Workgroup (groupshared) 64-bit atomics are not
// covered. SPIR-V profiles need the Int64 and Int64Atomics capabilities, and WGSL needs no feature.
public interface IAtomicAccumulator64
{
    void Add(uint index, Wide64 value);
    void Sub(uint index, Wide64 value);
    void Store(uint index, Wide64 value);
    Wide64 Load(uint index);
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

    [ForceInline]
    public T Load(uint index)
    {
        return data[index];
    }
}

public struct BoundReadWriteBuffer<T> : IReadWriteBuffer<T>
{
    RWStructuredBuffer<T> data;

    [ForceInline]
    public T Load(uint index)
    {
        return data[index];
    }

    [ForceInline]
    public void Store(uint index, T value)
    {
        data[index] = value;
    }
}

public struct BoundAtomicBuffer<T : IShimAtomicScalar> : IAtomicBuffer<T>
    where T : IBitAtomicable
{
    RWStructuredBuffer<Atomic<T>> data;

    [ForceInline]
    public T Load(uint index)
    {
        return data[index].load();
    }

    [ForceInline]
    public void Store(uint index, T value)
    {
        data[index].store(value);
    }

    [ForceInline]
    public T Add(uint index, T value)
    {
        return data[index].add(value);
    }

    [ForceInline]
    public T Sub(uint index, T value)
    {
        return data[index].sub(value);
    }

    [ForceInline]
    public T Min(uint index, T value)
    {
        return data[index].min(value);
    }

    [ForceInline]
    public T Max(uint index, T value)
    {
        return data[index].max(value);
    }

    [ForceInline]
    public T And(uint index, T value)
    {
        return data[index].and(value);
    }

    [ForceInline]
    public T Or(uint index, T value)
    {
        return data[index].or(value);
    }

    [ForceInline]
    public T Xor(uint index, T value)
    {
        return data[index].xor(value);
    }

    [ForceInline]
    public T Increment(uint index)
    {
        return data[index].increment();
    }

    [ForceInline]
    public T Decrement(uint index)
    {
        return data[index].decrement();
    }

    [ForceInline]
    public T Exchange(uint index, T value)
    {
        return data[index].exchange(value);
    }

    [ForceInline]
    public T CompareExchange(uint index, T compare_value, T value)
    {
        return data[index].compareExchange(compare_value, value);
    }
}

// The Bound form on SPIR-V: one 64-bit atomic for each element.
public struct BoundAccumulator64 : IAtomicAccumulator64
{
    RWStructuredBuffer<Atomic<int64_t>> data;

    [ForceInline]
    public void Add(uint index, Wide64 value)
    {
        data[index].add(Wide64ToInt64(value));
    }

    [ForceInline]
    public void Sub(uint index, Wide64 value)
    {
        data[index].sub(Wide64ToInt64(value));
    }

    [ForceInline]
    public void Store(uint index, Wide64 value)
    {
        data[index].store(Wide64ToInt64(value));
    }

    [ForceInline]
    public Wide64 Load(uint index)
    {
        return Wide64FromInt64(data[index].load());
    }
}

// The Bound form on WGSL: an element is an array of two atomic words, low then high, which has the layout of a little-endian
// int64_t. The low word adds first, and its carry goes to the high word. A reader that runs at the same time as an Add can see
// the low word with no carry. An array element, unlike a struct of two words, adds no host type that only this profile has. No
// 64-bit type appears in this struct or in any function that it calls.
public struct BoundAccumulator64Words : IAtomicAccumulator64
{
    RWStructuredBuffer<Atomic<uint>[2]> data;

    [ForceInline]
    public void Add(uint index, Wide64 value)
    {
        const uint oldLow = data[index][0].add(value.low);
        const uint carry = (oldLow + value.low) < oldLow ? 1u : 0u;

        data[index][1].add(value.high + carry);
    }

    [ForceInline]
    public void Sub(uint index, Wide64 value)
    {
        Add(index, value.Negated());
    }

    [ForceInline]
    public void Store(uint index, Wide64 value)
    {
        data[index][0].store(value.low);
        data[index][1].store(value.high);
    }

    [ForceInline]
    public Wide64 Load(uint index)
    {
        const uint low = data[index][0].load();
        const uint high = data[index][1].load();

        return Wide64.Make(low, high);
    }
}

public struct BoundTexture2D : ITexture2DRef
{
    Texture2D<float4> texture;

    [ForceInline]
    public Texture2D<float4> Get()
    {
        return texture;
    }
}

public struct BoundSampler : ISamplerRef
{
    SamplerState sampler;

    [ForceInline]
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

    [ForceInline]
    public void Store(uint2 coordinate, float4 value)
    {
        texture.Store(coordinate, F.DataType.FromFloat4(value));
    }
}

// The Pointer form: a buffer device address. Read access is checked by Slang's type checker only, and the SPIR-V carries no
// NonWritable for it. The pointee is std430, like the element of a StructuredBuffer, so both forms have one layout. Slang
// reflection ignores that data layout (shader-slang/slang#13188), so the cooker computes the stride itself.
public struct PointerReadBuffer<T> : IReadBuffer<T>
{
    [ls_shim_value]
    Ptr<T, Access.Read, AddressSpace.Device, Std430DataLayout> data;

    [ForceInline]
    public T Load(uint index)
    {
        return data[index];
    }
}

public struct PointerReadWriteBuffer<T> : IReadWriteBuffer<T>
{
    [ls_shim_value]
    Ptr<T, Access.ReadWrite, AddressSpace.Device, Std430DataLayout> data;

    [ForceInline]
    public T Load(uint index)
    {
        return data[index];
    }

    [ForceInline]
    public void Store(uint index, T value)
    {
        data[index] = value;
    }
}

// The pointee is Atomic<T>, so a write access is the only one that Slang accepts. The WGSL profile never uses this form:
// Slang emits invalid WGSL for a pointer to an atomic.
public struct PointerAtomicBuffer<T : IShimAtomicScalar> : IAtomicBuffer<T>
    where T : IBitAtomicable
{
    [ls_shim_value]
    Ptr<Atomic<T>, Access.ReadWrite, AddressSpace.Device, Std430DataLayout> data;

    [ForceInline]
    public T Load(uint index)
    {
        return data[index].load();
    }

    [ForceInline]
    public void Store(uint index, T value)
    {
        data[index].store(value);
    }

    [ForceInline]
    public T Add(uint index, T value)
    {
        return data[index].add(value);
    }

    [ForceInline]
    public T Sub(uint index, T value)
    {
        return data[index].sub(value);
    }

    [ForceInline]
    public T Min(uint index, T value)
    {
        return data[index].min(value);
    }

    [ForceInline]
    public T Max(uint index, T value)
    {
        return data[index].max(value);
    }

    [ForceInline]
    public T And(uint index, T value)
    {
        return data[index].and(value);
    }

    [ForceInline]
    public T Or(uint index, T value)
    {
        return data[index].or(value);
    }

    [ForceInline]
    public T Xor(uint index, T value)
    {
        return data[index].xor(value);
    }

    [ForceInline]
    public T Increment(uint index)
    {
        return data[index].increment();
    }

    [ForceInline]
    public T Decrement(uint index)
    {
        return data[index].decrement();
    }

    [ForceInline]
    public T Exchange(uint index, T value)
    {
        return data[index].exchange(value);
    }

    [ForceInline]
    public T CompareExchange(uint index, T compare_value, T value)
    {
        return data[index].compareExchange(compare_value, value);
    }
}

// The Pointer form: one 64-bit atomic for each element, at the std430 stride of 8.
public struct PointerAccumulator64 : IAtomicAccumulator64
{
    [ls_shim_value]
    Ptr<Atomic<int64_t>, Access.ReadWrite, AddressSpace.Device, Std430DataLayout> data;

    [ForceInline]
    public void Add(uint index, Wide64 value)
    {
        data[index].add(Wide64ToInt64(value));
    }

    [ForceInline]
    public void Sub(uint index, Wide64 value)
    {
        data[index].sub(Wide64ToInt64(value));
    }

    [ForceInline]
    public void Store(uint index, Wide64 value)
    {
        data[index].store(Wide64ToInt64(value));
    }

    [ForceInline]
    public Wide64 Load(uint index)
    {
        return Wide64FromInt64(data[index].load());
    }
}

// The Handle form: a descriptor handle, a uint2 whose x component is the index into the descriptor heap.
public struct HandleTexture2D : ITexture2DRef
{
    [ls_shim_value]
    DescriptorHandle<Texture2D<float4>> texture;

    [ForceInline]
    public Texture2D<float4> Get()
    {
        return texture;
    }
}

public struct HandleSampler : ISamplerRef
{
    [ls_shim_value]
    DescriptorHandle<SamplerState> sampler;

    [ForceInline]
    public SamplerState Get()
    {
        return sampler;
    }
}

public struct HandleWriteStorageTexture2D<F : ITexelElement> : IWriteStorageTexture2D
    where F.DataType : IStorageTexelFromFloat4
{
    [ls_shim_value]
    DescriptorHandle<WTexture2D<F>> texture;

    [ForceInline]
    public void Store(uint2 coordinate, float4 value)
    {
        texture.Store(coordinate, F.DataType.FromFloat4(value));
    }
}
