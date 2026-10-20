#include "ShaderManifestValidator.hpp"
#include "ShaderManifestFormat.hpp"
#include "ShaderManifestRead.hpp"
#include "ShaderManifest.hpp"
#include "ShaderLibraryTypes.hpp"
#include "VariantKey.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <span>
#include <tuple>
#include <utility>

namespace lodestone::manifest::detail
{

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

namespace
{

    /** One table to bounds-check: `Count` records of `RecordSize` bytes, starting at `Offset`. */
    struct Section64
    {
        ShaderManifestTable Table{ ShaderManifestTable::Invalid };
        TableRef64 Loc{ 0u, 0u };
        size_t RecordSize{ 0u };
    };

    struct Section
    {
        ShaderManifestTable Table{ ShaderManifestTable::Invalid };
        TableRef Loc{ 0u, 0u };
        size_t RecordSize{ 0u };
    };

    using BundleValidator = ErrorState (*)(const Header&, std::span<const std::byte>) noexcept;
    using EnvironmentValidator = ErrorState (*)(const EnvironmentContext&) noexcept;

    /** True when a table of `count` records of `record_size` bytes starts at `offset` and stays inside a
     * region of `region_size` bytes. An empty table at any offset is in bounds. */
    bool TableIsInBounds(TableRef loc, size_t record_size, uint64_t region_size) noexcept;
    bool TableIsInBounds(TableRef64 loc, size_t record_size, uint64_t region_size) noexcept;
    /** True when a grid of `rows * columns` records fits a region, checked without an overflow. */
    bool GridFitsRegion(uint64_t rows, uint64_t columns, size_t record_size, uint64_t region_size) noexcept;
    ErrorState ValidateSections(std::span<const Section> sections, uint64_t region_size) noexcept;
    ErrorState ValidateSections(std::span<const Section64> sections, uint64_t region_size) noexcept;

    ErrorState ValidateBundleHeader(const Header& parsed, size_t span_size) noexcept;
    ErrorState ValidateBundleSections(const Header& parsed, std::span<const std::byte> bytes) noexcept;
    ErrorState ValidateStrings(const Header& parsed, std::span<const std::byte> bytes) noexcept;
    ErrorState ValidateAxes(const Header& parsed, std::span<const std::byte> bytes) noexcept;
    ErrorState ValidateProfiles(const Header& parsed, std::span<const std::byte> bytes) noexcept;
    ErrorState ValidateModuleHeaders(const Header& parsed, std::span<const std::byte> bytes) noexcept;
    ErrorState ValidateEntryPoints(const Header& parsed, std::span<const std::byte> bytes) noexcept;
    ErrorState ValidateModuleAxes(const Header& parsed, std::span<const std::byte> bytes) noexcept;
    ErrorState ValidateDirectory(const Header& parsed, std::span<const std::byte> bytes) noexcept;

    ErrorState ValidateEnvironmentSections(const EnvironmentContext& context) noexcept;
    ErrorState ValidateSources(const EnvironmentContext& context) noexcept;
    ErrorState ValidateBindings(const EnvironmentContext& context) noexcept;
    ErrorState ValidateEntryPointConstants(const EnvironmentContext& context) noexcept;
    ErrorState ValidateResourceIndices(const EnvironmentContext& context) noexcept;
    ErrorState ValidateRunTables(const EnvironmentContext& context) noexcept;
    ErrorState ValidateSlots(const EnvironmentContext& context) noexcept;
    ErrorState ValidateVariantKeys(const EnvironmentContext& context) noexcept;
    ErrorState ValidateVariants(const EnvironmentContext& context) noexcept;
    ErrorState ValidateAxisMasks(const EnvironmentContext& context) noexcept;
    ErrorState ValidateAxisRelevance(const EnvironmentContext& context) noexcept;
    ErrorState ValidateRasterStates(const EnvironmentContext& context) noexcept;
    ErrorState ValidateVertexInputs(const EnvironmentContext& context) noexcept;
    ErrorState ValidateUniformMembers(const EnvironmentContext& context) noexcept;
    ErrorState ValidateSpecializationConstants(const EnvironmentContext& context) noexcept;

    // Yes, I know how branchy all of this is. But we pay it once when opening the manifest and then there's
    // no branches during actual queries, and the error type here can hold rich information that makes
    // debugging easier. This is mainly intended to catch errors in the manifest file itself, like corruption
    // or truncation: a rare issue, but a huge pain to debug if no infrastructure exists for catching it.
    // You should read this code as "I once spent a weekend debugging corrupted shader variants when trying
    // to ship a game, and it messed me up so bad I'm willing to write this BS"
    // The order matters: each validator may read what the validators before it proved in bounds.
    constexpr std::array<BundleValidator, 8u> k_BundleValidators{
        &ValidateBundleSections, &ValidateStrings,      &ValidateAxes,       &ValidateProfiles,
        &ValidateModuleHeaders,  &ValidateEntryPoints,  &ValidateModuleAxes, &ValidateDirectory,
    };

    constexpr std::array<EnvironmentValidator, 15u> k_EnvironmentValidators{
        &ValidateEnvironmentSections, &ValidateSources,      &ValidateBindings,
        &ValidateEntryPointConstants,
        &ValidateResourceIndices,     &ValidateRunTables,    &ValidateSlots,
        &ValidateVariantKeys,         &ValidateVariants,     &ValidateAxisMasks,
        &ValidateAxisRelevance,       &ValidateRasterStates, &ValidateVertexInputs,
        &ValidateUniformMembers,      &ValidateSpecializationConstants,
    };

    bool TableIsInBounds(TableRef loc, size_t record_size, uint64_t region_size) noexcept
    {
        if (loc.Count == 0u)
        {
            return true;
        }

        // divide first, so a huge count cannot overflow the multiply
        if (loc.Count > region_size / record_size)
        {
            return false;
        }

        const uint64_t span = loc.Count * record_size;
        return loc.Offset <= region_size && span <= region_size - loc.Offset;
    }

