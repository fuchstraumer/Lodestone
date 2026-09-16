# Agent handoff

Written on 2026-08-22. Rewritten on 2026-08-28, after the compiler split finished. Updated on
2026-09-01, after six build faults were fixed, the C++ emitter was removed, and phase E steps E0c and
E0 completed. Updated again on 2026-09-04, after phase E steps E1 and E2 landed and
`PermutationConstraintTest` was verified. Updated on 2026-09-14, after phase E finished through E7 and
the manifest variant-key work began (§14). This document records the state, the measured facts, and the
next task. Read it first.

Text in this file follows ASD-STE100.

---

## 1. State on 2026-09-14

**The compiler split is complete and the pipeline works. Phase E is done through E7; only E8, the
documentation pass, remains.** E3 made enumeration one depth-first walk with propagated `Require`
pruning and an in-walk `MaxVariants` guard. E4 replaced the mixed-radix storage index with a sorted key
table, so a variant's dense index is now its rank. E4a made `ShaderManifestView::Open` validate the
whole manifest graph once, so the runtime accessors trust the data. E5 moved the cook policy out of the
compiled-in registry and into a TOML file that a `PolicyDocument` reads. **E6 moved the axis
declarations into the shader and deleted the compiled-in registry whole**: the cooker reads each axis
off its `extern static const` through reflection, so no module data is compiled in. **E7 added interface
axes**: an `extern struct : IFoo` declared with `ls_axis_interface`, whose implementations carry
`ls_axis_interface_impl`, cooked by a per-variant `export struct` (see §14). `PolicyDocumentTest`,
`SymbolTableTest`, and `InterfaceAxisCookTest` bring the suite to seventeen test targets. Phase E step
E0c added `AccessModelRejectTest`.

**In progress, after E7: the manifest variant-key retrieval path (see §14).** The variant key is now a
strong type, the cooker and client share one packing codec, and the manifest carries the axis schema.
The client-side query surface (decode, enumerate, filter) is the open work.

| Configuration | Build | Tests |
|---|---|---|
| RelWithDebInfo, `ninja-clang-cl` | green (before the in-progress §14 edits) | 17 of 17 |

Thirteen targets are unit tests, and four are cooks. `scripts\run-tests.bat` reports
`all targets passed`, and `python scripts/check-known-good.py` reports all six stage dumps match. The
manifest-key edits in §14 are unverified; build and run both before you trust a green claim.

**Only the clang tree was rebuilt on 2026-09-01.** `build/ninja-msvc` went with the rest of `build/`
and has not been configured since. Build it before you trust a claim about MSVC.

### The numbers to compare against

A green cook of `OceanFft` reports 35 variants over an index space of 56, 105 entry point variants,
77 unique sources, 4 resources, 1 resource list, 7 footprint lists, and 2 visibility lists, and it
emits 663 KiB of WGSL. Those numbers are the regression check. The whole cook took 1080 ms in
RelWithDebInfo on 2026-09-01, and `CookTest` was still green on 2026-09-04.

A cook now builds this space from the shader, not from a compiled-in table. The count matches the old
registry because `OceanFftDims.slang` declares `IFFT_WAVE_SIZE` with all four values, 16 included. The
16 value stays for testing only. A shipped shader would not cook it.

**`python scripts/check-known-good.py` is the finer check.** It cooks a module and compares each of
the six stage dumps against `tests/known_good/`. Those files are current as of 2026-09-11, when all
six matched a fresh RelWithDebInfo cook. Run it
after a change to reflection, to a stage, or to a dump. It found a reflection regression on the day
it was written, and section 13 records that.

| Cook | Module | Proves |
|---|---|---|
| `CookTest` | `OceanFft.slang` | The permutation path, end to end. Runs `--verify-deterministic`. |
| `EntryPointParamsCookTest` | `EntryPointParams.slang` | The entry point scope walk. |
| `ParameterBlocksCookTest` | `ParameterBlocks.slang` | The parameter block walk. |

### The escape hatches work

Each one was run on 2026-08-28 and each exited 0.

| Flag | Tables it produced |
|---|---|
| default | 77 sources, 4 resources, 1 resource list, 7 footprint lists, 2 visibility lists |
| `--single-threaded` | identical to default |
| `--no-dedupe` | 105 sources, 140 resources, 35 resource lists, 35 footprint lists, 105 visibility lists |

Both dedupe arms emitted **663 KiB of WGSL**. Dedup changed what the tables cost and nothing the cook
measured, which is the line `DedupeInfluenceTest` holds. `--single-threaded` matched the threaded
cook exactly, so threading did not let an unordered container reach the output.

---

## 2. The machine, and the one path still compiled in

The repository moved from `D:\ShaderTools` on a laptop to `C:\SoftwareDev\Lodestone` on a desktop.

**The three scripts work.** Visual Studio 18 Community is installed at the path
`scripts\build.bat` and `scripts\configure.bat` name, and `vswhere.exe` sits where they expect it.
Both build trees are configured: `build/ninja-msvc` holds `cl.exe` 14.51.36231, and
`build/ninja-clang-cl` holds `clang++.exe`, which is full Clang and not the clang-cl frontend.

**One absolute path is still compiled in.** `src/compile/impl/SlangModuleContext.cpp:82` holds
`std::filesystem::canonical("C:/SoftwareDev/Lodestone/tests/assets/")`, which finds the attributes
module. `canonical` raises an exception on a path that does not exist, so a cook on any other machine
stops with exit code 3 before it prints a second line. The move from `D:` to `C:` fixed the symptom
and not the cause. The other three search paths derive from `create_info.ModulePath`. Give this one
the same treatment, or add a field to `SlangCompilerCreateInfo`.

### Build and test

Use the scripts. Do not use a bare `cmake --build`, and do not use a bare `cmake --preset`. The build
tree keeps the compiler that `CMakeCache.txt` holds.

