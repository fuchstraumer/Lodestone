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


/** @brief One value of a permutation axis. */
struct QueryAxisValue
{
    AxisValueDomain Type{ AxisValueDomain::None };
    AxisValueType IntegralValue{ 0u };
    // the implementation name of an interface axis, or the case name of an enum axis
    std::string_view Name;
    /** @brief Set by decoding. False when the variant does not use this axis. Not compared by `==`. */
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

/** @brief How a constraint treats variants where its axis is inactive (an `ActiveWhen` gate is off). */
enum class PinMode : uint8_t
{
    /** The axis must be active, so a pin on a gated axis also selects the gate. */
    Required,
    /** Variants where the axis is inactive also match. */
    WhenActive,
};

/** @brief The values allowed on one axis. The builder generates these. */
struct QueryAxisRange
{
    std::string_view AxisName;
    std::vector<QueryAxisValue> Values;
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

/** @brief A constraint that had no effect. Notes never fail a terminal. */
enum class QueryNoteCode : uint8_t
{
    PinAxisNotDeclared, // a preset pinned an axis that this module does not declare, and Apply skipped the pin
    PinAxisIrrelevant, // the query constrains an axis that cannot change any entry point given to NotesFor
    DuplicateValue, // a constraint names a value that the constraints on its axis already name
};

struct QueryNote
{
    QueryNoteCode Code{ QueryNoteCode::PinAxisNotDeclared };
    std::string AxisName;
};

/** @brief One pin of a `QueryPreset`. It owns its strings, so that one preset can serve many manifests. */
struct QueryPresetPin
{
    std::string AxisName;
    AxisValueDomain Domain{ AxisValueDomain::None };
    std::vector<AxisValueType> IntegralValues;
    std::vector<std::string> NamedValues;
    PinMode Mode{ PinMode::Required };
};

/** @brief Axis pins applied to the query of any module through `ManifestQueryBuilder::Apply`. A pin on an axis the
  * module does not declare is skipped with a `QueryNote`. A pin on a declared axis is checked as `Where` checks it. */
class QueryPreset
{
public:
    void Pin(std::string_view axis_name, bool value, PinMode mode);
    void Pin(std::string_view axis_name, uint32_t value, PinMode mode);
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

template<typename T>
using QueryResult = std::expected<T, QueryErrorCode>;

struct DecodedVariant
{
    VariantKey Key{};
    std::vector<QueryAxisValue> Values;
};

/** @brief A declarative variant query. Use it to build a render graph or pipeline caches, not per frame. */
struct ManifestQueryBuilder
{
    ManifestQueryBuilder(const class ManifestIndex& index) noexcept;
    
    /** @brief Each call returns a new builder, so that one query can branch into several. */
    [[nodiscard]] ManifestQueryBuilder Where(std::string_view axis_name, bool value) const noexcept;
    [[nodiscard]] ManifestQueryBuilder Where(std::string_view axis_name, uint32_t value) const noexcept;
    /** @brief `domain` is required: a value name alone cannot tell an interface axis from an enum axis. */
    [[nodiscard]] ManifestQueryBuilder Where(std::string_view axis_name,
                                             AxisValueDomain domain,
                                             std::string_view type_or_enum_name) const noexcept;

    /** @brief `Where` with `PinMode::WhenActive`. */
    [[nodiscard]] ManifestQueryBuilder WhereWhenActive(std::string_view axis_name, bool value) const noexcept;
    [[nodiscard]] ManifestQueryBuilder WhereWhenActive(std::string_view axis_name, uint32_t value) const noexcept;
    [[nodiscard]] ManifestQueryBuilder WhereWhenActive(std::string_view axis_name,
                                                       AxisValueDomain domain,
                                                       std::string_view type_or_enum_name) const noexcept;

    /** @brief Applies each pin of `preset`, in order. Keep the preset alive while the builder is in use: the builder
      * keeps views into its strings. */
    [[nodiscard]] ManifestQueryBuilder Apply(const QueryPreset& preset) const;

