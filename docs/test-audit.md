# Test audit

Audit of `tests/` on 2026-10-23, at commit `e746d3c`. The auditor read every suite, the harness, `tests/CMakeLists.txt`,
and `scripts/run-tests.bat`. Nothing was built or run. A line number refers to that commit.

`docs/` is in `.gitignore`. Force-add this file to keep it: `git add -f docs/test-audit.md`.

## Status on 2026-10-23 (end of the first audit session)

Done (43 targets green, RelWithDebInfo):

- §2 harness: `Claim`, `Check` returns bool, `CheckEqual`, `CheckOk`, `CheckError`, scoped sections and contexts,
  empty sections fail, failures on stderr. `tests/TestFormat.hpp` prints enums (ToString, magic_enum, flag bits), ranges,
  optionals, variants, tuples, and aggregates (Boost.PFR field counting). `TestHarnessTest` proves each helper can fail.
  §2.7 (per-section diagnostic capture) is not done.
- Every suite converted to the helpers (delegates, report in `docs/test-harness-conversion-report.md`).
- §3.1 `RunCook(options, sink, diagnostics)`, and every cooker message goes through the sink (no `std::println(stderr)`
  outside `Diagnostics.cpp` and `ParseCommandLine`). §3.2 `CookedBundle::Result` is a `CookError`. §3.5 not done (rename).
- §4 all four test defects fixed. `MaxVariants` off-by-one fixed in `PermutationSpace.cpp`.
- §5: diagnostics content checked in `AccessModelRejectTest` (each rejection names its module), `ShimExternTest`
  (file, line, extern), `PolicyDocumentTest`. Known-answer keys and the partial-assignment check in
  `PermutationIndexTest`. `EnumAxisTest` replaced `EnumAxisCookTest`.
- §6.1 `CookerOptionsTest`. It found two parser bugs, both fixed: a missing `--policy-file` was ignored, `--O2x` read as 2.
- §7 dead code deleted (`tests/scripts/`, `tests/assets/common/`, `DeclKindProbe`). The other §7 rows are open.

Open: §2.7, §3.3 and §3.4 (shared cook helpers and rejection-table runner), §3.5, §3.6 (`run-tests.bat` reads
`TEST_ARGS` from a file, shows cook failures), §3.7 (fixed temp folder names), the rest of §5 (validator mismatch
attribution, ShimTable negative rows, Require pruning cell, Suggest ordering, interner bucket walk, no short circuit,
VariantsFromKeys content, StageDump substring), §6.2 (manifest byte-flip sweep), the rest of §7, §8 (keystone labels).

The questions for each test:

1. Can it fail? A check that cannot fail proves nothing.
2. When it fails, does the output say what went wrong without a debugger?
3. Does it prove the claim that its description, or `CLAUDE.md`, makes?
4. Does another test already prove the same claim?

---

## 1. Summary

1. **A failed check never prints the value it got.** `Check` takes a `bool`. About 760 checks exist. Some put loop
   context into the description with `std::format`, and none print the actual value. A failure says "two of three integral values is two thirds", and not the count. This is the
   largest single cost to whoever debugs a failure. Section 2 has the fix.
2. **A cook test cannot read its own diagnostics.** `RunCook` makes a `StderrDiagnosticSink` itself
   (`src/driver/steps/PrepareCookStep.cpp:50`). So no cook-level test can check that an error names the file, the line,
   or the axis, and every expected rejection prints to stderr. `AxisDefaultTests.cpp:166` says so: "the test cannot
   read stderr". Section 3.1.
3. **Four test defects.** A falsifiability check passes on an error, undefined behavior in a failure path, a null
   dereference, and a mislabeled section. Section 4.
4. **One likely product defect that no test covers.** `MaxVariants = N` rejects a space of exactly N variants.
   Section 4.
5. **About fifteen claims are stated and not proved.** In most of them, a negative test accepts any failure, so it
   can pass for the wrong reason. Section 5.
6. **Two ingestion surfaces have little coverage.** These are the command line and the manifest reader under random
   damage. Section 6.
