# Agent handoff

Read this first. It gives the state, the open faults, and the facts that are expensive to measure again.
`CLAUDE.md` gives the build rules, the code map, and the author's positions. This file is newer than `CLAUDE.md`
on anything that moves. It holds no plan and no history of a finished task: a plan lives in its own document, and
git history holds the rest (the long form of this file is in `git show 9fe93d3:docs/agent-handoff.md`).

Text in this file follows ASD-STE100. Compressed on 2026-09-24 and on 2026-10-21. Keep it short: when a fault is
fixed, remove it; when a task is done, add one line to section 7 and remove its notes.

`docs/` is in `.gitignore`. This file survives only as a force-add: `git add -f docs/agent-handoff.md`.

---

## 1. State on 2026-10-24

- RelWithDebInfo, `ninja-msvc`: build green, 45 of 45 tests (after F4.0, not committed). Known-good dumps: 60 of 60
  (checked 2026-10-24).
- Debug: do not run it unattended (section 2, item 2).
- Manifest format version 17. `CLAUDE.md` ("One output form") holds the layout of each version's addition.
- Phases D and E are complete. Phase F has F0, F1, F2, F3.1 to F3.4c, F3.5, C11, F4.0, F4.A, F4.1, F4.2, F4.3, and most of F4.4 done; F4, F5, and F6 are open.
- **Phase F is the current work.** The VeloxRhi rendergraph tasks (`docs/rendergraph-integration-tasks.md`) are done
  except L11 (optional dispatch extent) and a JSON writer for the resource report. L5 and L6 became phase F slice F3:
  F3.1 to F3.4c are done (format 15, committed), and F3.5 (the capability-axis check) is done. **Next: the rest of F4 for `spirv-modern`**
  (the heap in the manifest, F4.5 offsets against the code, sampler pairs of handles; then the profile is complete and the
  test-only `AllowIncompleteProfiles` goes), then F4.6 (plan in `docs/phase-f-plan.md`). F3.4d (bindings per stage and
  bind groups) waits for a consumer to name its limits. After F3: F2 (the shim module and the two tables), F4 (Pointer and
  Handle placements, and unbounded binding arrays), F5 (binding-time lowering), and F6. `docs/phase-f-plan.md` has the
  slices and a progress log. The author calls phase F the last major feature work.
- **DiamondDogs requests still open** (`docs/diamonddogs-requests.md`): C6
  (generated host layout headers, after phase F, as a second writer over the written manifest), C3 (live cook,
  deferred), C4 (bindless, F4). C1, C2, C5 (all but F3.4d), C7, C8, C9, C11, C12, and C13 are done.
- **Comments are compressed** (2026-10-21). Write one-line comments by default. Technique explanations go to
  `docs/code-notes.md`, which links to the code. Code never links to `docs/`.
- **DiamondDogs** embeds Lodestone and sends requests in `docs/diamonddogs-requests.md`. Each row has a state.
  Do not edit the DiamondDogs tree: the author passes news to its agent.
- **The author wants feature discipline.** A feature goes in only with a named consumer and a test that proves
  it. Say so when a proposal has neither.
- **An audit track runs beside phase F** (2026-10-23). One agent audits and cleans up; another builds features. The test
  audit is in `docs/test-audit.md` (its status section lists what is done and open). The code audit (cognitive
  complexity, hidden costs, rule breaks) starts next; `docs/code-audit.md` holds the candidates found so far.

## 2. Open faults

Ordered by what can write wrong output first.

1. **Only codegen reads an error that Slang returns with a success code.** `link`, `loadModule`, and
   `createCompositeComponentType` still read the return code alone. No case is measured for them yet.
2. **The Debug Slang build asserts on `KsMaterial`.** A Slang fault (section 4). A Debug run can also hang. Run
   the Debug tests one at a time, with a timeout, and skip `KitchenSinkCookTest`. The Debug trees in
   `build/ninja-msvc` and `build/ninja-msvc-nowgsl` were built from an old Slang: rebuild them before any Debug
   run, or their cooks poison the shared module cache.
