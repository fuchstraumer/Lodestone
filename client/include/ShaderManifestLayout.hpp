#pragma once
#ifndef LODESTONE_SHADER_MANIFEST_LAYOUT_HPP
#define LODESTONE_SHADER_MANIFEST_LAYOUT_HPP
#include "ShaderManifestFormat.hpp"
#include "ResourceFlags.hpp"
#include "ShaderManifestDevice.hpp"
#include "ShaderLibraryTypes.hpp"
#include "VariantKey.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>

/** @brief Views over one variant, its entry points, and merged pipeline layouts. */
namespace lodestone::manifest
{

class ModuleView;
class EnvironmentView;

/** One member of a uniform block, to check a CPU struct against the shader layout. */
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
    /** @brief Finds a member by its whole path (through nested scopes with a uniform block) */
    [[nodiscard]] std::optional<UniformMemberView> Find(std::string_view path) const noexcept;

private:
    friend class ResolvedResource;
    friend class EntryPointConstantsView;
    UniformMemberRange(const EnvironmentView* _environment, std::span<const UniformMember> _members) noexcept;
    const EnvironmentView* environment{ nullptr };
    std::span<const UniformMember> members;
};

/** A binding record and its footprint, as one entry point sees them. Keep the `EnvironmentView` alive and unmoved. */
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
    [[nodiscard]] TextureSampleType SampleType() const noexcept;
    [[nodiscard]] bool IsComparisonSampler() const noexcept;
    [[nodiscard]] uint32_t ElementStride() const noexcept;
    [[nodiscard]] uint32_t ArrayCount() const noexcept;
    [[nodiscard]] uint64_t ByteSize() const noexcept;
    /** @brief `[ls_element_count]` for this variant. Zero when not declared, and the caller supplies the size. */
    [[nodiscard]] uint64_t ElementCount() const noexcept;
    /** @brief `[ls_extent_*]` for this variant. Zero when not declared, and the caller supplies the size. */
    [[nodiscard]] uint32_t ExtentX() const noexcept;
    [[nodiscard]] uint32_t ExtentY() const noexcept;
    [[nodiscard]] uint32_t ExtentZ() const noexcept;
    [[nodiscard]] UniformMemberRange Members() const noexcept;
    /** @brief `[ls_resource_role]`: usages of a buffer outside of its shader bindings. */
    [[nodiscard]] ResourceRoleFlags Roles() const noexcept;
    [[nodiscard]] uint32_t TagCount() const noexcept;
    [[nodiscard]] std::string_view Tag(uint32_t position) const noexcept;
    [[nodiscard]] bool HasTag(std::string_view tag) const noexcept;
    /** @brief The creation usage of the binding kind and the roles. Copies and maps belong to the graph. */
    [[nodiscard]] BufferUsageFlags RequiredBufferUsage() const noexcept;
    /** @brief The axes that change the element count or extent, in the word layout of `EnvironmentView::AxisMask`. */
    [[nodiscard]] std::span<const uint64_t> FootprintAxes() const noexcept;
    [[nodiscard]] bool IsFootprintAxis(uint32_t local_axis) const noexcept;
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

enum class CreationSource : uint8_t
{
    NotApplicable = 0,
    /** Fixed by the shader for every variant. */
    Shader,
    /** Fixed for each variant, and changed by `ResolvedResource::FootprintAxes`. */
    ShaderPerVariant,
    /** The renderer supplies the value. */
    Cpu,
};

/** Where each creation field of a resource comes from. A resource with no `Cpu` field is complete. */
struct CreationFields
{
    CreationSource Size{ CreationSource::NotApplicable };
    CreationSource Usage{ CreationSource::NotApplicable };
    CreationSource Dimension{ CreationSource::NotApplicable };
    CreationSource Format{ CreationSource::NotApplicable };
    CreationSource Extent{ CreationSource::NotApplicable };
    CreationSource ArrayLayers{ CreationSource::NotApplicable };
    CreationSource MipLevels{ CreationSource::NotApplicable };
    CreationSource SampleCount{ CreationSource::NotApplicable };
    /** Filters and address modes. Comparison is fixed by the shader. */
    CreationSource SamplerState{ CreationSource::NotApplicable };

    [[nodiscard]] bool IsComplete() const noexcept;
};

[[nodiscard]] CreationFields DescribeCreation(const ResolvedResource& resource) noexcept;

struct MergedSamplerTexturePair
{
    ResolvedResource Texture;
    ResolvedResource Sampler;
    ShaderStageKind Stages{ ShaderStageKind::Invalid };
};

struct MergedResource
{
    /** The view of the first stage given to `Merge` that reads the resource. */
    ResolvedResource Resource;
    ShaderStageKind Stages{ ShaderStageKind::Invalid };
};

/** The resources one entry point reads, in placement order, which `EnvironmentView::Open` checks. */
class LayoutRange
{
public:
    LayoutRange() noexcept = default;

