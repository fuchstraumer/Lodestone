# Test Harness Conversion Report

This report describes the conversion of test suites in batches 1 to 4.
Batches 5, 6, and 7 remain unconverted.

## 1. Converted Files and Check Counts

We converted 14 test files across batches 1 to 4.
The list below shows the count of converted checks for each file.

### Batch 1
- `tests/SuggestTests.cpp`: 9 checks converted
- `tests/EnumTagDecodeTests.cpp`: 23 checks converted
- `tests/ContentInternerTests.cpp`: 14 checks converted
- `tests/ReflectionSchemaTests.cpp`: 27 checks converted
- `tests/DiagnosticParserTests.cpp`: 32 checks converted

### Batch 2
- `tests/PermutationIndexTests.cpp`: 11 checks converted
- `tests/PermutationConstraintTests.cpp`: 10 checks converted
- `tests/PolicyDocumentTests.cpp`: 37 checks converted
- `tests/SymbolTableTests.cpp`: 13 checks converted

### Batch 3
- `tests/ResolveStageTests.cpp`: 24 checks converted
- `tests/StageDumpTests.cpp`: 32 checks converted
- `tests/DedupeInfluenceTests.cpp`: 36 checks converted

### Batch 4
- `tests/ShaderManifestRejectTests.cpp`: 76 checks converted
- `tests/MergedLayoutTests.cpp`: 37 checks converted
- `tests/ResourceReportTests.cpp`: 18 checks converted

Total converted checks in batches 1 to 4: 399 checks.

## 2. Unconverted Conjunctions and Checks

We left conjunctions unconverted when they contain operations other than equality comparisons.
The list below documents each site and the reason.

### `tests/DiagnosticParserTests.cpp`
- `tests/DiagnosticParserTests.cpp:68`: Conjunction contains `File.empty()` call.
- `tests/DiagnosticParserTests.cpp:130`: Conjunction contains `!unreadable.empty()` call.
- `tests/DiagnosticParserTests.cpp:132`: Conjunction contains `!unreadable.empty()` call.
- `tests/DiagnosticParserTests.cpp:164`: Conjunction contains `!tabbed.empty()` call.
- `tests/DiagnosticParserTests.cpp:175`: Conjunction contains `Code.empty()` call.

### `tests/PermutationIndexTests.cpp`
- `tests/PermutationIndexTests.cpp:168`: Conjunction contains null pointer check `canonicalWaveSize != nullptr`.

### `tests/PolicyDocumentTests.cpp`
- `tests/PolicyDocumentTests.cpp:75`: Conjunction contains boolean checks `!AsBool()` and `AsBool()`.
- `tests/PolicyDocumentTests.cpp:80`: Conjunction contains `CookValues.empty()` and `CookIf.empty()` calls.
- `tests/PolicyDocumentTests.cpp:97`: Conjunction contains `Line > 0u` inequality comparison.
- `tests/PolicyDocumentTests.cpp:101`: Conjunction contains `Line > 0u` inequality comparison.

### `tests/SymbolTableTests.cpp`
- `tests/SymbolTableTests.cpp:79`: Conjunction contains boolean helper calls to `DeclarationMatches`.
- `tests/SymbolTableTests.cpp:196`: Conjunction contains null pointer check `clusterCount != nullptr`.
- `tests/SymbolTableTests.cpp:198`: Conjunction contains null pointer check `clusterCount != nullptr`.
- `tests/SymbolTableTests.cpp:224`: Conjunction contains boolean helper call to `DeclarationMatches`.
- `tests/SymbolTableTests.cpp:253`: Conjunction contains boolean helper call to `DeclarationMatches`.

### `tests/ResolveStageTests.cpp`
- `tests/ResolveStageTests.cpp:139`: Conjunction contains null pointer check `buffer != nullptr`.
- `tests/ResolveStageTests.cpp:186`: Conjunction contains null pointer check `buffer != nullptr`.
- `tests/ResolveStageTests.cpp:188`: Conjunction contains `!std::holds_alternative` type check.
- `tests/ResolveStageTests.cpp:199`: Conjunction contains null pointer check `line != nullptr`.
- `tests/ResolveStageTests.cpp:206`: Conjunction contains null pointer check `first != nullptr`.
- `tests/ResolveStageTests.cpp:208`: Conjunction contains null pointer check `first != nullptr`.
- `tests/ResolveStageTests.cpp:209`: Conjunction contains `!std::holds_alternative` type check.
- `tests/ResolveStageTests.cpp:217`: Conjunction contains null pointer check `second != nullptr`.
- `tests/ResolveStageTests.cpp:320`: Conjunction contains null pointer check `named != nullptr`.