3. **The capability-axis check has a known gap (F3.5).** `VerifyCapabilityAxes` compares the kind `capability` with the
   measured names, and the author declares no names. An axis that controls two capabilities and leaves one in its off
   path passes. Slang warning 41012 is off, so nothing else catches it. Slang also states no capability for a dynamic
   index into a binding array; the SPIR-V reader adds it (C12).
4. **Specialization constants are reserved, not cooked.** The table is always empty. Push constants are cooked
   since format 11.
5. **The thread count is not tuned.** Each worker loads its own core module: KitchenSink commits 1.62 GB with 16
   workers and 0.89 GB single-threaded. Slang does not check `malloc`, so a cook out of commit crashes with no
   message (0xC0000005, 0xC0000409, or 0xC00000FD). Measure before you tune. `todo.md` holds the item.
6. **The Slang global-session convoy.** `slang_createGlobalSession` serializes per-thread startup.
   `k_UseSlangWorkaround` in `SlangModuleContext.cpp` (now `true`) toggles a bypass. It helps a release cook and
   hurts a Debug cook. Read it before you measure anything.
7. **shader-slang/slang#13789 has a workaround in the cook.** `ApplySlangWorkaround13789` (`BuildModuleStep.cpp`).
   `SlangMetadataCanaryTest` fails when Slang fixes the issue, and its header lists what to delete.
8. **shader-slang/slang#13895 has a workaround in the SPIR-V validator.** An array of write-only storage images gets no
   `NonReadable`. `IsSlangNonReadableArrayFault` (`SpvValidator.cpp`) accepts it with a warning, and
   `SlangNonReadableCanaryTest` fails when Slang fixes it, and its header lists what to delete.
9. **Axis declarations have no source location.** The axis-default errors (`AxisDefaultMissing`, `AxisDefaultNotDeclared`, ...)
   print none, because `getDeclSourceLocation` fails for the axis `Variable` decl and `RawAxisDeclaration::SourceFile` is
   `<unknown>`. Not measured why. The symbol table has the line, and could supply it.

`todo.md` holds the rest, grouped by area.

## 3. Build and test points that each cost a session

`CLAUDE.md` holds the full rules.

- Read the exit code of the build itself. A pipe into `grep` or `tail` gives the exit code of that command.
- A change to the `third_party/slang` submodule rebuilds all of Slang, about five minutes. Minimize kicking this off when you can, to a sensible degree, but don't fear it too much.
- A Debug link can fail with `LNK1163` on a COMDAT in `ShaderManifest.cpp.obj` after an interrupted
  regeneration. A clean recompile of that file fixes it. On a clean tree, it is a real fault.
- **An incremental link can keep stale code** (2026-09-25). If a fix seems to have no effect, delete the `.exe`
  and the `.ilk`, then build.
- `python scripts\check-known-good.py` takes about six minutes. Run it after a change to reflection, resolve,
  intern, or freeze, and accept a changed dump only after you read the diff. A Haiku subagent can run it and
  summarize the diff by line kind (`sed 's/[0-9]\+/N/g' | sort | uniq -c`), which saves context.
- Bash heredocs with `'''` break. Write a Python edit script with the Write tool. Edits must keep CRLF.
- `gh` is at `C:\Program Files\GitHub CLI\gh.exe`, logged in. Read only: never post for the author.
- **A removed test target leaves its `.exe` in `build/<preset>/tests/<config>/`**, and `run-tests.bat` runs every
  `*Test.exe` it finds, so the stale binary fails with `NoOutputSpecified`. Delete it after you remove a target.
- `git clang-format` fails here (its wrapper finds no Python). To format only changed lines, pass the diff hunks to
  `C:\Program Files\LLVM\bin\clang-format.exe --lines=<a>:<b>`. Do not reformat lines the author wrote.
- `std::tuple_size_v<T>` in a concept is a hard error on MSVC for a type that is not a tuple. Use
  `std::tuple_size<T>::value` (`tests/TestFormat.hpp` carries the NOLINT and the reason).
- A multi-line regex rewrite of C++ must stop at the end of its own statement: a lazy `.*?` over `re.S` once ran
  from one `println` to the next `return` and swallowed code. Back up the file, and read the diff.