```
scripts\build.bat [Debug|RelWithDebInfo] [preset]
scripts\configure.bat [preset]
scripts\run-tests.bat [Debug|RelWithDebInfo] [preset]
```

Run `configure.bat` after a change to a `CMakeLists.txt` that adds or removes a target.

**Read the exit code of the build itself.** A pipe into `grep` or `tail` gives you the exit code of
that command, and a failed build then looks like a success.

**Run the tests after each change.** A build proves less than it looks. Section 9 records a crash
that a green build hid.

---

## 3. What the compiler split did. Complete.

`src/compile/SlangCompiler.cpp` held 1732 lines and did two jobs. It built modules, entry points, and
target text. It also reflected on what it built. The split gives each job a file, and it adds a
thread pool between them.

`src/compile/SlangCompiler.cpp` now holds 104 lines and names no Slang type in its header. Five files
under `src/compile/impl/` hold every Slang type.

| File | Owns | Lifetime |
|---|---|---|
| `SlangCompilerTypes.{hpp,cpp}` | The enum conversions, `BindingScope`, `RawBindingDraft`, the option table. | Free functions. |
| `SlangModuleContext.{hpp,cpp}` | One global session, one session, the root module, the entry points, the source texts. | One for each thread. |
| `SlangVariantCompiler.{hpp,cpp}` | Link, target codegen, entry point metadata. Returns `LinkedVariant`. | One for each job. |
| `SlangReflector.{hpp,cpp}` | Every reflection walk. Takes `LinkedVariant`, returns `RawVariant`. | One for each job. |
| `ThreadPool.{hpp,cpp}` | An atomic job index and a latch. Distributes variants across workers. | One for each compiler. |

`SlangCompiler` is the facade. It holds a bootstrap `SlangModuleContext` and a `ThreadPool`, and it
exposes `Initialize`, `PrepareRawModule`, and `Compile`.

**The call order changed.** The old code called `CompileVariantRaw` once for each variant, on the main
thread. The new code calls `Compile` once with every variant, and the pool spreads them.
`CookerDriver.cpp` already makes the new call.

### What is finished

`SlangReflector` is complete. It sets the suffix, the description, and the index. It extracts the
global bindings, walks the scope of each entry point, appends the drafts, reads visibility from the
entry point metadata, and reads the raster state and the workgroup size.

`SlangVariantCompiler` is complete. It builds one synthetic module for each active axis value,
composites, links, generates the target text for each entry point, and collects the metadata.

`SlangModuleContext` has a complete bootstrap path. `Initialize`, `RunBootstrap`, and the accessors
all work, and the main thread uses them today.

The Slang wall holds. `include/compile/SlangCompiler.hpp` names no Slang type. Every Slang type stays
under `src/compile/impl/`.

The driver kept its shape, and all four validators still run.

---

## 4. Ten measured facts about Slang

Probe modules measured each fact. Slang documents none of them. Do not measure them a second time.
Each fact now belongs to `src/compile/impl/SlangReflector.cpp`.

1. **`getVarLayout()->getName()` gives nothing** on an entry point layout. The scope name comes from
   `k_EntryPointScopeName` and from `CollectScopeNames`.
2. **Slang names the entry point scope `entryPointParams`.** The string is a name hint in
   `slang-ir-entry-point-uniforms.cpp` line 584. It is a Slang convention, so it stays inside
   `src/compile/impl/`.
3. **`getFieldBindingRangeOffset` is the only link from a field to its binding ranges.** Slang
   flattens a scope into one list of ranges. `CollectScopeNames` walks the fields to recover the name
   path the emitter writes.
4. **A scope has two bases.** `Base` is where a binding declared in the scope sits. `SpaceBase` is
   where a block declared in the scope starts to count spaces. An entry point scope reported a slot
   offset of 0 and a sub-element space offset of 1, and its block took space 1.
5. **`getSubObjectRangeSpaceOffset` is not the space of a block.** It reported 0 for a block that took
   space 1. Read the space from the offset var layout of the sub-object range.
6. **The contents of a block start at binding zero.** Their own descriptor range offsets already count
   from the start of the space, and they already step over the container.
7. **A container exists only when a block holds ordinary data.** A block of resources alone emits no
   such slot. `getSize(SLANG_PARAMETER_CATEGORY_UNIFORM)` on the element says which case this is.
8. **Slang refuses `[vx_*]` on a bare entry point parameter**, and accepts one on a struct field. So
   an entry point resource declares a footprint through a struct parameter. `MaterialCS` in
   `EntryPointParams.slang` covers that form.
9. **`isParameterLocationUsed` answers for the contents of a block** at global scope.
10. **An `import` is a requirement, and never a component.** `slang-check-shader.cpp:2820` adds each
    `ImportDecl` to the module's `m_requirements`. The `CompositeComponentType` constructor
    (`slang-linkable-impls.cpp:53`) treats every direct child module as a satisfied requirement and
    keeps the rest as requirements of the composite, which `link()` resolves. So an imported module
    must **not** be a component. A synthetic axis module must be, because nothing imports it and an
    explicit component is the only way the linker sees it. Compositing a module also unions its
    global shader parameters into `m_shaderParams`, which is what `ProgramLayout` lays out, so an
    extra component can move binding placement. The reference list is `[root, ep0, ep1, ep2]`.

---

## 5. Phase E steps E0a, E0b, E0c, and E0 are complete

**E0a reads the entry point scope.** `collectBindingRangeDrafts` walks any scope, and
`extractRawEntryPoint` calls it a second time with `entryPointLayout->getTypeLayout()`. An entry point
owns the parameters it declares, so ownership states the visibility. A placement query must not,
because Slang generates each entry point as its own artifact.