### `tests/StageDumpTests.cpp`
- `tests/StageDumpTests.cpp:198`: Conjunction contains boolean function calls to `IsStageDumpRequested`.
- `tests/StageDumpTests.cpp:231`: Conjunction contains boolean function calls to `IsStageDumpRequested`.
- `tests/StageDumpTests.cpp:323`: Conjunction contains boolean function calls to `Contains`.
- `tests/StageDumpTests.cpp:344`: Conjunction contains boolean function calls to `PlacementLess`.
- `tests/StageDumpTests.cpp:376`: Conjunction contains boolean function calls to `Contains`.

### `tests/ShaderManifestRejectTests.cpp`
- `tests/ShaderManifestRejectTests.cpp:285`: Conjunction contains boolean predicate call `HasCodeFacts()`.
- `tests/ShaderManifestRejectTests.cpp:414`: Conjunction contains multiple `!has_value()` calls.
- `tests/ShaderManifestRejectTests.cpp:559`: Conjunction contains boolean predicate call `HasSamplerTexturePairs()`.
- `tests/ShaderManifestRejectTests.cpp:612`: Conjunction contains boolean predicate call `!HasSamplerTexturePairs()`.

### `tests/MergedLayoutTests.cpp`
- `tests/MergedLayoutTests.cpp:256`: Conjunction contains boolean helper call to `IsAt`.
- `tests/MergedLayoutTests.cpp:258`: Conjunction contains boolean helper call to `IsAt`.
- `tests/MergedLayoutTests.cpp:260`: Conjunction contains boolean helper calls to `IsAt`.
- `tests/MergedLayoutTests.cpp:281`: Conjunction contains boolean helper call to `IsAt`.
- `tests/MergedLayoutTests.cpp:318`: Conjunction contains `!named.has_value()` check.
- `tests/MergedLayoutTests.cpp:388`: Conjunction contains boolean predicate call `HasSamplerTexturePairs()`.
- `tests/MergedLayoutTests.cpp:478`: Conjunction contains `has_value()` calls.
- `tests/MergedLayoutTests.cpp:491`: Conjunction contains `has_value()` calls.
- `tests/MergedLayoutTests.cpp:497`: Conjunction contains `has_value()` calls.
- `tests/MergedLayoutTests.cpp:503`: Conjunction contains `has_value()` calls.
- `tests/MergedLayoutTests.cpp:508`: Conjunction contains `!conflict.has_value()` check.

### `tests/ResourceReportTests.cpp`
- `tests/ResourceReportTests.cpp:161`: Conjunction contains null pointer check `counts != nullptr`.
- `tests/ResourceReportTests.cpp:163`: Conjunction contains null pointer check `counts != nullptr`.
- `tests/ResourceReportTests.cpp:166`: Conjunction contains null pointer check `lut != nullptr`.
- `tests/ResourceReportTests.cpp:173`: Conjunction contains null pointer check `sparseCounts != nullptr`.
- `tests/ResourceReportTests.cpp:176`: Conjunction contains null pointer check `sparseCounts != nullptr`.
- `tests/ResourceReportTests.cpp:179`: Conjunction contains null pointer check `sparseLut != nullptr`.
- `tests/ResourceReportTests.cpp:230`: Conjunction contains boolean predicate calls to `IsFootprintAxis`.
- `tests/ResourceReportTests.cpp:232`: Conjunction contains boolean predicate calls to `IsFootprintAxis`.
- `tests/ResourceReportTests.cpp:257`: Conjunction contains boolean predicate call `IsComplete()`.
- `tests/ResourceReportTests.cpp:261`: Conjunction contains boolean predicate call `IsComplete()`.
- `tests/ResourceReportTests.cpp:263`: Conjunction contains boolean predicate call `!IsComplete()`.
- `tests/ResourceReportTests.cpp:295`: Conjunction contains null pointer check `countsEntry != nullptr`.
- `tests/ResourceReportTests.cpp:299`: Conjunction contains null pointer check and `.empty()` check.

## 3. Types that Print No Formatter

The types below have no formatter defined in `FormatForTest`.
A failure on these types prints `<no formatter>`.
All tests for these types passed.

