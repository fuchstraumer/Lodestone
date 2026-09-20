# Agent handoff

Read this first. It gives the state of the repository, the facts that are expensive to re-measure, and
the next task. `CLAUDE.md` gives the build rules and the code map, and this file is newer than
`CLAUDE.md` on anything that moves.

Text in this file follows ASD-STE100. Updated on 2026-09-19. Sections 1 to 8 hold the 2026-09-17 state.
Section 9 holds the 2026-09-19 update, and it supersedes section 1 on the build state.

`docs/` is in `.gitignore`. This file survives only as a force-add. Use `git add -f
docs/agent-handoff.md`. Run `git ls-files docs/` to see which other documents are tracked.

---

## 1. State on 2026-09-17

**Superseded by section 9.** The 2026-09-17 green baseline predates the cooker driver refactor now in
progress. Read section 9 for the current state before you trust this table.

Phase E is done through E7, and enum axes followed it. The client query surface is complete and frozen.
The next task is the manifest redesign for Phase F, then E8, the documentation pass.

| Configuration | Build | Tests | Known-good dumps |
|---|---|---|---|
| RelWithDebInfo, `ninja-clang-cl` | green | 21 of 21 | 12 of 12 |

Sixteen targets are unit tests. Five are cooks. `scripts\run-tests.bat` reports `all targets passed`,
and `python scripts\check-known-good.py` reports every dump matches. Verified on 2026-09-17.

The MSVC tree is stale. Only `build/ninja-clang-cl` is current. Build `build/ninja-msvc` before you
trust a claim about MSVC.

## 2. What this phase built

The cooker is a pipeline of eight stages between two problem domains. `CLAUDE.md` holds the stage map.
This phase built the parts below, and each one is complete.

- **The compiler split.** Stage 3, compile, and stage 4, resolve, are separate. No file under
  `src/compile/` evaluates a `[ls_*]` attribute, and `model/ResolveStage.cpp` names no Slang type. A
  thread pool spreads variants across workers.
- **The axis and policy engine.** An axis is declared in the shader, on its `extern static const` (or
  `extern struct`), as an `ls_axis_*` attribute. The cook policy is a TOML file. Constraint expressions
  gate and prune the space. Interface axes and enum axes both work.
- **The manifest.** The cook writes one binary manifest for each module. A consumer finds a variant by
  key, not by index. The manifest carries the axis schema. `ShaderManifestView::Open` validates the
  whole graph once, so the runtime accessors trust the data.
- **The client query surface.** See section 3.

## 3. The client query surface (frozen)

`client/include/ShaderManifestIndex.hpp` and `client/src/ShaderManifestIndex.cpp` hold it, in
`lodestone::client_internal`. The API and its semantics are frozen. The manifest format under it is
not, so section 4 can change the format without touching this surface.

`ManifestIndex` opens a manifest, decodes a key, enumerates every variant, and returns a
`ManifestQueryBuilder`. The builder is value-semantic, so each `Where` returns a new builder. It
resolves axis names and values by name, and pushes a `QueryError` with a nearest-name suggestion on a
miss.

- `Where` / `WhereAnyOf` / `WhereNoneOf` constrain one axis. `WhereNoneOf` keeps the complement, because
  each axis is a closed value set. An empty value set is an error.
- Terminals: `Keys`, `Variants`, `VariantsFromKeys`, `First`, `IsValid`, `Errors`.
- A malformed query returns an error code. A valid query with no matching variant returns an empty set.
  These are different results, and a caller acts on each one differently.
- `VariantKey` is an in-session handle, not a save token. To persist a variant, save the decoded axis
  names and values, then rebuild the key on load.

`ManifestIndexTest` proves the surface with one axis of each domain, every error path, and a sparse and
a gated manifest. `EnumTagDecodeTest` proves the enum tag-blob read with no compiler.

## 4. Next: the manifest redesign, then E8

The manifest becomes multi-module and multi-level. `todo.md`, under "Phase F preparation", holds the
plan. The decisions settled in review:

- **Three scopes.** Whole-cook data holds the string table, the source table, the axes table, and the
  target-profile table. Per-module data holds the logical variant table and the `ModuleAxis` runs, which
  do not vary by profile. Per-(module, profile) data holds the baked layout: the access model, the
  per-target member offsets, and the capability requirement each variant was cooked for. The query layer
  reads the logical scope, so it stays profile-agnostic.