**E0b reads the parameter blocks.** `collectSubObjectDrafts` walks the sub-object ranges.
`ReadParameterBlock` reads one range, and `ReadBlockContainer` reads the container.

**The two walks partition the binding ranges on one test.** The range walk keeps every range the
parent placed itself. The sub-object walk keeps the rest. Both test
`getBindingRangeDescriptorSetIndex(range) >= 0`. A global `ConstantBuffer<T>` proves that the test is
necessary, because the parent places it and it also reports a sub-object range.

**A binding carries `ScopeName` beside `Name`.** The two together are the identity. Two entry points
can each declare `albedoMap`. The field runs from `RawBinding` to `ManifestBinding` and `BindingInfo`.
It is empty at global scope. `ExpectedDeclaredName` builds `ScopeName + "_" + Name`, and the
cross-check compares that against the de-mangled emitted name.

---

## 6. What the wiring took, for the record

Six gaps sat between the split and a working cook. All six are closed. They are listed here because
each one is a shape that can come back, and not because any of them is open.

1. **`ThreadPool::Initialize` had no caller.** `SlangCompiler::Initialize` now calls it after the
   bootstrap succeeds, and hands it the serialized modules.
2. **An unrun job read as a compiled job.** `CompileResultList` was default constructed, and a
   default `std::expected` holds a value. The driver's `if (!result)` passed, and
   `VerifyLibraryRoundTrip` then compared 35 empty variants against 35 empty variants and agreed.
   The vector is now seeded with `std::unexpected(CookError::VariantNotCompiled)`. **A validator that
   is given data nobody wrote cannot tell.** Seed the failure, do not test for it.
3. **The worker context was never bootstrapped.** The worker now calls `RunWorkerSetup`.
4. **The two setup paths built different component lists.** See §7.
5. **`CompileBatch::ThreadSinks` was an empty span.** Now allocated, one for each worker.
6. **Stage 3 diagnostics reached nobody.** `CompileModuleVariants` now takes the driver's sink.

---

## 7. One rule the two setup paths depend on

`RunBootstrap` and `RunWorkerSetup` must end with the same `baseComponents`. A variant that links
against a different component list is a variant that emits different text.

Loading a module and registering it as a component are two jobs. When one function does both, the
contents of `baseComponents` depend on call order, and the two paths drift. `buildSlangComponents` is
now the only place that writes the list, so call order cannot change it. The paths differ only in how
the session got its modules: parsed from source, or loaded from IR blobs.

Priming the session from cache replaces **parsing**, and never **compositing**.
`loadModuleFromIRBlob` registers the module in `mapNameToLoadedModules` (`slang-session.cpp:1223`),
and `findOrImportModule` checks that map first (line 1501). So an `import` resolves to the cached
module instead of reading the file. Fact 10 in §4 states which modules belong in the list.

---

## 8. Phase E is done through E7. The next task is the manifest key path (§14), then E8

`docs/phase-e-data-driven-permutations.md` holds the plan. **Every step through E7 is complete, and item
D2 of §10 is settled.** The per-step history below stays for the record. E8 is the documentation pass and
a fresh measurement of the numbers.

**E7 is done. Interface axes work.** An `extern struct SHADE_MODE : IShadeMode` marked
`ls_axis_interface` is the axis; each implementation struct carries `ls_axis_interface_impl("IShadeMode")`;
`SlangModuleContext` stages the extern and the impls while walking every loaded module, then matches them
by `isSubType` against the interface from the program layout, and rejects a conformance that declares a
resource member. `SlangVariantCompiler` cooks each value by loading a synthetic module
`import <impl module>; export struct SHADE_MODE : IShadeMode = <impl>;`. A `Type` `PermutationValue`
holds the ordinal into the axis's implementation list. `InterfaceAxisCookTest` cooks it end to end.
`docs/phase-e-attribute-spike.md` records the reflection facts this needed.

**The current work is the manifest variant-key retrieval path. §14 holds its state and plan.**

**E1 is done, on 2026-09-01.** The attribute expression evaluator gained a comparison level, a
logical level, and unary `!`. The file `SizeExpression.{hpp,cpp}` became `AttributeExpression.{hpp,cpp}`,
`EvaluateSizeExpression` became `EvaluateExpression`, the four `SizeExpression*` cook errors became
`AttributeExpression*`, and `SizeExpressionTest` became `AttributeExpressionTest`. `&&` and `||` do
not short circuit, because the parser evaluates as it descends; the class comment in
`AttributeExpression.cpp` records the one effect this has. All six stage dumps stayed byte identical,
because no shader uses a comparison yet.

**E2 is done, on 2026-09-04.** The axis model gained `AxisValueDomain`, `AxisKind`, and
`EarliestBindingTime`, and `ActiveWhen` replaced `ParentIndex`. `ValidateConstraints` runs at load and
takes the descending-graph rule: an `ActiveWhen` may name only an axis declared before it, so a cycle
cannot be written and a forward reference fails at load. `Require` prunes a forbidden combination
during enumeration. The space dump now carries the filled enum fields, an `activeWhen` string in place
of `parent`/`requiredParentValue`, and a space-level `require` array; the other five dumps stayed byte
identical, because `OceanFft` cooks the same variant set. `PermutationConstraintTest` proves the
engine: it gates an axis with `ActiveWhen`, prunes a combination with `Require`, and rejects a forward
reference, an unknown symbol, and a malformed expression at load. It is written and green.

