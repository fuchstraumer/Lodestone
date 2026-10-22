#pragma once
#ifndef LODESTONE_SHADER_MANIFEST_LAYOUT_HPP
#define LODESTONE_SHADER_MANIFEST_LAYOUT_HPP
#include "ShaderManifestFormat.hpp"
#include "ResourceFlags.hpp"
#include "ShaderLibraryTypes.hpp"
#include "VariantKey.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>

/**
 * @brief The views over one variant and its entry points: resources, layouts, and merged pipeline layouts. Each
 * view points at its `EnvironmentView`, which `ShaderManifest.hpp` declares. Include that header.
 */
namespace lodestone::manifest
{

class ModuleView;
class EnvironmentView;

/** One member of a uniform block. Use it to check that a CPU struct matches the shader layout. */
class UniformMemberView
{
public:
    [[nodiscard]] std::string_view Name() const noexcept;
    [[nodiscard]] uint32_t Offset() const noexcept;
    [[nodiscard]] uint32_t Size() const noexcept;
    [[nodiscard]] uint32_t ArrayCount() const noexcept;
    [[nodiscard]] uint32_t ElementStride() const noexcept;
    [[nodiscard]] MatrixLayout Layout() const noexcept;
    [[nodiscard]] const UniformMember& Record() const noexcept;

private:
    friend class UniformMemberRange;
    UniformMemberView(const EnvironmentView* _environment, const UniformMember* _record) noexcept;
    const EnvironmentView* environment{ nullptr };
    const UniformMember* record{ nullptr };
};

/** The members of one uniform block, in declaration order. Empty for every other binding kind. */
class UniformMemberRange
{
public:
    class Iterator
    {
    public:
        Iterator() noexcept = default;
        explicit Iterator(const UniformMemberRange* _range, uint32_t _position) noexcept
            : range(_range), position(_position) {}

        using iterator_concept = std::input_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = UniformMemberView;
        using reference = UniformMemberView;
        using pointer = void;

        [[nodiscard]] UniformMemberView operator*() const noexcept
        {
            return range->operator[](position);
        }

        Iterator& operator++() noexcept
        {
            ++position;
            return *this;
        }

        Iterator operator++(int) noexcept
        {
            Iterator previous = *this;
            ++position;
            return previous;
        }

        [[nodiscard]] bool operator==(const Iterator& other) const noexcept
        {
            return range == other.range && position == other.position;
        }

    private:
        const UniformMemberRange* range{ nullptr };
        uint32_t position{ 0u };
    };

    [[nodiscard]] Iterator begin() const noexcept
    {
        return Iterator{ this, 0u };
    }

    [[nodiscard]] Iterator end() const noexcept
    {
        return Iterator{ this, Size() };
    }

