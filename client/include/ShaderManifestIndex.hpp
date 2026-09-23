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
#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lodestone
{


/** @brief One axis value: represents a single possible value for a permutation axis in the shader. */
struct QueryAxisValue
{
    AxisValueDomain Type{ AxisValueDomain::None };
    // removed previously untagged union. manifest stores everything as AxisValueType anyways.
    AxisValueType IntegralValue{ 0u };
    // for Interface axes, the name of the specific implementation
    // for Enum axes, the name of the specific case
    std::string_view Name;

    constexpr bool operator==(const QueryAxisValue& other) const noexcept
    {
        if (Type != other.Type)
        {
            return false;
        }
        else
        {
            return Type == AxisValueDomain::Integral ? IntegralValue == other.IntegralValue : Name == other.Name;
        }
    }
};

/** @brief Collection of values constraining a single axis. Most clients should not use or build
  * these directly; they are generated sequentially by the builder during query build */
struct QueryAxisRange
{
    std::string_view AxisName;
    std::vector<QueryAxisValue> Values;
    // AxisIndex to keep constraints sorted. Trivial and tiny and easy to add 
    // so that's why we're growing this struct to include it
    uint32_t AxisIndex{ 0u };
};

enum class QueryErrorCode : uint8_t
{
    Success = 0,
    UnknownAxis,
    ValueNotInAxis,
    IncorrectValueDomain, // e.g, tried to use a uint with a bool axis
    EmptyConstraintSet, // you can't provide an empty value set to a constraint function
    NoVariantForConstraints, // if query.First() returns invalid variant, it's bc the constraints are invalid
    Count
};

struct QueryError
{
    QueryErrorCode Code{ QueryErrorCode::Success };
    std::string AxisName; // can name user input, so can't be a string_view
    uint32_t Detail{}; // additional context-specific detail about the error
    std::string_view Suggestion; // read from manifest blob, can be a view
};

// QueryResult comes from the terminal functions - it tells you 
// mostly just success or failure. To get the full error state,
// you'll need to get the error vector
template<typename T>
using QueryResult = std::expected<T, QueryErrorCode>;

struct DecodedVariant
{
    VariantKey Key{};
    std::vector<QueryAxisValue> Values;
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
    /** @brief Must provide `domain`, as a named axis value could be an interface or enum, so we can't
     * disambiguate user intention by name alone. */
    [[nodiscard]] ManifestQueryBuilder Where(std::string_view axis_name,
                                             AxisValueDomain domain,
                                             std::string_view type_or_enum_name) const noexcept;

    // For the boolean multi-parameter case, there's no need to even pass values... it's true or false
    [[nodiscard]] ManifestQueryBuilder WhereAnyOfBoolean(std::string_view axis_name) const;
    [[nodiscard]] ManifestQueryBuilder WhereAnyOf(std::string_view axis_name, std::span<const uint32_t> values) const;
    [[nodiscard]] ManifestQueryBuilder WhereAnyOf(std::string_view axis_name,
                                                  AxisValueDomain domain,
                                                  std::span<const std::string_view> values) const;

    // Exclusion filters, which allow you to specify values that should be excluded from the query results.
    [[nodiscard]] ManifestQueryBuilder WhereNoneOf(std::string_view axis_name, bool value) const noexcept;
    [[nodiscard]] ManifestQueryBuilder WhereNoneOf(std::string_view axis_name, std::span<const uint32_t> values) const noexcept;
    [[nodiscard]] ManifestQueryBuilder WhereNoneOf(std::string_view axis_name,
                                                   AxisValueDomain domain,
                                                   std::span<const std::string_view> values) const noexcept;

    // These are the terminal functions, which effectively close a query and return the final result
    [[nodiscard]] QueryResult<std::vector<VariantKey>> Keys() const noexcept;
    [[nodiscard]] QueryResult<std::vector<DecodedVariant>> Variants() const noexcept;
    /** @brief If you've already built Keys(), then this will be a (slightly) cheaper way to get the variants 
      * Returns without using QueryResult because Keys() can fail, but Variants() cannot. */
    [[nodiscard]] std::vector<DecodedVariant> VariantsFromKeys(const std::vector<VariantKey>& keys) const noexcept;
    /** @brief Returns the first VariantKey matching the query, or the query's error state.
     *  @note This is quite an inefficient accessor, so prefer using Keys() and Variants() when at all suitable. */
    [[nodiscard]] QueryResult<VariantKey> First() const noexcept;

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] std::span<const QueryError> Errors() const noexcept;
private:
    // resolves the given axis name, domain, and input values. returns the axis index if successful, nullopt if
    // any of the validations fails
    [[nodiscard]] std::optional<uint32_t> resolveAndValidate(ManifestQueryBuilder& result,
                                                             std::string_view axis_name,
                                                             std::span<const QueryAxisValue> values) const;
    // sorted lower_bound insert/merge of `allowed` values either into existing constraints or new placement
    // operates directly on *this, since we call it through the result object right before it's returned
    void insertConstraint(std::string_view axis_name,
                          uint32_t axis_index,
                          std::vector<QueryAxisValue> allowed);

    // overload to coalesce work for WhereAnyOf functions, assuming shared value domain
    [[nodiscard]] ManifestQueryBuilder whereAnyOf(std::string_view axis_name, std::vector<QueryAxisValue> value) const noexcept;
    const class ManifestIndex* index{ nullptr };
    std::vector<QueryError> errors;
    // Since we can have multiple values as constraints per axis, we use a vector of QueryAxisRange.
    // Kept in insertion order here; Select sorts the derived scan constraints by axis index.
    std::vector<QueryAxisRange> constraints;
};