7. **Prune about 4,300 tracked lines.** About 4,100 of them are dead velox Python and Slang. Section 7.
8. **Keystone list.** Section 8.

---

## 2. The harness

`TestRunner` is about 110 lines. It is a sound base: section headings print only on a failure, and each failure prints
its `source_location`. Each change below adds diagnostic power, and none of them needs a framework. Together they are
the argument against gtest: the output becomes better than gtest's, for a smaller cost.

### 2.1 `CheckEqual`: print both values

```cpp
template<typename Actual, typename Expected>
bool CheckEqual(const Actual& actual, const Expected& expected, Claim claim);
```

On a failure it prints `expected: X` and `actual: Y`. The template formats each value through one function
`FormatForTest`, which takes the first of these that compiles:

1. `std::formattable<T, char>`. This covers integers, strings, and ranges of them. MSVC 14.51 has range formatting.
2. An ADL `ToString(value)`. Every repository enum has one: `CookError`, `BindingKind`, `ResourceShape`, and
   `manifest::ErrorCode`, if it has one.
3. `std::to_underlying` for any other enum.
4. The text `<no formatter>`. This keeps the call compiling, so a new type does not block a test.

`CheckEqual` is a template, so it goes in the header. The style rule allows that for a template.

Most of the 760 checks are `a == b`, so they convert mechanically. A check of the form `x.has_value() && x->F == v`
becomes a `CheckOk` and a `CheckEqual`. Section 2.3 shows the guard that this needs.

### 2.2 `CheckOk` and `CheckError` for result types

```cpp
template<typename T, typename E> bool CheckOk(const std::expected<T, E>& result, Claim claim);
template<typename T, typename E> bool CheckError(const std::expected<T, E>& result, E expected, Claim claim);
```

`CheckOk` prints `ToString(result.error())` when the result failed. `CheckError` prints the error that came back, or
`succeeded` when it did not fail. This covers `CookResult`, `ManifestResult`, and `QueryResult`. For example,
`runner.Check(cooked.has_value(), "the module cooks")` drops the `CookError` today. It is the most common check in the
Slang suites.

### 2.3 `Check` returns its condition

Change the return type to `bool`, and do not mark it `[[nodiscard]]`. The guard then becomes the check:

```cpp
if (runner.CheckOk(bundle, "the manifest opens"))
{
    ...
}
```

This removes the pattern of a `Check` followed by an `if` on the same condition. It also removes the silent skip in
section 2.4.

### 2.4 A section with no checks fails

Count the checks of each section. In `Report`, fail every section that ran zero checks, and fail a suite that ran zero
checks. This is the structural fix for the **silent skip**: `if (x.has_value()) { checks }` with no check of `x`. When
`x` fails, the section passes with nothing proved. These sites skip silently today:

| Site | What is skipped |
|---|---|
| `ManifestIndexTests.cpp:890` | the damaged default digit check, when the bundle does not open |
| `ManifestIndexTests.cpp:964` | the whole "VariantsFromKeys agrees with Variants" section |
| `MergedLayoutTests.cpp:405` | the vertex-and-compute pair check |
| `ShaderManifestRejectTests.cpp:465` | "an extent read on its own opens against that bundle" |
| `ShaderManifestRejectTests.cpp:605` | the "not recorded" pair check |
| `ShimCookTests.cpp:44` | every resource comparison, when a bundle does not open |
| `StorageFormatAxisTests.cpp:101` | every subject-to-control comparison, when a bundle does not open |
| `PolicyDocumentTests.cpp:288` | the "no target sections" check |

This is the rule "prefer the structure where the wrong thing cannot be written down", applied to the tests.

### 2.5 Scoped sections

`BeginSection` sets a label that stays until the next call. `TestExternConstExtraction` (`SymbolTableTests.cpp:262`)
has no `BeginSection`, so its failures print under "accumulation and unknown module". Make a section an RAII object
(`const auto section = runner.Section("...");`) that restores the parent label when it leaves scope. Then a function
cannot inherit a stale label.

### 2.6 Context for loops

