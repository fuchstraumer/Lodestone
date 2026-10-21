# Rendergraph integration: Lodestone tasks

Status: started 2026-10-14. Text follows ASD-STE100.

`docs/` is in `.gitignore`. Force-add this file: `git add -f docs/rendergraph-integration-tasks.md`.

## 1. Purpose

The VeloxRhi rendergraph is the first real consumer of the client. Its shape will change the client
API and parts of phase F. So phase F is paused, and this list comes first. Build the graph path for
WebGPU (`wgsl`, Bound), then go back to phase F with what the graph taught us.

The source is `C:\SoftwareDev\VeloxRhi\docs\rendergraph-lodestone-integration.md` (2026-10-09 and
2026-10-10). Its section 8 lists the Lodestone changes L1 to L18. This file keeps the same numbers, so
a task can be traced back to the reason in that note. The Velox-side work (its sections 4, 7, and 9) is
not in this file.

## 2. Rules for this work

- Each task names its proof: what the result is compared against. A task with no comparison is not
  done.
- Ask the author before each decision in section 6.
- Phase F stays paused. Do not start F2 to F6 work. Section 5 lists the tasks that touch phase F, and
  what each one must not decide alone.
- At the end of each task, update section 4 of this file.

## 3. Tasks

The order is: first what blocks the first graph integration, then what the graph needs to be correct,
then the larger design work.

### R1. Unblock the first integration (all small)

| # | Task | Why the graph needs it | Proof |
|---|---|---|---|
| L10 | Add a client-only configure. One option (for example `LODESTONE_BUILD_COOKER`). When it is off, add `tools/suggest` and `client` only. Move `find_package(Vulkan)`, Slang, xxHash, Tint, the embed step, and `lodestone` into the cooker branch. Let an Emscripten configure pass: today every compiler branch ends in `FATAL_ERROR` for Emscripten. | The web build cannot build Slang. Velox adds Lodestone on every platform. | A client-only configure builds `lodestone::client_library` and `manifest_dump` with no Slang, no Tint, and no Vulkan SDK. The full configure still passes every test. |
| L1 | **Closed: `First` removed** (2026-10-14, by the author). It returned the smallest matching key, an arbitrary choice that no graph wants. `Single` covers the one-variant case. Old text: fix `ManifestQueryBuilder::First` with no constraints. `convertToScanConstraints` returns an empty list both for "no constraints" and for "a value matched nothing", and `First` reads both as a failure (`ShaderManifestIndex.cpp:249`). Give the two cases different results. | A one-variant module, or an unpinned query, cannot use `First`. | `ManifestIndexTest` rows: no constraints gives the smallest key. A value that matches nothing gives an error. |
| L2 | **Done** (2026-10-14). Add a `Single()` terminal. Success only for exactly one variant. Otherwise an error that names the axes that still vary across the result. | The graph must never choose a variant in silence. `First` returns the smallest key, which is an arbitrary choice. | `ManifestIndexTest` rows: one match, zero matches, and two matches that name the axis. |
| L9 | Add `ModuleView::FindEntryPoint(name)`, beside `BundleView::FindModule`. | A graph node names its entry point. | `ManifestIndexTest` rows: a found name, and an unknown name. |
| L13 | **Done** (2026-10-14): `lodestone::NearestName` in `ShaderManifestIndex.hpp`. Expose the nearest-name suggestion (`tools/suggest`) through the client headers. | A binding-name typo in the graph gets the same "did you mean" as a query. | `SuggestTest` covers the function. A client test calls it through the client header only. |
| L12 | **Decided (D-R1), no change yet.** Lodestone keeps the usage flags for now: they will become shader annotations, a condensed set over Vulkan and WebGPU (see L17). Decide who owns `ResourceFlags.hpp`. The client carries Velox's creation vocabulary (`BufferUsageFlags`, `TextureUsageFlags`), and the names already differ from Velox's copy (`RGBA8Unorm` against `Rgba8Unorm`). Recommendation: Lodestone keeps the shader-facing types (`TextureFormat`, `TextureViewDimension`). Velox owns the usage flags. | One definition. Today the values agree by position only. | Velox builds against the Lodestone `TextureFormat`, and the Velox copy is gone. Decision D-R1 first. |
| — | **Done** (2026-10-14). A cook as a build step. A CMake function that runs `lodestone_cooker_console` with `add_custom_command`, with the `.slang` files as dependencies, and a cache path (for example `LODESTONE_COOKER_EXECUTABLE`) for a host cooker in a cross build. | A shader edit must re-cook. The Emscripten build needs a cooker that a native build made. | Velox edits one `.slang` file and the next build re-cooks it. A second build with no edit does not. |