- **Axes move to the root.** Each module holds a `ModuleAxis` run, one record per axis, that gives a
  root axis index and a mask of the values that module uses. The root axis holds the union of the
  values, so two modules that declare the same axis name stay coherent by construction. The variant key
  packs against the root value count (root radix), so decode stays simple and the mask is metadata only.
- **A per-variant mask over axes** records which axes are active in a variant. This closes the
  over-return, where a query for a gated axis's default value also returns the variants where the axis
  is inactive.
- **A profile is a target and an access model, plus a capability floor.** It is a cooked form, not a
  device power tier. Device power is an axis, chosen at run time. The index returns a profile-scoped
  builder for the run-time path, and the logical layer under it stays profile-agnostic.
- **Placement is not a group and a binding.** A bound placement is a group and a binding. A pointer
  placement is a byte offset. Model placement as a variant, so the manifest does not harden a WebGPU
  assumption. This is Phase F item D4, and it is cheap now.

E8 is the last Phase E step. It is the documentation pass and a fresh measurement of the cook numbers.

## 5. Build and test

`CLAUDE.md` holds the full rules. Two points repeat here, because each one costs a session.

Read the exit code of the build itself. A pipe into `grep` or `tail` gives the exit code of that
command, so a failed build reads as a success.

Run the tests after each change. A green build proves less than it looks. A null-pointer read once
passed every build and stopped every cook, and `scripts\run-tests.bat` found it on the first cook.

`python scripts\check-known-good.py` is the finer check. It cooks each module and compares every stage
dump against `tests/known_good/`. It once found a reflection defect that no validator saw: a bad texture
read gave every texture an invalid sample type, and the cook still exited 0. Accept a changed dump only
after you read the diff.

## 6. Measured Slang facts (do not re-measure)

Probe modules measured the Slang reflection behavior this repository depends on. Slang documents none of
it. The facts live in the code that uses them and in the spike documents. Read those before you touch
reflection.

- `docs/phase-e-attribute-spike.md` — the attribute and axis-read facts, including the `__include`
  recursion and the soft source location.
- `docs/phase-e-interface-spike.md` — the interface-axis facts: a link-time `extern` type works, an
  interface axis carries no resource, and `getFullName` is not module-qualified.
- `src/compile/impl/SlangReflector.cpp` — the scope, block, and binding-range walk. One fact governs it:
  an imported module is a requirement, so a synthetic axis module must be a component instead.

## 7. Open work

Ordered by what can write wrong output first.

- **A compiled-in absolute path.** `src/compile/impl/SlangModuleContext.cpp` calls
  `std::filesystem::canonical` on `C:/SoftwareDev/Lodestone/tests/assets/`. `canonical` throws on a path
  that does not exist, so a cook on another machine stops with exit code 3. Give this search path the
  treatment the other three get, or add a field to `SlangCompilerCreateInfo`.
- **A push constant cannot be cooked.** `FromSlangBindingType` has no row for
  `slang::BindingType::PushConstant`, and `BindingKind` has no value for it. WGSL has no push constants,
  so nothing is lost yet. `docs/phase-f-vocabulary.md` makes this a question.
- **The thread count is wrong.** `ThreadPool::Initialize` does not read
  `SlangCompilerCreateInfo::ExpectedBatchSize`. The old optimum of three to four workers was measured
  with LTO on and a Slang record lock in place. Both changed, so measure again before you tune.
- **The Slang global-session convoy.** `slang_createGlobalSession` serializes per-thread startup across
  the process. `k_UseSlangWorkaround` in `src/compile/impl/SlangModuleContext.cpp` toggles a bypass. The
  bypass is a release-only win, and it makes a Debug cook worse. Read its current value before you
  measure anything.
- **The MSVC tree is stale.** Section 1.
- `todo.md` holds the rest, grouped by area.

## 8. How this author works

Read "Working with this author" in `CLAUDE.md`. Three points need repetition.

The author reserves the implementation work she enjoys. Plan it, point at the call sites, and let her
write it unless she asks.

Establish that a cost exists before you help her remove it. State plainly when a cost is negligible.

Measure, do not estimate. A probe module and a temporary print answer a Slang question in one build.

---

## 9. Update 2026-09-19

### 9.1 State

The cooker driver refactor is in progress, so the tree is not fully green. The 16 unit tests still pass,
and the client query surface is unchanged and frozen. The cook path is mid-refactor. `KitchenSinkCookTest`
is blocked on the uniform buffer walk bug in section 9.3. A cook now needs an explicit `--target`. Verify
the whole suite once the refactor settles, because some cook tests may need the target argument.

### 9.2 The cooker driver is now a set of discrete steps

