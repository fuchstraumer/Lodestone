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
    /** @brief Only set by decoding. False when the variant does not use this axis: the value is then the
     * canonical default, which is packed into the key but never read by the shader. Not compared by `==`. */
    bool Active{ true };

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

/** @brief How a constraint treats a variant where its axis is inactive (an `ActiveWhen` gate is off).
  * The constraint tests the value of an active axis the same way in both modes. */
enum class PinMode : uint8_t
{
    /** The axis must be active and hold an allowed value. A variant with the axis inactive does not match,
      * so a required pin on a gated axis also selects the gate. */
    Required,
    /** The axis holds an allowed value when it is active. A variant with the axis inactive matches, so the
      * pin narrows the active variants and keeps the gated-off ones. */
    WhenActive,
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
    PinMode Mode{ PinMode::Required };
};

enum class QueryErrorCode : uint8_t
{
    Success = 0,
    UnknownAxis,
    ValueNotInAxis,
    IncorrectValueDomain, // e.g, tried to use a uint with a bool axis
    EmptyConstraintSet, // you can't provide an empty value set to a constraint function
    NoVariantForConstraints, // Single() matched no variant
    AmbiguousVariant, // Single() matched more than one variant. AxisName lists the axes that still vary
    UnknownEntryPoint, // SingleFor() got an entry point name the module does not declare. AxisName holds the name
    EmptyEntryPointSet, // SingleFor() got no entry point names
    Count
};

struct QueryError
{
    QueryErrorCode Code{ QueryErrorCode::Success };
    std::string AxisName; // can name user input, so can't be a string_view
    uint32_t Detail{}; // additional context-specific detail about the error
    std::string_view Suggestion; // read from manifest blob, can be a view
};

/** @brief The kind of a query note. A note never fails a terminal. It names a constraint that had no effect,
  * so the caller can remove it or log it. */
enum class QueryNoteCode : uint8_t
{
    PinAxisNotDeclared, // a preset pinned an axis that this module does not declare, and Apply skipped the pin
};

struct QueryNote
{
    QueryNoteCode Code{ QueryNoteCode::PinAxisNotDeclared };
    std::string AxisName;
};

/** @brief One pin of a `QueryPreset`: one axis, the values it allows, and its mode. The pin owns its strings,
  * so one preset can serve many modules and many manifests. */
struct QueryPresetPin
{
    std::string AxisName;
    AxisValueDomain Domain{ AxisValueDomain::None };
    // the values of a Boolean (0 or 1) or Integral pin
    std::vector<AxisValueType> IntegralValues;
    // the implementation names of a Type pin, or the case names of an Enum pin
    std::vector<std::string> NamedValues;
    PinMode Mode{ PinMode::Required };
};

/** @brief A set of axis pins that a caller builds once and applies to the query of any module, through
  * `ManifestQueryBuilder::Apply`. A pin on an axis that the module does not declare is skipped and gives a
  * `QueryNote`, so one preset serves modules with different axes. A pin on a declared axis is checked as
  * `Where` checks it, so a value the axis does not declare is an error. */
class QueryPreset
{
public:
    void Pin(std::string_view axis_name, bool value, PinMode mode);
    void Pin(std::string_view axis_name, uint32_t value, PinMode mode);
    /** @brief Must provide `domain`, for the same reason as `ManifestQueryBuilder::Where`. */
    void Pin(std::string_view axis_name, AxisValueDomain domain, std::string_view type_or_enum_name, PinMode mode);
    void PinAnyOf(std::string_view axis_name, std::span<const uint32_t> values, PinMode mode);
    void PinAnyOf(std::string_view axis_name,
                  AxisValueDomain domain,
                  std::span<const std::string_view> values,
                  PinMode mode);

    [[nodiscard]] std::span<const QueryPresetPin> Pins() const noexcept;
private:
    std::vector<QueryPresetPin> pins;
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

    /** @brief The `PinMode::WhenActive` form of `Where`. The value applies only to variants where the axis is
      * active, and a variant where the axis is inactive still matches. */
    [[nodiscard]] ManifestQueryBuilder WhereWhenActive(std::string_view axis_name, bool value) const noexcept;
    [[nodiscard]] ManifestQueryBuilder WhereWhenActive(std::string_view axis_name, uint32_t value) const noexcept;
    [[nodiscard]] ManifestQueryBuilder WhereWhenActive(std::string_view axis_name,
                                                       AxisValueDomain domain,
                                                       std::string_view type_or_enum_name) const noexcept;

