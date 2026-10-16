# Agent handoff

Read this first. It gives the state, the next work, the open faults, and the facts that are expensive
to measure again. `CLAUDE.md` gives the build rules, the code map, and the author's positions. This file
is newer than `CLAUDE.md` on anything that moves.

Text in this file follows ASD-STE100. Compressed on 2026-09-24. Section 10 keeps one line for each
earlier update. Git history holds the full text.

`docs/` is in `.gitignore`. This file survives only as a force-add: `git add -f docs/agent-handoff.md`.

---

## 1. State on 2026-09-24

| Configuration | Build | Tests | Known-good dumps |
|---|---|---|---|
| RelWithDebInfo, `ninja-msvc` | green | 23 of 23 | 30 of 30 |
| Debug, `ninja-msvc` | green | Not run in full since F1.4. `SpvValidatorTest` passes. Skip `KitchenSinkCookTest` (section 3, item 3). | — |

- Phases D and E are complete. E8 closed Phase E. Their plans were removed on 2026-10-14. Read them
  from history: `git show e113d8c:docs/phase-e-data-driven-permutations.md` (§11a holds the final
  measurements).
- The manifest is one bundle for each cook, format version 8. Version 7 added `Profile::CodeFormat`, and
  version 8 added `ModuleAxis::DefaultDigit` (2026-10-15, task L15). `CLAUDE.md` ("One output form") holds the
  layout.
- The client reads the bundle through `BundleView`, `ModuleView`, `EnvironmentView`, and the view types
  (`VariantView`, `EntryPointInstanceView`, `LayoutRange`, `ResolvedResource`, `UniformMemberRange`).
  `ShaderSourceProvider` allocates nothing, and `BindingInfo` is gone.
- The query layer reads the axis-active mask. A constraint on an axis selects only the variants where
  that axis is active. `Decode` and `Enumerate` set `QueryAxisValue::Active`.
- `--target` is required. A command line with no target fails with `NoTargetSpecified`.
- The KitchenSink set is the reference. `KitchenSinkCookTest` cooks 616 variants. The known-good check
  compares its five modules, six stages each.

**2026-10-14: phase F is paused.** F0 and F1 are done (two targets, `wgsl` and `spirv`, both Bound;
`docs/phase-f-plan.md` section 7). The next work is the rendergraph integration in VeloxRhi:
`docs/rendergraph-integration-tasks.md`. RelWithDebInfo: 24 of 24 tests, 60 of 60 known-good dumps
(`wgsl` and `spirv`), when the PC has free commit (section 3, item 8).

**2026-10-15.** R1 is complete (L10, L9, L1 removed with `First`, L2 `Single`, L13 `NearestName`, the cook
build step with `--depfile`). L15 (declared axis defaults, format 8) is done. 25 of 25 tests, 60 of 60
known-good dumps. The author is now writing the stage 4 half of derived constants (section 2a). After
that: L3.

**2026-10-16.** Derived constants are done (section 2a). 26 of 26 tests (RelWithDebInfo), 60 of 60
known-good dumps. Next: L3.

## 2a. Derived constants: notes for the stage 4 work (2026-10-15)

`todo.md` ("Permutation system") holds the item. Size expressions may name a module-scope `static const`
whose value is computed from axes, for example `VTF_CLUSTER_COUNT = GRID_X * GRID_Y * GRID_Z`.

**Done (capture and carry).**

- `SymbolTable::DerivedConstantsForModule` returns each non-`extern` `static const NAME = <expr>;` line, with
  the expression text, the source path, and the line number, in source order. `AddSource` takes the source
  path. One keyword scan (`ScanDeclarationKeywords`, whole tokens) classifies every line for all three
  extractors.
- The line scan cannot tell scopes apart, so `SlangModuleContext::BuildModuleScopeVariableNames`
  (reflection) lists the module-scope variable names, and `PrepareRawModule` keeps only the matches. The
  walk enters `__include` fragment nodes only, and skips structs, functions, generics, and namespaces.
- `RawModule::DerivedConstants` (`RawDerivedConstant`: name, expression, file, line) carries them to stage 4.
  The Raw stage dump lists them (file name only). KitchenSink has one: `TILE = 64u` in KsVolume.
