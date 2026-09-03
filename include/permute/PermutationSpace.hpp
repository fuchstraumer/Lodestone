#pragma once
#ifndef LODESTONE_PERMUTATION_SPACE_HPP
#define LODESTONE_PERMUTATION_SPACE_HPP
#include "CookerErrors.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationAxis.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lodestone
{

class PermutationSpace;
class DiagnosticSink;

struct ExternConstantDefault
{
    std::string Name;
    int64_t Value{ 0 };
};

/**
 * One variant's identity.
 *
 * `Active`: Contains only the axes this variant actually uses. The linker uses this to export the final
 * values (i.e., what shows up in source)
 *
 * `Canonical`: A complete list of EVERY axis in the permutation space, in declaration order. This is
 * *not* what the shader will actually use.
 *
 * If an axis is disabled, it will not appear in Active but it will appear in Canonical. This happens with
 * an axis (or axes) whose parent does not match the value required to enable it. The permutation evaluator
 * will fill in the first value of that axis as the value in Canonical.
 *
 * Why? Because this then provides a truly stable index for the variant, which several properties/values we
 * associate with a variant are computed. This also makes it simpler for further retrieval: we don't need to
 * know the fully evaluated "correct" value of each axis to retrieve it, we can just use our unique values
 * and the canonicalized values to retrieve the variant. Think how trivial that is: if you know just the
 * set of values you want to use, you can get your variant.
 */
struct VariantDescriptor
{
    PermutationAssignment Active;
    CanonicalAssignment Canonical;
    int32_t Index{ 0 };
};

/** Everything one permutation space expands to. `SpaceSize` counts the dense index range, holes
 * included: a disabled dependent axis leaves gaps, and the plan accepts them rather than pay for a
 * lookup structure. */
struct VariantSet
{
    const PermutationSpace* Space{ nullptr };
    std::vector<VariantDescriptor> Variants;
    int32_t SpaceSize{ 0u };
};

class PermutationSpace
{
public:
    PermutationSpace(std::string _name,
                     std::span<const PermutationAxis> _axes) noexcept;
    // same as permutation axis: this is just to keep our nasty gross internal constructors
    // alive until we complete the next round of work to get data-driven permutations
    PermutationSpace(std::string _name,
                     std::initializer_list<PermutationAxis> _axes) noexcept;
    ~PermutationSpace() noexcept = default;

    /** The space owns its axes, and `PermutationBinding` points into them. A copy would leave every
     * binding of the original aimed at a different object, so a space is moved and never copied. A
     * move keeps the vector's buffer, so the axis addresses survive it. */
    PermutationSpace(const PermutationSpace&) = delete;
    PermutationSpace& operator=(const PermutationSpace&) = delete;
    PermutationSpace(PermutationSpace&&) noexcept = default;
    PermutationSpace& operator=(PermutationSpace&&) noexcept = default;

    [[nodiscard]] std::string_view Name() const noexcept;
    [[nodiscard]] std::span<const PermutationAxis> Axes() const noexcept;
    [[nodiscard]] std::size_t AxisCount() const noexcept;
    [[nodiscard]] bool IsEmpty() const noexcept;

    [[nodiscard]] CookResult<std::vector<PermutationAssignment>> EnumerateActiveCombinations(DiagnosticSink& sink) const;
    [[nodiscard]] CookResult<VariantSet> EnumerateVariants(DiagnosticSink& sink) const;
    [[nodiscard]] CanonicalAssignment CanonicalizeAssignment(const PermutationAssignment& assignment) const;
    [[nodiscard]] int32_t ComputeVariantIndex(const CanonicalAssignment& canonical) const;
    [[nodiscard]] int32_t ComputeVariantSpaceSize() const noexcept;
    /**Every axis name must match an `extern static const` declaration in the shader. A mismatch links a
     * symbol nobody references, leaves the shader on its default, and errors nowhere -- this will result in
     * a set of variants with duplicate source code and behavior, when we explicitly don't want that. */
    [[nodiscard]] CookError VerifyAxisNamesAreDeclared(std::span<const std::string_view> source_texts,
                                                       std::string_view module_name) const;
    /**The other direction: an `extern` constant that no axis drives keeps its default in every variant.
     * This is what our resource sizing annotations rely on, in the Slang compiler machinery (though they
     * don't actually affect source code: they just carry through to the data we extract still) */
    void ReportUndrivenExternConstants(std::span<const std::string_view> source_texts,
                                       std::string_view module_name) const;
    [[nodiscard]] CookResult<std::vector<ExternConstantDefault>> CollectUndrivenExternDefaults(
        std::span<const std::string_view> source_texts, DiagnosticSink& sink) const;
private:
    std::string name;
    std::vector<PermutationAxis> axes;
};

} // namespace lodestone

#endif // !LODESTONE_PERMUTATION_SPACE_HPP