Done in this slice:l

- **L10, the client-only configure** (2026-10-14). `LODESTONE_BUILD_COOKER` defaults to `NOT EMSCRIPTEN`.
  With it OFF, the build adds `tools/suggest`, `client`, `tools/json`, and `manifest_dump` only.
  `cmake/LodestoneCooker.cmake` holds the cooker half. The tests follow the cooker, and tests ON with
  the cooker OFF is a configure error. Preset `ninja-msvc-client`. Proof: that preset configures in
  1.4 s, builds 10 steps, and its cache holds no Slang, Tint, Vulkan, or xxHash entry. Its
  `manifest_dump` reads the KitchenSink bundle. The full preset passes 24 of 24 tests and 60 of 60
  known-good dumps. The Emscripten proof is the Velox web build. The old claim that every compiler
  branch fails under Emscripten was stale: `590d80a` removed those branches.
- **L9, `ModuleView::FindEntryPoint`**, by the author (`e113d8c`).
- **L8, the merged layout.** `MergedLayoutView::Merge` (`01dc278`) gives each
resource of a pipeline once, with a stage mask, in (group, binding) order. `MergedLayoutTest` proves it.
The source note asked for `MergeLayoutInto`. The final shape is a validated range, with no template and
no allocation.

### R2. Make the graph correct (small to medium)

