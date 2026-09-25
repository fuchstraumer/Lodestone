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
| RelWithDebInfo, `ninja-msvc` | green | 22 of 22 | 30 of 30 |
| Debug, `ninja-msvc` | green | 21 of 22: all but `KitchenSinkCookTest` (section 3, item 3) | — |

- Phases D and E are complete. E8 closed Phase E. Its final measurements are in
  `docs/phase-e-data-driven-permutations.md` §11a.
- The manifest is one bundle for each cook, format version 6. Version 6 added `Binding::SampleType`. `CLAUDE.md` ("One output form") holds the
  layout.
- The client reads the bundle through `BundleView`, `ModuleView`, `EnvironmentView`, and the view types
  (`VariantView`, `EntryPointInstanceView`, `LayoutRange`, `ResolvedResource`, `UniformMemberRange`).
  `ShaderSourceProvider` allocates nothing, and `BindingInfo` is gone.
- The query layer reads the axis-active mask. A constraint on an axis selects only the variants where
  that axis is active. `Decode` and `Enumerate` set `QueryAxisValue::Active`.
- `--target` is required. A command line with no target fails with `NoTargetSpecified`.
- The KitchenSink set is the reference. `KitchenSinkCookTest` cooks 616 variants. The known-good check
  compares its five modules, six stages each.

**Phase F is the next work.** Section 2 gives the order.

## 2. Next work, in order

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
3. **The Debug Slang build asserts on `KsMaterial`.** This is a Slang fault, not ours. Section 5 has
   the cause and a two-file reproduction. A Debug run can also hang: the full Debug test script once
   waited 15 minutes with no output. Do not run `run-tests.bat Debug` unattended. Run the Debug tests
   one at a time, with a timeout, and skip `KitchenSinkCookTest`.
4. **The capability requirement is always zero.** `Profile::CapabilityFloor` and
   `Variant::CapabilityRequirement` wait for a capability set: a bitmask over a string table of
   capability names, such as Vulkan extension names. The author agreed to that shape.
5. **Push constants and specialization constants are reserved, not cooked.** `BindingKind::PushConstant`
   exists, and nothing produces it. The specialization-constant table is always empty. Section 7 holds
   the plan.
6. **`--dump-sources` wrote no source file** for the one-variant cook of the Q3 workgroup probe on
   2026-09-24. Not investigated. Check it before you rely on the flag.
7. **No guard fails a cook that emits no manifest.** The required `--target` closes the zero-target
   case. A guard in the emit path is still open.
8. **The thread count is not tuned.** `ThreadPool::Initialize` uses `hardware_concurrency` and ignores
   `SlangCompilerCreateInfo::ExpectedBatchSize`. Measure before you tune.
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
  `manifest_dump --with-sources` JSON before the change. Compare both after it.

## 5. Measured facts (do not measure again)

Slang documents none of these. Probe modules measured them.

**Documents that hold more facts.** `docs/phase-e-attribute-spike.md` holds the attribute and axis-read
facts, including the `__include` recursion. `docs/phase-e-interface-spike.md` holds the interface-axis
facts. `src/compile/impl/SlangReflector.cpp` holds the scope, block, and binding-range walk.

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

**Cook cost.** `docs/phase-e-data-driven-permutations.md` §11a. A one-variant cook takes about 0.4 s,
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
- **2026-09-24, F0.** A reflection mismatch and a codegen error fail the cook again. `ThreadPool`
  passes the real compile error. The builtins are compiled in and served by `EmbeddedFileSystem`. The
  WGSL switch builds OFF. `run-tests.bat` catches a crash. Every `CookError` has a printed name.
- **2026-09-24, later.** Phase F §9 answered. §9a corrected: link-time specialization selects the
  access model. Two validator faults found, and F0 fixed both. `docs/phase-f-plan.md` written.
- **2026-09-24.** On-demand bindings, and `BindingInfo` removed. `--target` became required. The query
  layer reads the axis-active mask. `AxisNames` and `AxisValues` were added. The stage dumps were fixed
  and the KitchenSink known-good baseline was accepted. The stale `CookTest` line was retired. E8 closed
  Phase E. Format version 6 added `Binding::SampleType`, read through `ResolvedResource::SampleType()`.