    class Iterator
    {
    public:
        // end() must be semiregular
        Iterator() noexcept = default;
        explicit Iterator(const LayoutRange* _range, uint32_t _position) noexcept
            : range(_range), position(_position) {}

        // a proxy iterator: it returns ResolvedResource by value
        using iterator_concept = std::input_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = ResolvedResource;
        using pointer = value_type*;
        using reference = value_type&;

        // the value is a temporary, so there is no address to return
        void* operator->() const noexcept = delete;


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

/** The entry point constants of one instance. `UniformBuffer`: write a buffer. `PushConstant`: declare a range
 * from offset 0. */
class EntryPointConstantsView
{
public:
    [[nodiscard]] EntryPointConstantsDelivery Delivery() const noexcept;
    [[nodiscard]] uint32_t ByteSize() const noexcept;
    [[nodiscard]] UniformMemberRange Members() const noexcept;
    /** @brief The position of the uniform buffer in `Layout()`. Only for `UniformBuffer`. */
    [[nodiscard]] uint32_t BindingPosition() const noexcept;
    [[nodiscard]] const EntryPointConstants& Record() const noexcept;

private:
    friend class EntryPointInstanceView;
    EntryPointConstantsView(const EnvironmentView* _environment, const EntryPointConstants* _record) noexcept;
    const EnvironmentView* environment{ nullptr };
    const EntryPointConstants* record{ nullptr };
};

/** One entry point of one variant. Keep the range from `Layout()` alive while iterating. */
class EntryPointInstanceView
{
public:
    [[nodiscard]] std::string_view Source() const noexcept;
    [[nodiscard]] std::span<const uint32_t> SpirvWords() const noexcept;
    [[nodiscard]] ShaderStageKind Stage() const noexcept;
    [[nodiscard]] WorkgroupSize Workgroup() const noexcept;
    /** @brief Zero for stages other than compute. */
    [[nodiscard]] uint32_t WorkgroupMemoryBytes() const noexcept;
    /** @brief The capabilities the code needs, over `EnvironmentView::CapabilityNames()`. */
    [[nodiscard]] std::span<const uint64_t> RequiredCapabilities() const noexcept;
    [[nodiscard]] bool RequiresCapability(uint32_t capability_index) const noexcept;
    /** @brief Whether a device can run this instance. `supported` is `EnvironmentView::MakeDeviceCapabilities`. */
    [[nodiscard]] DeviceFit Fit(std::span<const uint64_t> supported, const DeviceLimits& limits) const noexcept;
    [[nodiscard]] LayoutRange Layout() const noexcept;
    /** @brief Sorted pairs of positions in `Layout()`. Empty when the profile did not record pairs. */
    [[nodiscard]] std::span<const SamplerTexturePair> SamplerTexturePairs() const noexcept;
    [[nodiscard]] EntryPointConstantsView Constants() const noexcept;
    //[[nodiscard]] RasterView Raster() const noexcept;
    [[nodiscard]] uint32_t Raster() const noexcept;
    [[nodiscard]] const EntryPointInstance& Record() const noexcept;
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
    uint32_t entryPoint{ 0u };
};

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

/** The push constant block of a pipeline: the largest stage block, since every stage puts its block at offset 0.
 * Declare one range from offset 0 with this size and `Stages`. */
struct MergedPushConstants
{
    EntryPointConstantsView Constants;
    ShaderStageKind Stages{ ShaderStageKind::Invalid };
};

/** @brief Each resource of one pipeline once, with the stages that read it, in placement order. A change of group
 * starts the next bind group layout. Keep the instances' environments and this view alive and unmoved. */
class MergedLayoutView
{
public:
    static constexpr uint32_t k_MaxPipelineStages = 8u;

    class Iterator
    {
    public:
        Iterator() noexcept = default;

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
        // a stage at the end of its range has position == size
        std::array<uint32_t, k_MaxPipelineStages> positions{};
    };

    /** @brief Merges one instance for each stage, checking each shared placement once. Fails on too many stages, a
     * repeated stage, a placement that is not Bound, resources at one placement that differ in anything but name, or
     * push constant blocks that differ where they overlap. `RecordIndex` is the binding, `Detail` the instance. */
    [[nodiscard]] static ManifestResult<MergedLayoutView> Merge(
        std::span<const EntryPointInstanceView> entry_points) noexcept;

    [[nodiscard]] Iterator begin() const noexcept;
    [[nodiscard]] Iterator end() const noexcept;
    [[nodiscard]] uint32_t StageCount() const noexcept;

    /** Each placement pair of every stage once. WebGPU needs it for the filtering rule of samplers and textures. */
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

    [[nodiscard]] PairRange SamplerTexturePairs() const noexcept;
    /** @brief True when every stage's profile recorded its pairs. */
    [[nodiscard]] bool HasSamplerTexturePairs() const noexcept;

    /** @brief Empty when no stage uses `PushConstant`. A uniform buffer delivery is a resource of the layout. */
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