- Probe asset: `tests/assets/DerivedConstants/` (module scope, a constant that names another, an
  `__include` fragment, a struct member, a function local).

**Done 2026-10-16 (the agent wrote it, on the author's request).** The shape differs from the plan in two ways. `MakeDerivedConstantTable` runs once for each module, before the variant compile, and checks the names: a derived constant with the name of an axis or an extern constant is an error (the author's decision), and so is an ambiguous name, even when nothing reaches it. `MakeResolveContext` takes the table and the `RawVariant`, so its messages can name the binding. Errors: 150 shadows, 151 ambiguous, 152 cycle, 153 unreadable. Measured: Slang rejects a constant cycle itself (E39999), so the cycle check fires only when the text reader and Slang disagree. The original plan follows.

**The plan:**

1. `MakeResolveContext` (`model/ResolveStage.cpp`, called once per variant from `BuildModuleStep.cpp`)
   also takes `raw_module.DerivedConstants` and the variant's `SizeAttributes`, takes the sink, and returns
   `CookResult<ResolveContext>`.
2. Roots: call `CollectExpressionIdentifiers` on each `RawSizeAttribute::Arguments` string. These strings are
   the expressions as written (`"IFFT_SIZE * 4"`); an attribute has no name of its own.
3. Precedence: `context.Symbols` already contains the axes and the extern defaults, so it is the exclusion
   set. A name found there is a leaf. A name that is a derived constant is visited. A name that is neither
   is left for `EvaluateExpression` to report as `AttributeExpressionUnknownSymbol`. No separate axis set is
   needed.
4. Lookup: index the derived constants by name once per module (a sorted vector of indices and
   `equal_range`). A reached name with two different expressions is ambiguous: report every `file:line`.
   This is real: a function-local constant can share a name with a module-scope one, because reflection
   filters by name only.
5. Order: depth-first search with three-color marking. Edges come from `CollectExpressionIdentifiers` on
   each constant's own expression. Reaching a gray node is a cycle: report the chain with each `file:line`.
   Post-order is a topological order, and black nodes are skipped, so a diamond is evaluated once. The walk
   reaches only what an attribute names, so a constant that cannot be evaluated (a `float`, a function call)
   is harmless while nothing names it.
6. Evaluate in post-order with `EvaluateExpression(expression, context.Symbols, sink)`, and push each result
   as an `AttrExprSymbol`. Names are views into `RawModule` strings, which outlive the loop.
7. Errors: bands 40 and 140 are full, so use a new band (150 is free): cycle, ambiguous, unreadable. The
   author wants verbose messages with source locations, because nothing checks the value independently
   (reflection cannot fold an expression, and the element count is not in the emitted bindings).
   `BuildModuleStep` already adds the variant description.
8. Docs: the header comment of `ResolveStage.hpp` ("two halves"), the `ResolveContext` row and "Attribute
   expressions" in `CLAUDE.md`, and the `todo.md` item.

**Tests.** `ResolveStageTest` rows with no Slang: one level, two levels, a diamond, a cycle, an ambiguous
name, an unused constant that cannot be evaluated, and axis precedence. End to end: add
`[ls_element_count("LIGHT_SLOTS")]` to `gLightIndices` in the probe asset. `GRID_X` is 4 or 8 and `GRID_Y`
is 2 or 4, so `LIGHT_SLOTS = X * Y * 16` gives 128, 256, 256, and 512. Read the counts back through the
manifest, as `AxisDefaultTest` reads defaults.

**Not in scope.** An axis initializer that names another constant (`= BASE * 2u`) still fails as
unreadable. Axis defaults are resolved before any variant exists, so only axis-independent constants
could be used there.

## 2. Next work, in order

0. **The rendergraph tasks** (`docs/rendergraph-integration-tasks.md`), from R1 down. Phase F waits.
1. **Answer the open Phase F questions.** Done on 2026-09-24. `docs/phase-f-vocabulary.md` §9a to §9e
   hold the answers. §9e corrects §9a: an interface axis selects the access model at link time.
   WebGPU stays Bound (`DescriptorHandle` gives invalid WGSL). Through Slang, a WGSL `override` sizes
   nothing. The Vulkan names and feature bits are in §9d. They also fill the
   capability name table (section 3, item 4).