**E3 is done, on 2026-09-06.** Enumeration is now one depth-first backtracking walk, `expandFrom`, in
`src/permute/PermutationSpace.cpp`. `EnumerateActiveCombinations` is gone, folded into the walk. Each
`Require` is bucketed by its ready-depth — the deepest axis index it names — in a `RequireReadyMap`,
so the walk evaluates it the instant its last operand binds and prunes the whole subtree, rather than
filtering at the leaf. A `Require` that names no axis is now rejected at load. The `MaxVariants` budget
is enforced during the walk, before compilation, through a new `max_variant_count` parameter on
`EnumerateVariants` and the `PermutationVariantBudgetExceeded` error; the driver reads it from
`FindPolicyForModule` (which returns `&k_EmptyPolicy`, budget 0 = unlimited, for an unregistered
module). All six stage dumps stayed byte identical, because the sort by index makes visitation order
invisible to the output. One open nit: the walk's budget test is `>=` while the post-hoc
`CheckVariantBudget` is `<=`, so they disagree at exactly the budget; no module hits it yet.
(E5 changed this signature. `EnumerateVariants` now takes a `const TargetPolicy&`, and
`FindPolicyForModule` and `k_EmptyPolicy` are gone. The budget is `TargetPolicy::MaxVariants`, and 0
still means unlimited.)

`EnumerateVariants` takes its policy as a required parameter, not a defaulted one: the author dislikes
default arguments, so every call site states the policy it means. E3 passed a `max_variant_count`
`size_t`; E5 replaced it with a `const TargetPolicy&`, and the tests pass a named
`k_UnboundedTargetPolicy`.

**E4 is done, on 2026-09-07.** It replaced the mixed-radix storage index with a sorted table of packed
canonical keys. A variant's dense index is now its rank in that table, so the holes are gone.
`ComputeVariantIndex` became `ComputeVariantKey` and returns a `uint64` `VariantKey`.
`EnumerateVariants` sorts the variants by key, then gives each one its rank. `SpaceSize` stays the
nominal product for the report, but it no longer sizes any table. The manifest carries a `VariantKeys`
table in place of the old hole-filled index table, and `FindSlot` finds a variant by a `lower_bound` on
the keys. Five stage dumps changed, because the index values compacted, and `check-known-good.py`
recorded the new dumps after a review of the diff. The `space` dump did not change.

**E4a is done, on 2026-09-09. It was a diversion, not a numbered step.** It hardened the client trust
boundary. `ShaderManifestView::Open` now validates the whole manifest graph once, at load. It runs a
data-driven section-bounds pass, then per-table checks for the context-free indices, then one
variant-outwards pass for the relational checks. Every runtime accessor then trusts the data:
`FindSlot` dropped its bounds branches, and `EntryPointId` is now zero-based. The error type changed
from a bare `ShaderManifestErrorCode` enum to a `ShaderManifestError` struct. The struct carries the
code, the table, the record index, and a `Detail` value. `DescribeShaderManifestError` turns the struct
into one console line, and the cooker and `manifest_dump` both use it. The manifest format version is
now 2, because the E4 key table changed the bytes. One item stays open: the `Open` validation has no
dedicated reject test yet.

**E5 is done, on 2026-09-11.** The cook policy moved out of the compiled-in registry and into a TOML
file. A `PolicyDocument` reads the file through toml++ (`marzer/tomlplusplus`), behind a facade: no
toml++ type leaves `src/permute/PolicyDocument.cpp`. Each module names an `InertAxesForEntryPoints`
table and one section for each target profile. A target section carries `MaxVariants`, a `CookValues`
allow-list, and a `CookIf` predicate. `CookValues` restricts an axis to a subset of its declared
values. `CookIf` keeps only the assignments the predicate accepts, and it reuses `EvaluateExpression`,
exactly as `Require` does. `EnumerateVariants` now takes a `const TargetPolicy&` and applies both
filters in the walk. `PolicyDocument::ValidateAgainstSpace` checks every axis name and value in the file
against the declared space, and the driver runs it before enumeration. The `--policy-file` option names
the file, and it fills `CookerOptions::PolicyFile`. `PermutationValue` lost its signed alternative,
because a TOML integer has one integer type and an axis value is a non-negative magnitude; a boolean now
reads first, and an integer builds a `uint`. The registry lost its policy half: `FindPolicyForModule`,
`k_OceanFftPolicy`, the old `ModulePolicy` and `ExpectedAxisInfluence` types, and `PermutationPolicy.hpp`
are all gone. `EnforceModulePolicy` now reads its expectations from a `ModulePolicyEntry` the driver
hands it. `PolicyDocumentTest` proves the reader, the query surface, and the space validation.

**The cook reads no policy file yet, on purpose.** The `OceanFft` policy file exists on disk and is
correct, but `CookTest` passes no `--policy-file`. A policy that pruned `OceanFft` would change the
variant set, renumber the E4 ranks, and change five stage dumps. So the driver loads an empty
`PolicyDocument` for the suite, every filter is inert, and all six dumps stay byte identical.
`PolicyDocumentTest` covers the reader on its own. Wire the file into the cook only with a non-pruning
policy, or accept the new dumps on purpose.

**E6 is done, on 2026-09-11. The registry is gone.** The axis declaration now lives on the
`extern static const` line in the shader, as an `ls_axis_*` attribute. `SlangCompiler::PrepareRawModule`
reads the attributes at the bootstrap compile, through `SlangModuleContext::ReadDeclaredAxes`, and the
driver builds the `PermutationSpace` from what it reads with `BuildPermutationSpace`.
`src/permute/PermutationRegistry.cpp`, the `k_ModuleSpaces` table, and `FindPermutationSpaceForModule`
are deleted whole. No module data is compiled in. `SymbolTableTest` is the seventeenth target, and it
proves the reachability prune that keeps an imported-but-unused axis out of a shader's space.