    [[nodiscard]] uint32_t Size() const noexcept;
    [[nodiscard]] bool Empty() const noexcept;
    [[nodiscard]] UniformMemberView operator[](uint32_t position) const noexcept;

private:
    friend class ResolvedResource;
    friend class EntryPointConstantsView;
    UniformMemberRange(const EnvironmentView* _environment, std::span<const UniformMember> _members) noexcept;
    const EnvironmentView* environment{ nullptr };
    std::span<const UniformMember> members;
};

/** A resource as seen by one entry point: its binding record and its footprint.
 * @note Views point into their `EnvironmentView`, so keep that environment alive and do not move it
 * while a view exists. */
class ResolvedResource
{
public:
    [[nodiscard]] std::string_view Name() const noexcept;
    [[nodiscard]] std::string_view ScopeName() const noexcept;
    [[nodiscard]] PlacementKind Placement() const noexcept;
    [[nodiscard]] PlacementPayload PlacementValue() const noexcept;
    [[nodiscard]] BindingKind Kind() const noexcept;
    [[nodiscard]] ResourceShape Shape() const noexcept;
    [[nodiscard]] ResourceAccess Access() const noexcept;
    [[nodiscard]] TextureFormat StorageFormat() const noexcept;
    /** @brief How a shader reads a sampled texture. Invalid for every other resource. */
    [[nodiscard]] TextureSampleType SampleType() const noexcept;
    [[nodiscard]] bool IsComparisonSampler() const noexcept;
    /** @brief The size of one structured buffer element, in bytes. Zero for a texture or a sampler. */
    [[nodiscard]] uint32_t ElementStride() const noexcept;
    [[nodiscard]] uint32_t ArrayCount() const noexcept;
    /** @brief The size of a uniform block, in bytes. Zero for every other binding kind. */
    [[nodiscard]] uint64_t ByteSize() const noexcept;
    /** @brief The element count from `[ls_element_count]`, for this variant. Zero when the shader does not
     * declare one, or when the resource has no footprint. In that case, the caller must provide the size. */
    [[nodiscard]] uint64_t ElementCount() const noexcept;
    /** @brief The texture extent from `[ls_extent_1d]`, `[ls_extent_2d]`, or `[ls_extent_3d]`. Zero when the shader does not
     * declare one, or when the resource has no footprint. In that case, the caller must provide the size. */
    [[nodiscard]] uint32_t ExtentX() const noexcept;
    [[nodiscard]] uint32_t ExtentY() const noexcept;
    [[nodiscard]] uint32_t ExtentZ() const noexcept;
    [[nodiscard]] UniformMemberRange Members() const noexcept;
    /** @brief The roles from `[ls_resource_role]`: usages of a buffer outside of its shader bindings. */
    [[nodiscard]] ResourceRoleFlags Roles() const noexcept;
    /** @brief The count of tags from `[ls_resource_tags]`. */
    [[nodiscard]] uint32_t TagCount() const noexcept;
    /** @brief One tag, in the order of the tags' text. */
    [[nodiscard]] std::string_view Tag(uint32_t position) const noexcept;
    [[nodiscard]] bool HasTag(std::string_view tag) const noexcept;
    /** @brief The usage a buffer needs at creation for its binding kind and its roles. Copy and map usages are not
     * included: the graph knows them. `Invalid` for a resource that is not a uniform or storage buffer. */
    [[nodiscard]] BufferUsageFlags RequiredBufferUsage() const noexcept;
    /** @brief The axes that change this resource's element count or extent in its environment, in the word layout of
     * `EnvironmentView::AxisMask`. A size that no axis changes has an empty mask. */
    [[nodiscard]] std::span<const uint64_t> FootprintAxes() const noexcept;
    [[nodiscard]] bool IsFootprintAxis(uint32_t local_axis) const noexcept;
    /** @brief The position of the binding record in `EnvironmentView::Bindings()`. */
    [[nodiscard]] uint32_t Index() const noexcept;
    [[nodiscard]] const Binding& Record() const noexcept;
    /** @brief Null when the footprint list is shorter than the resource list. */
    [[nodiscard]] const Footprint* FootprintRecord() const noexcept;
    [[nodiscard]] bool operator==(const ResolvedResource& rhs) const noexcept;
    [[nodiscard]] bool operator!=(const ResolvedResource& rhs) const noexcept;
private:
    friend class LayoutRange;
    ResolvedResource(const EnvironmentView* _environment,
                     const Binding* _record,
                     const Footprint* _footprint) noexcept;
    const EnvironmentView* environment{ nullptr };
    const Binding* record{ nullptr };
    const Footprint* footprint{ nullptr };
};

/** Where the value of one creation field comes from. */
enum class CreationSource : uint8_t
{
    /** The binding kind has no such field. */
    NotApplicable = 0,
    /** The shader fixes the value, and no axis changes it. */
    Shader,
    /** The shader fixes the value for each variant, and the axes in `ResolvedResource::FootprintAxes` change it. */
    ShaderPerVariant,
    /** The shader does not state the value, so the renderer must supply it. */
    Cpu,
};

/** The source of each field that a renderer needs to create the resource of one binding. A resource with no `Cpu`
 * field is complete: the manifest gives every value, and the graph needs no code for it. */
struct CreationFields
{
    /** A buffer's size: the byte size of a uniform block, or the element count of a storage buffer. */
    CreationSource Size{ CreationSource::NotApplicable };
    CreationSource Usage{ CreationSource::NotApplicable };
    /** A texture's dimension (1D, 2D, 3D, or cube). */
    CreationSource Dimension{ CreationSource::NotApplicable };
    CreationSource Format{ CreationSource::NotApplicable };
    CreationSource Extent{ CreationSource::NotApplicable };
    CreationSource ArrayLayers{ CreationSource::NotApplicable };
    CreationSource MipLevels{ CreationSource::NotApplicable };
    CreationSource SampleCount{ CreationSource::NotApplicable };
    /** A sampler's filters and address modes. Whether it compares is fixed by the shader. */
    CreationSource SamplerState{ CreationSource::NotApplicable };

