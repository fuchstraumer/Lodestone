# Agent handoff

Read this first. It gives the state, the open faults, and the facts that are expensive to measure again.
`CLAUDE.md` gives the build rules, the code map, and the author's positions. This file is newer than `CLAUDE.md`
on anything that moves. It holds no plan and no history of a finished task: a plan lives in its own document, and
git history holds the rest (the long form of this file is in `git show 9fe93d3:docs/agent-handoff.md`).

Text in this file follows ASD-STE100. Compressed on 2026-09-24 and on 2026-10-21. Keep it short: when a fault is
fixed, remove it; when a task is done, add one line to section 7 and remove its notes.

`docs/` is in `.gitignore`. This file survives only as a force-add: `git add -f docs/agent-handoff.md`.

---

## 1. State on 2026-10-21

- RelWithDebInfo, `ninja-msvc`: build green, 29 of 29 tests, 60 of 60 known-good dumps.
- Debug: do not run it unattended (section 2, item 2).
- Manifest format version 12. `CLAUDE.md` ("One output form") holds the layout of each version's addition.
- Phases D and E are complete. Phase F is paused after F1 (two Bound targets, `wgsl` and `spirv`).
- **The current work is the VeloxRhi rendergraph tasks**: `docs/rendergraph-integration-tasks.md`. R1 and R2 are
  done, and L17 and L18 of R3. Open: L5 and L6 (device features, with phase F), L16 (the resource report), L11
  (optional dispatch extent).
- **DiamondDogs** embeds Lodestone and sends requests in `docs/diamonddogs-requests.md`. Each row has a state.
  Do not edit the DiamondDogs tree: the author passes news to its agent.
- **The author wants feature discipline.** A feature goes in only with a named consumer and a test that proves
  it. Say so when a proposal has neither.

## 2. Open faults

Ordered by what can write wrong output first.

1. **Only codegen reads an error that Slang returns with a success code.** `link`, `loadModule`, and
   `createCompositeComponentType` still read the return code alone. No case is measured for them yet.
2. **The Debug Slang build asserts on `KsMaterial`.** A Slang fault (section 4). A Debug run can also hang. Run
   the Debug tests one at a time, with a timeout, and skip `KitchenSinkCookTest`. The Debug trees in
   `build/ninja-msvc` and `build/ninja-msvc-nowgsl` were built from an old Slang: rebuild them before any Debug
   run, or their cooks poison the shared module cache.
3. **The capability requirement is always zero.** `Profile::CapabilityFloor` and `Variant::CapabilityRequirement`
   wait for a capability set (a bitmask over a string table of capability names; the author agreed to that
   shape). Slang warning 41012 is off, so a variant can need a device capability and no cook states it. Phase F
   steps F3.3 and F3.4 close this. Do not let them slip.
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
8. **`tests/DeclKindProbe.cpp`** is a throwaway Slang probe. Remove it when it stops earning its place.

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
- **An interface conformance cannot carry a resource.** Codegen fails with E56003 at the use site. An interface
  axis carries behaviour only, and resource presence stays a constant axis. Phase F step F2 must respect this.
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
  variant. Under bindless the schema facts are the only guard a driver would otherwise give. Add unbounded and
  runtime-array detection then. DiamondDogs request C4 waits for this.
- **Device capabilities (F3, with rendergraph tasks L5 and L6).** DiamondDogs request C5 waits for this.
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