A check inside a loop has one description for every pass. `BindingArrayTests.cpp` checks six resources with the text
"each resource reads back with its kind and the length", and a failure does not say which resource failed. Add a scoped
context that prints on a failure:

```cpp
const auto context = runner.Context(std::format("resource {}", expected.Name));
```

`std::format` into the description also works today. The scoped form keeps the claim text stable, and the claim text
is what a reader searches for.

### 2.7 Diagnostics that print only on a failure

Many suites send expected rejections to a `StderrDiagnosticSink`. A passing run then prints pages of errors, and
`CLAUDE.md` tells the reader to "read the last line". Give the runner a `RecordingDiagnosticSink` for each section,
through `runner.Diagnostics()`. On a failure, print the records of that section after the failure. On a pass, discard
them. A passing suite then prints one line, and a failing suite prints the compiler's own explanation beside the
failure.

### 2.8 Output order

Failures go to stdout, and diagnostics go to stderr. When both go to one file, the two streams interleave out of order,
so a `FAIL` line can appear far from the diagnostic that explains it. Write failures to stderr, or flush after each
one.

### 2.9 `Claim` keeps `source_location` working with formatted text

A variadic `Check(condition, format, args...)` cannot also take a defaulted `std::source_location`. A small type
solves this:

```cpp
struct Claim
{
    Claim(std::string_view text, std::source_location location = std::source_location::current());
    ...
};
```

This is a default argument. It is the one place where nothing else works, because `source_location::current()` gives
the caller's location only as a default argument. Use a `Claim` for every description, so the change happens once.

### 2.10 Do not add

Do not add test registration, filters, fixtures, or a test discovery macro. One executable for each suite, with
`main`, is easy to debug and easy to read. Keep it.

---

## 3. Cook-level test infrastructure

### 3.1 `RunCook` needs a diagnostic sink parameter

Add `RunCook(CookerOptions, OutputSink&, DiagnosticSink&)`. Keep the two-argument form as an overload that passes a
`StderrDiagnosticSink`, and do not use a default argument. `SharedCookState::Diagnostics` is a `unique_ptr` today, so
the step chain needs a non-owning pointer instead. DiamondDogs request C3 (live cook) needs the same seam.

With the sink, these claims become provable. Each one is unproved today:

- `ShimExternTests.cpp:107`: "fails the cook, naming the line"
- `PolicyDocumentTests.cpp:284`: "rejected by name"
- `ShimTableTests.cpp`: "each fails with its own Slang error"
- `AxisDefaultTests.cpp:166`: the warning for a dropped default
- The repository rule "an error message is user feedback first: name the file, the entry point, and the value". No
  test checks any cook diagnostic for those fields.

### 3.2 Keep the `CookError` in `CookedBundle`

`CookComparison.hpp` stores `bool Cooked`. Store the `CookError`. One change there gives every comparison suite an
error name on a failure.

### 3.3 One cook-and-open helper

Nine files repeat this sequence: cook into a `MemoryOutputSink`, copy `k_ManifestFileName` into a byte vector, open the
bundle, open the environment. Each copy guards the result differently. Three of them define their own `ManifestBytes`.
Move all of it to `CookComparison.hpp`, or to a `TestCook.hpp` beside it, as functions that take the runner and check
each step with `CheckOk`.

### 3.4 One rejection-table runner

`AccessModelRejectTests`, `AxisDefaultTests`, `CapabilityAxisTests`, `DerivedConstantTests`, and `ResourceRoleTests`
each define a table of (module, expected error, claim) and the same loop. That loop prints
"expected X, and the cook returned Y". Write it once. With section 3.1, the shared runner can also check that a
diagnostic names the module file.

### 3.5 Rename `AccessModelRejectTest`

It has nine rows. Five of them have no relation to the access model: the codegen error, the interface axis resource,
and three storage format axis rows. Name it `CookRejectTest`. `CLAUDE.md` calls it "the table of cooks that must fail"
already.

### 3.6 `run-tests.bat` hides cook test failures