| # | Task | Why the graph needs it | Proof |
|---|---|---|---|
| L15 | **Done 2026-10-15** (format 8, `AxisDefaultTest`). **Decided 2026-10-15:** `DefaultDigit` on `ModuleAxis` (per module, because root axes are shared), format 8. An axis with no initializer fails the cook. A default outside the declared values fails the cook. A profile whose policy drops the default gets a warning. `GetDefault()` becomes `GetCanonicalValue()`, beside a new `GetDeclaredDefault()`; keys do not change. Measured 2026-10-15: `getDefaultValueBlob` returns literal initializers only (bool, `uint` 512, the enum tag 5 for `QualityLevel::Low`). It fails on an expression (`2u * 2u`), and reflection does not expose the `= Passthrough` of an `extern struct` (Slang keeps it in `StructDecl::aliasedType`, which no reflection call returns). So the symbol table text is the primary reader for all four domains, and the reflection blob is a second opinion where it returns a value. Record the declared default of each axis. `PermutationAxis::GetDefault()` returns the first listed value, not the initializer in the shader (`IFFT_SIZE` declares `= 512u` and lists 128 first). Read the initializer at the bootstrap compile, and store it on the manifest `Axis` record. | The graph fills an unpinned axis with the default (Velox decision V3). Without this, the warning names one value and the graph uses another. | A test module whose initializer is not its first value. The manifest default equals the initializer. Manifest format version 8. |
| L3 | Per-entry-point axis relevance in the manifest. The cook already measures axis influence for each entry point. Emit one mask for each (module, entry point). | An exact ambiguity check: two variants that differ only in an inert axis give one entry point instance. The graph pins only the axes that matter for a node. | The emitted mask equals the measured influence. `DedupeInfluenceTest` holds the measurement, so compare against it. |
| L4 | Query presets, with a match rule, and a "constrain when active" form for a gated axis. The rule that the source note proposes: a preset skips an axis that a module does not declare, and the graph fails when no module in the graph declares it. | Graph pins are presets. Today a pin on a gated axis removes every variant where the gate is off. | `ManifestIndexTest` rows on the gated manifest: a "when active" pin keeps the gated-off variants. Decision D-R2 first. |
| L7 | Sampler and texture pairs. WebGPU needs a non-filtering sampler for an `unfilterable-float` texture. Tint's inspector gives the pairs. Store them in the manifest. | Explicit bind group layouts. Without the pairs, Velox must guess the sampler type. | The `WgslValidator` reads the pairs from Tint. The manifest pairs equal Tint's pairs for KitchenSink. |
| L14 | **Done 2026-10-19** (format 11, `EntryPointConstantsTest`; D-R6 answered 2026-10-20, workaround for shader-slang/slang#13789). Push constants in the merged layout done 2026-10-20 (`MergedLayoutView::PushConstants`). State how an entry point `uniform` parameter appears in the manifest, and fix the cross-check for it (phase F step F2.7, the empty `entryPointParams` name). | The graph treats an entry point parameter as per-dispatch data. Ocean FFT `SpectrumConjugateCS` uses one. | `EntryPointParamsCookTest` passes with the cross-check on. `AccessModelRejectTest` keeps a mismatch row (F2.7 replaces it). Question to the author first. |
| L18 | **Done** (`DerivedConstantTest` reads a `[ls_extent_1d]` width from the manifest). Add `ls_extent_1d`. | `IfftSinCosLUT` is a `Texture1D`. Its extent cannot be stated in the shader today. | A `ResolveStageTest` row, and a footprint in the manifest. |

### R3. Larger work (design first)

| # | Task | Why the graph needs it | Note |
|---|---|---|---|
| L5 | Device features and limits, with a WGSL table: `enable` directives (`f16`, `subgroups`), workgroup size and invocation product, `groupshared` bytes, bindings per stage, bind group count. | The device filter. Ocean FFT uses `half`, so it needs `shader-f16` today, and no cook states it. | This is phase F slice F3. Section 5. |
| L6 | An axis bound to a device property. `IFFT_WAVE_SIZE` must equal the subgroup size. | The device filter picks the wave size, and the "prefer on" order for a boolean capability. | Design first, with L5. |
| L16 | A resource report beside the dedupe report: for each binding, which creation fields the shader fixes, and which the CPU must give. Measure the axis set of each footprint, do not parse it, and compare the two (a validator). Put the measured set in the manifest. | A shader edit prints the CPU work it causes. A resource that is complete needs no graph code. | Source note §8.2. Build the report from the written manifest through the client views. |
| L17 | **Done 2026-10-20** (format 12, `ResourceRoleTest`). Roles `IndirectDispatch`, `IndirectDraw`, `IndirectDrawIndexed`, `IndexData`, `VertexData`, checked against the kind and the stride (stride 4 needs an element count of whole commands); `IndirectCount` waits for capabilities. Free tags (`[ls_resource_tags]`) added beside the roles, stored and not interpreted. `ResolvedResource::RequiredBufferUsage` gives the creation usage. A role annotation, for example `[ls_resource_role("IndirectArgs")]`, for the usages that no binding shows. Check the stride of an `IndirectArgs` buffer (12, 16, or 20 bytes). | Velox decision V2: derive the creation usage. | Source note §8.3. The value joins the binding interning key (rule 3). |
| L11 | Optional: a dispatch extent attribute, resolved per variant like a footprint. | The thread count of a dispatch gets one source. | Velox decision V6: the graph-side `DispatchThreadsOver` comes first. Do this only when a case needs it. |

### Lodestone items found in this work (not in the source note)

| Item | State |
|---|---|
| Worker thread count against memory | `todo.md`, "Cook driver and manifest". A 16-worker cook commits about 0.7 GB more than a single-threaded one. With no page file, an out-of-commit cook crashes with no error. |
| `manifest_dump` writes the key `"slots"` | Rename to `"entryPointInstances"`. The code no longer says "slot". |
| Slang checkout | On 2026-10-12 the checkout moved back to `ac945e5`, the commit the superproject records (13 commits before `6eb89786c`). Tests and known-good dumps pass on `ac945e5`. Decide which commit to keep, and record it. |
| A module cache from another Slang version stops every cook | Found 2026-10-14. `%TEMP%\LodestoneShaderCooker` held IR version 32 modules from `6eb89786c`. `ac945e5` reads version 31 only, so the root module load fails (E00131). A multi-threaded cook then crashes with 0xC0000409 and no message (handoff fault 2). Fix: put the cache in a subfolder named by the Slang build tag. |

## 4. Progress

- 2026-10-14. List written. L8 done (`01dc278`). Next: R1, starting with L10.
- 2026-10-14. L10 done (not committed). L9 done by the author (`e113d8c`). D-R3 answered: `LODESTONE_BUILD_COOKER`, default `NOT EMSCRIPTEN`. Next: L1.
- 2026-10-14. L1 closed: the author removed `First` and `first`. L2 done: `Single()` returns
  `std::expected<VariantKey, QueryError>`, because a bare code cannot name the varying axes.
  `AmbiguousVariant` puts the axis names in `AxisName` and the match count in `Detail`. An axis varies when
  its digit differs between two matches (D-R5: inert axes count until L3). L13 done: `NearestName` is
  public in `ShaderManifestIndex.hpp`. The query errors already used it for an unknown axis and for an
  enum or type value. `ManifestIndexTest` has 87 checks. Next: L12 (D-R1), then the cook build step.
- 2026-10-14. L12: the usage flags stay in Lodestone (D-R1). The cook build step is done:
  `lodestone_add_shader_cook` (`cmake/LodestoneShaderCook.cmake`), and a cooker switch `--depfile` that
  writes `ShaderLibrary.d` with every file Slang read (`emit/Depfile`). The cooker console now builds
  whenever the cooker does, because a parent turns the tools off. `LODESTONE_COOKER_EXECUTABLE` names a
  host cooker for a build with no cooker. Proof, measured: a scratch consumer with a client-only Lodestone
  and a host cooker cooks once; a second build does nothing; an edit to the `__include`d fragment alone
  re-cooks; an edit to the root module re-cooks. The in-tree path (the `lodestone_cooker_console` target)
  cooked once and did nothing on the second build. R1 is complete. Next: R2, from L15.
- 2026-10-15. L15 done. `ModuleAxis::DefaultDigit`, format version 8. The symbol table reads the initializer
  text, including `extern struct NAME : I = Impl;` in a table of its own. Reflection (`getDefaultValueBlob`)
  is the second opinion for literal initializers, and a disagreement fails the cook (`AxisDefaultMismatch`).
  New errors 140 to 144 and `ManifestAxisDefaultMismatch` (108). `VerifyManifestRoundTrip` reads each default
  back as a client does. `GetDefault()` is now `GetCanonicalValue()`. Client: `ModuleView::AxisDefaultDigit`,
  `ManifestIndex::AxisDefault`, and a `"default"` key in `manifest_dump` and the space stage dump (the eight
  KitchenSink space known-good dumps were accepted: one added line for each axis, nothing else). Every
  existing asset resolves its defaults. Not covered by a test: the mismatch error itself, because no shader
  makes the two readers disagree. Next: L3.

- 2026-10-15. Digression: derived constants in size expressions (`todo.md`). Capture and carry done; the author writes the stage 4 evaluation (`docs/agent-handoff.md` section 2a). Then L3.
- 2026-10-16. Derived constants done (stage 4, errors 150 to 153, `DerivedConstantTest`). 26 of 26 tests, 60 of 60 known-good dumps. Next: L3.
- 2026-10-16. Slang moved to `v2026.19-87-ge9fc02c8d` (IR module versions 34 to 35). The module cache folder is now
  `%TEMP%\LodestoneShaderCooker\<Slang tag>-<config>`, the bootstrap thread writes the cache once (temporary file,
  then rename), `SLANG_VERSION_FULL` is cleared on each configure, and the Slang DLL copies refresh. 15 known-good
  dumps differ, all from the new Slang: code length and hash, and `OutVolume` storage format `Invalid` -> `R32Float`
  (the emitted WGSL always said `r32float`). Not accepted yet: the author decides.
- 2026-10-16. L3, decisions D1 to D4 agreed and built (D5 to D7 wait for the author). D1: relevance compares the
  whole entry point instance (source, layout with footprints, raster, workgroup), in `model/AxisInfluence`, moved
  out of `emit/DedupeReport`. D2: per environment. D3: Undetermined counts as relevant. D4: format 9,
  `EnvironmentHeader::AxisRelevanceTableOffset` (was `Reserved`), `EntryPointCount * AxisMaskWordCount` words,
  client `AxisRelevance` and `IsAxisRelevant`, `Open` error `InvalidAxisRelevance` (50), round-trip error
  `ManifestAxisRelevanceMismatch` (109), `manifest_dump` key `"relevantAxes"`. Proof: a `DedupeInfluenceTest` row
  that fails under the old code-only measurement (verified), `ManifestIndexTest` rows (100 checks), 26 of 26 tests.
  KitchenSink cook time unchanged (7.05 to 7.17 s). Open: D5 (`Single` scoped to entry points), D6 (a validator that
  derives relevance from the written manifest), D7.
- 2026-10-17. L3 done. D5: `ManifestQueryBuilder::SingleFor(entry_points)` (the author chose a separate terminal).
  It joins the relevance masks of the named entry points, ignores the other axes, and returns the smallest key
  when the matches differ only in those. New query errors `UnknownEntryPoint` (with a suggestion) and
  `EmptyEntryPointSet`. `Single` is unchanged. D6: `VerifyAxisRelevance` in `VerifyManifestRoundTrip` reads the
  written environment only. It derives the mask again (content comparison through the views: source bytes,
  binding records except `FirstUniformMember`, uniform members, footprint, raster records, workgroup) and checks
  that each class of variants that agree on an entry point's relevant axes holds one instance
  (`ManifestAxisRelevanceIncomplete`, 113). It replaced the plain read-back comparison. Found: the per-axis
  measurement is incomplete when a sparse cook breaks a chain (a `ManifestIndexTest` fixture shows it), and such a
  cook now fails. Decision for the author: keep failing, or make the cooker close the mask (mark every axis that
  varies inside an inconsistent class, until no class is inconsistent). `ShaderManifestRejectTest`'s fixture had
  two variants and no axis, which the validator rejects, so it got the one axis it modelled. 26 of 26 tests,
  `ManifestIndexTest` 111 checks; KitchenSink passes with and without `--no-dedupe`. Next: R2 continues with L4.
- 2026-10-18. The author chose option 2: the cooker closes the mask (`CloseRelevance` in `model/AxisInfluence`). In
  each class of variants that agree on the relevant axes and hold more than one instance, it marks every axis that
  varies within the class, until no class does. The validator runs the same closing pass on the written bytes, so
  its separate consistency check and error 113 (`ManifestAxisRelevanceIncomplete`) were removed: an equal mask is
  closed. Correction to the explanation of 2026-10-17: the broken-chain fixture closes to {A, B, C}, not {A, B}. A
  pair-based rule depends on which pair it finds first, and the minimal choice is not unique. Proof: with the
  cooker's closing pass off, three `ManifestIndexTest` rows fail (the mask, the validator, and a `SingleFor` query
  on C alone that would return one of two code paths); with the validator's closing pass off, two rows fail. The
  hand-built fixtures in `ManifestIndexTest` and `MergedLayoutTest` set no `Canonical` assignment; the closing pass
  read past it and `MergedLayoutTest` crashed, so the fixtures now canonicalize, and `ComputeActualInfluence`
  asserts the length (Debug only). The KitchenSink masks did not change. 26 of 26 tests, `ManifestIndexTest` 113
  checks.