- `lodestone::ReflectedUniformMember`
- `lodestone::ResourcePlacement`

## 4. Empty Sections and Changed Results

- No section ran zero checks.
- No test changed its result.
- The known failure in `PermutationIndexTest` remains the single failure.

## 5. Test Suite Output

Below is the failure output and the final line from `scripts\run-tests.bat RelWithDebInfo`:

```
[FAIL] PermutationIndexTest
  [MaxVariants is the largest variant count that cooks]
    FAIL: 16 variants against a budget of 16
      expected: a value
      actual:   error PermutationVariantBudgetExceeded
      at C:\SoftwareDev\Lodestone\tests\PermutationIndexTests.cpp:207 in int __cdecl main(void)
PermutationIndexTests : 1 of 20 checks FAILED, 0 sections ran no checks
```

```
[tests] at least one target failed
```


---

# Session 2: Batch 5 (`ManifestIndexTests`)

Batch 5 was done by the coordinator (not a subagent). Batches 6 and 7 are in a later session section.
Line numbers below are the current line numbers in the converted file.

## 2.1 Converted file and count

- `tests/ManifestIndexTests.cpp`: 143 original `runner.Check` calls.
- 112 of them are converted. They now use 153 helper calls (`CheckEqual`, `CheckOk`, `CheckError`), because rule 4 splits conjunctions.
- 31 original checks stay as `runner.Check` (see 2.2).
- 20 more `runner.Check` calls remain as the guard of a converted check (`runner.Check(!x.has_value(), claim)`, then `CheckEqual` on the error fields).
- The file now includes `<format>`. A check inside the `keepDebugOn` loop in `CheckAxisRelevance` carries the loop variable in its claim.

## 2.2 Checks left unconverted

Every one of these is a conjunction or a check that is not only `==` comparisons (rule 4), or a plain boolean (rule 1).

- Plain boolean, no comparison: 390, 391, 1000, 1042, 1043, 1047, 1155, 1170, 1201, 1231, 1261, 1292, 1377.
- Conjunction of `IsAxisRelevant` booleans: 392, 503, 505, 828.
- Conjunction of a `has_value` guard and a boolean call: 672 (`opened->HasCodeFacts()`), 777 (`!unreadOpened->HasCodeFacts()`).
- Conjunction of `!IsValid()` (or `!Errors().empty()`) with an `==` on `Errors().front().Code`: 1048, 1059, 1065, 1068, 1086, 1090, 1094, 1098, 1298.
- Conjunction with `!Active` and `Active` booleans: 1223.
- Mixed conjunction of `IsValid()`, `Notes().size() == 1u`, and `==` comparisons: 1283.
- Mixed conjunction of `!IsValid()`, `!Keys().has_value()`, and `==` comparisons: 782.

## 2.3 Choices that go beyond the literal text of the rules

1. `CheckError` does not accept a `QueryError` (only `ErrorState`, enums, and `QueryErrorCode`). The checks `!r.has_value() && r.error().Code == X && ...` on `std::expected<..., QueryError>` use a guard and field checks:
   `if (runner.Check(!r.has_value(), claim)) { runner.CheckEqual(r.error().Code, X, claim); ... }`.
   The guard check prints no value. Suggested follow-up: add `ErrorCodeOf(const QueryError&)` and a `QueryError` formatter to `tests/TestHarness.*`. The author decides. This session did not change the harness.
   The guard lines are 402, 417, 455, 461, 468, 477, 697, 714, 729, 741, 752, 762, 841, 1118, 1128, 1173, 1214.
2. Where the original `&&` chain protected an index or a `front()` call (`names[...]`, `(*quality)[0]`, `Notes().front()`), the conversion nests the field checks inside `if (runner.CheckEqual(size, N, claim))`. This keeps the short-circuit safety.
3. `a.has_value() && b.has_value()` guards that feed an unchanged `if` (two places for `full`/`pruned`, and `large`/`reversed`) became two `CheckOk` calls with the same claim. The `if` is unchanged.
4. A claim that several converted checks share is a local `const std::string` or `const char*` in the same scope. The source location in a failure still points to the check call.

## 2.4 Types that print a degraded value on failure

- `<no formatter>`: `lodestone::QueryError` (in `CheckOk` of a `std::expected<..., QueryError>`), `std::vector<DecodedVariant>`-style containers are not printed (no check compares one).
- `N (no ToString)`: `QueryErrorCode`, `QueryNoteCode`, `AxisValueDomain`, `VariantKey` (an `enum class`). These print the underlying number.
- Formatted by name: `CookError` (via `ToString`).