**Two Slang facts about `__include` decided the shape of `ReadDeclaredAxes`, and neither matched the E0
probe.** `OceanFft` pulls its axes in with `__include OceanFftDims;` and `implementing OceanFft;`, not
with `import`. First, an `__include`d fragment reflects as an Unsupported (kind 0) child node, and the
axis `extern static const`s sit inside it, one level down. So `ReadDeclaredAxes` recurses through the
child nodes, rather than reading the module's top level alone. Second, `getDeclSourceLocation` fails for
a decl reached this way, so `buildAxisDecl` treats a missing location as soft, not fatal.
`docs/phase-e-attribute-spike.md` records both, with the probe results and this addendum.

**Two E6 loose ends stay open.** `VerifyAxisNamesAreDeclared` is now dead: it is declared in
`permute/PermutationSpace.hpp` with no caller and no definition. Delete the declaration. And
`ExternConstantScanner` still reads the undriven `extern static const` defaults for size expressions.
The `SymbolTable` already tokenizes every source, so a later step folds that read into the tokenizer and
deletes the scanner. `todo.md` records it.

**E7 is next.** It adds interface axes, and `docs/phase-e-interface-spike.md` holds the answers.

**`docs/phase-e-interface-spike.md` holds the E0 answers.** Read it before E7. Three results matter
early: a link-time `extern` type works and uses the mechanism the constant axis already uses, an
interface axis can never carry a resource, and `getFullName` is not module qualified.

The rest of the phase E order is the one §11 of that document gives. The parts worth knowing before
you read it:

- The axis declaration now lives on the `extern static const` in the shader, as an attribute. This
  removed the drift that rule 6 of `CLAUDE.md` guards against, because the name cannot be stated twice.
  E6 did this, and it deleted `src/permute/PermutationRegistry.cpp` and `k_ModuleSpaces` whole.
- E7 adds interface axes. §8 of `docs/phase-e-data-driven-permutations.md` holds the spike answers, and
  `docs/phase-e-interface-spike.md` holds the detail.
- E8 is the documentation pass and a fresh measurement of the numbers.

---

## 9. A crash a green build hid

`BlobToString` lost its null check in commit `aff9204`. `GenerateOneEntryPoint` calls it with a
diagnostic blob, and Slang leaves that blob null when codegen has nothing to report. Every clean
compile then read through a null pointer, and every cook stopped with a segmentation fault.

The build stayed green, because the fault is a run time fault. `scripts\run-tests.bat` finds this
class of defect on the first cook. Run it.

---

## 10. Extraction in the compile folder does not reduce the line count

Four agent cleanup rounds each predicted a reduction and each measured an increase.

The cause is the convention of these files. Each function carries a doc comment that states a Slang
behaviour, and each extraction therefore costs 8 to 12 lines of prose. The duplication that goes away
is smaller than that.

Each round still bought something. One tree walker for four collectors stops the collectors from
drifting. A split of `CollectSubObjectDrafts` cleared a cognitive complexity of 36. One reader for
`SLANG_UNKNOWN_SIZE` gave that rule one place and one wording.

**The split of the file was the change that reduced the line count.** `SlangCompiler.cpp` went from
1732 lines to 104. A refactor inside one file did not.

---

## 10b. The global session convoy, measured

`slang_createGlobalSession` serializes across the whole process. `slang-api.cpp:262` starts
`slang_createGlobalSession2` with `RECORD_STATIC_CALL()`, which takes a process-wide
`std::recursive_mutex` on the `ReplayContext` singleton (`replay-context.h:408` and line 820). The
macro takes the lock **before** any test of whether recording is on, and the lock lives to the end of
the function. So the lock is held across the core module load.

The record layer is idle by default. `m_mode` starts at `Mode::Idle`, and only the `SLANG_RECORD_LAYER`
environment variable turns it on. `beginStaticCall` returns at once when the layer is idle, and
`wrapObject` returns the object unwrapped. So a disabled feature serializes every global session.

**The lock touches nothing else.** `RECORD_STATIC_CALL()` appears once in all of Slang. Every other
`RECORD_CALL` sits in a proxy class, and no proxy exists while the layer is idle. `createSession`,
`loadModule`, `link`, and `getEntryPointCode` never take it. Variant compilation is parallel. Only
the per-thread startup is not.

Measured on `OceanFft`, 16 logical cores, one global session for each thread.

| Build | One session, uncontended | 17 sessions, serialized | Whole cook |
|---|---|---|---|
| Debug | 937 ms | 19.3 s | 21 to 24 s |
| RelWithDebInfo | 111 ms | 1.79 s | 2.04 s |

**Do not judge this cost in Debug.** RelWithDebInfo is 8.5 times cheaper for each session.

### The bypass, and when it works

`slang_createGlobalSessionWithoutCoreModule` (`slang.h:5912`) plus `slang_getEmbeddedCoreModule`
(line 5921) plus `IGlobalSession::loadCoreModule` (line 4231) do the same work. None of the three
takes the record lock. The core module loads then run in parallel.

RelWithDebInfo, whole cook, by worker count:

| Workers | `createGlobalSession` | Split call |
|---|---|---|
| 3 | 848 ms | 760 ms |
| 8 | 1172 ms | **738 ms** |
| 15 | 2009 ms | 1094 ms |

The split path with `--verify-deterministic` and 8 workers cooks in 676 ms, exits 0, and reports 4
artifacts identical across two cooks. Every recorded number in section 1 still holds.

**In Debug the split makes the cook worse**, 42 s against 21 s. The contention moves to a lower
level, most likely the RTTI arena mutex at `slang-rtti-info.cpp:48`. This is a release-only win.

**The bypass is committed, behind a toggle.** `k_UseSlangWorkaround` in
`src/compile/impl/SlangModuleContext.cpp` is a `constexpr bool`. **Read its current value before you
measure anything**, because it decides which path a build takes and it has been set both ways. Both
entry points it selects carry a "not ready for production code" note in `slang.h`. Turn it on only in
a release build, and re-run `--verify-deterministic` after.