- 2026-10-19, L4. `PinMode { Required, WhenActive }` on each constraint. A WhenActive constraint matches a variant
  where its axis is inactive, and tests the value only where the axis is active. Two constraints on one axis join
  their values, and the result is WhenActive when either is, because (active and A) or (inactive or B) is
  (inactive or A or B). `filterKeys` keeps digit 0 in the key band for a WhenActive axis, because an inactive axis
  stores its canonical digit, 0. `WhereWhenActive` (bool, uint, named). `QueryPreset` owns its pins, and
  `ManifestQueryBuilder::Apply` skips a pin on an undeclared axis with a `PinAxisNotDeclared` note (`Notes()`);
  a note never fails a terminal. A bad value on a declared axis is `ValueNotInAxis`. Proof: `ManifestIndexTest` 122
  checks; with the `filterKeys` change reverted, three rows fail. 26 of 26 tests. Then two more notes:
  `NotesFor(entry_point_names)` adds a `PinAxisIrrelevant` note for each constrained axis that cannot change the
  named entry points (it shares `joinRelevance` with `SingleFor`, because a const terminal cannot add notes), and
  `DuplicateValue` for a value that the axis constraint or the same call already names. Only the callers that take
  user values check duplicates, so overlapping `WhereNoneOf` complements give no note. `ManifestIndexTest` 129 checks.
