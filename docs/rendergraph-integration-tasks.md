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
| L14 | State how an entry point `uniform` parameter appears in the manifest, and fix the cross-check for it (phase F step F2.7, the empty `entryPointParams` name). | The graph treats an entry point parameter as per-dispatch data. Ocean FFT `SpectrumConjugateCS` uses one. | `EntryPointParamsCookTest` passes with the cross-check on. `AccessModelRejectTest` keeps a mismatch row (F2.7 replaces it). Question to the author first. |
| L18 | Add `ls_extent_1d`. | `IfftSinCosLUT` is a `Texture1D`. Its extent cannot be stated in the shader today. | A `ResolveStageTest` row, and a footprint in the manifest. |

### R3. Larger work (design first)

| # | Task | Why the graph needs it | Note |
|---|---|---|---|
| L5 | Device features and limits, with a WGSL table: `enable` directives (`f16`, `subgroups`), workgroup size and invocation product, `groupshared` bytes, bindings per stage, bind group count. | The device filter. Ocean FFT uses `half`, so it needs `shader-f16` today, and no cook states it. | This is phase F slice F3. Section 5. |
| L6 | An axis bound to a device property. `IFFT_WAVE_SIZE` must equal the subgroup size. | The device filter picks the wave size, and the "prefer on" order for a boolean capability. | Design first, with L5. |
| L16 | A resource report beside the dedupe report: for each binding, which creation fields the shader fixes, and which the CPU must give. Measure the axis set of each footprint, do not parse it, and compare the two (a validator). Put the measured set in the manifest. | A shader edit prints the CPU work it causes. A resource that is complete needs no graph code. | Source note §8.2. Build the report from the written manifest through the client views. |
| L17 | A role annotation, for example `[ls_resource_role("IndirectArgs")]`, for the usages that no binding shows. Check the stride of an `IndirectArgs` buffer (12, 16, or 20 bytes). | Velox decision V2: derive the creation usage. | Source note §8.3. The value joins the binding interning key (rule 3). |
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
| D-R2 | L4: what does a preset do with an axis a module does not declare? | Skip it. The graph fails when no module in the graph declares it. |
| D-R3 | L10: the option name, and its default when Lodestone is not the top-level project. | **Answered:** `LODESTONE_BUILD_COOKER`, default `NOT EMSCRIPTEN`. |
| D-R4 | L1: how `First` reports "no constraints" against "no match". | **Moot:** `First` is removed. |
| D-R5 | L2: does `Single()` ignore an inert axis (L3) from the start, or only after L3? | After L3. Until then, two variants that differ only in an inert axis are an error that names the axis. |