    [[nodiscard]] bool IsComplete() const noexcept;
};

/** @brief Returns the source of each creation field of one resource. It reads only what the manifest stores: the
 * binding kind and shape, the storage format, the footprint, the roles, and the footprint axes. */
[[nodiscard]] CreationFields DescribeCreation(const ResolvedResource& resource) noexcept;

/** One texture and one sampler that a pipeline samples together, and the stages that sample them. */
struct MergedSamplerTexturePair
{
    ResolvedResource Texture;
    ResolvedResource Sampler;
    ShaderStageKind Stages{ ShaderStageKind::Invalid };
};

/** One resource of a merged layout, and the stages that use it. */
struct MergedResource
{
    /** The view of the first stage that reads the resource, in the order given to `Merge`. */
    ResolvedResource Resource;
    ShaderStageKind Stages{ ShaderStageKind::Invalid };
};

/** The resources used by one entry point of one variant, in placement order. `EnvironmentView::Open`
 * validates this order: Bound resources come first, in strictly ascending (group, binding) order. */
class LayoutRange
{
public:
    /** An empty range. This is used by `MergedLayoutView`, which stores a fixed array of ranges. */
    LayoutRange() noexcept = default;

    class Iterator
    {
    public:
        // std::ranges::range needs a default constructor: end() must be semiregular.
        Iterator() noexcept = default;
        explicit Iterator(const LayoutRange* _range, uint32_t _position) noexcept
            : range(_range), position(_position) {}

        // contiguous iterator is not a valid tag, because this is a 
        // a proxy iterator that returns ResolvedResource by value, not by reference.
        using iterator_concept = std::input_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = ResolvedResource;
        using pointer = value_type*;
        using reference = value_type&;

        // because ResolvedResource is always returned by value, operator-> is strictly invalid
        // and we need to make sure it's not possible to make this iterator return an address
        // of a temporary ResolvedResource
        void* operator->() const noexcept = delete;

        // these are all defined in the header to aid with inlining/lto across 
        // translation units, since they're more likely to be hot

        [[nodiscard]] ResolvedResource operator*() const noexcept
        {
            return range->operator[](position);
        }

        Iterator& operator++() noexcept
        {
            ++position;
            return *this;
        }

        Iterator operator++(int) noexcept
        {
            Iterator previous = *this;
            ++position;
            return previous;
        }

        Iterator operator+(difference_type n) const noexcept
        {
            return Iterator{ range, position + static_cast<uint32_t>(n) };
        }

        Iterator& operator+=(difference_type n) noexcept
        {
            position += static_cast<uint32_t>(n);
            return *this;
        }

        [[nodiscard]] bool operator==(const Iterator& other) const noexcept
        {
            return range == other.range && position == other.position;
        }

        [[nodiscard]] bool operator!=(const Iterator& other) const noexcept
        {
            return !(*this == other);
        }

    private:
        friend class LayoutRange;
        const LayoutRange* range{ nullptr };
        uint32_t position{ 0u };
    };

    [[nodiscard]] Iterator begin() const noexcept
    {
        return Iterator{ this, 0u };
    }

    [[nodiscard]] Iterator end() const noexcept
    {
        return Iterator{ this, Size() };
    }