**The defect is filed upstream.** `slang_global_session_convoy.cpp` in the repository root is the
repro that went with the report, and it is untracked. It builds standalone at C++20 against `slang.h`
alone, and it prints a ratio that tracks the thread count: 15.5x to 16.1x on 16 threads, against
1.19x on the bypass.

### Thread count is not `hardware_concurrency`

With the lock, each thread costs one serialized session, so the best count is
`sqrt(variant_work / session_cost)`. The measured optimum was 3 to 4 workers in both builds, and
`hardware_concurrency` was 2.4 times worse. With the split, the cost of a thread is parallel again
and the count can rise with the variant count. 15 workers still lost to 8, because session creation
oversubscribes the machine.

---

## 11. Open work

Ordered by what can write wrong output, then by what wastes time.

- **A compiled-in absolute path.** §2. It stops a cook on any other machine.
- **Two unguarded reads.** `VisitLeaves` reads `getOffset(category)`, and `collectUniformMembers`
  reads a leaf `getSize`. Neither tests `SLANG_UNKNOWN_SIZE`. Neither is a placement, so neither can
  misplace a binding.
- **Two silent gaps in `applyLeafTypeLayout`.** A texture with no result type leaves `SampleType`
  invalid, and a uniform block may end with a byte size of zero while a comment above the line states
  that the size is fully determined. The second one is a comment that no code enforces. Section 13
  records what the first one already cost.
- **`hardware_concurrency` is the wrong thread count. Started.**
  `SlangCompilerCreateInfo::ExpectedBatchSize` exists for this, and `ThreadPool::Initialize` does not
  read it yet. §10b measured 3 to 4 workers as the optimum **with LTO on and the record lock in
  place**. Both of those changed on 2026-09-01, so measure again before you tune.
- **A global session for each batch.** Each worker creates one inside the outer loop. One for each
  thread is correct, because a global session is not thread safe. One for each batch is more than
  necessary.
- **One pool for each module.** The driver builds one `SlangCompiler` for each module, so a cook of
  several modules pays the startup cost once for each. `CompileBatch` would have to carry the module
  state for one pool to serve several.
- **A push constant cannot be cooked.** `FromSlangBindingType` has no row for
  `slang::BindingType::PushConstant`, and `BindingKind` has no value to map it to. The walk stops at
  an invalid binding kind. WGSL has no push constants, so nothing is lost yet.
  `docs/phase-f-vocabulary.md` is where this becomes a question.
- **`/arch:AVX2` is vestigial.** `CMakeLists.txt` asks for it, and the comment justifies it by a
  `Math.hpp` that this repository does not have. No AVX2 intrinsic appears anywhere in `include/`,
  `src/`, or `client/`. Removing it changes code generation, so it is the author's call.
- **The Slang PDB is no longer copied.** `copy_slang_dlls_to_target` copied it through a path that
  went stale, and the repair dropped it. `$<TARGET_PDB_FILE:slang>` is the spelling that brings it
  back.
- **Four of the ten unit tests never ran under MSVC on this machine.** See §1.
- **`docs/` is in `.gitignore`.** This file is force-added, so it survives. Use
  `git add -f docs/agent-handoff.md`. Run `git ls-files docs/` to see which others did.

## 12. How this author works

Read the "Working with this author" section of `CLAUDE.md` first. Three points need repetition.

**The author reserves the implementation work she enjoys.** Plan it, explain it, and ask before you
start.

**Establish that a cost exists before you help her remove it.** Say so plainly when a cost is
negligible.

**Measure, and do not estimate.** A probe module and a temporary `std::println` answer a question
about Slang in one build. Every fact in section 4 came that way, and three of them contradicted a
plan document.

---

## 13. What a clean tree found on 2026-09-01

`build/` was deleted and reconfigured. Six faults appeared at once. **Not one was new code.** Each
was an old flag or path whose artifact had been cached for weeks, so no build had run the broken step.

| Fault | Where | What it did |
|---|---|---|
| `-flto` on every configuration | `cmake/ConfigureBuildFlags.cmake` | Forced monolithic LTO on Slang. `lld` 22.1.8 crashed. |
| `SLANG_ENABLE_RELEASE_LTO ON` | `cmake/ConfigureSlang.cmake` | Overrode the Slang default, which is off. |
| `/Ob2 /arch:AVX2` with no compiler test | `CMakeLists.txt` | The full Clang driver reads them as file names. |
| A written path to the Slang library | `cmake/ConfigureSlang.cmake` | Slang moved its output directory in v2026.16. |
| `,gitattributes` | repository root | A comma in place of a dot, so git never read the file. |
| `*.json text` | `.gitattributes` | With `core.autocrlf` on, that gives a CRLF working tree. The cooker writes LF. |

**LTO is now off everywhere.** Slang sets it off by default for the same class of reason. Nobody
measured what it bought here, and the cook is about 1080 ms without it.

**Two lessons hold beyond these six.**

`ninja-clang-cl` uses `clang++`, the GNU driver, and not the clang-cl frontend. **Any flag spelled
the MSVC way is a latent break that waits for the next clean configure.** Every flag decision must
go through the compiler test at the top of `CMakeLists.txt`.

**Ask CMake where a file is. Do not write the path.** `$<TARGET_FILE:slang>` cannot go stale. A
written path fails at the end of a long build, on the day a submodule moves.

### The regression the known good dumps found

`applyLeafTypeLayout` became a switch on the binding kind. The texture arm lost
`getResourceResultType` and read `getType` on the wrong object. `getScalarType` of a texture is none,
so **all 35 textures of `OceanFft` reported an invalid sample type, and the cook still exited 0.**

No validator could see it. The WGSL cross-check compares group and binding alone, and both were
right. The three round trips replay the cooker against itself, and the cooker agreed with itself.