A unit test that fails runs again with its output shown. A cook test runs once with `>nul 2>&1` and prints only
`[FAIL]`. The seven cook command lines are also written twice, once in `tests/CMakeLists.txt` (`TEST_ARGS`) and once in
the script, and the two copies can drift.

Fix: in `add_lodestone_unit_test`, write the `TEST_ARGS` of each test to `<name>.args` beside the executable. The
script then reads that file for every test, and runs each failure again with output. The skip list and the seven
blocks go away. A new cook test then needs one CMake line, not two edits.

### 3.7 Parallel runs collide

Five suites use a fixed folder under `%TEMP%`. These are `LodestoneStaleManifestTest`, `LodestoneShimExternTest`,
`LodestoneShimTableTest`, `LodestoneStorageFormatAxisTest`, and `LodestoneStorageFormatTest`. Some of them
`remove_all` that folder. `ctest -j` can therefore make one suite delete the files of another. Add the process ID to
each name, or use one helper that makes a unique folder.

---

## 4. Defects

### In the tests

| Site | Defect | Effect |
|---|---|---|
| `DedupeInfluenceTests.cpp:433` | `finalResult.has_value() ? !finalResult.value() : true` | The falsifiability check **passes when the helper returns an error**. Write `CheckOk` and then a check of `!*finalResult`. |
| `StorageFormatAxisTests.cpp:119` | `std::unexpected(controlBundle.error())` on a `controlBundle` that has a value | Undefined behavior. It runs only when a control module is not found, which is the path where clean output matters most. |
| `ShimTableTests.cpp:113` to `126` | `*readBuffer` after `FindShimInterface` can return null | A crash, not a failure, if the table loses `IReadBuffer`. |
| `SymbolTableTests.cpp:262` | no `BeginSection` | Failures print under the wrong section heading. |
| `AttributeExpressionTests.cpp:48` | `static int64_t diagCount` assumes one diagnostic per call | A call that reports two diagnostics makes the next check pass even when that check emits none. Read `FailureCount()` before the call and compare after it. |

### In the product: likely, and untested

`src/permute/PermutationSpace.cpp:426`: `expanded.size() >= policy.MaxVariants` runs after the `emplace_back`. A
budget of N therefore fails on the N-th variant, so a space of exactly N variants is rejected. The message says
"max N variants". No test enforces `MaxVariants`. `PolicyDocumentTest` only reads the value back. Add a test with a
space of 4 at budgets 3, 4, and 5 before you change the code, so that the test fails first.

---

## 5. Claims stated and not proved

A negative test that accepts any failure can pass for the wrong reason: a missing file, a typo in the asset, or an
unrelated Slang error.