    [[nodiscard]] uint32_t Size() const noexcept;
    [[nodiscard]] bool Empty() const noexcept;
    [[nodiscard]] ResolvedResource operator[](uint32_t position) const noexcept;

private:
    friend class EntryPointInstanceView;
    LayoutRange(const EnvironmentView* _environment,
                std::span<const uint32_t> _visible,
                std::span<const uint32_t> _resources,
                std::span<const Footprint> _footprints) noexcept;
    const EnvironmentView* environment{ nullptr };
    std::span<const uint32_t> visible;
    std::span<const uint32_t> resources;
    std::span<const Footprint> footprints;
};

/** The entry point constants of one entry point instance: its `uniform` parameters of ordinary data, in the layout
 * of the environment's target. The delivery tells a renderer how to supply the data: a buffer write for
 * `UniformBuffer`, or a push constant range from offset 0 for `PushConstant`. */
class EntryPointConstantsView
{
public:
    [[nodiscard]] EntryPointConstantsDelivery Delivery() const noexcept;
    [[nodiscard]] uint32_t ByteSize() const noexcept;
    [[nodiscard]] UniformMemberRange Members() const noexcept;
    /** @brief The position in the instance's `Layout()` of the uniform buffer that contains the data. Only for the
     * `UniformBuffer` delivery. */
    [[nodiscard]] uint32_t BindingPosition() const noexcept;
    [[nodiscard]] const EntryPointConstants& Record() const noexcept;

private:
    friend class EntryPointInstanceView;
    EntryPointConstantsView(const EnvironmentView* _environment, const EntryPointConstants* _record) noexcept;
    const EnvironmentView* environment{ nullptr };
    const EntryPointConstants* record{ nullptr };
};

/** One entry point of one variant.
 * @note Iterators point into their range, so keep the range returned by `Layout()` alive while iterating. */
class EntryPointInstanceView
{
public:
    /** @brief The WGSL text. Only for a `Wgsl` profile. */
    [[nodiscard]] std::string_view Source() const noexcept;
    /** @brief The SPIR-V words. Only for a `Spirv` profile. */
    [[nodiscard]] std::span<const uint32_t> SpirvWords() const noexcept;
    [[nodiscard]] ShaderStageKind Stage() const noexcept;
    [[nodiscard]] WorkgroupSize Workgroup() const noexcept;
    [[nodiscard]] LayoutRange Layout() const noexcept;
    /** @brief The textures and samplers this entry point samples together, sorted. Each pair stores two positions
     * in `Layout()`. Empty when the profile did not record pairs (`EnvironmentView::HasSamplerTexturePairs`). */
    [[nodiscard]] std::span<const SamplerTexturePair> SamplerTexturePairs() const noexcept;
    /** @brief The `uniform` parameters of ordinary data. The delivery is `None` when the entry point reads none. */
    [[nodiscard]] EntryPointConstantsView Constants() const noexcept;
    //[[nodiscard]] RasterView Raster() const noexcept;
    [[nodiscard]] uint32_t Raster() const noexcept;
    [[nodiscard]] const EntryPointInstance& Record() const noexcept;
    /** @brief The environment this instance belongs to. */
    [[nodiscard]] const EnvironmentView& Environment() const noexcept;

private:
    friend class VariantView;
    EntryPointInstanceView(const EnvironmentView* _environment,
                           const EntryPointInstance* entry_point_instance,
                           const Variant* _variant,
                           uint32_t entry_point) noexcept;
    const EnvironmentView* environment{ nullptr };
    const EntryPointInstance* entryPointInstance{ nullptr };
    const Variant* variant{ nullptr };
    // the position of the entry point in the module, which gives the stage
    uint32_t entryPoint{ 0u };
};

/** One variant of one environment. */
class VariantView
{
public:
    [[nodiscard]] uint32_t Index() const noexcept;
    [[nodiscard]] VariantKey Key() const noexcept;
    [[nodiscard]] std::string_view Suffix() const noexcept;
    [[nodiscard]] bool IsAxisActive(uint32_t local_axis) const noexcept;
    [[nodiscard]] uint32_t EntryPointCount() const noexcept;
    [[nodiscard]] EntryPointInstanceView EntryPoint(uint32_t entry_point) const noexcept;
    [[nodiscard]] const Variant& Record() const noexcept;

private:
    friend class EnvironmentView;
    VariantView(const EnvironmentView* _environment, uint32_t _index) noexcept;
    const EnvironmentView* environment{ nullptr };
    uint32_t index{};
};

/** The push constant block of one pipeline. Slang puts the block of each stage at offset 0, so the blocks of
 * the stages overlap, and each byte has one meaning for all stages. `Constants` is the block of the stage
 * with the largest byte size. A renderer declares one push constant range from offset 0, with that byte size
 * and with `Stages`. */
struct MergedPushConstants
{
    EntryPointConstantsView Constants;
    ShaderStageKind Stages{ ShaderStageKind::Invalid };
};

/** @brief The merged resources of the entry point instances of one pipeline, in placement order.
 *
 * Pipeline layouts need each resource once, with every stage that reads it. `Merge` takes one instance
 * for each stage and validates every shared placement once, so that iteration afterwards runs unchecked.
 * Iteration returns one `MergedResource` for each (group, binding). Each change of group in the sequence
 * marks the start of the next bind group layout (or descriptor set layout).
 *
 * The instances can come from different variants, modules, or environments.
 * @note This view points into the environments of its instances, so keep them alive and do not move
 * them. Iterators point into the view, so keep the view alive while iterating. */
class MergedLayoutView
{
public:
    // over what we'll actually need, but keeps things nicely rounded
    static constexpr uint32_t k_MaxPipelineStages = 8u;

    class Iterator
    {
    public:
        // std::ranges::range needs a default constructor
        Iterator() noexcept = default;

        // a proxy iterator, as LayoutRange::Iterator is: it returns MergedResource by value
        using iterator_concept = std::input_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = MergedResource;
        using pointer = value_type*;
        using reference = value_type&;

        void* operator->() const noexcept = delete;

        [[nodiscard]] MergedResource operator*() const noexcept;
        Iterator& operator++() noexcept;
        Iterator operator++(int) noexcept;
        [[nodiscard]] bool operator==(const Iterator& other) const noexcept;
        [[nodiscard]] bool operator!=(const Iterator& other) const noexcept;