`scripts/check-known-good.py` found it in one run, as 35 changed lines. **This is why that script
exists, and why a schema change must be accepted deliberately rather than in passing.** Accepting the
new dumps without reading them would have made the defect the baseline.

---

## 14. The manifest variant-key retrieval path

This is the current work, after E7. It lets a runtime consumer find a variant by its axis values. It is
built in four phases. The text follows ASD-STE100.

**Design decisions, settled:**

- The output sink treats `-o` as a **directory**. Every artifact goes inside it. The C++ header emitter
  is gone, so there is no primary file. `tests/CMakeLists.txt`, `scripts\run-tests.bat`, and
  `scripts/check-known-good.py` all pass a directory now. `check-known-good.py` cooks into a temp
  directory and reads the dumps from it.
- The variant key is a strong type: `enum class VariantKey : uint64_t` in `client/include/VariantKey.hpp`.
  `PackVariantKey`/`UnpackVariantKey` (a mixed-radix fold in declaration order, and its inverse) are in
  `client/src/VariantKey.cpp`. `PermutationSpace::ComputeVariantKey` routes through `PackVariantKey`, so
  the cooker and the client share one packing algorithm.
- The client never constructs a key from raw axis values. The cooked variants are already
  canonicalized, gated, and pruned, so the client decodes the keys that exist and matches against them.
  No expression evaluator and no canonicalization on the client.
- Radices are `ManifestAxis::ValueCount`, already serialized. Do not add a manifest radices field. Cache
  the radices and the per-axis place-values on the view at `Open`. A single-axis constraint is one
  div-and-mod on the raw key: `digit_j = (key / place_j) % radix_j`, no full decode. The filter type is a
  value-set per axis (`QueryAxisRange`).

**Phase state:**

- **Phase 0 done.** Variant lookup is by key. The base `ShaderSourceProvider`, `FindSlot`, and the
  `ManifestShaderSourceProvider` methods all take `VariantKey`. A fossil off-by-one is fixed: the verify
  used `entry_point_index + 1`, but `EntryPointId` is zero-based (E4a) and a direct slot offset, so it is
  the index. That bug hid because `VerifyManifestRoundTrip` was orphaned (`EmitLibraryModules` had no
  caller, against rule 3 of `CLAUDE.md`); it is wired back in.
- **Phase 1 done.** The strong key and the shared codec, described above.
- **Phase 2 done.** The manifest carries the axis schema. `ManifestAxis` holds the name, value count,
  and `Kind`/`Domain`/`BindingTime` bytes (the axis enums live in `client/include/ShaderLibraryTypes.hpp`,
  one source of truth for the cooker and the client). An `AxisValues` table holds each value; for a
  `Type` axis the value names a string, the implementation type name. `Open` validates the axis and value
  tables, string indices included. A `BuildAxisTables` fossil that wrote the value table twice was
  removed.
- **Phase 3 mostly done (builds green; 17 tests + 6 known-good dumps pass).** The client query surface is
  `client/include/ShaderManifestIndex.hpp` + `client/src/ShaderManifestIndex.cpp`, in
  `lodestone_client_internal`. Built and working:
  - `ManifestIndex` holds the view plus caches built at construction: `radices` (= `ManifestAxis::ValueCount`),
    `placeValues` (suffix product via `exclusive_scan` over reversed radices; the multiplier for axis j),
    and an `axisNameToIndex` map.
  - `Decode(key)` and `Enumerate()` unpack keys through `UnpackVariantKey` and a shared private
    `decodeAxis` (one place, so the two loops cannot drift).
  - `scan(span<ScanConstraint>)` filters the sorted `VariantKeys()` by a per-axis digit test, after a
    `lower_bound`/`upper_bound` band pre-narrowing (min/max key from per-axis min/max digits; a cursor
    merge over sorted constraints, O(K+C)). `ScanConstraint` is `{ uint32_t AxisIndex;
    std::vector<uint32_t> AllowedValueIndices }` (owned).
  - `Select(span<QueryAxisRange>)` resolves each range to a `ScanConstraint`, sorts by axis, calls `scan`.
    Unknown axis or value returns an empty result, never throws.
  - `ManifestQueryBuilder` is value-semantic (every `Where` returns a new builder, so a cached base query
    stays immutable). `Where(bool/uint32/string_view)` validate against the axis, push a `QueryError` on a
    miss, and merge into one `QueryAxisRange` per axis. `QueryError::Suggestion` carries the nearest
    accepted name (`lodestone::suggest`, `max(1, len/3)` budget) for a mistyped axis name or Type value.
  - `QueryAxisValue` is `{ Domain Type; uint32_t IntegralValue; string_view TypeName }` -- the old
    `bool BoolValue` union member was deleted; booleans store 0/1 in `IntegralValue`.

  Still open (the leftover items):
  - **Builder terminals declared, not defined:** `Keys`, `First`, `Variants`, `Size`, `IsValid`,
    `Errors`. Check validity, then route through `scan` (Keys), `Decode` each (Variants), early-out
    (First), count without materializing (Size). Nothing exercises `Select`/`scan`/suggestions end to end
    until these land.
  - **`WhereAnyOf` / `WhereAnyOfBoolean` declared, not defined.** Add multiple values to one axis
    constraint; `WhereAnyOfBoolean` is the sugar for `{true,false}`.
  - **Value factories** (`AxisBool`/`AxisInt`/`AxisType`) for the raw `Select` path were never added;
    with the union gone, `AxisBool` writes `IntegralValue = 0/1`. Optional until a caller hand-builds a
    `QueryAxisRange`.
  - **Duplicate-axis coalescing not implemented.** Two `Where`s on one axis append both values;
    `scan`'s cursor merge assumes one constraint per axis. Plan: coalesce on insert in the builder
    (intersect allowed sets; empty intersection is the impossible-query error), so `scan` never sees a
    duplicate. Harmless only because the terminals do not run yet.
  - **No `ManifestIndexTest`.** Build one from `ShaderManifestRejectTest`'s byte-builder with a Boolean +
    a Type axis; first end-to-end test of the query path, and it confirms the boolean fix through `Select`.
  - **`QueryError::AxisName` dangles on an unknown axis** -- it views the caller's string, not the
    manifest (`Suggestion` is always a manifest view and is safe). Pre-existing; decide copy-vs-document
    when the `Errors()` surface is finalized.