| Test | Claim | What the check proves | Fix |
|---|---|---|---|
| `EnumAxisCookTest` | (CMake comment) a cook that read "case index == case value" makes wrong data | Exit code only. With digit 0 linked as value 0 and digit 2 as value 2, `Low` and `Medium` both take the `else` branch. The axis stays Active through `High`, and every validator passes. | Check from the manifest that the three `QUALITY` variants with `DITHER` fixed have three different sources. |
| `ShimCookTest` | `ShimLocal.slang` must not cook | `!local.Cooked`. A missing file also passes. | `CheckError` with the expected code. With section 3.1, check the diagnostic too. |
| `ShimTableTest` | three disagreeing tables, each with its own Slang error | `!CookAgreementModule(...)` | Return the `CookError`, and check the Slang code in the diagnostics after section 3.1. |
| `SpvValidatorTest`, `WgslValidatorTest` | each mutation is a mismatch of that kind | `!Matches && !Report.empty()`. Any mismatch passes. | Check that the report names the mutated binding. The deeper fix is in the code: `BindingComparison::Report` is a formatted string. The rule "a stage builds the record, a sink decides what it looks like" asks for a list of mismatch records (binding, field). Tests could then compare records. This is an item for the code audit. |
| `PermutationConstraintTest` | `Require` drops exactly the 32x128 cell | the variant count is 8 | Check that no variant has `TILE=32` and `REG=128`. |
| `PermutationIndexTest` | the index is "stable" (`CLAUDE.md`) | nothing pins a key value | Add known-answer keys: three assignments and their exact keys. A change to the packing order must fail here. |
| `PermutationIndexTest` | a partial assignment resolves to one variant | it matches *some* variant | Check that it is the variant with `TEST_SIZE=512` and wave operations off. |
| `SuggestTest` | the nearest names come first | one candidate is in range | Use two candidates, at distances 1 and 2. |
| `ContentInternerTest` | an equal payload collapses | it repeats payload 0, the head of the bucket | Repeat payload 7, so the scan must walk past entries that do not match. |
| `AttributeExpressionTest` | `&&` does not short circuit (`docs/code-notes.md` §6) | not tested | `0 && (1 / 0)` must fail with `DivideByZero`. |
| `ManifestIndexTest` | `VariantsFromKeys` agrees with `Variants` | the counts agree | Compare the decoded values. |
| `StageDumpTest` | provenance names variant 1 | substring `"variantIndex": 1`, which also matches 10 to 19 | Match `"variantIndex": 1,` or parse the dump. |
| `BindingArrayTest` | an unbounded array fails "until F4" | `ReflectionMismatch`, which is an effect of the gap and not a named rejection | Use a dedicated error when the SPIR-V reader meets a runtime array. F4 then deletes one row, and does not have to reason about a mismatch. |
| `ShimExternTest` | "fails the cook, naming the line" | the error code | Read `sink.Records()`. This one needs no section 3.1, because the suite already has a recording sink. |
| `PolicyDocumentTest` | "rejected by name" | the error code | Read `sink.Records()`. This one also has a recording sink already. |

---

## 6. Coverage gaps on ingestion surfaces

"Validate at the ingestion surface, then trust inside" puts the weight of the argument on those surfaces. Two of them
are thin.

### 6.1 The command line

`ParseCommandLine` is tested only in `StageDumpTests.cpp`: `--dump-stage`, and the case with no target. These are not
tested: an unknown `--target` (which `CLAUDE.md` says "is rejected at the command line and never in the driver"), a
missing `-o`, a missing module path, a repeated `--include-dir`, an unknown flag, `--O0` to `--O3`, and `--policy-file`.
Add a `CookerOptionsTest`. It needs no Slang and takes milliseconds. Move `CheckCommandLine` out of `StageDumpTest`
into it.

### 6.2 The manifest reader under damage

`ShaderManifestRejectTest` damages a hand-picked set of fields, and each one is good. The reader promises more: `Open`
validates every index once, so that no view can read out of bounds afterwards. One test can prove that promise:

- For each byte of the small valid manifest, flip it.
- Call `BundleView::Open`, and then open every environment.
- If a scope opens, walk every view: each variant, entry point, layout, pair, constant, and capability name.
- The test passes when nothing crashes.

The small manifest has a few kilobytes, so the sweep is a few thousand opens and takes well under a second. Without a
sanitizer it finds crashes, not every silent out-of-bounds read. `CLAUDE.md` explains why ASan is off. It is still the
only test of the promise itself. It is a keystone candidate.

---

## 7. Prune and merge