- 2026-10-19, L18. `ls_extent_1d` (`RawSizeAttributeKind::Extent1d`, one argument). The footprint already held
  three extents, so the format did not change for it. Proof: `ResolveStageTest` rows (a 1d extent fills X and
  leaves Y and Z at 1; no argument is a parse error), and `DerivedConstantTest` reads the width of a `Texture1D`
  sized by `CLUSTER_COUNT` from the manifest for each variant. Not done: no check that the attribute's arity
  matches the texture shape, for any arity.
- 2026-10-19, L7, format 10. The sampler and texture pairs come from the emitted code: Tint's inspector for WGSL,
  and each `OpSampledImage` traced back to its variables for SPIR-V (the SPIR-V reader now parses the function
  bodies). The reader runs with `--no-validate` too, because the pairs are cook output. Each entry point instance
  stores a pair list (positions in its visibility list), interned in a seventh interner. `Profile::Flags` records
  whether the profile read its pairs. Both profiles store them (author's decision: Vulkan needs them to check
  filter support, which is undefined behavior otherwise, and one client path serves both APIs). Validators: the
  manifest round trip compares the pairs, `Open` checks them, and `VerifySamplerPairsAcrossProfiles` compares wgsl
  against spirv for each shared variant. Client: `EntryPointInstanceView::SamplerTexturePairs`,
  `EnvironmentView::HasSamplerTexturePairs`, `MergedLayoutView::SamplerTexturePairs` (the pipeline union, with
  stage bits). `manifest_dump` writes `"samplerTexturePairs"` only when recorded. Proof: KitchenSink and
  ParameterBlocks cook with both targets and the two readers agree (KitchenSink: the pairs follow the
  `NormalMap` and `DetailMap` axes, and the depth `Cascades` pairs with `CompareSampler`); rows in
  `WgslValidatorTest`, `SpvValidatorTest`, `ShaderManifestRejectTest`, and `MergedLayoutTest`. 26 of 26 tests.
  `check-known-good.py`: 33 of 60 dumps differ, only by the new pair fields and the 15 Slang-update differences.