    /** @brief Applies each pin of `preset`, in order. A pin on an undeclared axis is skipped and gives a
      * `PinAxisNotDeclared` note. Any other pin works as `Where` or `WhereWhenActive` does, and its errors
      * are the same.
      * @note The builder keeps views into the preset's strings, so keep the preset alive while the builder
      * is in use. */
    [[nodiscard]] ManifestQueryBuilder Apply(const QueryPreset& preset) const;

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
    /** @brief The one variant the query names. A query will only return a result if it has *exactly* one
      * matching variant.
      * @return The key when exactly one variant matches. Otherwise the error: the first builder error, with
      * its suggestion; `NoVariantForConstraints` for no match; or `AmbiguousVariant` for more than one.
      * @note `AmbiguousVariant` puts the varying axis names in `AxisName` ("A, B", in axis order) and the match
      * count in `Detail`.  */
    [[nodiscard]] std::expected<VariantKey, QueryError> Single() const noexcept;
    /** @brief The one entry point instance per named entry point that the query names, for use in a pipeline
      * of those entry points. An axis that cannot change any of them (`EnvironmentView::IsAxisRelevant`) does
      * not count: matches that differ only in such axes give the same instances, so they are one answer, and
      * the smallest of their keys is returned.
      * @return The key, or the error: the first builder error; `EmptyEntryPointSet`; `UnknownEntryPoint` with
      * the name in `AxisName` and a suggestion; `NoVariantForConstraints`; or `AmbiguousVariant`, which lists
      * only the varying axes that are relevant to the named entry points. */
    [[nodiscard]] std::expected<VariantKey, QueryError> SingleFor(
        std::span<const std::string_view> entry_point_names) const noexcept;

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] std::span<const QueryError> Errors() const noexcept;
    /** @brief The notes of this query. A note does not fail a terminal. Log them to show which constraints
      * had no effect. */
    [[nodiscard]] std::span<const QueryNote> Notes() const noexcept;
private:
    // resolves the given axis name, domain, and input values. returns the axis index if successful, nullopt if
    // any of the validations fails
    [[nodiscard]] std::optional<uint32_t> resolveAndValidate(ManifestQueryBuilder& result,
                                                             std::string_view axis_name,
                                                             std::span<const QueryAxisValue> values) const;
    // sorted lower_bound insert/merge of `allowed` values either into existing constraints or new placement
    // operates directly on *this, since we call it through the result object right before it's returned.
    // a merge is the union of both constraints: the values join, and the result is WhenActive when either is
    void insertConstraint(std::string_view axis_name,
                          uint32_t axis_index,
                          std::vector<QueryAxisValue> allowed,
                          PinMode mode);

    // the shared body of Single and SingleFor: one answer when the matches agree on every axis whose bit is
    // set in `relevant_axes` (in the word layout of EnvironmentView::AxisRelevance)
    [[nodiscard]] std::expected<VariantKey, QueryError> singleOver(std::span<const uint64_t> relevant_axes) const noexcept;

    // overload to coalesce work for WhereAnyOf functions, assuming shared value domain
    [[nodiscard]] ManifestQueryBuilder whereAnyOf(std::string_view axis_name,
                                                  std::vector<QueryAxisValue> value,
                                                  PinMode mode) const noexcept;
    const class ManifestIndex* index{ nullptr };
    std::vector<QueryError> errors;
    std::vector<QueryNote> notes;
    // Since we can have multiple values as constraints per axis, we use a vector of QueryAxisRange.
    // Kept in insertion order here; Select sorts the derived scan constraints by axis index.
    std::vector<QueryAxisRange> constraints;
};

/** @brief Returns the candidate closest to a mistyped name, or an empty view when none is close enough.
  * The query errors use this to fill `QueryError::Suggestion`. Renderers can use it for their own names
  * (bindings, entry points), so that every "did you mean" suggestion follows the same rule. The edit budget
  * is one edit per three characters of `input`, with a minimum of one. The result is a view into
  * `candidates`. */
[[nodiscard]] std::string_view NearestName(std::string_view input, std::span<const std::string_view> candidates) noexcept;

/** @brief This object represents an "index" in the database and relational query sense,
  * returning objects used to actually run queries for VariantKey values. It offers an 
  * interface to simply retrieve all keys and related data - to precache pipelines or layouts
  * - while querying for concrete subsets based on permutation values is left to the query
  * objects spawned by the index.
  * @note Each index reads one environment: one module, cooked for one profile. Variant keys are per
  * module, and each profile can cook a different subset of them, so a query needs both. */
class ManifestIndex
{
public:
    explicit ManifestIndex(manifest::EnvironmentView view);