2. **The Phase F plan is written.** `docs/phase-f-plan.md` holds the decisions (D1 to D11), the slices
   (F0 to F6), the open decisions (O1 to O6), and the progress. Read its section 7 first. F0 is done.
   The next step is F1.1.

Two client decisions are open. Ask the author before you build either.

- **Query presets.** A preset is a module-agnostic constraint set that matches an axis by name. The match
  rule is open: what a preset does with an axis that a module does not declare.
- **The persisted form of a variant.** `DecodedVariant` holds string views into one manifest and no axis
  names, so it cannot outlive that manifest. A persisted form needs (axis name, value) pairs with owned
  strings, and a load that re-keys them through a query.

## 3. Open faults

Ordered by what can write wrong output first.

1. **Only codegen reads an error that Slang returns with a success code.** `ParseSlangDiagnostics` now
   returns the count of failure records, and codegen fails on it. `link`, `loadModule`, and
   `createCompositeComponentType` still read the return code alone. No case is measured for them yet.
2. **A worker setup failure crashes the cook, and its cause never prints.** In
   `src/compile/impl/ThreadPool.cpp`, the helper on the calling thread throws `std::runtime_error` when
   `RunWorkerSetup` fails. Nothing catches it, so the process ends with 0xC0000409. A worker thread
   returns instead, but its thread sink is not merged, so the Slang diagnostic is lost. Measured on
   2026-09-24 during step F0.3. Return a `CookError`, and merge the thread sinks on every path.
   On 2026-10-14 a stale module cache caused it. `%TEMP%\LodestoneShaderCooker` held IR version 32
   modules from Slang `6eb89786c`, and the `ac945e5` build reads version 31 only. **Found 2026-10-16:** the
   writer is our own stale Debug tree. `build/ninja-msvc` Debug was last built on 2026-10-01 from
   `6eb89786c`, its DLL still carries the `ac945e5` tag string, and `run-tests.bat` defaulted to Debug. Each
   bare run poisoned the cache for the next RelWithDebInfo cook. The script now defaults to RelWithDebInfo.
   `build/ninja-msvc-nowgsl` is also on `6eb89786c`. Rebuild or delete both trees. The cache-folder fix
   below would also make this harmless. A second defect is in the same place: every worker thread calls
   `WriteModuleCache` (`ThreadPool.cpp`) and writes the same files at once, while other sessions can read
   them through the cache search path. No failure is measured from that yet. Every Slang test
   crashed with 0xC0000409 and no message. A `--single-threaded` cook prints the cause (E00131). Rename
   the folder to clear it. The cache path must name the Slang build tag
   (`docs/rendergraph-integration-tasks.md`, found items).
3. **The Debug Slang build asserts on `KsMaterial`.** This is a Slang fault, not ours. Section 5 has
   the cause and a two-file reproduction. A Debug run can also hang: the full Debug test script once
   waited 15 minutes with no output. Do not run `run-tests.bat Debug` unattended. Run the Debug tests
   one at a time, with a timeout, and skip `KitchenSinkCookTest`.
4. **The capability requirement is always zero.** `Profile::CapabilityFloor` and
   `Variant::CapabilityRequirement` wait for a capability set: a bitmask over a string table of
   capability names, such as Vulkan extension names. The author agreed to that shape. Since F1.2, Slang
   warning 41012 is off, so a variant can need a device capability and no cook states it. Phase F plan
   F3.3 and F3.4 close this. Do not let them slip.
5. **Push constants and specialization constants are reserved, not cooked.** `BindingKind::PushConstant`
   exists, and nothing produces it. The specialization-constant table is always empty. Section 7 holds
   the plan.
6. **Fixed 2026-09-25: `--dump-sources` wrote nothing without `--dump-stage`.** Its block sat inside the
   Cooked dump branch. The dumper still adds a text header, so a SPIR-V dump is invalid until F1.3.
7. **No guard fails a cook that emits no manifest.** The required `--target` closes the zero-target
   case. A guard in the emit path is still open.