    private:
        friend class MergedLayoutView;
        Iterator(const MergedLayoutView* _layout,
                 const std::array<uint32_t, k_MaxPipelineStages>& _positions) noexcept;
        const MergedLayoutView* layout{ nullptr };
        // one position for each stage's range; a stage at the end of its range has position == size
        std::array<uint32_t, k_MaxPipelineStages> positions{};
    };

    /** @brief Merges the layouts of one pipeline's entry point instances, one instance for each stage.
     *
     * Fails when:
     * - there are more than `k_MaxPipelineStages` instances (`MergedLayoutTooManyStages`)
     * - two instances have the same stage (`MergedLayoutDuplicateStage`)
     * - a resource is not Bound (`MergedLayoutPlacementUnsupported`) (other binding/layout forms coming later)
     * - two resources at one (group, binding) differ in kind, shape, access, storage format, sample type,
     *   comparison, array count, byte size, roles, or tags (`MergedLayoutConflict`). Names are not compared.
     * - two push constant blocks differ where they overlap (`MergedLayoutPushConstantConflict`): the members of
     *   the shorter block must be the first members of the longer block, with the same offset, size, array
     *   count, element stride, and matrix layout. Names are not compared.
     *
     * On failure, `RecordIndex` stores the index of the binding record, and `Detail` stores the position of
     * the instance. */
    [[nodiscard]] static ManifestResult<MergedLayoutView> Merge(
        std::span<const EntryPointInstanceView> entry_points) noexcept;

    [[nodiscard]] Iterator begin() const noexcept;
    [[nodiscard]] Iterator end() const noexcept;
    [[nodiscard]] uint32_t StageCount() const noexcept;

    /** The union of the sampler and texture pairs of every stage. Each (texture, sampler) placement appears
     * once, with every stage that samples it. This is the input of the WebGPU layout rule: a sampler paired with a
     * texture that cannot filter must be non-filtering, and a texture paired with a filtering sampler must be
     * filterable. A Vulkan renderer can use it to check a format's filter support before it binds. */
    class PairIterator
    {
    public:
        PairIterator() noexcept = default;

        using iterator_concept = std::input_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = MergedSamplerTexturePair;
        using pointer = value_type*;
        using reference = value_type&;

        void* operator->() const noexcept = delete;

        [[nodiscard]] MergedSamplerTexturePair operator*() const noexcept;
        PairIterator& operator++() noexcept;
        PairIterator operator++(int) noexcept;
        [[nodiscard]] bool operator==(const PairIterator& other) const noexcept;
        [[nodiscard]] bool operator!=(const PairIterator& other) const noexcept;

    private:
        friend class MergedLayoutView;
        PairIterator(const MergedLayoutView* _layout, uint32_t _stage, uint32_t _pair) noexcept;
        // moves forward to the first pair that no earlier stage has, or to the end
        void skipRepeats() noexcept;
        const MergedLayoutView* layout{ nullptr };
        uint32_t stage{ 0u };
        uint32_t pair{ 0u };
    };

    class PairRange
    {
    public:
        [[nodiscard]] PairIterator begin() const noexcept;
        [[nodiscard]] PairIterator end() const noexcept;

    private:
        friend class MergedLayoutView;
        explicit PairRange(const MergedLayoutView* _layout) noexcept : layout(_layout) {}
        const MergedLayoutView* layout{ nullptr };
    };

    /** @brief The pairs of every stage, each placement pair once. Keep the view alive while iterating. */
    [[nodiscard]] PairRange SamplerTexturePairs() const noexcept;
    /** @brief True when the profile of every stage recorded its pairs, so the union is complete. */
    [[nodiscard]] bool HasSamplerTexturePairs() const noexcept;

    /** @brief The push constant block of the pipeline. Empty when no stage has the `PushConstant` delivery. A
     * `UniformBuffer` delivery is a resource in the merged layout, so it is not here. */
    [[nodiscard]] std::optional<MergedPushConstants> PushConstants() const noexcept;

private:
    MergedLayoutView() noexcept = default;
    uint32_t stageCount{ 0u };
    bool pairsRecorded{ true };
    std::array<LayoutRange, k_MaxPipelineStages> layoutRanges{};
    std::array<ShaderStageKind, k_MaxPipelineStages> shaderStages{};
    std::array<std::span<const SamplerTexturePair>, k_MaxPipelineStages> pairLists{};
    std::optional<MergedPushConstants> pushConstants;
};

} // namespace lodestone::manifest

#endif // !LODESTONE_SHADER_MANIFEST_LAYOUT_HPP
