#pragma once
#ifndef LODESTONE_SHADER_MANIFEST_HPP
#define LODESTONE_SHADER_MANIFEST_HPP
#include "ShaderManifestFormat.hpp"
#include "ShaderManifestLayout.hpp"
#include "ShaderLibraryTypes.hpp"
#include "VariantKey.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>


/** @brief Read-only views over one cook bundle in a byte span, which must outlive every view. Every cross-reference is
 * an index or an offset, so opening allocates nothing and fixes up no pointer. */
namespace lodestone::manifest
{

/** @brief The header region of one bundle, checked once by `Open`, so every accessor runs unchecked. The span can be
 * the whole file or the header region alone: open the extents with `EnvironmentView::Open` or `OpenEnvironment`.
 * todo: open from a file with a better read backend than ifstream */
class BundleView
{
public:
    BundleView() noexcept;

    static ManifestResult<BundleView> Open(std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] std::string_view String(uint32_t string_index) const noexcept;
    [[nodiscard]] uint32_t StringCount() const noexcept;
    [[nodiscard]] std::span<const Axis> Axes() const noexcept;
    [[nodiscard]] const Axis& AxisData(uint32_t axis_index) const noexcept;
    [[nodiscard]] std::span<const AxisValueType> AxisValues(uint32_t axis_index) const noexcept;
    [[nodiscard]] std::span<const Profile> Profiles() const noexcept;
    [[nodiscard]] uint32_t ModuleCount() const noexcept;
    [[nodiscard]] ModuleView Module(uint32_t module_index) const noexcept;
    /** @brief Returns -1 when no module has this name. */
    [[nodiscard]] int32_t FindModule(std::string_view module_name) const noexcept;
    /** @brief Returns the first profile for this target, or -1. */
    [[nodiscard]] int32_t FindProfile(std::string_view target_name) const noexcept;
    [[nodiscard]] const EnvironmentDirectoryEntry& Environment(uint32_t profile_index,
                                                               uint32_t module_index) const noexcept;
    /** @brief The span must reach the extent: a header region alone returns `EnvironmentExtentOutOfBounds`. */
    [[nodiscard]] ManifestResult<EnvironmentView> OpenEnvironment(uint32_t profile_index,
                                                                  uint32_t module_index) const noexcept;

private:
    friend class ModuleView;
    std::span<const std::byte> bytes;
    const Header* header{ nullptr };
    std::span<const StringRef> strings;
    std::span<const Axis> axes;
    std::span<const AxisValueType> axisValues;
    std::span<const Profile> profiles;
    std::span<const ModuleRootHeader> moduleHeaders;
    std::span<const EnvironmentDirectoryEntry> directory;
};

/** @brief One module: name, entry points, and axes. A module axis selects values of a root axis through a mask, and
 * digit D of a key selects the D-th set bit. */
class ModuleView
{
public:
    ModuleView() noexcept;

    [[nodiscard]] const BundleView& Bundle() const noexcept;
    [[nodiscard]] uint32_t Index() const noexcept;
    [[nodiscard]] std::string_view Name() const noexcept;
    [[nodiscard]] std::span<const EntryPoint> EntryPoints() const noexcept;
    [[nodiscard]] ManifestResult<const EntryPoint*> FindEntryPoint(std::string_view name) const noexcept;
    [[nodiscard]] ManifestResult<uint32_t> FindEntryPointIndex(std::string_view name) const noexcept;
    [[nodiscard]] std::span<const ModuleAxis> ModuleAxes() const noexcept;
    [[nodiscard]] uint32_t AxisCount() const noexcept;
    [[nodiscard]] const Axis& AxisData(uint32_t local_axis) const noexcept;
    /** @brief The values this module uses on one axis, which is also the axis radix. */
    [[nodiscard]] uint32_t AxisValueCount(uint32_t local_axis) const noexcept;
    [[nodiscard]] AxisValueType AxisValue(uint32_t local_axis, uint32_t digit) const noexcept;
    /** @brief The digit of the default the shader declares. */
    [[nodiscard]] uint32_t AxisDefaultDigit(uint32_t local_axis) const noexcept;
    [[nodiscard]] uint32_t AxisMaskWordCount() const noexcept;

private:
    friend class BundleView;
    ModuleView(const BundleView& _bundle, uint32_t module_index) noexcept;
    BundleView bundle;
    uint32_t moduleIndex{ 0u };
    std::span<const EntryPoint> entryPoints;
    std::span<const ModuleAxis> moduleAxes;
};

/** @brief One module cooked for one profile: spans over one extent, checked once by `Open`. Keys, variants, and axis
 * masks are parallel. The instance of (variant V, entry point E) is at `V * EntryPointCount + E`. */
class EnvironmentView
{
public:
    EnvironmentView() noexcept;

