# Agent handoff

Read this first. It gives the state of the repository, the facts that are expensive to re-measure, and
the next task. `CLAUDE.md` gives the build rules and the code map, and this file is newer than
`CLAUDE.md` on anything that moves.

Text in this file follows ASD-STE100. Updated on 2026-09-17.

`docs/` is in `.gitignore`. This file survives only as a force-add. Use `git add -f
docs/agent-handoff.md`. Run `git ls-files docs/` to see which other documents are tracked.

---

## 1. State on 2026-09-17

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
