module LodestoneShims;

// Shim interfaces for resources, and their Bound implementations. A shader declares `extern struct X : IReadBuffer<T>`
// and calls the interface. The cooker links the implementation that the target profile uses for that resource class.

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