    static ManifestResult<EnvironmentView> Open(const BundleView& bundle,
                                                uint32_t profile_index,
                                                uint32_t module_index,
                                                std::span<const std::byte> extent_bytes) noexcept;

    [[nodiscard]] const ModuleView& Module() const noexcept;
    [[nodiscard]] uint32_t ProfileIndex() const noexcept;
    [[nodiscard]] const Profile& ProfileRecord() const noexcept;
    [[nodiscard]] std::string_view String(uint32_t string_index) const noexcept;
    [[nodiscard]] ShaderCodeFormat CodeFormat() const noexcept;
    [[nodiscard]] std::string_view Source(uint32_t source_index) const noexcept;
    [[nodiscard]] std::span<const uint32_t> SpirvWords(uint32_t source_index) const noexcept;

    [[nodiscard]] std::span<const VariantKey> VariantKeys() const noexcept;
    [[nodiscard]] std::span<const Variant> Variants() const noexcept;
    [[nodiscard]] std::optional<uint32_t> FindVariant(VariantKey key) const noexcept;
    [[nodiscard]] std::optional<VariantView> VariantByKey(VariantKey key) const noexcept;
    [[nodiscard]] VariantView VariantAt(uint32_t variant_index) const noexcept;
    [[nodiscard]] std::span<const uint64_t> AxisMask(uint32_t variant_index) const noexcept;
    [[nodiscard]] bool IsAxisActive(uint32_t variant_index, uint32_t local_axis) const noexcept;
    /** @brief Bit k is set when axis k can change the entry point's instance. One cooked value counts as relevant. */
    [[nodiscard]] std::span<const uint64_t> AxisRelevance(uint32_t entry_point_index) const noexcept;
    [[nodiscard]] bool IsAxisRelevant(uint32_t entry_point_index, uint32_t local_axis) const noexcept;
    /** @brief Bit k is set when axis k changes the resource's element count or extent. */
    [[nodiscard]] std::span<const uint64_t> FootprintAxes(uint32_t binding_index) const noexcept;
    [[nodiscard]] bool IsFootprintAxis(uint32_t binding_index, uint32_t local_axis) const noexcept;
    /** @brief String indices sorted by text: SPIR-V capability and extension names, or WGSL `enable` directives. */
    [[nodiscard]] std::span<const uint32_t> CapabilityNames() const noexcept;
    [[nodiscard]] std::string_view CapabilityName(uint32_t capability_index) const noexcept;
    [[nodiscard]] uint32_t CapabilityMaskWordCount() const noexcept;
    [[nodiscard]] std::span<const uint64_t> SourceCapabilities(uint32_t source_index) const noexcept;
    [[nodiscard]] bool SourceNeedsCapability(uint32_t source_index, uint32_t capability_index) const noexcept;
    [[nodiscard]] uint32_t SourceWorkgroupMemoryBytes(uint32_t source_index) const noexcept;

    [[nodiscard]] std::span<const EntryPointInstance> EntryPointInstanceTable() const noexcept;
    [[nodiscard]] std::span<const EntryPointInstance> VariantEntryPoints(uint32_t variant_index) const noexcept;
    /** @brief Returns null when this environment did not cook the key. */
    [[nodiscard]] const EntryPointInstance* FindEntryPointInstance(uint32_t entry_point, VariantKey variant) const noexcept;

