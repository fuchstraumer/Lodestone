#include "permute/ExternConstantScanner.hpp"
#include "TestHarness.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

// A link-time constant never reaches a program layout, so Slang cannot report one. This scanner reads
// the declaration out of the source text, and the value it reads becomes a `AttrExprSymbol`. A size
// expression that names one then decides how large a buffer the engine allocates.
//
// A misread declaration therefore gives a wrong allocation and no other symptom: the cook exits 0, and
// no round trip disagrees, because every side reads the same wrong number. Nothing compares this
// answer against a second opinion, so the tests below are the only check that exists.

using lodestone::DeclaresExternConstantNamed;
using lodestone::ExternConstantDeclaration;
using lodestone::ScanExternConstants;
using lodestone::TrimWhitespace;

namespace
{

constexpr std::string_view k_Source = R"(module OceanFft;

extern static const uint IFFT_SIZE = 256;
extern static const bool IFFT_USE_WAVE_OPS = false;
extern static const uint IFFT_NUM_WAVE_CASCADES = IFFT_SIZE * 4;

static const uint NOT_EXTERN = 8;
extern static const uint IFFT_SIZE_LOG2;

RWStructuredBuffer<float4> OutputSpectrum;
)";

/** True when `index` holds a declaration of `name` whose value text trims to `value`. */
bool DeclarationMatches(const std::vector<ExternConstantDeclaration>& found,
                        size_t index,
                        std::string_view name,
                        std::string_view value)
{
    return index < found.size() && found[index].Name == name &&
           TrimWhitespace(found[index].ValueText) == value;
}

} // namespace

int main()
{
    lodestone::tests::TestRunner runner{ "ExternConstantScannerTests" };

    const std::vector<ExternConstantDeclaration> found = ScanExternConstants(k_Source);

    runner.BeginSection("the scan reads every declaration and no more");
    runner.Check(found.size() == 3u, "a line needs both `extern` and `=` to be a declaration");

    runner.BeginSection("the scan reads the name and the value text");
    runner.Check(DeclarationMatches(found, 0u, "IFFT_SIZE", "256"), "an integer default reads back");
    runner.Check(DeclarationMatches(found, 1u, "IFFT_USE_WAVE_OPS", "false"), "a bool default reads back");
    runner.Check(DeclarationMatches(found, 2u, "IFFT_NUM_WAVE_CASCADES", "IFFT_SIZE * 4"),
                 "a default that names an earlier constant keeps its whole expression");

    runner.BeginSection("the name is the identifier that ends at the `=`");
    const std::vector<ExternConstantDeclaration> spaced =
        ScanExternConstants("extern static const uint SPACED   =   16 ;\n");
    runner.Check(spaced.size() == 1u && spaced.front().Name == "SPACED",
                 "spaces between the name and the `=` are not part of the name");
    runner.Check(spaced.size() == 1u && TrimWhitespace(spaced.front().ValueText) == "16",
                 "the value text stops at the `;`, and the trim removes the rest");

    runner.BeginSection("a name check matches a whole identifier only");
    runner.Check(DeclaresExternConstantNamed(k_Source, "IFFT_SIZE"), "a declared name is found");
    runner.Check(DeclaresExternConstantNamed(k_Source, "IFFT_SIZE_LOG2"),
                 "a declaration with no `=` still declares the name");
    runner.Check(!DeclaresExternConstantNamed(k_Source, "IFFT_SIZ"),
                 "a prefix of a declared name is not a match");
    runner.Check(!DeclaresExternConstantNamed(k_Source, "NOT_EXTERN"),
                 "a name on a line without `extern` is not a declaration");
    runner.Check(!DeclaresExternConstantNamed(k_Source, "OutputSpectrum"),
                 "a resource is not an extern constant");

    // `IFFT_SIZE` is a whole identifier inside the default of `IFFT_NUM_WAVE_CASCADES`, and that line
    // holds `extern`. The check must not read a use as a declaration, so it has to fail here if the
    // real declaration is removed.
    runner.BeginSection("a use is not a declaration");
    runner.Check(!DeclaresExternConstantNamed("extern static const uint OTHER = IFFT_SIZE * 4;\n",
                                              "IFFT_SIZE"),
                 "a name used in the default of another constant does not declare it");

    runner.BeginSection("nothing to read gives nothing back");
    runner.Check(ScanExternConstants("").empty(), "empty source has no declaration");
    runner.Check(!DeclaresExternConstantNamed("", "IFFT_SIZE"), "empty source declares no name");
    runner.Check(TrimWhitespace("   ").empty(), "text that is all spaces trims to nothing");

    return runner.Report();
}