## 4. Measured facts (do not measure again)

Slang documents none of these. Probe modules measured them. `src/compile/impl/SlangReflector.cpp` holds the
scope, block, and binding-range walk.

**Axes and reflection.** The phase E spikes have the full text (`git show e113d8c:docs/phase-e-attribute-spike.md`
and `phase-e-interface-spike.md`).

- An `extern static const` reflects as a `Variable` decl, and reflection reads each attribute string.
- An imported axis is not in the importer's reflection. The session holds every loaded module, so the axis read
  iterates the loaded modules.
- An `__include`d fragment reflects as an Unsupported (kind 0) node named for the file, with its decls as
  children. Do not call `getType` on it: it crashes. `getDeclSourceLocation` fails inside a fragment.
- Link-time specialization fills an `extern struct` of interface type. No API lists the types that conform to an
  interface: walk the decls and test each struct.
- **Where an interface conformance can carry a resource** (F2.0 spike, 2026-10-22, slangc 2026.18.1). Only where the
  `extern struct` is the type of a parameter: a field of a `ParameterBlock`, a global, or an entry point `uniform`.
  The linked impl then supplies the binding on both targets, also over a default with no resource, and reflection of the
  linked program agrees with the emitted code. As a function-local (`Buf b = {}`) a resource impl is unusable: the
  2026-09 spike got E56003, and slangc now emits a local `var<storage>` with no binding. An interface axis (a technique)
  is used as a local, so it carries behaviour only, and resource presence stays a constant axis. A shim extern (F2) is
  a parameter type, and carries a resource.
- A synthetic axis module must be a component, because an imported module is a requirement.
- A user attribute cannot take a type argument, and it has no optional or variadic parameters. Reflection reads
  only int, float, and string arguments. A list goes in one string.
- `getScalarType()` on an enum returns `None` across a module boundary (`EnumTagKindFromByteWidth`).
- A `TypeReflection` is interned per session, so pointer identity holds across modules.
- A `ParameterBlock` that holds only nested blocks reflects as a zero-size `ConstantBuffer` container of type kind
  `ParameterBlock`. The walks discriminate on the type kind.

**Entry point constants (2026-10-19).**

- Slang wraps the ordinary-data `uniform` parameters of an entry point in one constant buffer: a
  `var<uniform> entryPointParams` (std140) on WGSL, a push constant block (std430, no binding) on SPIR-V.
- Slang's metadata does not track push constants, and it removes a block that the entry point does not read. The
  emitted SPIR-V decides (`ReconcileEntryPointConstants`).
- `collectMetadataFromInst` marks the whole slot range of that buffer as used: shader-slang/slang#13789.
- Every SPIR-V stage puts its block at push constant offset 0.

**Codegen.**

- Slang names every SPIR-V entry point `main` unless `VulkanUseEntryPointName` is set. The cook sets it
  (2026-10-21), and the cook fails when the `OpEntryPoint` name differs from the manifest name.
- Slang can report an error and return a success code (E55205 for a specialization constant in `numthreads` on
  WGSL). Codegen fails on any failure record.
- Slang does not infer a depth texture from usage. Use `DepthTexture2D`, `DepthTexture2DArray`, or
  `DepthTextureCube` with a `SamplerComparisonState`, or the WGSL is invalid (shader-slang/slang#6942).

**Sessions and the file system.**

- `loadModuleFromSourceString` for a builtin breaks the worker sessions with no diagnostic. `EmbeddedFileSystem`
  serves the builtins through an import for that reason.
- A plain `ISlangFileSystem` hides the paths behind a content-hash cache. Implement `ISlangFileSystemExt`.
- A module cache from another Slang version stops every cook. The cache path names the Slang build tag and the
  configuration (`%TEMP%\LodestoneShaderCooker\<tag>-<config>`). A stale binary still writes the tag it was built
  with.

**Arrays and write-only images (F4.0 spike, 2026-10-23, Slang `e9fc02c8d`).**

- An array of shim externs (`Level levels[16]`) is one binding array with `ArrayCount()` 16, in a block or as a global.
- The direct SPIR-V path drops `NonReadable` on an array of `WTexture*` (`maybeEmitWriteOnlyImageDecoration` does not
  unwrap the array). The glslang path keeps it. The cross-check fails on access. Draft issue in `docs/`.
- Slang does not inline a shim method, so `levels[3].Store(...)` passes the index as a parameter and needs
  `StorageImageArrayDynamicIndexing`. `[ForceInline]` keeps the constant, and every Bound method has it since F4.0.
- A plain or shim array of storage textures fails on WGSL in Tint. E56105 covers buffer arrays only. Upstream #13708 (after
  our submodule) emits `binding_array` for texture arrays.
