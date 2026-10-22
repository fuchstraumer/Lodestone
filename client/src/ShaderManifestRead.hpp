#pragma once
#ifndef LODESTONE_CLIENT_SHADER_MANIFEST_READ_HPP
#define LODESTONE_CLIENT_SHADER_MANIFEST_READ_HPP
#include "ShaderManifest.hpp"
#include "ShaderLibraryTypes.hpp"
#include "VariantKey.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
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

    /** Group in the high half, binding in the low half: the order of the cooker's `PlacementLess`. */
    inline constexpr uint64_t BoundPlacementKey(const PlacementPayload& placement) noexcept
    {
        return (static_cast<uint64_t>(placement.Word0) << 32u) | static_cast<uint64_t>(placement.Word1);
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
    template<typename RecordType> requires(std::is_trivially_copyable_v<RecordType> && !std::is_pointer_v<RecordType>)
    inline std::span<const RecordType> Map(std::span<const std::byte> bytes, TableRef table_ref) noexcept
    {
        return std::span<const RecordType>{ reinterpret_cast<const RecordType*>(bytes.data() + table_ref.Offset),
                                            static_cast<size_t>(table_ref.Count) };
    }
    
    template<typename RecordType> requires(std::is_trivially_copyable_v<RecordType> && !std::is_pointer_v<RecordType>)
    inline std::span<const RecordType> Map(std::span<const std::byte> bytes, TableRef64 table_ref) noexcept
    {
        return std::span<const RecordType>{ reinterpret_cast<const RecordType*>(bytes.data() + table_ref.Offset),
                                            static_cast<size_t>(table_ref.Count) };
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