## 2.5 Sections that ran no checks, and changed results

- No section ran zero checks.
- No test changed result.

## 2.6 Validation after batch 5

- `scripts\build.bat RelWithDebInfo`: exit code 0.
- `scripts\run-tests.bat RelWithDebInfo`: exit code 1.
- `[ ok ] ManifestIndexTest`.
- The only `[FAIL]` line: `[FAIL] PermutationIndexTest` with `FAIL: 16 variants against a budget of 16` and `PermutationIndexTests : 1 of 20 checks FAILED, 0 sections ran no checks`. This is the known product defect.
- Final line: `[tests] at least one target failed`.


---

# Session 3: Batches 6 and 7 (subagent)

We converted the test files in batch 6 and batch 7.
Batch 6 contains `tests/WgslValidatorTests.cpp` and `tests/SpvValidatorTests.cpp`.
Batch 7 contains the 15 Slang-cooking test files.

## 3.1 Converted files and check counts

We converted 15 test files across batches 6 and 7.
Two test files in batch 7 had no equality or result checks to convert.
The lists below show the converted check counts for each file.

### Batch 6
- `tests/WgslValidatorTests.cpp`: 9 original checks converted (14 helper calls: 7 `CheckOk`, 6 `CheckEqual`, 1 `CheckError`). 19 original checks remain unconverted.
- `tests/SpvValidatorTests.cpp`: 15 original checks converted (21 helper calls: 8 `CheckOk`, 10 `CheckEqual`, 3 `CheckError`). 25 original checks remain unconverted.

Total converted checks in batch 6: 24 original checks converted into 35 helper calls.

### Batch 7
- `tests/AccessModelRejectTests.cpp`: 2 original checks converted (2 helper calls: 2 `CheckEqual`). 4 original checks remain unconverted.
- `tests/AxisDefaultTests.cpp`: 11 original checks converted (13 helper calls: 9 `CheckOk`, 4 `CheckEqual`). 3 original checks remain unconverted.
- `tests/BindingArrayTests.cpp`: 5 original checks converted (8 helper calls: 3 `CheckOk`, 3 `CheckEqual`, 2 `CheckError`). 2 original checks remain unconverted.
- `tests/CapabilityAxisTests.cpp`: 9 original checks converted (9 helper calls: 9 `CheckEqual`). 1 original check remains unconverted.
- `tests/DerivedConstantTests.cpp`: 7 original checks converted (7 helper calls: 3 `CheckOk`, 4 `CheckEqual`). 1 original check remains unconverted.
- `tests/EntryPointConstantsTests.cpp`: 9 original checks converted (13 helper calls: 4 `CheckOk`, 8 `CheckEqual`, 1 `CheckError`). 3 original checks remain unconverted.
- `tests/ResourceRoleTests.cpp`: 6 original checks converted (7 helper calls: 3 `CheckOk`, 3 `CheckEqual`, 1 `CheckError`). 5 original checks remain unconverted (including 1 optional guard check).
- `tests/ShimCookTests.cpp`: 1 original check converted (2 helper calls: 2 `CheckOk`). 4 original checks remain unconverted.
- `tests/ShimExternTests.cpp`: 10 original checks converted (17 helper calls: 2 `CheckOk`, 14 `CheckEqual`, 1 `CheckError`). 2 original checks remain unconverted.
- `tests/ShimLinkSourceTests.cpp`: 18 original checks converted (24 helper calls: 3 `CheckOk`, 21 `CheckEqual`). 3 original checks remain unconverted.
- `tests/ShimModuleTests.cpp`: 3 original checks converted (3 helper calls: 3 `CheckOk`). 3 original checks remain unconverted.
- `tests/ShimTableTests.cpp`: 2 original checks converted (3 helper calls: 3 `CheckEqual`). 10 original checks remain unconverted.
- `tests/StorageFormatTests.cpp`: 5 original checks converted (5 helper calls: 3 `CheckOk`, 2 `CheckEqual`). 10 original checks remain unconverted.
- `tests/StorageFormatAxisTests.cpp`: 0 checks converted. 11 original checks remain unconverted.
- `tests/SlangMetadataCanaryTests.cpp`: 0 checks converted. 3 original checks remain unconverted.

