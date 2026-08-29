# Agent handoff

Written on 2026-08-22. Rewritten on 2026-08-28, after the compiler split finished and the tests came
back green. This document records the state, the measured facts, and the next task. Read it first.

Text in this file follows ASD-STE100.

---

## 1. State on 2026-08-28

**The compiler split is complete and the pipeline works.** Both build configurations are green, and
all thirteen test targets pass in each one.

| Configuration | Build | Tests |
|---|---|---|
| Debug | green | 13 of 13 |
| RelWithDebInfo | green | 13 of 13 |

Ten targets are unit tests. Three are cooks. `scripts\run-tests.bat` reports `all targets passed`.

### The numbers to compare against

A green cook of `OceanFft` reports 35 variants over an index space of 56, 105 entry point variants,
77 unique sources, 4 resources, 1 resource list, 7 footprint lists, and 2 visibility lists, and it
emits 663 KiB of WGSL. Those numbers are the regression check, and they were confirmed again on
2026-08-28.

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

## 5. Phase E steps E0a and E0b are complete

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

## 8. The next task: phase E

The compiler work is done. `docs/phase-e-data-driven-permutations.md` holds the plan, and steps E0a
and E0b are already complete.

**Read §9 of that document first.** It lists what phase D was asked to leave behind for phase E, and
one item is still open: the space dump does not carry the axis fields that E2 adds. §9 states the two
possible answers, and the author must settle it **before** E2 starts.

**Step E0c comes first.** §1e of the same document. A pointer type reaches WGSL and no check sees it.
It is a defect and not a missing capability, so a cook can exit 0 and write invalid output. It is
also small, and it fits the "correctness is proved by comparison" rule: the fix is a validator.

After E0c, the phase E order is the one §10 of that document gives. The parts worth knowing before
you read it:

- The axis declaration moves onto the `extern static const` in the shader, as an attribute. That is
  what removes the drift rule 6 of `CLAUDE.md` guards against, by making the name impossible to state
  twice.
- The policy moves into a data file a tech artist owns.
- `src/permute/PermutationRegistry.cpp` and its `k_ModuleSpaces` table are deleted whole by step E6.
  Only `OceanFft` has a row today.
- The mixed radix index is replaced by a ranking index. §6 of that document says take form 1, a
  sorted key table with a binary search, and it names the three places an index is implemented.

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
- **Step E0c.** A pointer type reaches WGSL and no check sees it. A cook can exit 0 and write invalid
  output. §8 says it comes first.
- **Two unguarded reads.** `VisitLeaves` reads `getOffset(category)`, and `CollectUniformMembers`
  reads a leaf `getSize`. Neither tests `SLANG_UNKNOWN_SIZE`. Neither is a placement, so neither can
  misplace a binding.
- **`hardware_concurrency` is the wrong thread count. Started.**
  `SlangCompilerCreateInfo::ExpectedBatchSize` exists for this, and `ThreadPool::Initialize` does not
  read it yet, so the count is still `hardware_concurrency`. §10b measured 3 to 4 workers as the
  optimum with the lock, against `hardware_concurrency` at 2.4 times worse. The count should follow
  the variant count.
- **A global session for each batch.** Each worker creates one inside the outer loop. One for each
  thread is correct, because a global session is not thread safe. One for each batch is more than
  necessary.
- **One pool for each module.** The driver builds one `SlangCompiler` for each module, so a cook of
  several modules pays the convoy of §10b once for each. `CompileBatch` would have to carry the
  module state for one pool to serve several.
- **MSVC raises warnings that Clang does not.** Three C4244 trace to the ranges pipeline at
  `src/compile/impl/SlangReflector.cpp:547`, where `std::views::enumerate` yields an `int64_t` index
  and `std::ranges::to<std::vector<uint32_t>>` narrows it. Two C4715 sit at
  `src/permute/PermutationValue.cpp:99` and line 114. Build both arms before you call a file clean.
- **The space dump does not carry the axis fields that E2 adds.** §9 of the phase E document states
  the two answers. Settle it before E2 starts.
- **`docs/` is in `.gitignore`.** This file is force-added, so it survives. Use
  `git add -f docs/agent-handoff.md`. Run `git ls-files docs/` to see which others did.

---

## 12. How this author works

Read the "Working with this author" section of `CLAUDE.md` first. Three points need repetition.

**The author reserves the implementation work she enjoys.** Plan it, explain it, and ask before you
start.

**Establish that a cost exists before you help her remove it.** Say so plainly when a cost is
negligible.

**Measure, and do not estimate.** A probe module and a temporary `std::println` answer a question
about Slang in one build. Every fact in section 4 came that way, and three of them contradicted a
plan document.