    [[nodiscard]] const manifest::EnvironmentView& View() const noexcept;
    /** @brief Direct decode: "expand" a variant key into the values matching that key. `Active` is
      * read from the variant when this environment cooked the key, and is true otherwise. */
    [[nodiscard]] std::vector<QueryAxisValue> Decode(VariantKey key) const;
    /** @brief Returns every variant that exists, in (sorted) key order. Useful for total 
      * precaching of everything a manifest could generate as shader state */
    [[nodiscard]] std::vector<DecodedVariant> Enumerate() const;
    /** @brief Opens a new query, used to retrieve specific variants for actual runtime rendering or use */
    [[nodiscard]] ManifestQueryBuilder Query() const noexcept;
    /** @brief Returns the axis names of this module, in axis order. This is useful for building a query UI
      * or a preset. */
    [[nodiscard]] std::vector<std::string_view> AxisNames() const;
    /** @brief Returns every value declared by one axis, in digit order, or `UnknownAxis`. Values are listed
      * even when this environment cooked no variant that uses them. */
    [[nodiscard]] QueryResult<std::vector<QueryAxisValue>> AxisValues(std::string_view axis_name) const;
    /** @brief Returns the default the shader declares for one axis, or `UnknownAxis`. Graph code uses it to
      * fill an axis that it does not pin. */
    [[nodiscard]] QueryResult<QueryAxisValue> AxisDefault(std::string_view axis_name) const;

private:

    // Besides needing to get to the private functions, this also lets us read things like
    // axisNameToIndex + the manifest, both of which are nearly essential for query building.
    friend struct ManifestQueryBuilder;

    struct ScanConstraint
    {
        uint32_t AxisIndex;
        std::vector<uint32_t> AllowedValueIndices;
        PinMode Mode;
    };
    
    [[nodiscard]] std::vector<ScanConstraint> convertToScanConstraints(std::span<const QueryAxisRange> query) const;
    /** @brief Effectively the form and system that Query() uses when closed: every axis absent from input 
      * constraints is considered unconstrained and uses just the default value (canonical value, effectively) */
    [[nodiscard]] std::vector<VariantKey> select(std::span<const QueryAxisRange> constraints) const;
    [[nodiscard]] QueryAxisValue decodeAxis(uint32_t axis_index, uint32_t value_index) const noexcept;
    [[nodiscard]] std::vector<uint32_t> integralValueIndices(const uint32_t axis_index, const QueryAxisRange& range) const;
    [[nodiscard]] std::vector<uint32_t> stringValueIndices(const uint32_t axis_index, const QueryAxisRange& range) const;
    /** @brief Filters the keys based on the provided scan constraints - returns a view into manifest
      * that's better bounded based on the input constraints, to reduce iteration complexity. */
    [[nodiscard]] std::span<const VariantKey> filterKeys(std::span<const ScanConstraint> constraints) const;
    /** @brief True when every constraint matches the variant. A `Required` constraint needs its axis active
      * and the key's digit allowed. A `WhenActive` constraint needs the digit allowed only when its axis is
      * active. Inactive axes store their canonical digit, so testing the digit alone gives the wrong answer
      * for both modes. */
    [[nodiscard]] bool matches(uint32_t variant_index,
                               VariantKey key,
                               std::span<const ScanConstraint> constraints) const noexcept;
    void markInactiveAxes(uint32_t variant_index, std::span<QueryAxisValue> values) const noexcept;
    [[nodiscard]] std::vector<VariantKey> scan(std::span<const ScanConstraint> constraints) const;
    /** @brief The digit of one axis in a key, in the mixed radix of this module. */
    [[nodiscard]] uint32_t digitOf(VariantKey key, uint32_t axis_index) const noexcept;
    /** @brief Returns the axes whose digit differs between any two of the keys, in axis order. Distinct keys
      * differ in at least one digit, so the result is never empty for two or more keys. */
    [[nodiscard]] std::vector<uint32_t> varyingAxes(std::span<const VariantKey> keys) const;

    [[nodiscard]] std::vector<std::string_view> stringTableForAxis(uint32_t axis_index) const;
    manifest::EnvironmentView environment;
    /** @brief The values used by each module axis, in digit order. Module axes select their values from a
     * root axis through a mask, so these are not one contiguous run of the root table. */
    std::vector<std::vector<AxisValueType>> axisValues;
    std::vector<uint32_t> radices;
    std::vector<uint64_t> placeValues;
    // todo: maybe a packed vector (sorted) that we use std::find on might be better for our use case? (test)
    std::unordered_map<std::string_view, uint32_t> axisNameToIndex;
};

}

#endif // !LODESTONE_CLIENT_SHADER_MANIFEST_INDEX_HPP