Total converted checks in batch 7: 88 original checks converted into 127 helper calls.
Total converted checks in batches 6 and 7: 112 original checks converted into 162 helper calls.

## 3.2 Checks left unconverted

We did not convert checks when they contain operations other than equality comparisons.
The list below documents each site and the reason.

### `tests/WgslValidatorTests.cpp`
- `tests/WgslValidatorTests.cpp:199`: Conjunction contains boolean field `match->Matches`.
- `tests/WgslValidatorTests.cpp:200`: Conjunction contains `match->Report.empty()` call.
- `tests/WgslValidatorTests.cpp:230`: Conjunction contains `CapabilityNames.empty()` call.
- `tests/WgslValidatorTests.cpp:232`: Conjunction contains `CapabilityNames.empty()` call.
- `tests/WgslValidatorTests.cpp:257`: Conjunction contains boolean field `!kindMismatch->Matches`.
- `tests/WgslValidatorTests.cpp:259`: Conjunction contains `!kindMismatch->Report.empty()` call.
- `tests/WgslValidatorTests.cpp:270`: Conjunction contains boolean field `!accessMismatch->Matches`.
- `tests/WgslValidatorTests.cpp:281`: Conjunction contains boolean field `!depthMismatch->Matches`.
- `tests/WgslValidatorTests.cpp:291`: Conjunction contains boolean field `structuredResult->Matches`.
- `tests/WgslValidatorTests.cpp:306`: Conjunction contains boolean field `formatMatch->Matches`.
- `tests/WgslValidatorTests.cpp:308`: Conjunction contains boolean field `!formatInvalid->Matches`.
- `tests/WgslValidatorTests.cpp:311`: Conjunction contains boolean field `!formatOther->Matches`.
- `tests/WgslValidatorTests.cpp:320`: Conjunction contains boolean field `!nameMismatch->Matches`.
- `tests/WgslValidatorTests.cpp:329`: Conjunction contains boolean field `!missingBinding->Matches`.
- `tests/WgslValidatorTests.cpp:338`: Conjunction contains boolean field `!extraBinding->Matches`.
- `tests/WgslValidatorTests.cpp:346`: Plain boolean check `!broken.has_value()`.
- `tests/WgslValidatorTests.cpp:358`: Conjunction contains pointer comparison `Validator != nullptr`.
- `tests/WgslValidatorTests.cpp:360`: Plain boolean check `!lodestone::FindTargetProfile("hlsl")`.
- `tests/WgslValidatorTests.cpp:367`: Conjunction contains multiple `has_value()` calls.
- `tests/WgslValidatorTests.cpp:373`: Conjunction contains multiple `has_value()` calls.

### `tests/SpvValidatorTests.cpp`
- `tests/SpvValidatorTests.cpp:388`: Plain boolean check `!shadeCs.empty()`.
- `tests/SpvValidatorTests.cpp:400`: Conjunction contains boolean field `match->Matches`.
- `tests/SpvValidatorTests.cpp:401`: Conjunction contains `match->Report.empty()` call.
- `tests/SpvValidatorTests.cpp:402`: Conjunction contains boolean helper call `std::ranges::contains`.
- `tests/SpvValidatorTests.cpp:431`: Plain boolean check `!undefImage.empty()`.
- `tests/SpvValidatorTests.cpp:440`: Plain boolean helper call `Mismatches(moved)`.
- `tests/SpvValidatorTests.cpp:445`: Plain boolean helper call `Mismatches(...)`.
- `tests/SpvValidatorTests.cpp:452`: Plain boolean helper call `Mismatches(...)`.
- `tests/SpvValidatorTests.cpp:459`: Plain boolean helper call `Mismatches(...)`.
- `tests/SpvValidatorTests.cpp:465`: Plain boolean helper call `Mismatches(...)`.
- `tests/SpvValidatorTests.cpp:470`: Plain boolean helper call `Mismatches(...)`.
- `tests/SpvValidatorTests.cpp:487`: Conjunction contains boolean field `formatMatch->Matches`.
- `tests/SpvValidatorTests.cpp:491`: Plain boolean helper call `Mismatches(...)`.
- `tests/SpvValidatorTests.cpp:495`: Plain boolean helper call `Mismatches(...)`.
- `tests/SpvValidatorTests.cpp:501`: Plain boolean helper call `Mismatches(...)`.
- `tests/SpvValidatorTests.cpp:506`: Plain boolean check `!imageArray.empty()`.
- `tests/SpvValidatorTests.cpp:518`: Conjunction contains boolean field `arrayMatch->Matches`.
- `tests/SpvValidatorTests.cpp:522`: Plain boolean helper call `Mismatches(...)`.
- `tests/SpvValidatorTests.cpp:526`: Plain boolean helper call `Mismatches(...)`.
- `tests/SpvValidatorTests.cpp:532`: Conjunction contains boolean helper `Mismatches(...)` and `!= std::string::npos`.
- `tests/SpvValidatorTests.cpp:539`: Plain boolean check `!noMemoryModel.empty()`.
- `tests/SpvValidatorTests.cpp:551`: Conjunction contains optional check `!noPush->PushConstantExtent.has_value()`.
- `tests/SpvValidatorTests.cpp:576`: Plain boolean check `!workgroupCs.empty()`.
- `tests/SpvValidatorTests.cpp:596`: Plain boolean check `!dynamicArray.empty()`.
- `tests/SpvValidatorTests.cpp:598`: Conjunction contains `!= constantIndex->CapabilityNames.end()`.

