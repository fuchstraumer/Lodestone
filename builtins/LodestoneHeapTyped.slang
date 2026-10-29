module LodestoneHeapTyped;

// Link module of the per-descriptor-type heap lowering. Slang calls this export in place of its default, which uses
// one mutable descriptor binding.
export T getDescriptorFromHandle<T : IOpaqueDescriptor>(DescriptorHandle<T> handle_value)
{
    return defaultGetDescriptorFromHandle(handle_value, BindlessDescriptorOptions.None);
}
