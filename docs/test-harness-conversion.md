# Task: convert the test suites to the new harness checks

For a delegate agent. Read `CLAUDE.md` and `.github/copilot-instructions.md` first, and obey both. Then read
`tests/TestHarness.hpp` and `tests/TestHarnessTests.cpp`. Section 2 of `docs/test-audit.md` explains why each helper
exists.

When you finish, write your report to `docs/test-harness-conversion-report.md`. Section 6 lists what it must contain.

## 1. Goal

A failed check must print the value it got. Today most checks are `runner.Check(a == b, "...")`, and a failure prints
only the text. Convert those checks to the helpers below. Do not change what a test proves. Do not add or remove a
claim. The only exception is section 4.

## 2. The helpers (already in `tests/TestHarness.hpp`)

| Helper | Use for |
|---|---|
| `runner.CheckEqual(actual, expected, claim)` | any `a == b`. Put the computed value first and the constant second. |
| `runner.CheckOk(result, claim)` | `result.has_value()` on a `std::expected` (`CookResult`, `ManifestResult`, `QueryResult`) |
| `runner.CheckError(result, code, claim)` | `!r.has_value() && r.error() == code`, and the same with `r.error().Code == code` for a `ManifestResult` |
| `runner.Check(condition, claim)` | everything else. It now returns the condition. |

A `claim` can be a literal, a `std::string_view`, or a `std::string` (for example, from `std::format`).

## 3. The conversion rules

1. **One comparison:**
   `runner.Check(x == 4u, "t")` becomes `runner.CheckEqual(x, 4u, "t")`.
   `runner.Check(x != y, "t")`: leave it as it is.
2. **A guard followed by a comparison:**
   `runner.Check(r.has_value() && r->Count == 3u, "t");`
   becomes:
   ```cpp
   if (runner.CheckOk(r, "t"))
   {
       runner.CheckEqual(r->Count, 3u, "t");
   }
   ```
   Keep the same claim text on both.
3. **An error check:**
   `runner.Check(!r.has_value() && r.error() == CookError::X, "t")` becomes `runner.CheckError(r, CookError::X, "t")`.
4. **A conjunction of comparisons**, such as `a == 1 && b == 2`: split it into one `CheckEqual` for each part, with
   the same claim. Do not split a conjunction that is not only `==` comparisons. Leave it, and list it in the report.
5. **A guard on its own line:** `runner.Check(x.has_value(), "t"); if (x.has_value()) { ... }` becomes
   `if (runner.CheckOk(x, "t")) { ... }`.
6. **A check inside a loop:** add the loop variable to the claim with `std::format`, for example
   `std::format("{}: each resource reads back", expected.Name)`. Add `#include <format>` if the file does not have it.
7. **A golden string comparison** that prints a diff by hand (`StageDumpTests.cpp`, `CheckSpaceDump`): replace the
   check and the manual print with one `CheckEqual`.
8. **Do not change the structure.** Do not move code, rename functions, or reorder sections. Do not reformat lines
   that you do not convert.
9. If a value type has no formatter, the failure prints `<no formatter>`. That is acceptable. List those types in the
   report.

## 4. The one behavior change you may make

The new harness fails a section that ran zero checks. If a conversion makes a section unreachable, or a test now fails
with "this section ran no checks", stop. Do not hide it. Put it in the report.

## 5. Order of work, and the build

Convert in batches. After each batch, build and run the tests:

```bash
scripts\build.bat RelWithDebInfo
```

```bash
scripts\run-tests.bat RelWithDebInfo
```

Read the exit code of each command itself. Do not pipe it into `tail` or `Select-Object`. Batches:

1. `SuggestTests`, `EnumTagDecodeTests`, `ContentInternerTests`, `ReflectionSchemaTests`, `DiagnosticParserTests`
2. `PermutationIndexTests`, `PermutationConstraintTests`, `PolicyDocumentTests`, `SymbolTableTests`
3. `ResolveStageTests`, `StageDumpTests`, `DedupeInfluenceTests`
4. `ShaderManifestRejectTests`, `MergedLayoutTests`, `ResourceReportTests`
5. `ManifestIndexTests` (the largest file, at 1300 lines: convert it alone)
6. `WgslValidatorTests`, `SpvValidatorTests`
7. Every Slang-cooking suite: `AccessModelReject`, `AxisDefault`, `BindingArray`, `CapabilityAxis`,
   `DerivedConstant`, `EntryPointConstants`, `ResourceRole`, `ShimCook`, `ShimExtern`, `ShimLinkSource`, `ShimModule`,
   `ShimTable`, `StorageFormat`, `StorageFormatAxis`, `SlangMetadataCanary`

`AttributeExpressionTests.cpp` is already converted. Use its `CheckValue` and `CheckError` as the model.

**Expected state:** `PermutationIndexTest` fails one check, "16 variants against a budget of 16". That is a known
product defect (`src/permute/PermutationSpace.cpp:426`), and the author decides the fix. Do not change `src/`. Any
other failure is yours: fix it or report it.

## 6. The report

Write `docs/test-harness-conversion-report.md` with these parts:

- The files converted, and the count of checks converted in each.
- Each conjunction or check you left unconverted, with the reason (`file:line`).
- Each type that printed `<no formatter>`.
- Each section that ran no checks, or each test that changed result.
- The last `run-tests.bat` output: the `[FAIL]` lines and the final line.
- Do not commit. The author reviews the diff.