### `tests/AccessModelRejectTests.cpp`
- `tests/AccessModelRejectTests.cpp:136`: Literal boolean `false` check for missing asset file.
- `tests/AccessModelRejectTests.cpp:157`: Plain boolean check `!std::filesystem::exists(manifestPath)`.
- `tests/AccessModelRejectTests.cpp:160`: Conjunction contains boolean call `std::filesystem::exists(manifestPath)`.
- `tests/AccessModelRejectTests.cpp:162`: Plain boolean check `!HasTemporaryFile(outputDirectory)`.

### `tests/AxisDefaultTests.cpp`
- `tests/AxisDefaultTests.cpp:125`: Conjunction contains `!has_value()` and `has_value()` checks on optional results.
- `tests/AxisDefaultTests.cpp:155`: Plain boolean check `!index.AxisDefault("NOT_AN_AXIS").has_value()`.
- `tests/AxisDefaultTests.cpp:182`: Conjunction contains `.empty()` call.

### `tests/BindingArrayTests.cpp`
- `tests/BindingArrayTests.cpp:106`: Conjunction contains `resource.has_value()` check on optional.
- `tests/BindingArrayTests.cpp:123`: Plain boolean check `needed`.

### `tests/CapabilityAxisTests.cpp`
- `tests/CapabilityAxisTests.cpp:175`: Literal boolean `false` check for missing asset file.

### `tests/DerivedConstantTests.cpp`
- `tests/DerivedConstantTests.cpp:183`: Conjunction contains `ramp.has_value()` check on optional.

### `tests/EntryPointConstantsTests.cpp`
- `tests/EntryPointConstantsTests.cpp:144`: Plain boolean helper call `MembersMatch(...)`.
- `tests/EntryPointConstantsTests.cpp:160`: Plain boolean check `!containerInLayout`.
- `tests/EntryPointConstantsTests.cpp:175`: Plain boolean check `tintsInLayout`.

### `tests/ResourceRoleTests.cpp`
- `tests/ResourceRoleTests.cpp:113`: Guard check `resource.has_value()` on optional protecting field comparisons.
- `tests/ResourceRoleTests.cpp:122`: Conjunction contains boolean helper call `TagsAre(...)`.
- `tests/ResourceRoleTests.cpp:123`: Conjunction contains boolean helper call `TagsAre(...)`.
- `tests/ResourceRoleTests.cpp:125`: Conjunction contains `taggedA.has_value()` check on optional.
- `tests/ResourceRoleTests.cpp:130`: Conjunction contains boolean predicate calls `HasTag(...)` and `!HasTag(...)`.

### `tests/ShimCookTests.cpp`
- `tests/ShimCookTests.cpp:39`: Plain boolean field `shim.Cooked`.
- `tests/ShimCookTests.cpp:40`: Plain boolean field `control.Cooked`.
- `tests/ShimCookTests.cpp:63`: Conjunction contains `!shimKeys.empty()` call.
- `tests/ShimCookTests.cpp:71`: Plain boolean check `!local.Cooked`.

### `tests/ShimExternTests.cpp`
- `tests/ShimExternTests.cpp:95`: Conjunction contains disjunction `(!hasArgument || ...)`.
- `tests/ShimExternTests.cpp:157`: Conjunction contains `.empty()` call.

