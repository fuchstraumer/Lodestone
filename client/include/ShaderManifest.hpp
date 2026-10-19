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


/**
 * @brief A read-only view over one cook bundle, stored as a flat byte span.
 *
 * Every cross-reference in the file is an index or an offset, not a pointer. The header region uses
 * absolute 64-bit offsets, and environment extents use 32-bit offsets relative to their own start. This
 * lets the reader work as a set of spans over one byte span, so opening a file needs no allocation and
 * no pointer fixup.
 *
 * The byte span must outlive every view and every provider that reads it. Names and shader text point
 * into that span.
 */
namespace lodestone::manifest
{

/**
* @brief Spans over the header region of one cook bundle, checked once when it opens.
*
* The header region contains every whole-cook table and every module header: strings, axes, profiles, the
* environment directory, and each module's entry points and axis runs. Open() checks all of it, so every
* accessor after it runs unchecked (effectively meaning it's branch-free).
*
* The span given to Open() can contain the whole file, or only the header region. Renderers that read the
* header region first and then the extents of one profile open each extent with `EnvironmentView::Open`.
* Callers with the whole file in memory use `OpenEnvironment`.
* @note This stays purely in the vocabulary of the manifest itself. For the vocabulary of authorship,
* use `ManifestIndex`.
*
* todo: File-reading open implementation using a better fileread backend (not ifstream)
*/
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
    /** @brief The index of the module with this name, or -1 when no module has it. */
    [[nodiscard]] int32_t FindModule(std::string_view module_name) const noexcept;
    /** @brief The index of the first profile for this target, or -1 when no profile has it. */
    [[nodiscard]] int32_t FindProfile(std::string_view target_name) const noexcept;
    [[nodiscard]] const EnvironmentDirectoryEntry& Environment(uint32_t profile_index,
                                                               uint32_t module_index) const noexcept;
    /** @brief Opens one environment out of the span this view was opened with. That span must reach the
     * extent, so a view over the header region alone returns `EnvironmentExtentOutOfBounds`. */
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

/**
* @brief One module of a bundle: its name, its entry points, and its axes. Nothing here varies by profile.
*
* Module axes are views of one root axis through the module's value mask. Digit D of a variant key
* selects the D-th set bit of that mask, so the radix of the axis in this module is the count of set bits.
*/
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
    /** @brief The root axis record behind module axis `local_axis`. */
    [[nodiscard]] const Axis& AxisData(uint32_t local_axis) const noexcept;
    /** @brief How many values this module uses on one axis. This is also the radix of that axis. */
    [[nodiscard]] uint32_t AxisValueCount(uint32_t local_axis) const noexcept;
    [[nodiscard]] AxisValueType AxisValue(uint32_t local_axis, uint32_t digit) const noexcept;
    /** @brief Returns the digit of the default the shader declares for one axis. Graph code uses it to fill an
     * axis that it does not pin. `Open` checked that it is below `AxisValueCount(local_axis)`. */
    [[nodiscard]] uint32_t AxisDefaultDigit(uint32_t local_axis) const noexcept;
    /** @brief The 64-bit words in one variant's axis-active mask: `ceil(AxisCount() / 64)`. */
    [[nodiscard]] uint32_t AxisMaskWordCount() const noexcept;

private:
    friend class BundleView;
    ModuleView(const BundleView& _bundle, uint32_t module_index) noexcept;
    BundleView bundle;
    uint32_t moduleIndex{ 0u };
    std::span<const EntryPoint> entryPoints;
    std::span<const ModuleAxis> moduleAxes;
};