**Two data-format follow-ups (both touch `AxisValues`; do them in one manifest version bump).** See
`todo.md` "Cook driver and manifest" and "Permutation system".
- **Axis values `int64` -> `uint32`.** They are `int64` only because the emitter reused
  `PermutationValueToInt64` (the size-expression evaluator's widener). Every value is `uint32` at the
  source and every consumer casts back down. Keep the evaluator on `int64`; store `uint32`.
- **Enum axes are name axes, like interface/Type axes** -- see §14a.

**Verify with:** `scripts\build.bat RelWithDebInfo ninja-clang-cl`, then `scripts\run-tests.bat
RelWithDebInfo ninja-clang-cl` (17 targets), then `python scripts\check-known-good.py` (6 dumps). The
tree builds green with all tests and dumps passing as of this writing.

## 14a. Enum axis plan (the current task, 2026-09-16)

A Slang `enum` used as the type of an `[ls_axis_enum]` `extern static const` is a permutation axis whose
values are the enum cases. Slang reflects the cases fully, so users declare nothing twice: `[ls_axis_enum]`
needs **no** string argument, because the const's type *is* the enum -- which also erases the
name-mismatch risk of listing case names in the attribute.

Reflection facts (verified in `third_party/slang`): the const's type reflects as `TypeReflection` with
`Kind::Enum`; `getFieldCount()`/`getFieldByIndex(i)` return the cases as `VariableReflection`
(`slang-reflection-api.cpp:572`/`:603`); each case gives `getName()` and `getDefaultValueInt(int64_t*)`
(the real assigned value, so explicit/non-ascending values are handled). `magic_enum` is the wrong tool
(it reflects C++ enums; this is a Slang enum seen only through reflection).

Key decision: the enum's integer value is **cook-time only** (size-expression evaluation, and generating
the bound shader literal). No runtime consumer needs it, so the manifest stores an enum value exactly
like a Type/interface axis: the case **name** as a string index. `Domain == Enum` distinguishes it. The
digit is the declaration-order position. This means enum reuses the Type/interface machinery on both
sides of the wall; the only enum-specific code is cook-side.

Ordered steps:
1. Declare `[ls_axis_enum]` in `tests/assets/LodestoneAttributes.slang` (targets `Var`, like
   `ls_axis_boolean`; no argument).
2. `RawAxisDeclaration` (`include/compile/RawLibrary.hpp:57`): add `bool IsEnumAxis` and
   `std::vector<RawEnumCase> EnumCases` with `RawEnumCase { std::string Name; int64_t Value; }`. The enum
   type name reuses the consolidated `RootName` field (shared with interface axes -- both anchor on one
   named root type; only the use differs). `Value` is optional for the first pass (see step 3).
3. `buildAxisDecl` (`src/compile/impl/SlangModuleContext.cpp:454`): add an `ls_axis_enum` check to the
   early-out at line 465 and to the mutual-exclusivity guards. When present, reflect
   `variableReflection->getType()`, assert `getKind() == TypeReflection::Kind::Enum` (a non-enum type is
   an author error, not a nullopt), store its `getName()` as `RootName`, and walk
   `getFieldCount()`/`getFieldByIndex(i)` (these return the enum CASES, in declaration order) ->
   `getName()` into `EnumCases`. The case value (`getDefaultValueInt`, deprecated in favor of
   `getDefaultValueBlob`) is only needed once a size expression may name an enum axis (step 6); the first
   pass captures names alone. Declare the axis const with an explicit enum type in the test shader to
   avoid inferred-type reflection surprises.
4. `BuildPermutationSpace` (`src/permute/PermutationSpace.cpp`): add an enum branch building a
   `PermutationAxis` with `ValueDomain = Enum`, one value per case. Reuse the Type/ordinal machinery (the
   axis holds the case names like an interface axis holds impl names); carry the case ints on the axis for
   the evaluator.
5. Literal generation (`src/permute/PermutationValue.cpp`, `ValueToSlangLiteral` /
   `MakeExportedConstantSource`): an enum value emits as `<EnumTypeName>.<CaseName>`, not an int.
6. Evaluator (only if a size expression may name an enum axis): map an enum axis value to its case int in
   `MakeResolveContext` / `PermutationValueToInt64` (`src/model/ResolveStage.cpp:253`). Skip until a
   shader needs an enum numerically; capture the int regardless.
7. Emitter (`src/emit/ShaderManifestEmitter.cpp`, the `BuildAxisTables` switch near line 722): move
   `Enum` from `AppendLiteralValues` to `AppendStringValues` -- write each case name's string index.
8. Client `decodeAxis` (`client/src/ShaderManifestIndex.cpp`): move `Enum` out of the Integral
   fallthrough into the `Type` branch (`TypeName = String(currValue)`).
9. Client `where` (same file): the `Integral`-satisfies-`Enum` interchange becomes
   `Type`(string/name)-satisfies-`Enum`; route enum resolution through `stringValueIndices`. Enum is
   queried by name via the `Where(string_view)` overload.
10. `ValidateAxes` (`client/src/ShaderManifest.cpp`): validate an `Enum` axis's values as string indices
    (like `Type`), not raw ints.
11. Test: add an enum axis (with at least one explicit, non-ascending case value) to a test shader and
    cover reflection -> manifest -> query-by-name -> decode -> suggestion.
