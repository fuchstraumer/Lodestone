# Agent handoff

Written on 2026-08-22. This document records the state, the measured facts, and the next task.
Read it after `docs/phase-d-tail-handoff.md`.

Text in this file follows ASD-STE100.

---

## 1. State

The build is green. Thirteen test targets pass. Ten are unit tests. Three are cooks.

`src/compile/SlangCompiler.cpp` holds 1732 lines. `clang-tidy` reports no warning in that file.

| Cook | Module | Proves |
|---|---|---|
| `CookTest` | `OceanFft.slang` | The permutation path, end to end. About 18 seconds. |
| `EntryPointParamsCookTest` | `EntryPointParams.slang` | The entry point scope walk. About one second. |
| `ParameterBlocksCookTest` | `ParameterBlocks.slang` | The parameter block walk. About one second. |

A green cook of `OceanFft` reports 35 variants over an index space of 56, 105 entry point variants,
77 unique sources, 4 resources, 1 resource list, 7 footprint lists, and 2 visibility lists. Those
numbers are the regression check.

## 2. Build and test

Use the scripts. Do not use a bare `cmake --build`, and do not use a bare `cmake --preset`. The build
tree keeps the compiler that `CMakeCache.txt` holds. Another environment gives that compiler the
headers of a different toolset, and every SPIRV-Tools target then fails.

```
scripts\build.bat [Debug|RelWithDebInfo] [preset]
scripts\configure.bat [preset]
scripts\run-tests.bat [Debug|RelWithDebInfo] [preset]
```

Run `configure.bat` after a change to a `CMakeLists.txt` that adds or removes a target.

**Read the exit code of the build itself.** A pipe into `grep` or `tail` gives you the exit code of
that command, and a failed build then looks like a success.

**Run the tests after each change.** A build proves less than it looks. Section 6 records a crash
that a green build hid.

To compare two states, cook each module into a directory and compare the directories:

```
build\ninja-msvc\tools\cooker_console\Debug\lodestone_cooker_console.exe -o <dir>\ShaderLibrary.hpp --dump-stage=raw --dump-stage=resolved --quiet <module>
```

## 3. Phase E steps E0a and E0b are complete

Section 11 and section 13 of `docs/phase-d-tail-handoff.md` record each step in full. The short form
follows.

**E0a reads the entry point scope.** `CollectBindingRangeDrafts` walks any scope, and
`ExtractRawEntryPoint` calls it a second time with `entryPointLayout->getTypeLayout()`. An entry point
owns the parameters it declares, so ownership states the visibility. A placement query must not,
because Slang generates each entry point as its own artifact.

**E0b reads the parameter blocks.** `CollectSubObjectDrafts` walks the sub-object ranges.
`ReadParameterBlock` reads one range, and `ReadBlockContainer` reads the container.

**The two walks partition the binding ranges on one test.** The range walk keeps every range the
parent placed itself. The sub-object walk keeps the rest. Both test
`getBindingRangeDescriptorSetIndex(range) >= 0`. A global `ConstantBuffer<T>` proves that the test is
necessary, because the parent places it and it also reports a sub-object range.

**A binding carries `ScopeName` beside `Name`.** The two together are the identity. Two entry points
can each declare `albedoMap`. The field runs from `RawBinding` to `ManifestBinding` and `BindingInfo`.
It is empty at global scope. `ExpectedDeclaredName` builds `ScopeName + "_" + Name`, and the
cross-check compares that against the de-mangled emitted name.

## 4. Nine measured facts about Slang reflection

Probe modules measured each fact. Slang documents none of them. Do not measure them a second time.

1. **`getVarLayout()->getName()` gives nothing** on an entry point layout. The scope name comes from
   `k_EntryPointScopeName` and from `CollectScopeNames`.
2. **Slang names the entry point scope `entryPointParams`.** The string is a name hint in
   `slang-ir-entry-point-uniforms.cpp` line 584. It is a Slang convention, so it stays in
   `src/compile/SlangCompiler.cpp`.
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

## 5. Four rounds of cleanup, and one measurement that repeated

The author made her own cleanup pass. She removed `CookResult<void>` for `CookError`, made several
functions `constexpr`, and ran `clang-format`.

Four agent rounds followed. Each one kept every artifact byte identical.

| Round | Work | Lines |
|---|---|---|
| A | One tree walker for four collectors | +26 |
| B | The compiler options as a table | +7 |
| E | Four `clang-tidy` findings | 0 |
| D | `CollectSubObjectDrafts` split into three | +38 |
| C | One reader for `SLANG_UNKNOWN_SIZE` | +28 |

**Extraction in this file does not reduce the line count.** The agent predicted a reduction three
times and measured an increase three times. The cause is the convention of this file: each function
carries a doc comment that states a Slang behaviour, and each extraction therefore costs 8 to 12 lines
of prose. The duplication that goes away is smaller than that.

Each round still bought something. A removed three copies of one tree walk, so the four collectors
cannot drift. D cleared a cognitive complexity of 36. C gave the `SLANG_UNKNOWN_SIZE` rule one place
and the diagnostic one wording.

## 6. A crash a green build hid

`BlobToString` lost its null check in commit `aff9204`. `GenerateOneEntryPoint` calls it with a
diagnostic blob, and Slang leaves that blob null when codegen has nothing to report. Every clean
compile then read through a null pointer, and every cook stopped with a segmentation fault.

The build stayed green, because the fault is a run time fault. `scripts\run-tests.bat` finds this
class of defect on the first cook. Run it.

## 7. The next task: split the compiler from the reflector

The author owns this task. Plan it with her, and let her decide the interfaces.

`src/compile/SlangCompiler.cpp` does two jobs. It **builds** modules, entry points, and target text.
It also **reflects** on what it built. The two jobs share only the linked program.

The split gives three things:

- A smaller file. Section 5 shows that no refactor inside one file reduces the line count. A split
  moves whole regions out.
- A home for the Slang behaviour notes. Nearly every note in section 4 belongs to the reflector.
- A seam for threading. The author removed the threading of `getEntryPointCode`, because it only
  helped while `OceanFft` was the one test module. Codegen is a build job, and the split names it.

Read `CompileVariantRaw` first. It is the function that calls both halves in order.

## 8. Open work

- **Step E0c.** `docs/phase-e-data-driven-permutations.md` §1e. A pointer type reaches WGSL, and no
  check sees it. This one is a defect, and not a missing capability. A cook can exit 0 and write
  invalid output.
- **Two unguarded reads.** `VisitLeaves` reads `getOffset(category)`, and `CollectUniformMembers`
  reads a leaf `getSize`. Neither tests `SLANG_UNKNOWN_SIZE`. Neither is a placement, so neither can
  misplace a binding.
- **`docs/` is in `.gitignore`.** This file is therefore untracked. Use `git add -f docs/agent-handoff.md`
  to keep it.

## 9. How this author works

Read the "Working with this author" section of `CLAUDE.md` first. Three points need repetition.

**The author reserves the implementation work she enjoys.** Plan it, explain it, and ask before you
start.

**Establish that a cost exists before you help her remove it.** Say so plainly when a cost is
negligible. Section 5 is the agent failing its own version of this rule.

**Measure, and do not estimate.** A probe module and a temporary `std::println` answer a question
about Slang in one build. Every fact in section 4 came that way, and three of them contradicted a
plan document.