8. **The thread count is not tuned.** `ThreadPool::Initialize` uses `hardware_concurrency` and ignores
   `SlangCompilerCreateInfo::ExpectedBatchSize`. Measure before you tune. Each worker loads its own core
   module: KitchenSink commits 1.62 GB with 16 workers and 0.89 GB single-threaded. Slang does not check
   `malloc`, so a cook that runs out of commit crashes with 0xC0000005, 0xC0000409, or 0xC00000FD and no
   message. On 2026-10-12 the author's PC had no page file, and `KitchenSinkCookTest` crashed that way.
   `todo.md` holds the item.
9. **The Slang global-session convoy.** `slang_createGlobalSession` serializes per-thread startup.
   `k_UseSlangWorkaround` in `SlangModuleContext.cpp` toggles a bypass. It helps a release cook and hurts
   a Debug cook. It is `true` now. Read it before you measure anything.
10. **`tests/DeclKindProbe.cpp`** is a throwaway Slang probe. Remove it when it stops earning its place.

Review points from the view-type work, not faults:

- `CheckManifestLayout` compares through `ResolvedResource::Record()` and the two record comparators.
  Moving it onto the view accessors is a separate change.
- A view points at its `EnvironmentView`. A view from a temporary environment, or from a moved
  `ShaderSourceProvider`, dangles. The headers document this.

`todo.md` holds the rest, grouped by area.

## 4. Build and test

`CLAUDE.md` holds the full rules. These points each cost a session once.

- Use `scripts\build.bat`, never a bare `cmake --build`. Read the exit code of the build itself. A pipe
  into `grep` or `tail` gives the exit code of that command.
- A change to the `third_party/slang` submodule rebuilds all of Slang, about ten minutes.
- After an interrupted regeneration, a Debug link can fail with `LNK1163` on a COMDAT in
  `ShaderManifest.cpp.obj`. A clean recompile of that file fixed it on 2026-09-24. If it comes back on a
  clean tree, it is a real fault.
- Run `scripts\run-tests.bat RelWithDebInfo` after each change. A green build proves less than it looks.
- **Before 2026-09-24, `run-tests.bat` reported a crash as a pass.** It checked `if errorlevel 1`, and a
  crash exits with a negative code. Do not trust a green run of the script from before that date.
- Run `python scripts\check-known-good.py` after a change to reflection, resolve, intern, or freeze. It
  cooks each KitchenSink module and compares every stage dump. It once found a defect that no validator
  saw. Accept a changed dump only after you read the diff.
- For a change that must not change output, save the KitchenSink bundle hash and a
  `manifest_dump --with-sources` JSON before the change. Compare both after it. The WGSL baseline since
  F1.3 is `ae72f733...`.
- **An incremental link can keep stale code.** Measured on 2026-09-25: after a change to
  `src/emit/StageDump.cpp`, ninja relinked `lodestone_cooker_console.exe`, and the exe still ran the old
  function. Delete the `.exe` and the `.ilk`, then build. Do this when a fix seems to have no effect.

## 5. Measured facts (do not measure again)

Slang documents none of these. Probe modules measured them.

**Facts from the phase E spikes** (removed on 2026-10-14, full text in
`git show e113d8c:docs/phase-e-attribute-spike.md` and `phase-e-interface-spike.md`):

- An `extern static const` reflects as a `Variable` decl, and reflection reads each attribute string.
- An imported axis is not in the importer's reflection. The session holds every loaded module, so the
  axis read iterates the loaded modules, not the root module alone.
- An `__include`d fragment reflects as an Unsupported (kind 0) node named for the file. Its decls are
  children of that node, so the axis read recurses. Do not call `getType` on an Unsupported node: it
  crashes. `getDeclSourceLocation` fails for a decl inside a fragment, so a missing location is soft.
- Link-time specialization fills an `extern struct` of interface type, by the same mechanism as an
  `extern static const`. No API lists the types that conform to an interface: walk the decls and test
  each struct for conformance. The type name is not module qualified.