    [[nodiscard]] std::span<const Binding> Bindings() const noexcept;
    [[nodiscard]] const Binding& BindingRecord(uint32_t binding_index) const noexcept;
    /** @brief Indices into `Bindings()`. */
    [[nodiscard]] std::span<const uint32_t> ResourceList(uint32_t list_index) const noexcept;
    /** @brief Parallel to the resource list. */
    [[nodiscard]] std::span<const Footprint> FootprintList(uint32_t list_index) const noexcept;
    /** @brief Indices into the resource list. */
    [[nodiscard]] std::span<const uint32_t> VisibilityList(uint32_t list_index) const noexcept;
    [[nodiscard]] std::span<const UniformMember> UniformMembers(const Binding& binding) const noexcept;
    [[nodiscard]] std::span<const UniformMember> UniformMembers(const EntryPointConstants& constants) const noexcept;
    [[nodiscard]] std::span<const uint32_t> ResourceTags(const Binding& binding) const noexcept;
    [[nodiscard]] std::span<const EntryPointConstants> EntryPointConstantsTable() const noexcept;
    [[nodiscard]] std::span<const VertexInput> VertexInputs(uint32_t raster_index) const noexcept;
    [[nodiscard]] std::span<const ColorTarget> ColorTargets(uint32_t raster_index) const noexcept;
    [[nodiscard]] bool WritesFragDepth(uint32_t raster_index) const noexcept;
    [[nodiscard]] std::span<const SpecializationConstant> SpecializationConstants() const noexcept;
    [[nodiscard]] std::span<const SamplerTexturePair> SamplerPairList(uint32_t list_index) const noexcept;
    /** @brief True when the cook read the pairs of this profile's code. */
    [[nodiscard]] bool HasSamplerTexturePairs() const noexcept;
    /** @brief True when the cook read the capability names and workgroup memory of this profile's code. */
    [[nodiscard]] bool HasCodeFacts() const noexcept;
    /** @brief The device's support as a mask over `CapabilityNames()`: bit k is set when `supports` accepts name k.
     * Names are in the terms of the profile's target. Call it twice, as in Vulkan: with an empty `masks` it returns the
     * word count and writes nothing, then with a span of exactly that many words it fills them and returns the same
     * count. It never allocates. */
    uint32_t MakeDeviceCapabilities(CapabilitySupportFn supports, void* user, std::span<uint64_t> masks) const noexcept;

private:
    ModuleView module;
    uint32_t profileIndex{ 0u };
    std::span<const std::byte> extent;
    std::span<const VariantKey> variantKeys;
    std::span<const Variant> variants;
    std::span<const uint64_t> axisMasks;
    std::span<const uint64_t> axisRelevance;
    std::span<const uint64_t> footprintAxes;
    std::span<const uint64_t> sourceCapabilities;
    std::span<const uint32_t> capabilityNames;
    std::span<const EntryPointInstance> entryPointInstances;
    std::span<const SourceRef> sources;
    std::span<const char> sourceBlob;
    std::span<const Binding> bindings;
    std::span<const Run> resourceLists;
    std::span<const uint32_t> resourceIndices;
    std::span<const Footprint> footprints;
    std::span<const Run> footprintLists;
    std::span<const Run> visibilityLists;
    std::span<const uint32_t> visibilityIndices;
    std::span<const RasterState> rasterStates;
    std::span<const VertexInput> vertexInputs;
    std::span<const ColorTarget> colorTargets;
    std::span<const UniformMember> uniformMembers;
    std::span<const uint32_t> resourceTags;
    std::span<const SpecializationConstant> specializationConstants;
    std::span<const Run> samplerPairLists;
    std::span<const SamplerTexturePair> samplerTexturePairs;
    std::span<const EntryPointConstants> entryPointConstants;
};

/** @brief Serves code and bindings from one environment, with no allocation. Do not move the provider while a range
 * from `Bindings()` is in use. `Generation()` is the hot-reload hook: a live provider increments it on change. */
class ShaderSourceProvider
{
public:
    ShaderSourceProvider(EnvironmentView view, uint64_t generation) noexcept;

    [[nodiscard]] std::string_view Source(uint32_t entry_point,
                                          VariantKey variant) const noexcept;
    /** @brief Ready for `VkShaderModuleCreateInfo::pCode`. */
    [[nodiscard]] std::span<const uint32_t> SpirvWords(uint32_t entry_point,
                                                       VariantKey variant) const noexcept;
    [[nodiscard]] LayoutRange Bindings(uint32_t entry_point, VariantKey variant) const noexcept;
    [[nodiscard]] WorkgroupSize Workgroup(uint32_t entry_point,
                                          VariantKey variant) const noexcept;
    [[nodiscard]] uint64_t Generation() const noexcept;
    [[nodiscard]] const EnvironmentView& View() const noexcept;

private:
    EnvironmentView view;
    uint64_t generation{ 0u };
};

} // namespace lodestone::manifest

#endif // !LODESTONE_SHADER_MANIFEST_HPP