### `tests/ShimLinkSourceTests.cpp`
- `tests/ShimLinkSourceTests.cpp:123`: Conjunction contains `.empty()` call.
- `tests/ShimLinkSourceTests.cpp:158`: Conjunction contains multiple `has_value()` calls.
- `tests/ShimLinkSourceTests.cpp:162`: Conjunction contains `.empty()` call.

### `tests/ShimModuleTests.cpp`
- `tests/ShimModuleTests.cpp:111`: Conjunction contains boolean helper call `IsAt(...)`.
- `tests/ShimModuleTests.cpp:116`: Conjunction contains `count.has_value()` check on optional.
- `tests/ShimModuleTests.cpp:121`: Conjunction contains `scratch.has_value()` check on optional.

### `tests/ShimTableTests.cpp`
- `tests/ShimTableTests.cpp:115`: Plain boolean variable `boundComplete`.
- `tests/ShimTableTests.cpp:118`: Pointer comparison `readBuffer != nullptr`.
- `tests/ShimTableTests.cpp:122`: Conjunction contains pointer comparison `readBuffer != nullptr`.
- `tests/ShimTableTests.cpp:125`: Conjunction contains `.empty()` calls.
- `tests/ShimTableTests.cpp:128`: Conjunction contains pointer comparison `writeStorage != nullptr` and boolean fields.
- `tests/ShimTableTests.cpp:135`: Plain boolean check `ShimImplName(...).empty()`.
- `tests/ShimTableTests.cpp:142`: Plain boolean function call `CookAgreementModule(...)`.
- `tests/ShimTableTests.cpp:147`: Plain boolean function call `!CookAgreementModule(...)`.
- `tests/ShimTableTests.cpp:151`: Plain boolean function call `!CookAgreementModule(...)`.
- `tests/ShimTableTests.cpp:156`: Plain boolean function call `!CookAgreementModule(...)`.

### `tests/StorageFormatTests.cpp`
- `tests/StorageFormatTests.cpp:85`: Conjunction contains boolean variable `complete`.
- `tests/StorageFormatTests.cpp:88`: Conjunction contains pointer comparison `snorm != nullptr`.
- `tests/StorageFormatTests.cpp:98`: Conjunction contains `.empty()` call and boolean variable `sameCases`.
- `tests/StorageFormatTests.cpp:126`: Literal boolean `false` check for environment failure.
- `tests/StorageFormatTests.cpp:139`: Conjunction contains iterator comparison `entry != found.end()`.
- `tests/StorageFormatTests.cpp:167`: Literal boolean `false` check for environment failure.
- `tests/StorageFormatTests.cpp:179`: Conjunction contains iterator comparison `entry != byScope.end()`.
- `tests/StorageFormatTests.cpp:210`: Literal boolean `false` check for environment failure.
- `tests/StorageFormatTests.cpp:226`: Literal boolean `false` check for variant lookup failure.
- `tests/StorageFormatTests.cpp:240`: Conjunction contains boolean variable `matches`.

### `tests/StorageFormatAxisTests.cpp`
- `tests/StorageFormatAxisTests.cpp:103`: Plain boolean field `subject.Cooked`.
- `tests/StorageFormatAxisTests.cpp:104`: Plain boolean field `controls.Cooked`.
- `tests/StorageFormatAxisTests.cpp:116`: Literal boolean `false` check for environment failure.
- `tests/StorageFormatAxisTests.cpp:123`: Inequality comparison `controlModule >= 0`.
- `tests/StorageFormatAxisTests.cpp:132`: Literal boolean `false` check for variant lookup failure.
- `tests/StorageFormatAxisTests.cpp:137`: Conjunction contains `.empty()` call.
- `tests/StorageFormatAxisTests.cpp:142`: Conjunction contains boolean predicate calls `HasCodeFacts()` and `!NamesCapability(...)`.
- `tests/StorageFormatAxisTests.cpp:179`: Conjunction contains boolean variable and predicate call `NamesCapability(...)`.
- `tests/StorageFormatAxisTests.cpp:186`: Plain boolean field `subset.Cooked`.
- `tests/StorageFormatAxisTests.cpp:194`: Literal boolean `false` check for environment failure.
- `tests/StorageFormatAxisTests.cpp:198`: Conjunction contains `has_value()` checks on optional results.