| Item | Lines | Reason |
|---|---|---|
| `tests/scripts/` and `tests/assets/common/` (tracked files together) | ~4100 | velox OceanFFT and VTF slangpy scripts. They load `slang_shaders/OceanFft.slang`, which does not exist here. Nothing runs them. |
| (the `dd*.slang` files in `tests/assets/common/`) | | Only the dead Python above references them. |
| `tests/DeclKindProbe.cpp` and its CMake target | 217 | The handoff calls it a throwaway probe. Its findings are in `docs/agent-handoff.md` §4. |
| `StorageFormatTests.cpp` section 4 (lines 187 to 244) | ~60 | `StorageFormatAxisTest` proves the same claim more strongly: each variant against a control written independently. Keep `ShimStorageAxis.slang`, because `ShimExternTest` reads it. |
| `PermutationConstraintTest`, "ActiveWhen Gating" | ~15 | `PermutationIndexTest` proves the same gating with more detail. |
| `StageDumpTest`, the four "two dumps agree byte for byte" checks | ~12 | Each one calls a pure function twice on one object in one process. They can fail only if a dump prints an address. `--verify-deterministic` is the real determinism proof. |
| `StageDumpTest`, `CheckRawPrimitives` | ~25 | It tests `RawPlacement` ordering and `ArgumentCountOf`, not the dumps. Move it to `ResolveStageTest`. |
| `ReflectionSchemaTest`, the `BindingKind` and `TextureSampleType` loops | ~20 | They test the switch of `ToString` and catch little. Keep the check of `CookError` above 127, because it found a real defect, and keep `DescribeBinding`. |
| `ShimModuleTest` and `ShimCookTest` | | They overlap. `ShimCookTest` compares against a control, which is the stronger form. `ShimModuleTest` alone checks the naming contract (scope `material_Points`, name `data`). Move those name checks into `ShimCookTest`, and delete `ShimModuleTest` and `ShimDirect.slang`. This is optional. |

Keep `SlangPrelinkRepro`. It is not a test, and it documents a Slang fault. Move it with the canary into
`tests/slang/` so that the folder says which code tests Slang and which tests Lodestone.

---

## 8. Keystone tests

A keystone test is one whose deletion removes a part of the correctness argument that no other test makes. Mark each
one in CMake (`set_tests_properties(<name> PROPERTIES LABELS keystone)`), so that `ctest -L keystone` runs them, and
add a "keystone" column to the table in `CLAUDE.md`.

| Test | The argument it carries |
|---|---|
| `KitchenSinkCookTest` | Every validator over 616 variants for each target, both targets, and determinism. It is the integration proof. |
| `ShaderManifestRejectTest` | The client ingestion surface: each damaged field is rejected by name. |
| `WgslValidatorTest`, `SpvValidatorTest` | Each cross-check can fail. Without them, a validator that always passes would leave every cook test green. |
| `DedupeInfluenceTest` | Rule 2: `--no-dedupe` reaches the same conclusions. |
| `ContentInternerTest` | Rule 1: a hash never decides equality. |
| `AccessModelRejectTest` (`CookRejectTest`) | The control row. Every other cook test proves that a good module cooks, and only this table proves that a bad one fails. |
| `ResolveStageTest` | The stage 3 and stage 4 split, with no Slang. |
| `EntryPointConstantsTest` | std140 and std430 layouts against values computed by hand. That is an independent opinion, not a reread. |
| `ShimCookTest`, `StorageFormatAxisTest` | Subject against control: two independent ways to write one shader must cook to the same resources. |
| `ManifestIndexTest` | The frozen client query surface. |
| `SlangMetadataCanaryTest` | It stops the slang#13789 workaround from outliving the fault. |
| (new) the reader damage sweep of §6.2 | The `Open` promise itself. |

**Useful but not keystone:** the other unit suites. Each proves one component, and most have no overlap.

**Smoke drivers:** `EntryPointParamsCookTest`, `ParameterBlocksCookTest`, `InterfaceAxisCookTest`, `EnumAxisCookTest`,
`CrossModuleCookTest`, and `WorkgroupMemoryCookTest`. KitchenSink covers most of what they cover. Their value is
localization: a small module that fails shows the cause faster than 616 variants do. Keep them, and give them output on
a failure (§3.6). `EnumAxisCookTest` claims more than an exit code can prove (§5).

---

## 9. Suggested order

1. The harness (§2.1 to §2.4, and §2.9). Every later item then reports better. Most of the conversion is mechanical.
2. The test defects of §4, and a failing test for `MaxVariants`.
3. The `RunCook` sink overload (§3.1), `CookedBundle` (§3.2), and the shared helpers (§3.3 and §3.4).
4. The claims of §5, cheapest first: the two suites that already have a recording sink, then the known-answer keys,
   then the rest.
5. `CookerOptionsTest` and the reader damage sweep (§6).
6. The prune list (§7), the `run-tests.bat` change (§3.6), and the keystone labels (§8).
