#pragma once
#ifndef LODESTONE_CLIENT_SHADER_MANIFEST_READ_HPP
#define LODESTONE_CLIENT_SHADER_MANIFEST_READ_HPP
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifestFormat.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace lodestone::manifest::detail
{

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

    inline constexpr ErrorState k_ManifestOk{ .Code = ErrorCode::Success };

    /** The order of the cooker's `PlacementLess`: bound placements by group and binding, then stored values by container,
     * block, and offset, then no placement. */
    struct PlacementOrderKey
    {
        uint32_t Rank{ 0u };
        uint32_t Container{ 0u };
        uint32_t Word0{ 0u };
        uint32_t Word1{ 0u };
        uint32_t Offset{ 0u };

        friend constexpr auto operator<=>(const PlacementOrderKey&, const PlacementOrderKey&) = default;
    };

    inline constexpr PlacementOrderKey PlacementOrderKeyOf(const Binding& record) noexcept
    {
        const bool bound = record.PlacementKind == std::to_underlying(PlacementKind::Bound);
        const bool stored = record.PlacementKind == std::to_underlying(PlacementKind::Handle) ||
                            record.PlacementKind == std::to_underlying(PlacementKind::Pointer);
        
        uint32_t rank = 0u;
        if (bound)
        {
            rank = 0u;
        }
        else if (stored)
        {
            rank = 1u;
        }
        else
        {
            rank = 2u;
        }

        return PlacementOrderKey{ .Rank = rank,
                                  .Container = record.ValueContainer,
                                  .Word0 = record.Placement.Word0,
                                  .Word1 = record.Placement.Word1,
                                  .Offset = record.ValueOffset };
    }

    // the reader reinterprets bytes as records, so a record must be a bag of bytes
    template<typename RecordType>
    inline constexpr bool k_IsManifestRecord =
        std::is_trivially_copyable_v<RecordType> && alignof(RecordType) <= 8u;

    static_assert(k_IsManifestRecord<Run>);
    static_assert(k_IsManifestRecord<TableRef>);
    static_assert(k_IsManifestRecord<TableRef64>);
    static_assert(k_IsManifestRecord<Header>);
    static_assert(k_IsManifestRecord<StringRef>);
    static_assert(k_IsManifestRecord<SourceRef>);
    static_assert(k_IsManifestRecord<Profile>);
    static_assert(k_IsManifestRecord<EnvironmentDirectoryEntry>);
    static_assert(k_IsManifestRecord<Axis>);
    static_assert(k_IsManifestRecord<ModuleAxis>);
    static_assert(k_IsManifestRecord<ModuleRootHeader>);
    static_assert(k_IsManifestRecord<EntryPoint>);
    static_assert(k_IsManifestRecord<EnvironmentHeader>);
    static_assert(k_IsManifestRecord<Variant>);
    static_assert(k_IsManifestRecord<PlacementPayload>);
    static_assert(k_IsManifestRecord<Binding>);
    static_assert(k_IsManifestRecord<Footprint>);
    static_assert(k_IsManifestRecord<EntryPointInstance>);
    static_assert(k_IsManifestRecord<VertexInput>);
    static_assert(k_IsManifestRecord<UniformMember>);
    static_assert(k_IsManifestRecord<SpecializationConstant>);
    static_assert(k_IsManifestRecord<ColorTarget>);
    static_assert(k_IsManifestRecord<RasterState>);
    static_assert(k_IsManifestRecord<SamplerTexturePair>);
    static_assert(k_IsManifestRecord<EntryPointConstants>);

    inline constexpr size_t k_AxisMaskWordBits = std::numeric_limits<uint64_t>::digits;
    
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    /** Starts the lifetime of `count` records in the bytes, so that a read through the typed pointer is defined.
     * The caller has checked the alignment. A library with no `start_lifetime_as` relies on the implicit object
     * creation of the buffer that holds the bytes. */
    template<typename RecordType> requires(std::is_trivially_copyable_v<RecordType> && !std::is_pointer_v<RecordType>)
    inline const RecordType* StartRecords(const std::byte* data, size_t count) noexcept
    {
#if defined(__cpp_lib_start_lifetime_as)
        return std::start_lifetime_as_array<RecordType>(data, count);
#else
        static_cast<void>(count);
        return reinterpret_cast<const RecordType*>(data);
#endif
    }

    template<typename RecordType> requires(std::is_trivially_copyable_v<RecordType> && !std::is_pointer_v<RecordType>)
    inline std::span<const RecordType> Map(std::span<const std::byte> bytes, TableRef table_ref) noexcept
    {
        const size_t count = static_cast<size_t>(table_ref.Count);
        return std::span<const RecordType>{ StartRecords<RecordType>(bytes.data() + table_ref.Offset, count), count };
    }
    
    template<typename RecordType> requires(std::is_trivially_copyable_v<RecordType> && !std::is_pointer_v<RecordType>)
    inline std::span<const RecordType> Map(std::span<const std::byte> bytes, TableRef64 table_ref) noexcept
    {
        const size_t count = static_cast<size_t>(table_ref.Count);
        return std::span<const RecordType>{ StartRecords<RecordType>(bytes.data() + table_ref.Offset, count), count };
    }
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

    template<typename PayloadType, typename RunIndexType>
    inline std::span<const PayloadType> RunOf(std::span<const Run> runs,
                                       std::span<const PayloadType> payloads,
                                       RunIndexType run_index) noexcept
    {
        const Run& run = runs[static_cast<size_t>(run_index)];
        return payloads.subspan(run.First, run.Count);
    }

    inline uint32_t SelectSetBit(uint32_t mask, uint32_t rank) noexcept
    {
        for (int32_t cleared = 0u; std::cmp_less(cleared, rank); ++cleared)
        {
            mask &= mask - 1u;
        }

        return static_cast<uint32_t>(std::countr_zero(mask));
    }

    inline uint32_t MaskWordCountForAxes(uint64_t axis_count) noexcept
    {
        return static_cast<uint32_t>((axis_count + k_AxisMaskWordBits - 1u) / k_AxisMaskWordBits);
    }

#ifdef __clang__
#pragma clang diagnostic pop
#endif

} // namespace lodestone::manifest::detail

#endif // !LODESTONE_CLIENT_SHADER_MANIFEST_READ_HPP