- No SPIR-V grammar entry requires an `...ArrayDynamicIndexing` capability, and glslang omits them. Only the device feature
  matters, and only our reader states it.

**Debug Slang.**

- `loadRootModule` on `KsMaterial` raises `unexpected: duplicate global instruction` (`checkIRDuplicate` in
  `slang-ir-link.cpp`, Debug only, inside `prelinkIR`). The cause is Slang PR #12574 (first release `v2026.17`).
- `tests/SlangPrelinkRepro.cpp` and `tests/assets/SlangPrelinkRepro/` reproduce it with two files and no Lodestone
  attribute: an imported struct implements an imported interface and calls core intrinsics, and the consumer
  calls one too. Build `SlangPrelinkRepro` in Debug after each Slang update: exit code 0 means the fault is gone.

**Cook cost.** A one-variant cook takes about 0.4 s, so startup dominates a small cook. KitchenSink takes about
3.4 s for 616 variants (`git show e113d8c:docs/phase-e-data-driven-permutations.md`, §11a).

## 5. Design decisions in force

- **Variant keys are per module.** Shared meaning lives in shared types and in query presets, never in a global
  axis. A root axis is storage and dedup only.
- **A type-backed axis gets its identity from its type.** Only an integral or boolean axis would need an explicit
  shared-axis reference, and that reference must be a string.
- **A constraint on an axis implies that the axis is active**, unless the pin is `WhereWhenActive`.
- **Plain integer indices in the public API.** Strong index types were tried and removed. Do not propose them
  again (`CLAUDE.md`, "A guardrail must pay for itself at the call site").
- **Views resolve on demand and allocate nothing.** The iterators are input iterators, because a proxy iterator
  returns its value by value.
- **Roles are interpreted, tags are not.** A role is checked and sets a usage. A tag is stored and read by the
  renderer. Copies, readback, and render attachments belong to the graph.
- **Phase F.** `docs/phase-f-plan.md` section 2 holds the decisions: WebGPU stays Bound, the access model lives on
  the profile and adds no axis, and shim interfaces selected by link-time specialization carry it.

## 6. Deferred capability areas (do not lose)

- **Bindless (Indexed and Pointer, phase F step F4).** The reflected schema is the invariant, and placement is the
  variant. Under bindless the schema facts are the only guard a driver would otherwise give. An unbounded binding array
  (`Texture2D A[]`) cooks on `spirv` since F4.A (format 16, `ResolvedResource::IsUnbounded`). DiamondDogs request C4
  waits for the Pointer and Handle forms.
- **Device capabilities (F3).** Built, except F3.4d (layout limits).
- **WGSL binding arrays.** Slang rejects an array of resources for WGSL (E56105). WebGPU's `binding_array` is a separate
  story, with F4.
- **Per-variant storage image format (DiamondDogs C11)** and **generated host layout headers (C6)**: see the requests file.
  Plan C11 with F2.
- **Specialization constants.** Slang reflects them, and WGSL spells them `override`. The table slot exists.
- **Push constant members as buffer device addresses.** This ties push constants to the Pointer model.
- **`IndirectCount` role.** Waits for capabilities: WebGPU has no count buffer.
- **Portable geometry (vertex and index pulling).** `todo.md` ("Phase F: portable geometry"). Not scheduled.
- **A live cook (DiamondDogs C3).** In-process cooks work through `RunCook` and `MemoryOutputSink`; no public
  header exists for them yet.

## 7. History