/**
* @brief One module, cooked for one profile: spans over one environment extent, checked once when it opens.
*
* Every offset inside an extent is relative to the extent start, so that each extent can be loaded into
* its own buffer.
* The variant keys, the variant records, and the axis masks are parallel: the position of a key is the
* index of its variant. The EntryPointInstance for (variant V, entry point E) is at `V * EntryPointCount + E`.
*/
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
    /** @brief The WGSL text of one source. Only for a `Wgsl` profile. */
    [[nodiscard]] std::string_view Source(uint32_t source_index) const noexcept;
    /** @brief The SPIR-V words of one source. Only for a `Spirv` profile. `Open` checked the alignment. */
    [[nodiscard]] std::span<const uint32_t> SpirvWords(uint32_t source_index) const noexcept;

    [[nodiscard]] std::span<const VariantKey> VariantKeys() const noexcept;
    [[nodiscard]] std::span<const Variant> Variants() const noexcept;
    /** @brief The index of the variant with this key, or no value when this environment did not cook it. */
    [[nodiscard]] std::optional<uint32_t> FindVariant(VariantKey key) const noexcept;
    /** @brief The variant with this key, or no value when this environment did not cook it. */
    [[nodiscard]] std::optional<VariantView> VariantByKey(VariantKey key) const noexcept;
    [[nodiscard]] VariantView VariantAt(uint32_t variant_index) const noexcept;
    [[nodiscard]] std::span<const uint64_t> AxisMask(uint32_t variant_index) const noexcept;
    [[nodiscard]] bool IsAxisActive(uint32_t variant_index, uint32_t local_axis) const noexcept;
    /** @brief The axis relevance mask of one entry point: bit k is set when axis k can change that entry point's
     * instance in this environment. An axis with only one cooked value counts as relevant. */
    [[nodiscard]] std::span<const uint64_t> AxisRelevance(uint32_t entry_point_index) const noexcept;
    [[nodiscard]] bool IsAxisRelevant(uint32_t entry_point_index, uint32_t local_axis) const noexcept;

    [[nodiscard]] std::span<const EntryPointInstance> EntryPointInstanceTable() const noexcept;
    [[nodiscard]] std::span<const EntryPointInstance> VariantEntryPoints(uint32_t variant_index) const noexcept;
    /** @brief The entry-point specific information for one entry point of one variant, or null when this
     * environment did not cook the key. */
    [[nodiscard]] const EntryPointInstance* FindEntryPointInstance(uint32_t entry_point, VariantKey variant) const noexcept;

    [[nodiscard]] std::span<const Binding> Bindings() const noexcept;
    [[nodiscard]] const Binding& BindingRecord(uint32_t binding_index) const noexcept;
    /** @brief The resources one variant declares. Indices into Bindings(). */
    [[nodiscard]] std::span<const uint32_t> ResourceList(uint32_t list_index) const noexcept;
    /** @brief The footprint of each resource, in the same order as the resource list. */
    [[nodiscard]] std::span<const Footprint> FootprintList(uint32_t list_index) const noexcept;
    /** @brief Which of a variant's resources one entry point reads. Indices into the resource list. */
    [[nodiscard]] std::span<const uint32_t> VisibilityList(uint32_t list_index) const noexcept;
    [[nodiscard]] std::span<const UniformMember> UniformMembers(const Binding& binding) const noexcept;
    [[nodiscard]] std::span<const VertexInput> VertexInputs(uint32_t raster_index) const noexcept;
    [[nodiscard]] std::span<const ColorTarget> ColorTargets(uint32_t raster_index) const noexcept;
    [[nodiscard]] bool WritesFragDepth(uint32_t raster_index) const noexcept;
    [[nodiscard]] std::span<const SpecializationConstant> SpecializationConstants() const noexcept;
    /** @brief One sampler and texture pair list. Positions in the visibility list of an entry point instance. */
    [[nodiscard]] std::span<const SamplerTexturePair> SamplerPairList(uint32_t list_index) const noexcept;
    /** @brief True when the cook read the sampler and texture pairs of this profile's code. Does not imply
      * that no entry points read textures, just answers if the cook recorded the sampler and texture pairs. */
    [[nodiscard]] bool HasSamplerTexturePairs() const noexcept;

private:
    ModuleView module;
    uint32_t profileIndex{ 0u };
    std::span<const std::byte> extent;
    std::span<const VariantKey> variantKeys;
    std::span<const Variant> variants;
    std::span<const uint64_t> axisMasks;
    std::span<const uint64_t> axisRelevance;
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
    std::span<const SpecializationConstant> specializationConstants;
    std::span<const Run> samplerPairLists;
    std::span<const SamplerTexturePair> samplerTexturePairs;
};

/**
 * @brief Serves shader sources out of one environment of a manifest.
 *
 * The provider does not allocate. Each accessor reads the environment when called. `Bindings()` returns
 * a range that points into this provider, so do not move the provider while a range from it is in use.
 *
 * `Generation()` is the future hot-reload hook. A provider for baked data will always return the same value,
 * but a live provider can increment the value when any source changes - allowing users to reload
 * shaders and reset state gracefully
 */
class ShaderSourceProvider
{
public:
    ShaderSourceProvider(EnvironmentView view, uint64_t generation) noexcept;

    /** @brief The WGSL text. Only for a `Wgsl` profile. */
    [[nodiscard]] std::string_view Source(uint32_t entry_point,
                                          VariantKey variant) const noexcept;
    /** @brief The SPIR-V words, ready for `VkShaderModuleCreateInfo::pCode`. Only for a `Spirv` profile. */
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