    bool TableIsInBounds(TableRef64 loc, size_t record_size, uint64_t region_size) noexcept
    {
        if (loc.Count == 0u)
        {
            return true;
        }

        // divide first, so a huge count cannot overflow the multiply
        if (loc.Count > region_size / record_size)
        {
            return false;
        }

        const uint64_t span = loc.Count * record_size;
        return loc.Offset <= region_size && span <= region_size - loc.Offset;
    }

    bool GridFitsRegion(uint64_t rows, uint64_t columns, size_t record_size, uint64_t region_size) noexcept
    {
        if (rows == 0u || columns == 0u)
        {
            return true;
        }

        return rows <= (region_size / record_size) / columns;
    }

    ErrorState ValidateSections(std::span<const Section> sections, uint64_t region_size) noexcept
    {
        for (const Section& section : sections)
        {
            if (!TableIsInBounds(section.Loc, section.RecordSize, region_size))
            {
                return { .Code = ErrorCode::SectionOutOfBounds, .Table = section.Table };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateSections(std::span<const Section64> sections, uint64_t region_size) noexcept
    {
        for (const Section64& section : sections)
        {
            if (!TableIsInBounds(section.Loc, section.RecordSize, region_size))
            {
                return { .Code = ErrorCode::SectionOutOfBounds, .Table = section.Table };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateBundleHeader(const Header& parsed, size_t span_size) noexcept
    {
        if (parsed.Magic != k_ShaderManifestMagic)
        {
            return { .Code = ErrorCode::BadMagic };
        }

        if (parsed.Version != k_ShaderManifestVersion)
        {
            return { .Code = ErrorCode::VersionMismatch, .Detail = parsed.Version };
        }

        if (parsed.HeaderSize < sizeof(Header) || (parsed.HeaderSize % 8u) != 0u ||
            parsed.HeaderSize > parsed.FileSize)
        {
            return { .Code = ErrorCode::SizeMismatch, .Detail = static_cast<uint32_t>(parsed.HeaderSize) };
        }

        // the span can contain only the header region, or the whole file. anything past the file is invalid
        if (span_size < parsed.HeaderSize)
        {
            return { .Code = ErrorCode::TooSmall, .Detail = static_cast<uint32_t>(span_size) };
        }

        if (span_size > parsed.FileSize)
        {
            return { .Code = ErrorCode::SizeMismatch, .Detail = static_cast<uint32_t>(parsed.FileSize) };
        }

        return k_ManifestOk;
    }

    //NOLINTBEGIN(modernize-use-designated-initializers)
    ErrorState ValidateBundleSections(const Header& parsed, [[maybe_unused]] std::span<const std::byte> bytes) noexcept
    {
        // every whole-cook table lives inside the header region, so a reader of that region alone can trust it
        const TableRef64 moduleLoc{ sizeof(Header), parsed.ModuleCount };
        const std::array<Section64, 6u> sections
        {
            Section64{ ShaderManifestTable::Modules, moduleLoc, sizeof(ModuleRootHeader) },
            Section64{ ShaderManifestTable::Profiles, parsed.Profiles, sizeof(Profile) },
            Section64{ ShaderManifestTable::Strings, parsed.Strings, sizeof(StringRef) },
            Section64{ ShaderManifestTable::Strings, parsed.StringBlobs, 1u },
            Section64{ ShaderManifestTable::Axes, parsed.Axes, sizeof(Axis) },
            Section64{ ShaderManifestTable::AxisValues, parsed.AxesValues, sizeof(AxisValueType) },
        };

        const ErrorState sectionsValid = ValidateSections(sections, parsed.HeaderSize);
        if (!sectionsValid)
        {
            return sectionsValid;
        }

        // the directory is a grid, so check the product without overflowing it
        const bool gridFits = GridFitsRegion(parsed.Profiles.Count,
                                             parsed.ModuleCount,
                                             sizeof(EnvironmentDirectoryEntry),
                                             parsed.HeaderSize);
        const TableRef64 envLoc{ parsed.EnvironmentDirectoryOffset, parsed.Profiles.Count * parsed.ModuleCount };
        const bool tableInBounds = TableIsInBounds(envLoc,
                                                   sizeof(EnvironmentDirectoryEntry),
                                                   parsed.HeaderSize);
        if (!gridFits || !tableInBounds)
        {
            return { .Code = ErrorCode::SectionOutOfBounds, .Table = ShaderManifestTable::Environments };
        }

        return k_ManifestOk;
    }
    //NOLINTEND(modernize-use-designated-initializers)

    ErrorState ValidateStrings(const Header& parsed, std::span<const std::byte> bytes) noexcept
    {
        const std::span<const StringRef> stringSpan = Map<StringRef>(bytes, parsed.Strings);
        for (int32_t i = 0; i < std::ssize(stringSpan); ++i)
        {
            const StringRef& reference = stringSpan[i];
            if (reference.Length > parsed.StringBlobs.Count || reference.Offset > parsed.StringBlobs.Count - reference.Length)
            {
                return { .Code = ErrorCode::StringOutOfBounds,
                         .Table = ShaderManifestTable::Strings,
                         .RecordIndex = static_cast<uint32_t>(i) };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateAxes(const Header& parsed, std::span<const std::byte> bytes) noexcept
    {
        // each axis stores a name string and a run in the axis value table. literal values reference
        // nothing, so we only check the name, the run, and named values
        const std::span<const Axis> axisSpan = Map<Axis>(bytes, parsed.Axes);
        const std::span<const AxisValueType> allAxesValueSpan = Map<AxisValueType>(bytes, parsed.AxesValues);
        for (int32_t i = 0; i < std::ssize(axisSpan); ++i)
        {
            const Axis& axis = axisSpan[i];
            if (axis.NameString >= parsed.Strings.Count)
            {
                return { .Code = ErrorCode::InvalidAxisName,
                         .Table = ShaderManifestTable::Axes,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = axis.NameString };
            }

            if (static_cast<uint64_t>(axis.FirstValue) + static_cast<uint64_t>(axis.ValueCount) > parsed.AxesValues.Count)
            {
                return { .Code = ErrorCode::InvalidAxisValueRange,
                         .Table = ShaderManifestTable::Axes,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = axis.ValueCount };
            }

            // both types and enums have values stored in string blob, verify integrity of the string indices
            if (axis.Domain != AxisValueDomain::Type && axis.Domain != AxisValueDomain::Enum)
            {
                continue;
            }

            for (const AxisValueType value : allAxesValueSpan.subspan(axis.FirstValue, axis.ValueCount))
            {
                if (value >= parsed.Strings.Count)
                {
                    return { .Code = ErrorCode::InvalidAxisTypeStrIndex,
                             .Table = ShaderManifestTable::Axes,
                             .RecordIndex = static_cast<uint32_t>(i),
                             .Detail = value };
                }
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateProfiles(const Header& parsed, std::span<const std::byte> bytes) noexcept
    {
        const std::span<const Profile> profileSpan = Map<Profile>(bytes, parsed.Profiles);
        for (int32_t i = 0; i < std::ssize(profileSpan); ++i)
        {
            const Profile& profile = profileSpan[i];
            if (profile.TargetNameString >= parsed.Strings.Count)
            {
                return { .Code = ErrorCode::InvalidProfileTargetName,
                         .Table = ShaderManifestTable::Profiles,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = profile.TargetNameString };
            }

            if (profile.AccessModel < PlacementKind::Bound || profile.AccessModel > PlacementKind::Pointer)
            {
                return { .Code = ErrorCode::InvalidProfileAccessModel,
                         .Table = ShaderManifestTable::Profiles,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = static_cast<uint32_t>(profile.AccessModel) };
            }

            if (profile.CodeFormat < ShaderCodeFormat::Wgsl || profile.CodeFormat > ShaderCodeFormat::Spirv)
            {
                return { .Code = ErrorCode::InvalidProfileCodeFormat,
                         .Table = ShaderManifestTable::Profiles,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = static_cast<uint32_t>(profile.CodeFormat) };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateModuleHeaders(const Header& parsed, std::span<const std::byte> bytes) noexcept
    {
        const TableRef64 modulesLoc{ sizeof(Header), parsed.ModuleCount };
        const std::span<const ModuleRootHeader> moduleSpan = Map<ModuleRootHeader>(bytes, modulesLoc);
        for (int32_t i = 0; i < std::ssize(moduleSpan); ++i)
        {
            const ModuleRootHeader& moduleHeader = moduleSpan[i];
            if (moduleHeader.ModuleNameString >= parsed.Strings.Count)
            {
                return { .Code = ErrorCode::InvalidModuleNameString,
                         .Table = ShaderManifestTable::Modules,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = moduleHeader.ModuleNameString };
            }

            if (!TableIsInBounds(moduleHeader.EntryPoints,
                                 sizeof(EntryPoint),
                                 parsed.HeaderSize))
            {
                return { .Code = ErrorCode::SectionOutOfBounds,
                         .Table = ShaderManifestTable::EntryPoints,
                         .RecordIndex = static_cast<uint32_t>(i) };
            }

            if (!TableIsInBounds(moduleHeader.ModuleAxes,
                                 sizeof(ModuleAxis),
                                 parsed.HeaderSize))
            {
                return { .Code = ErrorCode::SectionOutOfBounds,
                         .Table = ShaderManifestTable::ModuleAxes,
                         .RecordIndex = static_cast<uint32_t>(i) };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateEntryPoints(const Header& parsed, std::span<const std::byte> bytes) noexcept
    {
        const TableRef64 modulesLoc{ sizeof(Header), parsed.ModuleCount };
        for (const ModuleRootHeader& moduleHeader : Map<ModuleRootHeader>(bytes, modulesLoc))
        {
            const std::span<const EntryPoint> entryPoints = Map<EntryPoint>(bytes, moduleHeader.EntryPoints);
            for (int32_t i = 0; i < std::ssize(entryPoints); ++i)
            {
                const EntryPoint& entryPoint = entryPoints[i];
                if (entryPoint.NameString >= parsed.Strings.Count)
                {
                    return { .Code = ErrorCode::EntryPointInvalidName,
                             .Table = ShaderManifestTable::EntryPoints,
                             .RecordIndex = static_cast<uint32_t>(i),
                             .Detail = entryPoint.NameString };
                }

                // a stage is one bit of ShaderStageKind. A merged layout ORs stages into one mask, so a value
                // with two bits would read as two stages
                if (entryPoint.Stage >= static_cast<uint32_t>(ShaderStageKind::Count) ||
                    !std::has_single_bit(entryPoint.Stage))
                {
                    return { .Code = ErrorCode::EntryPointInvalidStage,
                             .Table = ShaderManifestTable::EntryPoints,
                             .RecordIndex = static_cast<uint32_t>(i),
                             .Detail = entryPoint.Stage };
                }
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateModuleAxes(const Header& parsed, std::span<const std::byte> bytes) noexcept
    {
        const std::span<const Axis> axisSpan = Map<Axis>(bytes, parsed.Axes);
        const TableRef64 modulesLoc{ sizeof(Header), parsed.ModuleCount };
        for (const ModuleRootHeader& moduleHeader : Map<ModuleRootHeader>(bytes, modulesLoc))
        {
            const std::span<const ModuleAxis> moduleAxisSpan = Map<ModuleAxis>(bytes, moduleHeader.ModuleAxes);
            for (int32_t i = 0u; i < std::ssize(moduleAxisSpan); ++i)
            {
                const ModuleAxis& moduleAxis = moduleAxisSpan[i];
                if (moduleAxis.AxisIndex >= axisSpan.size())
                {
                    return { .Code = ErrorCode::InvalidModuleAxisIndex,
                             .Table = ShaderManifestTable::ModuleAxes,
                             .RecordIndex = static_cast<uint32_t>(i),
                             .Detail = moduleAxis.AxisIndex };
                }

                // an empty mask would give the axis a radix of zero, and a bit past the root values selects nothing
                const uint32_t valueCount = axisSpan[moduleAxis.AxisIndex].ValueCount;
                const bool maskPastValues = valueCount < 32u && (moduleAxis.LiveValuesMask >> valueCount) != 0u;
                if (moduleAxis.LiveValuesMask == 0u || maskPastValues)
                {
                    return { .Code = ErrorCode::InvalidModuleAxisValueMask,
                             .Table = ShaderManifestTable::ModuleAxes,
                             .RecordIndex = static_cast<uint32_t>(i),
                             .Detail = moduleAxis.LiveValuesMask };
                }

                if (moduleAxis.DefaultDigit >= static_cast<uint32_t>(std::popcount(moduleAxis.LiveValuesMask)))
                {
                    return { .Code = ErrorCode::InvalidModuleAxisDefault,
                             .Table = ShaderManifestTable::ModuleAxes,
                             .RecordIndex = static_cast<uint32_t>(i),
                             .Detail = moduleAxis.DefaultDigit };
                }
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateDirectory(const Header& parsed, std::span<const std::byte> bytes) noexcept
    {
        const uint64_t dirCount = parsed.Profiles.Count * parsed.ModuleCount;
        const TableRef64 directoryLoc{ parsed.EnvironmentDirectoryOffset, dirCount };
        const std::span<const EnvironmentDirectoryEntry> directorySpan =
            Map<EnvironmentDirectoryEntry>(bytes, directoryLoc);
        for (int32_t i = 0; i < std::ssize(directorySpan); ++i)
        {
            const EnvironmentDirectoryEntry& entry = directorySpan[i];
            if (entry.ExtentSize == 0u)
            {
                continue;
            }

            // extents start past the header region, on an 8-byte boundary, and stay inside the file. we check
            // against the file size from the header, because the span may contain only the header region
            const bool valid = entry.ExtentOffset >= parsed.HeaderSize && (entry.ExtentOffset % 8u) == 0u &&
                               entry.ExtentSize >= sizeof(EnvironmentHeader) &&
                               entry.ExtentOffset <= parsed.FileSize &&
                               entry.ExtentSize <= parsed.FileSize - entry.ExtentOffset;
            if (!valid)
            {
                return { .Code = ErrorCode::EnvironmentExtentOutOfBounds,
                         .Table = ShaderManifestTable::Environments,
                         .RecordIndex = static_cast<uint32_t>(i) };
            }
        }

        return k_ManifestOk;
    }

    //NOLINTBEGIN(modernize-use-designated-initializers)
    ErrorState ValidateEnvironmentSections(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const uint64_t extentSize = context.Extent.size();
        const bool validGrid = GridFitsRegion(environment.VariantCount,
                                              context.EntryPointCount,
                                              sizeof(EntryPointInstance),
                                              extentSize);
        const TableRef64 slotTableLoc{ environment.EntryPointInstanceTableOffset,
                                       static_cast<uint64_t>(environment.VariantCount) * context.EntryPointCount };
        const bool tableInBounds = TableIsInBounds(slotTableLoc,
                                                   sizeof(EntryPointInstance),
                                                   extentSize);

        if (!validGrid || !tableInBounds)
        {
            return { .Code = ErrorCode::EntryPointInstanceGridSizeMismatch,
                     .Table = ShaderManifestTable::EntryPointInstances,
                     .Detail = environment.VariantCount };
        }

        const TableRef varKeysLoc{ environment.VariantKeyTableOffset, environment.VariantCount };
        const TableRef varsLoc{ environment.VariantTableOffset, environment.VariantCount };
        const TableRef axisMasksLoc{ environment.AxisMaskTableOffset,
                                     environment.VariantCount * context.AxisMaskWordCount };
        const TableRef axisRelevanceLoc{ environment.AxisRelevanceTableOffset,
                                         static_cast<uint32_t>(context.EntryPointCount) * context.AxisMaskWordCount };
        const std::array<Section, 21u> sections
        {
            Section{ ShaderManifestTable::AxisRelevance, axisRelevanceLoc, sizeof(uint64_t) },
            Section{ ShaderManifestTable::VariantKeys, varKeysLoc, sizeof(VariantKey) },
            Section{ ShaderManifestTable::Variants, varsLoc, sizeof(Variant) },
            Section{ ShaderManifestTable::AxisMasks, axisMasksLoc, sizeof(uint64_t) },
            Section{ ShaderManifestTable::Sources, environment.Sources, sizeof(SourceRef) },
            Section{ ShaderManifestTable::Sources, environment.Sources, 1u },
            Section{ ShaderManifestTable::Bindings, environment.Bindings, sizeof(Binding) },
            Section{ ShaderManifestTable::ResourceLists, environment.ResourceLists, sizeof(Run) },
            Section{ ShaderManifestTable::ResourceIndices, environment.ResourceIndices, sizeof(uint32_t) },
            Section{ ShaderManifestTable::Footprints, environment.Footprints, sizeof(Footprint) },
            Section{ ShaderManifestTable::FootprintLists, environment.FootprintLists, sizeof(Run) },
            Section{ ShaderManifestTable::VisibilityLists, environment.VisibilityLists, sizeof(Run) },
            Section{ ShaderManifestTable::VisibilityIndices, environment.VisibilityIndices, sizeof(uint32_t) },
            Section{ ShaderManifestTable::Rasters, environment.Rasters, sizeof(RasterState) },
            Section{ ShaderManifestTable::VertexInputs, environment.VertexInputs, sizeof(VertexInput) },
            Section{ ShaderManifestTable::ColorTargets, environment.ColorTargets, sizeof(ColorTarget) },
            Section{ ShaderManifestTable::UniformMembers, environment.UniformMembers, sizeof(UniformMember) },
            Section{ ShaderManifestTable::SpecializationConstants, environment.SpecConstants, sizeof(SpecializationConstant) },
            Section{ ShaderManifestTable::SamplerPairLists, environment.SamplerPairLists, sizeof(Run) },
            Section{ ShaderManifestTable::SamplerTexturePairs, environment.SamplerTexturePairs, sizeof(SamplerTexturePair) },
            Section{ ShaderManifestTable::EntryPointConstants, environment.EntryPointConstants, sizeof(EntryPointConstants) },
        };

        return ValidateSections(sections, extentSize);
    }
    //NOLINTEND(modernize-use-designated-initializers)

    ErrorState ValidateSources(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const SourceRef> sourceSpan = Map<SourceRef>(context.Extent, environment.Sources);
        // SPIR-V sources are read as words, so the blob, each offset, and each length must be multiples of 4
        const bool readAsWords = context.CodeFormat == ShaderCodeFormat::Spirv;
        if (readAsWords && (environment.SourceBlob.Offset % sizeof(uint32_t)) != 0u)
        {
            return ErrorState{ .Code = ErrorCode::SpirvSourceMisaligned,
                               .Table = ShaderManifestTable::Sources,
                               .Detail = environment.SourceBlob.Offset };
        }

        for (int32_t i = 0; i < std::ssize(sourceSpan); ++i)
        {
            const SourceRef& reference = sourceSpan[i];
            if (reference.Length > environment.SourceBlob.Count ||
                reference.Offset > environment.SourceBlob.Count - reference.Length)
            {
                return ErrorState{ .Code = ErrorCode::SourceOutOfBounds,
                                   .Table = ShaderManifestTable::Sources,
                                   .RecordIndex = static_cast<uint32_t>(i) };
            }

            if (readAsWords &&
                ((reference.Offset % sizeof(uint32_t)) != 0u || (reference.Length % sizeof(uint32_t)) != 0u))
            {
                return ErrorState{ .Code = ErrorCode::SpirvSourceMisaligned,
                                   .Table = ShaderManifestTable::Sources,
                                   .RecordIndex = static_cast<uint32_t>(i) };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateBindings(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const Binding> bindingSpan = Map<Binding>(context.Extent, environment.Bindings);
        for (int32_t i = 0; i < std::ssize(bindingSpan); ++i)
        {
            const Binding& binding = bindingSpan[i];
            if (binding.NameString >= context.StringCount || binding.ScopeString >= context.StringCount)
            {
                return { .Code = ErrorCode::ManifestBindingInvalidName,
                         .Table = ShaderManifestTable::Bindings,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = binding.NameString };
            }

            if (binding.FirstUniformMember > environment.UniformMembers.Count ||
                binding.UniformMemberCount > environment.UniformMembers.Count - binding.FirstUniformMember)
            {
                return { .Code = ErrorCode::ManifestBindingInvalidUniforms,
                         .Table = ShaderManifestTable::Bindings,
                         .RecordIndex = static_cast<uint32_t>(i) };
            }
        }

        return k_ManifestOk;
    }

    /** Checks each entry point constants record alone: a known delivery, a member run inside the member table, no
     * position on a push constant block, and no data for the delivery `None`. `ValidateVariants` checks the
     * position of a uniform buffer, because a position is local to one instance. */
    ErrorState ValidateEntryPointConstants(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const EntryPointConstants> constantsSpan =
            Map<EntryPointConstants>(context.Extent, environment.EntryPointConstants);
        for (int32_t i = 0; i < std::ssize(constantsSpan); ++i)
        {
            const EntryPointConstants& constants = constantsSpan[i];
            const bool knownDelivery =
                constants.Delivery <= std::to_underlying(EntryPointConstantsDelivery::PushConstant);
            const bool membersInRange =
                constants.FirstUniformMember <= environment.UniformMembers.Count &&
                constants.UniformMemberCount <= environment.UniformMembers.Count - constants.FirstUniformMember;
            const bool hasPosition = constants.Delivery == std::to_underlying(EntryPointConstantsDelivery::UniformBuffer);
            const bool isNone = constants.Delivery == std::to_underlying(EntryPointConstantsDelivery::None);
            const bool emptyWhenNone = !isNone || (constants.ByteSize == 0u && constants.UniformMemberCount == 0u);
            if (!knownDelivery || !membersInRange || (!hasPosition && constants.Binding != 0u) || !emptyWhenNone)
            {
                return { .Code = ErrorCode::InvalidEntryPointConstants,
                         .Table = ShaderManifestTable::EntryPointConstants,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = constants.Delivery };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateResourceIndices(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const uint32_t> resourceIndexList =
            Map<uint32_t>(context.Extent, environment.ResourceIndices);
        for (int32_t i = 0; i < std::ssize(resourceIndexList); ++i)
        {
            if (resourceIndexList[i] >= environment.Bindings.Count)
            {
                return { .Code = ErrorCode::InvalidResourceBindingIndex,
                         .Table = ShaderManifestTable::ResourceIndices,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = resourceIndexList[i] };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateRunTables(const EnvironmentContext& context) noexcept
    {
        struct RunTable
        {
            ShaderManifestTable Table;
            ErrorCode Code;
            TableRef Loc;
            uint32_t PayloadCount;
        };

        const EnvironmentHeader& environment = context.Environment;
        const std::array<RunTable, 4u> runTables
        {
            RunTable{ ShaderManifestTable::ResourceLists,
                          ErrorCode::InvalidResourceListRun,
                          environment.ResourceLists,
                          environment.ResourceIndices.Count },
            RunTable{ ShaderManifestTable::FootprintLists,
                          ErrorCode::InvalidFootprintListRun,
                          environment.FootprintLists,
                          environment.Footprints.Count },
            RunTable{ ShaderManifestTable::VisibilityLists,
                          ErrorCode::InvalidVisibilityListRun,
                          environment.VisibilityLists,
                          environment.VisibilityIndices.Count },
            RunTable{ ShaderManifestTable::SamplerPairLists,
                          ErrorCode::InvalidSamplerPairListRun,
                          environment.SamplerPairLists,
                          environment.SamplerTexturePairs.Count },
        };

        for (const RunTable& runTable : runTables)
        {
            const std::span<const Run> runSpan = Map<Run>(context.Extent, runTable.Loc);
            for (int32_t i = 0; i < std::ssize(runSpan); ++i)
            {
                // <= on the sum for the edge case: an empty trailing list starts at the payload count
                const Run& run = runSpan[i];
                if (static_cast<uint64_t>(run.First) + static_cast<uint64_t>(run.Count) > runTable.PayloadCount)
                {
                    return ErrorState{ .Code = runTable.Code,
                                       .Table = runTable.Table,
                                       .RecordIndex = static_cast<uint32_t>(i) };
                }
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateSlots(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const uint64_t numSlots = context.EntryPointCount * environment.VariantCount;
        const TableRef64 slotsLoc{ environment.EntryPointInstanceTableOffset, numSlots };
        const std::span<const EntryPointInstance> slotSpan = Map<EntryPointInstance>(context.Extent,
                                                                                     slotsLoc);
        for (int32_t i = 0u; i < std::ssize(slotSpan); ++i)
        {
            const EntryPointInstance& slot = slotSpan[i];
            if (slot.Source >= environment.Sources.Count)
            {
                return { .Code = ErrorCode::InvalidEntryPointInstanceSourceIndex,
                         .Table = ShaderManifestTable::EntryPointInstances,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = slot.Source };
            }

            if (slot.Visibility >= environment.VisibilityLists.Count)
            {
                return { .Code = ErrorCode::InvalidEntryPointInstanceVisibilityIndex,
                         .Table = ShaderManifestTable::EntryPointInstances,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = slot.Visibility };
            }

            if (slot.Raster >= environment.Rasters.Count)
            {
                return { .Code = ErrorCode::InvalidEntryPointInstanceRasterIndex,
                         .Table = ShaderManifestTable::EntryPointInstances,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = slot.Raster };
            }

            if (slot.SamplerPairs >= environment.SamplerPairLists.Count)
            {
                return { .Code = ErrorCode::InvalidEntryPointInstanceSamplerPairIndex,
                         .Table = ShaderManifestTable::EntryPointInstances,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = slot.SamplerPairs };
            }

            if (slot.Constants >= environment.EntryPointConstants.Count)
            {
                return { .Code = ErrorCode::InvalidEntryPointInstanceConstantsIndex,
                         .Table = ShaderManifestTable::EntryPointInstances,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = slot.Constants };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateVariantKeys(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const TableRef varKeysLoc{ environment.VariantKeyTableOffset, environment.VariantCount };
        const std::span<const VariantKey> variantKeySpan = Map<VariantKey>(context.Extent, varKeysLoc);
        // adjacent_find lets us combine is_sorted and the uniqueness check
        const auto outOfOrder = std::ranges::adjacent_find(variantKeySpan, std::greater_equal<VariantKey>{});
        if (outOfOrder != variantKeySpan.end())
        {
            return { .Code = ErrorCode::InvalidVariantKeyOrder,
                     .Table = ShaderManifestTable::VariantKeys,
                     .RecordIndex = static_cast<uint32_t>(std::distance(variantKeySpan.begin(), outOfOrder)) };
        }

        return k_ManifestOk;
    }

    ErrorState ValidateVariants(const EnvironmentContext& context) noexcept
    {
        // the most complex and annoying pass to handle, but the most beneficial: after this, all
        // cross-references between variants, resource lists, and visibility lists are validated
        // so that all accessors can run unchecked
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const std::byte> extent = context.Extent;
        const TableRef variantsLoc{ environment.VariantTableOffset, environment.VariantCount };
        const std::span<const Variant> variantSpan = Map<Variant>(extent, variantsLoc);
        const std::span<const Run> resourceLists = Map<Run>(extent, environment.ResourceLists);
        const std::span<const uint32_t> resourceIndexList = Map<uint32_t>(extent, environment.ResourceIndices);
        const std::span<const Run> visibilityLists = Map<Run>(extent, environment.VisibilityLists);
        const std::span<const uint32_t> visibilityIndices = Map<uint32_t>(extent, environment.VisibilityIndices);
        const std::span<const Binding> bindingSpan = Map<Binding>(extent, environment.Bindings);
        const std::span<const Run> samplerPairLists = Map<Run>(extent, environment.SamplerPairLists);
        const std::span<const SamplerTexturePair> samplerTexturePairs =
            Map<SamplerTexturePair>(extent, environment.SamplerTexturePairs);
        const std::span<const EntryPointConstants> constantsSpan =
            Map<EntryPointConstants>(extent, environment.EntryPointConstants);

        const uint64_t numSlots = context.EntryPointCount * environment.VariantCount;
        const TableRef64 slotsLoc{ environment.EntryPointInstanceTableOffset, numSlots };
        const std::span<const EntryPointInstance> entryPointInstanceSpan = Map<EntryPointInstance>(extent, slotsLoc);
        const size_t entryPointCount = static_cast<size_t>(context.EntryPointCount);

        for (int32_t variantIdx = 0; variantIdx < std::ssize(variantSpan); ++variantIdx)
        {
            const Variant& variant = variantSpan[variantIdx];
            if (variant.SuffixString >= context.StringCount)
            {
                return { .Code = ErrorCode::InvalidVariantSuffixString,
                         .Table = ShaderManifestTable::Variants,
                         .RecordIndex = static_cast<uint32_t>(variantIdx),
                         .Detail = variant.SuffixString };
            }

            if (variant.ResourceList >= environment.ResourceLists.Count)
            {
                return { .Code = ErrorCode::InvalidResourceListRun,
                         .Table = ShaderManifestTable::Variants,
                         .RecordIndex = static_cast<uint32_t>(variantIdx),
                         .Detail = variant.ResourceList };
            }

            // further open question for footprint lists: should we change it so that the null check
            // is no longer needed? We should have a sentinel value that indicates an empty or null
            // footprint for a resource, since that is still a valid case
            if (variant.FootprintList >= environment.FootprintLists.Count)
            {
                return { .Code = ErrorCode::InvalidVariantFootprintListIndex,
                         .Table = ShaderManifestTable::Variants,
                         .RecordIndex = static_cast<uint32_t>(variantIdx),
                         .Detail = variant.FootprintList };
            }

            const std::span<const uint32_t> variantResourceIndices =
                RunOf(resourceLists, resourceIndexList, variant.ResourceList);
            const std::span<const EntryPointInstance> variantSlots =
                entryPointInstanceSpan.subspan(static_cast<size_t>(variantIdx) * entryPointCount, entryPointCount);
            for (const EntryPointInstance& slot : variantSlots)
            {
                const std::span<const uint32_t> slotVisibilityIndices =
                    RunOf(visibilityLists, visibilityIndices, slot.Visibility);
                // absolute offset of this run into the visibility index table, so errors can report the row
                const uint32_t runOffset = static_cast<uint32_t>(slotVisibilityIndices.data() - visibilityIndices.data());

                // The placement order is checked for each instance, not once for each visibility list: an
                // entry indexes the resource list of the variant, and two variants can share one list.
                bool hasPreviousBound = false;
                bool seenNotBound = false;
                uint64_t previousKey = 0u;
                for (int32_t j = 0; j < std::ssize(slotVisibilityIndices); ++j)
                {
                    // the entry indexes the variant's resource list, and the resource list was checked
                    // against the binding table already
                    const uint32_t local = slotVisibilityIndices[j];
                    if (local >= variantResourceIndices.size())
                    {
                        return { .Code = ErrorCode::InvalidEntryPointInstanceVisibilityIndex,
                                 .Table = ShaderManifestTable::VisibilityIndices,
                                 .RecordIndex = runOffset + static_cast<uint32_t>(j),
                                 .Detail = local };
                    }

                    const uint32_t bindingIndex = variantResourceIndices[local];
                    const Binding& binding = bindingSpan[bindingIndex];
                    if (binding.PlacementKind != std::to_underlying(PlacementKind::Bound))
                    {
                        seenNotBound = true;
                        continue;
                    }

                    const uint64_t key = BoundPlacementKey(binding.Placement);
                    if (seenNotBound || (hasPreviousBound && key <= previousKey))
                    {
                        return { .Code = ErrorCode::InvalidVisibilityListOrder,
                                 .Table = ShaderManifestTable::VisibilityIndices,
                                 .RecordIndex = runOffset + static_cast<uint32_t>(j),
                                 .Detail = local };
                    }

                    previousKey = key;
                    hasPreviousBound = true;
                }

                // each pair names two positions in this instance's visibility list: a texture, then a sampler.
                // like the placement order, this is checked for each instance, because the positions are local
                const std::span<const SamplerTexturePair> slotPairs =
                    RunOf(samplerPairLists, samplerTexturePairs, slot.SamplerPairs);
                const uint32_t pairRunOffset = static_cast<uint32_t>(slotPairs.data() - samplerTexturePairs.data());
                auto kindAt = [&](uint32_t position) -> uint32_t
                {
                    return bindingSpan[variantResourceIndices[slotVisibilityIndices[position]]].Kind;
                };
                for (int32_t j = 0; j < std::ssize(slotPairs); ++j)
                {
                    const SamplerTexturePair& pair = slotPairs[j];
                    const bool inRange = pair.Texture < slotVisibilityIndices.size() &&
                                         pair.Sampler < slotVisibilityIndices.size();
                    const bool rightKinds = inRange &&
                                            kindAt(pair.Texture) == std::to_underlying(BindingKind::Texture) &&
                                            kindAt(pair.Sampler) == std::to_underlying(BindingKind::Sampler);
                    const bool ascending = j == 0 ||
                                           std::tie(slotPairs[j - 1].Texture, slotPairs[j - 1].Sampler) <
                                               std::tie(pair.Texture, pair.Sampler);
                    if (!rightKinds || !ascending)
                    {
                        return { .Code = ErrorCode::InvalidSamplerTexturePair,
                                 .Table = ShaderManifestTable::SamplerTexturePairs,
                                 .RecordIndex = pairRunOffset + static_cast<uint32_t>(j),
                                 .Detail = slot.SamplerPairs };
                    }
                }

                // a uniform buffer of entry point constants is a position in this instance's visibility list, and
                // that position must name a uniform buffer of the same size
                const EntryPointConstants& constants = constantsSpan[slot.Constants];
                if (constants.Delivery == std::to_underlying(EntryPointConstantsDelivery::UniformBuffer))
                {
                    const bool inRange = constants.Binding < slotVisibilityIndices.size();
                    const Binding* container =
                        inRange ? &bindingSpan[variantResourceIndices[slotVisibilityIndices[constants.Binding]]] : nullptr;
                    if (container == nullptr || container->Kind != std::to_underlying(BindingKind::UniformBuffer) ||
                        container->ByteSize != constants.ByteSize)
                    {
                        return { .Code = ErrorCode::InvalidEntryPointConstants,
                                 .Table = ShaderManifestTable::EntryPointConstants,
                                 .RecordIndex = slot.Constants,
                                 .Detail = constants.Binding };
                    }
                }
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateAxisMasks(const EnvironmentContext& context) noexcept
    {
        const uint32_t wordCount = context.AxisMaskWordCount;
        const auto usedBitsInLastWord = static_cast<uint32_t>(context.ModuleAxisCount % k_AxisMaskWordBits);
        if (wordCount == 0u || usedBitsInLastWord == 0u)
        {
            return k_ManifestOk;
        }

        const EnvironmentHeader& environment = context.Environment;
        const TableRef axisMaskLoc{ environment.AxisMaskTableOffset,
                                    environment.VariantCount * wordCount };
        const std::span<const uint64_t> maskSpan = Map<uint64_t>(context.Extent, axisMaskLoc);
        // only the last word of each mask can hold a bit past the module's axes
        for (int32_t i = 0; std::cmp_less(i, environment.VariantCount); ++i)
        {
            const uint64_t lastWord = maskSpan[(static_cast<size_t>(i) * wordCount) + wordCount - 1u];
            if ((lastWord >> usedBitsInLastWord) != 0u)
            {
                return { .Code = ErrorCode::InvalidVariantAxisMask,
                         .Table = ShaderManifestTable::AxisMasks,
                         .RecordIndex = static_cast<uint32_t>(i) };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateAxisRelevance(const EnvironmentContext& context) noexcept
    {
        const uint32_t wordCount = context.AxisMaskWordCount;
        const auto usedBitsInLastWord = static_cast<uint32_t>(context.ModuleAxisCount % k_AxisMaskWordBits);
        if (wordCount == 0u || usedBitsInLastWord == 0u)
        {
            return k_ManifestOk;
        }

        const EnvironmentHeader& environment = context.Environment;
        const TableRef relevanceLoc{ environment.AxisRelevanceTableOffset,
                                     static_cast<uint32_t>(context.EntryPointCount) * wordCount };
        const std::span<const uint64_t> relevanceSpan = Map<uint64_t>(context.Extent, relevanceLoc);
        // only the last word of each mask can hold a bit past the module's axes
        for (int32_t i = 0; std::cmp_less(i, context.EntryPointCount); ++i)
        {
            const uint64_t lastWord = relevanceSpan[(static_cast<size_t>(i) * wordCount) + wordCount - 1u];
            if ((lastWord >> usedBitsInLastWord) != 0u)
            {
                return { .Code = ErrorCode::InvalidAxisRelevance,
                         .Table = ShaderManifestTable::AxisRelevance,
                         .RecordIndex = static_cast<uint32_t>(i) };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateRasterStates(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const RasterState> rasterSpan = Map<RasterState>(context.Extent, environment.Rasters);
        for (int32_t i = 0; i < std::ssize(rasterSpan); ++i)
        {
            const RasterState& raster = rasterSpan[i];
            if (raster.FirstVertexInput + raster.VertexInputCount > environment.VertexInputs.Count)
            {
                return { .Code = ErrorCode::InvalidRasterVertexInputRange,
                         .Table = ShaderManifestTable::Rasters,
                         .RecordIndex = static_cast<uint32_t>(i) };
            }

            if (raster.FirstColorTarget + raster.ColorTargetCount > environment.ColorTargets.Count)
            {
                return { .Code = ErrorCode::InvalidRasterColorTargetRange,
                         .Table = ShaderManifestTable::Rasters,
                         .RecordIndex = static_cast<uint32_t>(i) };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateVertexInputs(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const VertexInput> vertexInputSpan = Map<VertexInput>(context.Extent, environment.VertexInputs);
        for (int32_t i = 0; i < std::ssize(vertexInputSpan); ++i)
        {
            if (vertexInputSpan[i].SemanticNameString >= context.StringCount)
            {
                return { .Code = ErrorCode::InvalidVertexInput,
                         .Table = ShaderManifestTable::VertexInputs,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = vertexInputSpan[i].SemanticNameString };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateUniformMembers(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const UniformMember> uniformMemberSpan = Map<UniformMember>(context.Extent, environment.UniformMembers);
        for (int32_t i = 0; i < std::ssize(uniformMemberSpan); ++i)
        {
            if (uniformMemberSpan[i].NameString >= context.StringCount)
            {
                return { .Code = ErrorCode::InvalidUniformMember,
                         .Table = ShaderManifestTable::UniformMembers,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = uniformMemberSpan[i].NameString };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateSpecializationConstants(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const SpecializationConstant> constantSpan =
            Map<SpecializationConstant>(context.Extent, environment.SpecConstants);
        for (int32_t i = 0; i < std::ssize(constantSpan); ++i)
        {
            if (constantSpan[i].NameString >= context.StringCount)
            {
                return { .Code = ErrorCode::InvalidSpecializationConstant,
                         .Table = ShaderManifestTable::SpecializationConstants,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = constantSpan[i].NameString };
            }
        }

        return k_ManifestOk;
    }

} // namespace

ErrorState ValidateBundle(const Header& parsed, std::span<const std::byte> bytes) noexcept
{
    const ErrorState headerValid = ValidateBundleHeader(parsed, bytes.size());
    if (!headerValid)
    {
        return headerValid;
    }

    for (const BundleValidator validator : k_BundleValidators)
    {
        const ErrorState result = validator(parsed, bytes);
        if (!result)
        {
            return result;
        }
    }

    return k_ManifestOk;
}

ErrorState OpenEnvironmentExtent(const BundleView& bundle,
                                 uint32_t profile_index,
                                 uint32_t module_index,
                                 std::span<const std::byte> extent_bytes) noexcept
{
    // The indices and the bytes come from the caller, so this is an ingestion surface and it checks them.
    if (profile_index >= bundle.Profiles().size() || module_index >= bundle.ModuleCount())
    {
        return { .Code = ErrorCode::IndexOutOfBounds,
                 .Table = ShaderManifestTable::Environments,
                 .Detail = module_index };
    }

    const EnvironmentDirectoryEntry& entry = bundle.Environment(profile_index, module_index);
    if (entry.ExtentSize == 0u)
    {
        return { .Code = ErrorCode::EnvironmentNotCooked,
                 .Table = ShaderManifestTable::Environments,
                 .RecordIndex = profile_index,
                 .Detail = module_index };
    }

    if (extent_bytes.size() != entry.ExtentSize)
    {
        return { .Code = ErrorCode::SizeMismatch, .Detail = static_cast<uint32_t>(entry.ExtentSize) };
    }

    if ((reinterpret_cast<uintptr_t>(extent_bytes.data()) % 8u) != 0u)
    {
        return { .Code = ErrorCode::Misaligned };
    }

    return k_ManifestOk;
}

ErrorState ValidateEnvironment(const EnvironmentContext& context) noexcept
{
    for (const EnvironmentValidator validator : k_EnvironmentValidators)
    {
        const ErrorState result = validator(context);
        if (!result)
        {
            return result;
        }
    }

    return k_ManifestOk;
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif

} // namespace lodestone::manifest::detail