One line for each step. Git history holds the full text.

- **2026-09-17 to 2026-09-24.** Phase E complete (axes in the shader, policy in TOML, enum and interface axes).
  The cooker driver became step functors. Tint replaced the WGSL text scanner. The multi-module bundle, `TableRef`,
  and the view types. Strong index types tried and removed.
- **2026-09-24 to 2026-10-01.** Phase F slices F0 and F1: builtins compiled in, `spirv` with `SpvValidator`,
  format 7 (`ShaderCodeFormat`), per-target policy.
- **2026-10-12 to 2026-10-16.** Phase F paused for the rendergraph. L8 merged layout, L10 client-only configure,
  L1/L2/L13 query terminals, the cook build step, L15 axis defaults (format 8), derived constants.
- **2026-10-19.** L3 axis relevance (format 9), L4 query presets, L18 `ls_extent_1d`, L7 sampler pairs (format 10),
  a file-split cleanup pass, L14 entry point constants (format 11), slang#13789 filed with its workaround.
- **2026-10-20.** Push constants in the merged layout (`MergedLayoutPushConstantConflict`).
- **2026-10-21.** L17 resource roles and `ls_resource_tags` (format 12, `ResourceRoleTest`). DiamondDogs E1 to E3
  (embedding) and C2 (`VulkanUseEntryPointName`). `ThreadPool` setup failures report and keep the thread
  (`WorkerSetupFailed`). Atomic artifact writes, and a cook removes the old manifest first. Both validators compare
  the storage texture format.
- **2026-10-21 (later).** L16 resource report (format 13, `ResourceReportTest`): footprint axis masks, `DescribeCreation`,
  `ShaderLibrary.resources.txt`, validator `VerifyFootprintAxes`.
- **2026-10-21 (late).** F3.1 and F3.2 (format 14): capability names and workgroup memory for each source, the
  generated `VulkanCapabilityTable.hpp`, L6 closed without a device-bound axis.
- **2026-10-22.** F3.3: `VerifySourceFactsAcrossProfiles` (`SourceFactsTargetMismatch` = 53), `VisitSharedVariants` shared with
  the sampler pair check, `CookedProfile::CodeFactsRead`, `WorkgroupMemoryCookTest`.
- **2026-10-22 (later).** F3.4a to c, format 15: `CodeFactsRead`, `EntryPointInstanceView::Fit`, `ManifestQueryBuilder::ForDevice`
  (C function pointer predicate), `NoVariantFitsDevice`. F3.4d deferred.
- **2026-10-22 (DiamondDogs).** C13 root cause: an import found only through the module cache loads in the first session
  and not in the workers. C8: `--include-dir` and `INCLUDE_DIRS`, `CrossModuleCookTest`. C7: `UniformMemberRange::Find`.
  C9: no code location on warnings, `File.cpp:line` on errors.
- **2026-10-22 (C12).** Binding arrays on SPIR-V: the reader takes the length, `SpvValidator` compares `ArrayCount`,
  and a dynamic index adds `...ArrayDynamicIndexing` (Slang states none). `BindingArrayTest`. Unbounded arrays wait for F4.
- **2026-10-22 (F3.5).** `VerifyCapabilityAxes`: an ungated axis of kind `capability` must change the capability names of
  some entry point, and a boolean axis must not need more when off than when on. No attribute: the author refused a
  declaration of names. `CapabilityAxisTest`. Next: F2.0.
- **2026-10-22 (F2.0).** Spike: a resource-bearing impl links as a parameter type (block field, global, entry point
  uniform) on both targets, and fails as a local. No code change.
- **2026-10-22 (F2.1).** `builtins/LodestoneShims.slang` and `ShimModuleTest`. The builtin list in CMake is a glob now: a new
  file in `builtins/` needs `scripts\configure.bat`.
- **2026-10-22 (F2.2).** `target/ShimTable` and `ShimTableTest` (decision D6: the table agrees with the shim module).
- **2026-10-22 (F2.3).** Shim externs are read from the source and confirmed by reflection (`RawModule::ShimExterns`,
  `ShimExternNotReflected` = 56). `ShimExternTest`.