- **An interface conformance cannot carry a resource.** The resource member does not become a
  parameter, the layout does not change, and codegen fails with E56003 ("use of uninitialized opaque
  handle") at the use site, not at the conformance. So an interface axis carries behaviour only, and
  resource presence stays a constant axis. Phase F step F2 (the shim module) must respect this.

Other fact sources: `src/compile/impl/SlangReflector.cpp` holds the scope, block, and binding-range walk.

**Reflection.**

- An imported module is a requirement, so a synthetic axis module must be a component.
- A user attribute cannot take a type argument, and a generic user attribute does not parse. Reflection
  reads only int, float, and string arguments.
- `getScalarType()` on an enum returns `None` across a module boundary. The enum case read uses the
  width of the case value blob and assumes a signed `int` (`EnumTagKindFromByteWidth`).
- A `TypeReflection` is interned for each session, so pointer identity holds across modules.
  `FindDeclaringModule` uses this to find the module that declares an axis type.
- A `ParameterBlock` that holds only nested blocks reflects as a zero-size `ConstantBuffer` container,
  with descriptor set index 0 and type kind `ParameterBlock`. The walks discriminate on the type kind,
  so the container is descended, not drafted as a uniform buffer.
- `extern static const` on a struct compiles. A grouped configuration struct is possible.

**WGSL.**

- Slang does not infer a depth texture from usage. A `Texture2DArray<float>` sampled with `SampleCmp`
  emits `texture_2d_array<f32>`, and Tint rejects it. Use `DepthTexture2D`, `DepthTexture2DArray`, or
  `DepthTextureCube` with a `SamplerComparisonState`. Issue shader-slang/slang#6942.

**Debug Slang.**

- `loadRootModule` on `KsMaterial` raises `unexpected: duplicate global instruction`. The check is
  `checkIRDuplicate` in `third_party/slang/source/slang/slang-ir-link.cpp`, inside `#ifdef _DEBUG`, so a
  release Slang does not run it. It fires inside `prelinkIR`.
- **The cause is Slang PR #12574** ("Let prelink supply imported interfaces instead of re-deriving
  them", Slang commit `4cf253d0c`). Our commit `557ea09` moved Slang from `28c755b09` to `ac945e536`,
  and #12574 is in that range. It lowers an interface that another module owns as a bare declaration,
  and `prelinkIR` clones the owner's definition in at load time.
- **The probe target `SlangPrelinkRepro`** (`tests/SlangPrelinkRepro.cpp`) links only Slang and loads
  the reproduction. Build it with `--target SlangPrelinkRepro` in Debug. Run it after each Slang update:
  exit code 0 means the fault is gone. The `ninja-msvc` cache now has `SLANG_ENABLE_SLANGC=ON`, so a
  Debug `slangc` is at `build/ninja-msvc/third_party/slang/Debug/bin/slangc.exe`. The repository default
  in `cmake/ConfigureSlang.cmake` is still OFF.
- **Upstream state on 2026-09-24.** Our Slang commit `6eb89786c` was the head of `master`, and the fault
  reproduces there. Release `v2026.18.2` contains #12574. The first release that contains it is
  `v2026.17`. Only `6eb89786c` was run.
- **A two-file reproduction** is in `tests/assets/SlangPrelinkRepro/`. It uses no Lodestone attribute
  and no axis. Measured on 2026-09-24 with the Debug Slang at `6eb89786c`, a fresh cache each time:

  | Case | Result |
  |---|---|
  | An imported struct implements an imported interface and calls `max`/`dot`. The consumer holds a local of that struct and calls `normalize`. | asserts |
  | The same, but the consumer calls no intrinsic | passes |
  | The same, but the implementation calls no intrinsic | passes |
  | The same, but no interface anywhere | passes |
  | An `extern struct` or a `typealias` to the concrete struct | asserts either way |

  In KsMaterial, `normalize` and `saturate` in `shadeOne` supply the consumer intrinsic, and KsShading
  supplies the rest. KsVolume passes only because it calls no core intrinsic of that kind.
- **The module cache is not the cause.** A fresh `--cache-dir` still asserts.
- **The module cache is shared by every build.** The default is `%TEMP%\LodestoneShaderCooker`, for
  Debug and RelWithDebInfo and for every Slang version. A stale Debug binary that reads modules a newer
  Slang wrote aborts with exit code 3 and prints nothing. Rebuild both configurations after a Slang
  update, or give each run its own `--cache-dir`.
- **An edited source reaches the cook through the shared cache.** Measured on 2026-09-24: an in-place
  edit of an imported module showed up in the next cook. The mechanism that skips the stale
  `.slang-module` is not known. Slang tries a binary before a source, and we do not set
  `UseUpToDateBinaryModule`.

**Slang behaviour found in phase F step F0.**

- **Slang can report an error and return a success code.** For WGSL, a specialization constant in
  `numthreads` gives error E55205, a success code from `getEntryPointCode`, and
  `@workgroup_size(1, 1, 1)`. Tint accepts that text. Codegen now fails on any failure record.
- **`loadModuleFromSourceString` for a builtin breaks the worker sessions.** The bootstrap loads, but each
  worker then fails to load the serialized root module from its IR blob, and Slang writes no diagnostic.
  An import through the file system works. `EmbeddedFileSystem` serves the builtins for that reason.
- **A plain `ISlangFileSystem` hides the paths.** Slang wraps it in a cache that uses a content hash as
  the identity, so `getDependencyFilePath` gives `name.slang:<hash>`. Implement `ISlangFileSystemExt`.

**Cook cost.** `git show e113d8c:docs/phase-e-data-driven-permutations.md`, §11a. A one-variant cook takes about 0.4 s,
so startup dominates a small cook. KitchenSink takes about 3.4 s for 616 variants.

## 6. Design decisions in force

- **Variant keys are per module.** A shared axis value set under one name in two modules was a footgun,
  and one global key forced a phase barrier. Shared meaning lives in shared types (a shared enum or
  interface) and in query presets, never in a global axis.
- **A type-backed axis gets its identity from its type.** An enum or interface axis needs no attribute
  argument. Only an integral or boolean axis would need an explicit shared-axis reference, and that
  reference must be a string.
- **A root axis is storage and dedup only.** Two modules share a root axis when name, kind, domain, and
  binding time agree and the values fit in order. No key crosses a module boundary.
- **A constraint on an axis implies that the axis is active.** `WhereNoneOf` is a complement
  `WhereAnyOf`, so it follows the same rule.
- **Plain integer indices in the public API.** Strong index types were tried and removed. `CLAUDE.md`
  ("A guardrail must pay for itself at the call site") holds the rule. Do not propose them again.
- **Bindings are resolved on demand.** `ShaderSourceProvider::Bindings` returns a `LayoutRange`. The
  provider caches nothing, because a client reads the bindings once to build a pipeline.
- **The iterators are input iterators.** A proxy iterator returns its value by value, so it cannot meet
  the legacy forward or random-access requirements.
- **Phase F decisions.** `docs/phase-f-plan.md` section 2 holds them. In short: WebGPU stays Bound. The
  access model lives on the profile and adds no axis. Shim interfaces (`IReadBuffer<T>` and others)
  carry it, and two tables in code map each interface and profile to one impl. Link-time
  specialization selects the impl.

## 7. Deferred capability areas (do not lose)

These cannot wait much longer once a second target exists.

- **Bindless (Indexed and Pointer).** The reflected schema (kind, shape, access, format, member layout)
  is the invariant, and placement is the variant. Under bindless the schema facts are the only guard
  against a mismatch a driver would otherwise catch, so they matter more. Add unbounded and runtime-array
  detection then. Do not design the Indexed and Pointer payloads until the target is concrete.
- **Specialization constants.** Slang reflects them (`SPECIALIZATION_CONSTANT`). WGSL spells them
  `override`. The table slot exists.
- **Push constants.** Slang reflects them (`PUSH_CONSTANT_BUFFER`). Model them as their own category. On
  Vulkan a push-constant member can be a buffer device address, which ties them to the Pointer model.
- **Portable geometry (vertex and index pulling).** `todo.md` ("Phase F: portable geometry") holds the
  design. One unknown comes first: entry-point synthesis for the input-assembler arm, because Slang ties
  varying inputs to entry-point parameters. Not scheduled.

## 8. How this author works

Read "Working with this author" and "How this repository thinks" in `CLAUDE.md`. Three points:

- She reserves the implementation work she enjoys. Plan it and explain it. Write it only when she asks.
- Ask before a design choice that is hers. State a recommendation, not a survey.
- Measure, do not estimate. A probe module answers a Slang question in one build.

## 9. Tooling notes

- `tools/manifest_dump` reads a bundle and writes JSON. `--with-sources` adds the source text.
- `tools/cooker_console` (`lodestone_cooker_console`) is the CLI. `--dump-stage=all` writes six dumps,
  named `<module>_<target>_<Stage>.json`. Before 2026-09-24, the Raw dump was empty and the Resolved file
  held the Interned dump. Do not trust a dump from before that date.

## 10. History

One line for each update. Git history holds the full text.

- **2026-09-17.** Phase E done through E7, plus enum axes. The client query surface was complete.
- **2026-09-19.** The cooker driver became a chain of step functors (`src/driver/steps/`). Cross-module
  enum resolution was fixed. The per-module key decision was made.
- **2026-09-22.** Tint replaced the text WGSL scanner. `BindingKind`, `ResourceShape` (a flag enum), and
  `ResourceAccess` were split. The member walk reached structured buffers. The block-of-blocks walk was
  fixed. Test targets reached 22.
- **2026-09-22, night.** The multi-module bundle (three scopes, root axes, per-environment extents) was
  built and wired into the driver. The round trip gained key-decode and axis-mask checks.
- **2026-09-23.** `TableRef` and format version 5. Strong index types were tried and removed. The view
  types were added.
- **2026-09-28.** F1.1 to F1.4 done. `spirv` is a second target with its own validator (`SpvValidator`, on
  SPIRV-Tools). Manifest format version 7 records the code format. The dedupe report prints the
  capabilities each environment needs. `docs/phase-f-plan.md` section 7 holds the detail.
- **2026-09-25.** F0 reviewed. Two style rules added to `.github/copilot-instructions.md`: an array
  initializer opens its brace on a new line, and a value leaves `std::expected` through
  `std::move(*result)`. `CLAUDE.md` ("Builtin modules") records what `EmbeddedFileSystem` covers.
- **2026-09-24, F0.** A reflection mismatch and a codegen error fail the cook again. `ThreadPool`
  passes the real compile error. The builtins are compiled in and served by `EmbeddedFileSystem`. The
  WGSL switch builds OFF. `run-tests.bat` catches a crash. Every `CookError` has a printed name.
- **2026-09-24, later.** Phase F §9 answered. §9a corrected: link-time specialization selects the
  access model. Two validator faults found, and F0 fixed both. `docs/phase-f-plan.md` written.
- **2026-09-24.** On-demand bindings, and `BindingInfo` removed. `--target` became required. The query
  layer reads the axis-active mask. `AxisNames` and `AxisValues` were added. The stage dumps were fixed
  and the KitchenSink known-good baseline was accepted. The stale `CookTest` line was retired. E8 closed
  Phase E. Format version 6 added `Binding::SampleType`, read through `ResolvedResource::SampleType()`.
- **2026-09-25 to 2026-10-01.** Phase F slices F0 and F1: the `spirv` profile, format version 7
  (`ShaderCodeFormat`), `SpvValidator`, per-target policy, and SPIR-V known-good dumps.
- **2026-10-12 to 2026-10-14.** `MergedLayoutView` (L8). The diagnosis of the commit-exhaustion crash.
  Phase F paused for the rendergraph tasks.
- **2026-10-14.** L10: `LODESTONE_BUILD_COOKER` and the client-only preset. The Slang checkout is
  `ac945e5` since 2026-10-12. A stale module cache from `6eb89786c` stopped every cook.
- **2026-10-15.** L1 (removed `First`), L2 `Single`, L13 `NearestName`, the cook build step and
  `--depfile`, L15 axis defaults (format 8, `AxisDefaultTest`), the comment style rules in
  `.github/copilot-instructions.md`, and the capture half of derived constants (section 2a). Two bugs in the
  refactored L15 resolver were fixed: a by-value output parameter, and `index.value()` on an error.
- **2026-10-16.** Derived constants, stage 4: `MakeDerivedConstantTable`, the three-color walk in
  `MakeResolveContext`, errors 150 to 153, `DerivedConstantTest`, and `ResolveStageTest` rows. The hang
  and E00131 failures came from the stale Debug test binaries (Slang `6eb89786c`), which `run-tests.bat`
  ran by default. The default is now RelWithDebInfo.