- 2026-10-19, L14, format 11. **Decided:** the record is `EntryPointConstants` (a name that does not suggest compute
  only), and a SPIR-V push constant block above 128 bytes fails the cook now. Measured: with ordinary data, Slang
  wraps the entry point parameters in one constant buffer, a uniform buffer `entryPointParams` on WGSL (std140) and
  a push constant block on SPIR-V (std430, no placement), and the resources sit in the buffer's element, which the
  scope walk never entered. Both targets remove data that the entry point does not read, and partial use keeps the
  whole block. Slang's metadata does not track push constants, so stage 3 states a block and the SPIR-V read clears
  it when the code has none (`ReconcileEntryPointConstants`), and it compares the block's extent with reflection's
  size. Each entry point instance stores a constants record (delivery, size, members, the uniform buffer's
  visibility position), interned in an eighth interner. The reader API became `ReadEmittedCodeFacts`
  (`EmittedCodeFacts`: the pairs and the push constant extent). Client: `EntryPointInstanceView::Constants()`.
  Proof: `EntryPointConstantsTest` (hand-computed std140 and std430 layouts for both targets, unread data, resources
  only, the 128-byte limit); rows in `SpvValidatorTest` (the extent, with a revert drill on the matrix major) and
  `ShaderManifestRejectTest`. 27 of 27 tests. `EntryPointResourceStruct.slang` now cooks, so the mismatch row of
  `AccessModelRejectTest` became `EntryPointUnusedResource.slang`: Slang's metadata marks every slot of the
  parameters' constant buffer as used (`collectMetadataFromInst`), so an unread resource beside read data is a
  mismatch. Open: decision D-R6 (section 6), and the merged layout does not read the constants.