    [[nodiscard]] ManifestQueryBuilder WhereAnyOfBoolean(std::string_view axis_name) const;
    [[nodiscard]] ManifestQueryBuilder WhereAnyOf(std::string_view axis_name, std::span<const uint32_t> values) const;
    [[nodiscard]] ManifestQueryBuilder WhereAnyOf(std::string_view axis_name,
                                                  AxisValueDomain domain,
                                                  std::span<const std::string_view> values) const;

    [[nodiscard]] ManifestQueryBuilder WhereNoneOf(std::string_view axis_name, bool value) const noexcept;
    [[nodiscard]] ManifestQueryBuilder WhereNoneOf(std::string_view axis_name, std::span<const uint32_t> values) const noexcept;
    [[nodiscard]] ManifestQueryBuilder WhereNoneOf(std::string_view axis_name,
                                                   AxisValueDomain domain,
                                                   std::span<const std::string_view> values) const noexcept;

    [[nodiscard]] QueryResult<std::vector<VariantKey>> Keys() const noexcept;
    [[nodiscard]] QueryResult<std::vector<DecodedVariant>> Variants() const noexcept;
    /** @brief A cheaper `Variants()` when `Keys()` already ran. */
    [[nodiscard]] std::vector<DecodedVariant> VariantsFromKeys(const std::vector<VariantKey>& keys) const noexcept;
    /** @brief Returns the key when exactly one variant matches. `AmbiguousVariant` lists the varying axes in
      * `AxisName` and the match count in `Detail`. */
    [[nodiscard]] std::expected<VariantKey, QueryError> Single() const noexcept;
    /** @brief `Single` for a pipeline of the named entry points. Axes that cannot change any of them
      * (`EnvironmentView::IsAxisRelevant`) do not count, and the smallest of the matching keys is returned. */
    [[nodiscard]] std::expected<VariantKey, QueryError> SingleFor(
        std::span<const std::string_view> entry_point_names) const noexcept;

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] std::span<const QueryError> Errors() const noexcept;
    [[nodiscard]] std::span<const QueryNote> Notes() const noexcept;
    /** @brief `Notes()` plus a `PinAxisIrrelevant` note for each constrained axis that cannot change any of the named
      * entry points. */
    [[nodiscard]] std::expected<std::vector<QueryNote>, QueryError> NotesFor(
        std::span<const std::string_view> entry_point_names) const;
private:
    [[nodiscard]] std::optional<uint32_t> resolveAndValidate(ManifestQueryBuilder& result,
                                                             std::string_view axis_name,
                                                             std::span<const QueryAxisValue> values) const;
    // a merge joins the values, and is WhenActive when either constraint is
    void insertConstraint(std::string_view axis_name,
                          uint32_t axis_index,
                          std::vector<QueryAxisValue> allowed,
                          PinMode mode);

    [[nodiscard]] std::expected<VariantKey, QueryError> singleOver(std::span<const uint64_t> relevant_axes) const noexcept;
    [[nodiscard]] std::expected<std::vector<uint64_t>, QueryError> joinRelevance(
        std::span<const std::string_view> entry_point_names) const;
    // WhereNoneOf builds a complement, so it does not call this
    void noteDuplicateValues(std::string_view axis_name,
                             uint32_t axis_index,
                             std::span<const QueryAxisValue> values);

    [[nodiscard]] ManifestQueryBuilder whereAnyOf(std::string_view axis_name,
                                                  std::vector<QueryAxisValue> value,
                                                  PinMode mode) const noexcept;
    const class ManifestIndex* index{ nullptr };
    std::vector<QueryError> errors;
    std::vector<QueryNote> notes;
    // insertion order. Select sorts the scan constraints by axis index
    std::vector<QueryAxisRange> constraints;
};

/** @brief Returns the candidate closest to a mistyped name, or an empty view. The edit budget is one edit per three
  * characters of `input`. Public so that renderers give the same "did you mean" as the query errors. */
[[nodiscard]] std::string_view NearestName(std::string_view input, std::span<const std::string_view> candidates) noexcept;

/** @brief The query index of one environment: one module, cooked for one profile. Variant keys are per module, and
  * each profile can cook a different subset. */
class ManifestIndex
{
public:
    explicit ManifestIndex(manifest::EnvironmentView view);