`RunCook` no longer holds one long loop. `src/driver/CookerDriver.cpp` calls a chain of step functors,
and `include/driver/CookerSteps.hpp` defines the state each step passes on. The step sources sit in
`src/driver/steps/`. The order is `PrepareCookStep` once for the whole cook, then, for each (module,
target) pair, `PrepareModuleStep`, `PreparePermutationSpaceStep`, `BuildModuleStep`, and
`FinalizeModuleStep`. `SharedCookState` carries the whole-cook data, and each per-module step takes it by
const reference, so the per-module chain can run in parallel later. A cook now holds several modules and
several targets at once. The old single-target assumption is gone.

Each step returns a `CookResult<NextState>`, so the chain stops on the first error. Each state struct
carries its own optional stage-dump strings, and the driver writes them through the sink. This keeps the
steps free of the sink, which is a whole-cook resource.

### 9.3 In progress: the uniform buffer walk on a block of blocks

A `ParameterBlock` whose element holds only nested `ParameterBlock`s, with no ordinary data and no direct
resource, breaks the reflection walk. `KsMaterial`'s `Surface` over `SurfaceResources` is the first case.
Measured facts:

- The outer block reflects at its parent scope as a binding range of type `ConstantBuffer`, with
  `descriptorSetIndex = 0` and a uniform size of zero. Its leaf type layout `getName()` returns
  `ParameterBlock`, and its type `getKind()` is `Kind::ParameterBlock` (value 11).
- The inner blocks reflect as `ParameterBlock` ranges with `descriptorSetIndex = -1`, which is the shape
  the sub-object walk expects.

Two failures follow. First, the ordinary range walk keeps the range, because its descriptor set index is
not negative, drafts it as a `UniformBuffer`, and then demands a nonzero byte size. Zero is the right
answer for an empty container, so the cook errors at `SlangReflector.cpp` in
`applyLeafTypeUniformBufferLayout`. Second, `ReadParameterBlock` skips the block, because its descriptor
set index is not negative, so the sub-object walk never steps into the block contents.

The fix must recognize a zero-size `ParameterBlock` container by its type kind, draft no uniform binding
for it, and step into its element with the binding-range and sub-object walk, not the uniform member
walk. The child scope must take the container's space offset, so the nested bindings land in the right
group. The author is writing this fix.

### 9.4 Bugs fixed this window

- **Cross-module type resolution for an enum axis.** An enum axis over an imported enum set its
  `RootModule` to the module where the axis variable lived, not the module that declares the enum. The
  synthetic per-variant module then imported the wrong module. `FindDeclaringModule` and `BelongsToModule`
  (anonymous namespace in `SlangModuleContext.cpp`) walk every loaded module's decl tree and match the
  type by pointer identity. A null-reflection guard skips a precompiled module with no reflection.
- **Enum scalar type on a cross-module reference.** `getScalarType()` on an enum returns `ScalarType::None`
  across a module boundary. The enum case read falls back to the case value blob width and assumes a
  signed `int`. `EnumTagKindFromByteWidth` does the mapping.
- **The empty cache directory.** The new `PrepareCookStep` now defaults an empty `ModuleCacheDirectory`,
  the way `ParseCommandLine` does, so a caller that builds `CookerOptions` by hand does not fail.
  `AccessModelRejectTest` hit this.
- **A crash on an empty `AllModuleNames`.** The new driver read `AllModuleNames[moduleIdx]` before
  anything filled the vector.
- **No target was set.** A cook now sets and reads a target, so a run cooks the modules instead of zero.
- **`ToString(CookError)`** did not name the filesystem error codes, so a cache failure printed a blank
  name. Confirm this is done.

### 9.5 Design decisions this window

- **Variant keys are per-module, not whole-cook global.** A shared axis value set under one name in two
  modules was a footgun, and a single global key forced a phase barrier before any module could be keyed.
  Both problems leave with per-module keys. Unreal keys per shader class and Unity keys per shader, so
  both engines key locally. See the memory note `axis-name-scope-open-question`.
- **Shared meaning lives in shared types and query presets, not a global axis.** A shared enum or
  interface gives a common vocabulary at the source level. A query preset gives a reusable, module-
  agnostic constraint bundle at the client level. Neither needs a global axis identity in the key space.
  A preset matches an axis by name, and it can check the axis type as a guard against a same-name clash.
- **An axis gets its identity from its type, for a type-backed axis.** An enum or interface axis needs no
  attribute argument. The variable's declared type carries the identity, and the declaring-module walk
  completes it. Only an integral or boolean axis, which has no type, needs an explicit shared-axis
  reference, and a string is the only reflectable attribute argument.