/** @brief This object represents an "index" in the database and relational query sense,
  * returning objects used to actually run queries for VariantKey values. It offers an 
  * interface to simply retrieve all keys and related data - to precache pipelines or layouts
  * - while querying for concrete subsets based on permutation values is left to the query
  * objects spawned by the index.
  * @note An index reads one environment: one module, cooked for one profile. Variant keys are per
  * module, and each profile can cook a different subset of them, so a query always names both. */
class ManifestIndex
{
public:
    explicit ManifestIndex(manifest::EnvironmentView view);

    [[nodiscard]] const manifest::EnvironmentView& View() const noexcept;
    /** @brief Direct decode: "expand" a variant key into that values matching that key */
    [[nodiscard]] std::vector<QueryAxisValue> Decode(VariantKey key) const;
    /** @brief Returns every variant that exists, in (sorted) key order. Useful for total 
      * precaching of everything a manifest could generate as shader state */
    [[nodiscard]] std::vector<DecodedVariant> Enumerate() const;
    /** @brief Opens a new query, used to retrieve specific variants for actual runtime rendering or use */
    [[nodiscard]] ManifestQueryBuilder Query() const noexcept;

private:

    // Besides needing to get to the private functions, this also lets us read things like
    // axisNameToIndex + the manifest, both of which are nearly essential for query building.
    friend struct ManifestQueryBuilder;

    struct ScanConstraint
    {
        uint32_t AxisIndex;
        std::vector<uint32_t> AllowedValueIndices;
    };
    
    [[nodiscard]] std::vector<ScanConstraint> convertToScanConstraints(std::span<const QueryAxisRange> query) const;
    /** @brief Effectively the form and system that Query() uses when closed: ever axis absent from input 
      * constraints is considered unconstrained and uses just the default value (canonical value, effectively) */
    [[nodiscard]] std::vector<VariantKey> select(std::span<const QueryAxisRange> constraints) const;
    [[nodiscard]] QueryAxisValue decodeAxis(uint32_t axis_index, uint32_t value_index) const noexcept;
    [[nodiscard]] std::vector<uint32_t> integralValueIndices(const uint32_t axis_index, const QueryAxisRange& range) const;
    [[nodiscard]] std::vector<uint32_t> stringValueIndices(const uint32_t axis_index, const QueryAxisRange& range) const;
    /** @brief Filters the keys based on the provided scan constraints - returns a view into manifest
      * that's better bounded based on the input constraints, to reduce iteration complexity. */
    [[nodiscard]] std::span<const VariantKey> filterKeys(std::span<const ScanConstraint> constraints) const;
    /** @brief Returns the first key that matches the given constraints. */
    [[nodiscard]] VariantKey first(std::span<const ScanConstraint> constraints) const;
    [[nodiscard]] std::vector<VariantKey> scan(std::span<const ScanConstraint> constraints) const;

    [[nodiscard]] std::vector<std::string_view> stringTableForAxis(uint32_t axis_index) const;
    manifest::EnvironmentView environment;
    /** @brief The values each module axis uses, in digit order. A module axis selects its values from a
     * root axis through a mask, so they are not one contiguous run of the root table. */
    std::vector<std::vector<AxisValueType>> axisValues;
    std::vector<uint32_t> radices;
    std::vector<uint64_t> placeValues;
    // todo: maybe a packed vector (sorted) that we use std::find on might be better for our use case? (test)
    std::unordered_map<std::string_view, uint32_t> axisNameToIndex;
};

}

#endif // !LODESTONE_CLIENT_SHADER_MANIFEST_INDEX_HPP