## 5. Phase F, paused

The graph shapes phase F, so these tasks must not decide a phase F question alone:

| Task | Phase F step | The risk |
|---|---|---|
| L5, L6 | F3 (capability set, device filter) | The feature and limit tables are the F3 design. Write them once, for WGSL and SPIR-V. |
| L14 | F2.7 | The same fix. Do it once. |
| L7, L15, L18 | — | Each changes the manifest. Batch them, so the format version changes as few times as possible. |
| L3 | F5 (binding-time lowering) | A moved axis leaves the key. The relevance mask must not assume the key never shrinks. |

Phase F state at the pause: F0 and F1 are done. F2 (the shim module and the two tables) was next. The
merged layout rejects every placement that is not Bound (`MergedLayoutPlacementUnsupported`). F4 must
decide how a pointer or a handle names its container before a merge can read it.

## 6. Decisions for the author

| # | Question | Recommendation |
|---|---|---|
| D-R1 | L12: which side owns which type in `ResourceFlags.hpp`? | **Answered:** Lodestone keeps the usage flags too, for now. Annotations will cover most of them, as a condensed set over the union of the Vulkan and WebGPU usages. |
| D-R2 | L4: what does a preset do with an axis a module does not declare? | **Answered (2026-10-19):** skip it, and keep a path open for a warning at query resolution that names the unused pin, so users learn they need not set it (no cargo-cult pins). Each pin has a mode, Required or WhenActive; a WhenActive pin passes a variant where the axis is inactive. A declared axis with an undeclared value is an error (`ValueNotInAxis`). Presets are a client value type only; a cooked form is in todo.md. |
| D-R3 | L10: the option name, and its default when Lodestone is not the top-level project. | **Answered:** `LODESTONE_BUILD_COOKER`, default `NOT EMSCRIPTEN`. |
| D-R4 | L1: how `First` reports "no constraints" against "no match". | **Moot:** `First` is removed. |
| D-R5 | L2: does `Single()` ignore an inert axis (L3) from the start, or only after L3? | After L3. Until then, two variants that differ only in an inert axis are an error that names the axis. |
| D-R6 | L14: Slang's metadata marks every resource beside entry point constants as used when any part of the parameters is used, so an unread resource there fails the cook. What decides its visibility? | **Answered (2026-10-20):** reported as shader-slang/slang#13789; a workaround takes the visibility of those resources from the emitted code, with a canary test that fails when Slang fixes it. Old text: Open (2026-10-19). Options: take the visibility of those resources from the emitted code (the cross-check then cannot find a missing binding there); fix `collectMetadataFromInst` in Slang and report it upstream; or leave the cook failing, so an author moves such a resource to the global scope or a `ParameterBlock`. |