    [[nodiscard]] const manifest::EnvironmentView& View() const noexcept;
    /** @brief Returns the axis values of a key. `Active` is true for a key this environment did not cook. */
    [[nodiscard]] std::vector<QueryAxisValue> Decode(VariantKey key) const;
    /** @brief Returns every cooked variant, in key order. */
    [[nodiscard]] std::vector<DecodedVariant> Enumerate() const;
    [[nodiscard]] ManifestQueryBuilder Query() const noexcept;
    /** @brief Returns the axis names, in axis order. */
    [[nodiscard]] std::vector<std::string_view> AxisNames() const;
    /** @brief Returns every declared value of one axis, in digit order, cooked or not. */
    [[nodiscard]] QueryResult<std::vector<QueryAxisValue>> AxisValues(std::string_view axis_name) const;
    /** @brief Returns the default the shader declares for one axis. */
    [[nodiscard]] QueryResult<QueryAxisValue> AxisDefault(std::string_view axis_name) const;

private:

    friend struct ManifestQueryBuilder;

    struct ScanConstraint
    {
        uint32_t AxisIndex;
        std::vector<uint32_t> AllowedValueIndices;
        PinMode Mode;
    };
    
    [[nodiscard]] std::vector<ScanConstraint> convertToScanConstraints(std::span<const QueryAxisRange> query) const;
    /** @brief Axes without a constraint take their canonical value. */
    [[nodiscard]] std::vector<VariantKey> select(std::span<const QueryAxisRange> constraints) const;
    [[nodiscard]] QueryAxisValue decodeAxis(uint32_t axis_index, uint32_t value_index) const noexcept;
    [[nodiscard]] std::vector<uint32_t> integralValueIndices(const uint32_t axis_index, const QueryAxisRange& range) const;
    [[nodiscard]] std::vector<uint32_t> stringValueIndices(const uint32_t axis_index, const QueryAxisRange& range) const;
    /** @brief Returns the bounded run of keys that can match `constraints`. */
    [[nodiscard]] std::span<const VariantKey> filterKeys(std::span<const ScanConstraint> constraints) const;
    /** @brief Tests the axis-active mask as well as the digit: an inactive axis stores its canonical digit. */
    [[nodiscard]] bool matches(uint32_t variant_index,
                               VariantKey key,
                               std::span<const ScanConstraint> constraints) const noexcept;
    void markInactiveAxes(uint32_t variant_index, std::span<QueryAxisValue> values) const noexcept;
    [[nodiscard]] std::vector<VariantKey> scan(std::span<const ScanConstraint> constraints) const;
    [[nodiscard]] uint32_t digitOf(VariantKey key, uint32_t axis_index) const noexcept;
    /** @brief Returns the axes whose digit differs between any two of the keys, in axis order. */
    [[nodiscard]] std::vector<uint32_t> varyingAxes(std::span<const VariantKey> keys) const;

    [[nodiscard]] std::vector<std::string_view> stringTableForAxis(uint32_t axis_index) const;
    manifest::EnvironmentView environment;
    /** @brief The values of each module axis, in digit order, selected from the root axis through its mask. */
    std::vector<std::vector<AxisValueType>> axisValues;
    std::vector<uint32_t> radices;
    std::vector<uint64_t> placeValues;
    // todo: maybe a packed vector (sorted) that we use std::find on might be better for our use case? (test)
    std::unordered_map<std::string_view, uint32_t> axisNameToIndex;
};

}

#endif // !LODESTONE_CLIENT_SHADER_MANIFEST_INDEX_HPP