### 9.6 The manifest layout plan (preserved from an earlier turn)

This design review happened right after the last compaction, and the message was not delivered at the
time. The manifest becomes one file per cook, in the container-with-directory shape.

**The header.** A fixed 16-byte prefix starts the file: `uint32 magic`, `uint32 version`, `uint64
HeaderSize`. A reader loads the prefix, learns the header size, then loads the header alone. The header
is flat: the whole-cook front matter first, then the per-module headers in series. Reading the header
gives every locator a reader needs to seek to one module or one profile, without loading the whole file.

- For an O(1) seek to module K, keep a fixed-size per-module header, or put a module directory (an offset
  and a size for each module) in the front matter. The same holds one level down: a per-profile directory
  inside the module header makes a profile a seek, not a walk.
- "Load one module" still loads the shared whole-cook tables (strings, axes, values) plus that module's
  section. It is not zero-shared, because a module's records index into the shared tables by string index.
- The interior offsets are `uint32` today, so a manifest caps at 4 GiB. The `uint64 HeaderSize` buys
  alignment and header room only, unless the interior offsets widen too. Decide the interior width against
  what the bundle stores: source text for N modules now, and later baked output for each variant and each
  profile.
- Decide whether `version` gates the container layout, the record layouts, or both. Records are not pinned
  today.

**The three scopes.** The 20 tables sort into the scopes as follows.

- Whole-cook: Strings, Sources, Axes, AxisValues, and the new target-profile table.
- Per-module logical: VariantKeys, EntryPoints, the new `ModuleAxis` runs, and the new per-variant axis
  mask.
- Per-(module, profile): the baked layout. Bindings, ResourceIndices, ResourceLists, Footprints,
  FootprintLists, VisibilityIndices, VisibilityLists, Slots, Rasters, VertexInputs, ColorTargets, and
  UniformMembers. Offsets are per target.

**The freeze holds by construction.** The client query surface reads only four tables, through five
accessors: `String`, `Axes`/`Axis`, `AxisValues`/`AxisValue`, and `VariantKeys`. All four sit in the
whole-cook or the logical scope. The split cannot disturb the frozen API, as long as those four stay
reachable there.

**`ManifestVariant` is the record the split cleaves.** It mixes the logical identity (`Index`, and its
key correspondence) with per-profile layout pointers (`ResourceListIndex`, `FootprintListIndex`,
`FirstSlot`, `SlotCount`). The identity half, plus the new per-variant axis mask, belongs in the logical
scope. The layout pointers move to the per-(module, profile) scope. In the same way, `ManifestEntryPoint`
(name, stage) is logical, and `ManifestSlot` (source index, raster index, workgroup) is per-profile,
because the emitted text differs per profile.

**Root radix already matches the code.** `ManifestIndex` sets `radices[i] = axis.ValueCount` and packs
against it, so if `ValueCount` becomes the root union count, decode needs no change. But note the newer
decision in 9.5: keys are per-module now, so the root axes table demotes from an identity mechanism to a
storage and dedup optimization, and the per-module value mask stops being a decode-path reconciliation.

### 9.7 New measured Slang facts

- A user attribute cannot take a type argument. A generic user attribute does not parse. Reflection reads
  only int, float, and string argument values. So a shared-axis reference must be a string.
- `getScalarType()` on an enum is `None` across a module boundary. See section 9.4.
- A `TypeReflection` is interned per session, so pointer identity holds across a module boundary.
- `extern static const` on a struct compiles. A grouped configuration struct is possible. It is a roadmap
  item, not scheduled work.
- A `ParameterBlock` of only nested blocks reflects as a zero-size `ConstantBuffer` container. See 9.3.

### 9.8 New test asset and probe

- `tests/assets/KitchenSink/` is a multi-module stress set. `KsTypes` and `KsShading` declare shared
  types. `KsGeometry`, `KsMaterial`, `KsVolume`, and `KsPost` cook against them. It crosses every axis
  kind, shares axis names across modules, and reaches every resource shape. `KitchenSinkCookTest` cooks
  it. It is meant to break, and each break is a worklist item. `KsGeometry` cooks 16 variants.
  `KsMaterial` hits the uniform buffer walk bug in section 9.3.
- `tests/DeclKindProbe.cpp` is a throwaway Slang-only probe. It links only Slang, so it builds while the
  cooker is mid-refactor. It answered the enum and the block-layout questions this window. Remove it when
  it stops earning its place.
