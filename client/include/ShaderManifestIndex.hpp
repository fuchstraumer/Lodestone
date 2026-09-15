#pragma once
#ifndef LODESTONE_CLIENT_SHADER_MANIFEST_INDEX_HPP
#define LODESTONE_CLIENT_SHADER_MANIFEST_INDEX_HPP
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "VariantKey.hpp"
#include <cstdint>
#include <cstddef>
#include <expected>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lodestone
{

enum class QueryErrorCode : uint8_t
{
    Success = 0,
    UnknownAxis,
    ValueNotInAxis,
    IncorrectValueDomain, // e.g, tried to use a uint with a bool axis
    Count
};

struct QueryError
{
    QueryErrorCode Code{ QueryErrorCode::Success };
    std::string_view AxisName; // reads into manifest, so should remain valid
    uint32_t Detail{}; // additional context-specific detail about the error
};

template<typename T>
using QueryResult = std::expected<T, QueryError>;

struct DecodedVariant
{
    VariantKey Key{};
    std::vector<ManifestAxisValue> Values;
};

/** @brief The ManifestIndex constructs these to allow users to construct hierarchical queries
  * for shader variants, using a declarative and expressive style. This should not be used on
  * the hotpath: use it to construct a rendergraph or framegraph, or to populate pipeline and
  * layout caches, but not for per-frame variance (currently). */
struct ManifestQueryBuilder
{
    ManifestQueryBuilder(const class ManifestIndex& index) noexcept;
    
    /** @brief Building queries is done by creating new objects - not returned by reference, as
      * cases where you may create high-level queries based on platform caps wouldn't work in that
      * case (this allows child queries to be built as sub-categories varied more dynamically) */
    [[nodiscard]] ManifestQueryBuilder Where(std::string_view axis_name, bool value) const noexcept;
    [[nodiscard]] ManifestQueryBuilder Where(std::string_view axis_name, uint32_t value) const noexcept;
    [[nodiscard]] ManifestQueryBuilder Where(std::string_view axis_name, std::string_view type_name) const noexcept;

    // For the boolean multi-parameter case, there's no need to even pass values... it's true or false
    [[nodiscard]] ManifestQueryBuilder WhereAnyOfBoolean(std::string_view axis_name) const noexcept;
    [[nodiscard]] ManifestQueryBuilder WhereAnyOf(std::string_view axis_name, std::span<const uint32_t> values) const noexcept;
    [[nodiscard]] ManifestQueryBuilder WhereAnyOf(std::string_view axis_name, std::span<const std::string_view> values) const noexcept;

    // These are the terminal functions, which effectively close a query and return the final result
    [[nodiscard]] QueryResult<std::vector<VariantKey>> Keys() const noexcept;
    [[nodiscard]] QueryResult<std::vector<DecodedVariant>> Variants() const noexcept;
    /** @brief Returns the first VariantKey matching the query, or INVALID_VARIANT if none exist. */
    [[nodiscard]] QueryResult<VariantKey> First() const noexcept;

    /** @brief Returns the size of the current query result set: doesn't trigger retrieval like others */
    [[nodiscard]] size_t Size() const noexcept;

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] std::span<const QueryError> Errors() const noexcept;
};

/** @brief This object represents an "index" in the database and relational query sense,
  * returning objects used to actually run queries for VariantKey values. It offers an 
  * interface to simply retrieve all keys and related data - to precache pipelines or layouts
  * - while querying for concrete subsets based on permutation values is left to the query
  * objects spawned by the index. */
class ManifestIndex
{
public:
    ManifestIndex(ShaderManifestView view);

    [[nodiscard]] const ShaderManifestView& View() const noexcept;
    /** @brief Direct decode: "expand" a variant key into that values matching that key */
    [[nodiscard]] std::vector<ManifestAxisValue> Decode(VariantKey key) const;
    /** @brief Returns every variant that exists, in (sorted) key order. Useful for total 
      * precaching of everything a manifest could generate as shader state */
    [[nodiscard]] std::vector<DecodedVariant> Enumerate() const;
    /** @brief Opens a new query, used to retrieve specific variants for actual runtime rendering or use */
    [[nodiscard]] ManifestQueryBuilder Query() const noexcept;
    /** @brief Effectively the form and system that Query() uses when closed: ever axis absent from input 
      * constraints is considered unconstrained and uses just the default value (canonical value, effectively) */
    [[nodiscard]] std::vector<VariantKey> Select(std::span<const ManifestAxisAssignmentRange> constraints) const;

private:

    struct ScanConstraint
    {
        uint32_t AxisIndex;
        std::vector<uint32_t> AllowedValueIndices;
    };

    [[nodiscard]] ManifestAxisValue decodeAxis(uint32_t axis_index, uint32_t value_index) const noexcept;
    [[nodiscard]] std::vector<uint32_t> integralValueIndices(const uint32_t axis_index, const ManifestAxisAssignmentRange& range) const;
    [[nodiscard]] std::vector<uint32_t> stringValueIndices(const uint32_t axis_index, const ManifestAxisAssignmentRange& range) const;
    [[nodiscard]] std::vector<VariantKey> scan(std::span<const ScanConstraint> constraints) const;

    ShaderManifestView manifest;
    std::vector<uint32_t> radices;
    std::vector<uint64_t> placeValues;
    // todo: maybe a packed vector (sorted) that we use std::find on might be better for our use case? (test)
    std::unordered_map<std::string_view, uint32_t> axisNameToIndex;
};

}

#endif // !LODESTONE_CLIENT_SHADER_MANIFEST_INDEX_HPP