### `tests/SlangMetadataCanaryTests.cpp`
- `tests/SlangMetadataCanaryTests.cpp:50`: Pointer comparison `module != nullptr`.
- `tests/SlangMetadataCanaryTests.cpp:65`: Pointer comparison `metadata != nullptr`.
- `tests/SlangMetadataCanaryTests.cpp:79`: Plain boolean variable `unusedReportedUsed`.

## 3.3 Types that print no formatter

The types below have no formatter in `FormatForTest`.
A failure on these types prints `<no formatter>`.
All checks on these types passed.

- `std::vector<lodestone::SamplerTexturePlacement>`
- `std::optional<uint32_t>`
- `std::optional<uint64_t>`
- `std::vector<std::string>::iterator`
- `std::vector<std::string_view>`

Types that print the underlying integer (no `ToString` function):
- `lodestone::EntryPointConstantsDelivery`
- `lodestone::ResourceRoleFlags`
- `lodestone::BufferUsageFlags`
- `lodestone::ShimResourceClass`
- `lodestone::AccessModel`

## 3.4 Empty sections and changed results

- No section ran zero checks.
- No test changed result.
- The known failure in `PermutationIndexTest` remains the single failure.

## 3.5 Test suite output

Below is the failure output and the final line from `scripts\run-tests.bat RelWithDebInfo`:

```
[FAIL] PermutationIndexTest
  [MaxVariants is the largest variant count that cooks]
    FAIL: 16 variants against a budget of 16
      expected: a value
      actual:   error PermutationVariantBudgetExceeded
      at C:\SoftwareDev\Lodestone\tests\PermutationIndexTests.cpp:207 in int __cdecl main(void)
PermutationIndexTests : 1 of 20 checks FAILED, 0 sections ran no checks
```

```
[tests] at least one target failed
```

## 3.6 Validation after session 3

- `scripts\build.bat RelWithDebInfo`: exit code 0.
- `scripts\run-tests.bat RelWithDebInfo`: exit code 1.
- All 17 converted test executables in batches 6 and 7 passed.
- The single failure remains the known product defect in `PermutationIndexTest`.


---

# Session 4: `QueryError` support in the harness (coordinator)

## 4.1 Harness change

The author approved this change. `QueryError` is a struct, not a `std::expected`, so `CheckError` could not read its code.

- `tests/TestHarness.hpp` now includes `ShaderManifestIndex.hpp`. It declares `QueryErrorCode ErrorCodeOf(const QueryError&)` and `std::string FormatForTest(const QueryError&)`.
- `tests/TestHarness.cpp` defines both. The formatter prints the code number, axis name, detail, and suggestion.
- `CheckError(result, QueryErrorCode::X, claim)` now works for `std::expected<..., QueryError>`.
- `QueryError` no longer prints `<no formatter>`. This removes the note in section 2.4 of Session 2 for `QueryError`.

## 4.2 Tests converted to use it

In `tests/ManifestIndexTests.cpp`, 12 checks changed from `Check(!r.has_value(), ...)` plus `CheckEqual(r.error().Code, ...)` to `CheckError`:
- Where the check also compared other error fields, those `CheckEqual` calls stay inside `if (runner.CheckError(...))`.
- Where the check compared only the code, a single `CheckError` replaces the guard and the comparison.
- These are `allAxes`, `fogUnpinned`, `noStageNotes`, `unknown`, `empty`, the `smallOnly.SingleFor(generate)` check, `noWave`, `tooMuchMemory`, `highQuality`, `partial`, `badSingle`, and `absentSingle`.

## 4.3 Guards that stay

These guards compare no error code in the original, only a field. `CheckError` cannot replace them without adding a claim. They stay as `Check(!r.has_value(), ...)` plus `CheckEqual` on the field:
- `joined` (axis name only).
- `tooManyInvocations`, `tooWide`, and `tooManyPushBytes` (detail only).
- `gatedSingle` (axis name only).

The other test files that hold a `QueryError` result (`StorageFormatTests`, `StorageFormatAxisTests`, `DerivedConstantTests`) do not use the guard pattern. They needed no change.

## 4.4 Validation

- `scripts\build.bat RelWithDebInfo`: exit code 0.
- `scripts\run-tests.bat RelWithDebInfo`: exit code 1.
- The only `[FAIL]` line is `[FAIL] PermutationIndexTest` (`16 variants against a budget of 16`, the known product defect).
- Final line: `[tests] at least one target failed`.
- No section ran zero checks.