- **2026-10-22 (F2.4 to F2.6).** Shim externs link the implementation of the profile (`ShimLinkSource`, one link module for each
  worker context), and a shim material cooks to the same resources as its hand-written control (`ShimCookTest`). Fixed:
  `BuildAxisDeclarations` swallowed axis-declaration errors, and the cook exited 0 with no axes.
- **2026-10-22 (C11.0).** Spike: this Slang names the storage format as a type argument (`WTexture2D<RGBA16F>`), so a generic
  shim impl serves every format, and WGSL allows `read_write` for `r32*` only. Plan in `docs/phase-f-plan.md`.
- **2026-10-23 (C11.1).** `StorageFormat` enum in the shim module, `target/StorageFormatTable`, `TextureFormat::RGBA8Snorm`, and
  `StorageFormatTest`. Fixed: the symbol table read declarations inside comments (`StripComments`).
- **2026-10-23 (C11.2).** The write-only storage shim (`BoundWriteStorageTexture2D<F>`, table row with
  `ImplementationTakesFormat`) and its checks in `StorageFormatTest` and `ShimTableTest`.
- **2026-10-23 (C11.3).** `ls_storage_format`: a storage shim extern takes its format from an enum axis over `StorageFormat`, one
  link module for each value (`MakeShimFormatLinkSources`), errors 180 to 182. Fixed: a pruned enum axis failed the cook,
  because its initializer was read as an extern constant default.
- **2026-10-23 (diagnostics).** Shader locations go in the record, not the message: `ShaderLocation` and two `ReportError`
  overloads (one with `Related` notes), and the sink prints `file(line): ` or `file(line,col): `. The shim, axis-default, and
  derived-constant errors use it (derived constants list each declaration as a note). `DescribeShimExtern`. Both validators
  report an unmatched binding through `ReportDeclaredOnly` and `ReportReflectedOnly`.
- **2026-10-23 (C11.4).** `StorageFormatAxisTest` (a mip kernel with a format axis against a control for each format), enum case
  names in `CookValues`. DiamondDogs C11 is done.
- **2026-10-23 (F4.0 spike).** Shim arrays work; the `NonReadable` array fault (issue drafted, workaround and canary chosen);
  `[ForceInline]` for constant indices. The `...ArrayDynamicIndexing` question closed as not a Slang fault.
- **2026-10-23 (test audit).** New harness (`CheckEqual`, `CheckOk`, `CheckError`, `TestFormat.hpp`), every suite converted,
  `RunCook` takes a `DiagnosticSink&` and every cooker message reaches it, `CookerOptionsTest`, `EnumAxisTest`, known-answer
  keys. Fixed: `MaxVariants` = N rejected N variants; a missing `--policy-file` was ignored; `--O2x` read as 2; three
  rejections named no module. `docs/test-audit.md` has the status.
- **2026-10-24 (F4.0).** Arrays of shim externs on `spirv`: `[ForceInline]` on the Bound methods, the `NonReadable` validator
  workaround and `SlangNonReadableCanaryTest`, `ShimArrayTest` (a pyramid against literal controls), `CookForTargets`.
- **2026-10-24 (F4.A).** Unbounded binding arrays on `spirv`, format 16: `ArrayCount` 0, `ResolvedResource::IsUnbounded`,
  `CreationFields::BindingCount`, the reader reads `OpTypeRuntimeArray`. Fixed: a `--no-validate` cook stored 4294967295.
- **2026-10-24 (F4.1).** The Pointer and Handle shim impls, `ShimForms` on the profile, `Indexed` renamed to `Handle`,
  `spirv-modern` (heap set 3) failing with `TargetProfileIncomplete`. The spike: reflection gives a pointer as kind `pointer` and
  a handle as a `uint2`, and no heap variable.
- **2026-10-24 (F4.2 to F4.4).** Format 17: stored values (`StoredValuePlacement`, `Binding::ValueOffset` and `ValueContainer`,
  `Profile::Forms`, `PlacementOrderKey`), `[ls_shim_value]`, the heap compared with the mutable lowering, `ShimPlacementTest`.
