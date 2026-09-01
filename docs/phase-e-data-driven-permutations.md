# Phase E: data-driven permutations

Phase E removes the compiled-in permutation registry. An axis declaration moves into the shader
source. Policy moves into a data file.

Phase D is complete, and so is the compiler split. Phase E is the current work.

Text in this file follows ASD-STE100. Condensed on 2026-08-28. Updated on 2026-09-01, after steps
E0a, E0b, E0c, and E0 completed and the C++ emitter was removed.

Read `docs/agent-handoff.md` first. It holds the state and the measured Slang facts.
`docs/phase-e-interface-spike.md` holds the E0 answers, and section 8 records what they changed.
Section 10 lists what phase D left for phase E. Every item there is now settled or done.

---

## 1. Why phase E needs phase D

Phase E changes the enumeration, the index, the declaration site, and the policy source. Each one
alters the emitted output.

`--dump-stage` came from phase D. A change to stage 1 now shows as a difference between two stage 1
dumps. Each phase E step therefore becomes "cook and compare" instead of "cook and hope".

The index change alone touches three places. Section 7 names them. With stage dumps, each of the
three moves on its own and proves itself.

---

## 2. What phase E does not build

`docs/phase-f-vocabulary.md` §6 divides the work between Slang and this repository. Read it before
you start. It removes work from this plan.

An axis has a **kind**. Each kind gets one mechanism.

| Axis kind | Mechanism | Owner |
|---|---|---|
| Capability that varies **by target** | `[require()]`, `__target_switch`, capability aliases | **Slang** |
| Capability that varies **by device** | A cooked axis, selected at run time | **Lodestone** |
| Technique | `interface` and generic specialization | **Slang**, driven by a Lodestone axis |
| Tuning | A cooked axis, or a specialization constant | **Lodestone** |
| Resource presence | The access model, and a null or -1 test | **Lodestone**, through the target profile |

The rule that separates the two capability rows:

> **A capability that varies between targets belongs to Slang. A capability that varies between
> devices inside one target belongs to Lodestone.**

Slang resolves a capability atom at cook time against a fixed target profile. An atom states what a
target *language* can express. It never states what a *device* supports. `subgroup_basic` includes
`wgsl` at `slang-capabilities.capdef:2504`, and WebGPU subgroups are an optional per-adapter feature.
Wave width is 32 or 64 on one Vulkan extension set.

`IFFT_USE_WAVE_OPS` and `IFFT_WAVE_SIZE` are therefore phase E axes. They ask a device question, and
not a portability question.

Two results follow. Capability atoms are a closed set, so a technique axis can never be one, and step
E7 stays as written. A portability requirement must never become an axis, so an axis that differs
only between targets is a defect in the declaration.

---

## 3. Found work

Three items came from probe modules, and none is a phase E idea. **All three are complete.** Each
one was small and independent, and all three touched the stage 3 code that E6 also changes.

### 3a. The entry point scope walk. Complete on 2026-08-21

`tests/assets/EntryPointParams.slang` cooks, and the cross-check agrees.
`EntryPointParamsCookTest` runs it.

`SlangReflector::collectBindingRangeDrafts` walks any scope. `extractRawEntryPoint` calls it a second
time with `entryPointLayout->getTypeLayout()`.

**Ownership states the visibility of an entry point parameter. A placement query must not.** Slang
generates each entry point as its own artifact, so two entry points can place different resources at
one group and binding. A placement query then lets one entry point claim the parameter of another.

`docs/agent-handoff.md` §4 records the measured Slang facts this step found.

### 3b. The `ParameterBlock` sub-object walk. Complete on 2026-08-21

`tests/assets/ParameterBlocks.slang` cooks, and the cross-check agrees.
`ParameterBlocksCookTest` runs it.

Slang describes a block with descriptor ranges alone. The binding range of a block therefore
reports a descriptor set index of -1, and the range walk drops it. `collectSubObjectDrafts` keeps exactly the
ranges the range walk dropped. The two walks partition the ranges, and no range is drafted twice.

`docs/agent-handoff.md` §4 records the four traps this step measured. Do not measure them again.

**One item stays open.** `FromSlangBindingType` maps `BindingType::ParameterBlock` to
`BindingKind::UniformBuffer`. That answer is wrong for a block of textures. No code reaches the line
today. With the code open, decide whether to remove the line or to make it correct by construction.

**Two probes nobody wrote.** A block that holds only ordinary data. Two entry points with a block on
each one.

### 3c. A pointer type reaches WGSL, and no check sees it. **Done 2026-09-01**

Found on 2026-08-20, during the phase F spike on buffer device addresses. This is a defect, and not a
missing capability. A cook can exit 0 and write invalid output.

A module declared `PointLight* Points` inside a push constant structure. `slangc` compiled that
module for WGSL and returned 0. The emitted text holds this:

```wgsl
struct Lights_std430_0
{
    @align(8) Points_0 : ptr<, PointLight_0>,
    @align(8) Count_0 : u32,
};
var<uniform> LightData_0 : Lights_std430_0;
```

WGSL has no such pointer form. A pointer must not appear in a host shareable structure. The address
space is also empty. No WGSL implementation accepts this text.

**Why the cross-check misses it.** `WgslBindingScanner` reads `@group` and `@binding`. The invalid
declaration carries neither. Every other binding agrees with reflection, so the cook exits 0. The
four validators compare the cooker against itself. **Not one of them asks whether the emitted text is
legal for the target.**

**The check to add.** Reject a pointer type at stage 3, and compare it against the access model of
the target profile. `slang::TypeReflection::Kind::Pointer` is value 18. Stage 3 reads Slang, so the
type kind is visible there. A `Pointer` kind under a `Bound` access model fails the cook and names
the resource.

Add the check at stage 3, and not later. `ReflectedUniformMember` holds a name, an offset, a size,
and an array count. It holds no type kind, so no later stage can ask the question. `CLAUDE.md` states
the rule: validate at the ingestion surface, then trust the data inside.

**This is a validator.** The target profile states the access model, and the author chose it on the
command line. Slang states the type kind, and the shader source decided it. Neither side derived the
other.

*`OceanFft` declares no pointer, and no artifact moved.*

`tests/assets/PointerMember.slang` is the probe, and `AccessModelRejectTest` runs it. The test holds
a table of one row for each claim, plus a control row that must cook. A later rejection, such as a
bound resource on a target that has pointers alone, is one module and one line.

**One thing the probe measured.** The check reads a uniform member, so the resource must be a
`ConstantBuffer`. Slang reports a push constant as `BindingType::PushConstant`, `FromSlangBindingType`
has no row for it, and the walk stops at an invalid binding kind before it reads any member. A push
constant therefore cannot be cooked at all today. WGSL has no push constants, so nothing is lost yet.
`docs/phase-f-vocabulary.md` is where that becomes a question.

---

## 4. The axis model holds two declaration mechanisms

Interfaces and constants are not two ways to do one thing. They sit at different levels, and one axis
model must hold both.

| | Interface axis | Constant axis |
|---|---|---|
| What varies | A type, and the functions it carries | An integer or a boolean value |
| Slang mechanism | Generic specialization over an `interface` | `extern const static` link-time constant |
| Source of the values | The set of types that conform | A list the author writes |
| Arithmetic can read it | No | Yes. `IFFT_SIZE * 4` is a size expression |
| Example | Which BRDF | How large the FFT is |

An interface axis has no integer value, so `EvaluateSizeExpression` cannot read it. **Give every axis
value an ordinal.** The evaluator then reads the ordinal, and the linker reads the type or the
constant.

An axis value therefore gets a domain:

- `Boolean` — false, true
- `Integer` — a list of values
- `Enum` — an ordinal with a name, for a value that is a label and not a number
- `Type` — an ordinal and a fully qualified Slang type name, for an interface axis

`PermutationValue` is a tagged union of `bool`, `uint32_t`, and `int32_t` today. `Enum` and `Type`
add a name, so the type grows. `PermutationValueToInt64` keeps working, because it returns the
ordinal.

### Two more fields, for phase F

An axis also carries a **kind** and an **earliest sound binding time**. Both come from
`docs/phase-f-vocabulary.md` §2 and §3. Both are enum fields on `PermutationAxis`.

- `AxisKind` — resource presence, capability, tuning, or technique. Section 2 maps each kind to its
  mechanism.
- `EarliestBindingTime` — cook, pipeline, draw, or thread. The earliest point at which the axis is
  **correct**. An axis that sizes a `groupshared` array is cook time and can never be later. An axis
  that only selects a pointer is sound at draw time, and at every earlier time.

The author states the earliest time. A target profile then decides how late the value really binds,
and the policy file of section 10 records that decision. This lets a client query a live adapter, map
it to a profile, and ask the manifest for a variant it can run.

**Add both fields in E2.** They cost two enums now. A later retrofit revisits every axis declaration.

### The hazard in an interface axis

An interface axis takes its values from the types that conform. **Any module linked later can add a
conformance.** The value set is therefore "what the cooker saw at cook time". A new BRDF renumbers every
variant of every shader that uses the axis.

Use both defenses:

1. **Sort conformances by a key of the module name and the type name.** Never by discovery order.
   Discovery order comes from a file system walk, and it is not stable across machines. Step E0
   measured the type name: `TypeReflection::getFullName` qualifies a namespace, and it does not
   qualify a module. So it gives `FooSampler` and not `common.FooSampler`, and two modules that
   each declare `FooSampler` produce one key. The cooker knows which module it walked, so the
   module name costs nothing to add.
2. **Let policy name the cooked set.** A new conformance then enters the space only when a person
   adds it to the policy file.

---

## 5. Where a declaration lives

Put the axis on the declaration, as an attribute, exactly as `[vx_element_count]` does.

```slang
[vx_axis_values("128, 256, 512, 1024")]
extern const static uint IFFT_SIZE;

[vx_axis_values("32, 64")]
[vx_axis_active_when("IFFT_USE_WAVE_OPS != 0")]
extern const static uint IFFT_WAVE_SIZE;
```

**The argument is one string, and not a list of integers.** A Slang attribute has fixed arity and no
optional parameters, so a four-value axis and a two-value axis would otherwise need two attribute
names. The repository already pays this tax for size expressions, for the same reason. A string also
leaves room for a range form later.

### What this gives

**`VerifyAxisNamesAreDeclared` becomes unnecessary, and rule 6 of `CLAUDE.md` becomes impossible to
break.** Today an axis name that does not match the `extern const static` name links a symbol nobody
references. It leaves the shader on its default, and it fails nowhere. That failure class disappears,
because the axis **is** the declaration. No second name exists.

This is the strongest argument for phase E. It is larger than the ergonomics.

Inheritance also follows. The bootstrap compile reads the dependency closure, so an axis declared in
`CommonLighting.slang` reaches every module that imports it.

### The cost, and one rule that makes it sound

The cost is a bootstrap compile. The cooker compiles the module once with defaults, reads the axis
attributes, and only then enumerates. `SlangCompiler::PrepareRawModule` already runs once for each
module, so the attribute reader is an addition to a call that exists.

The rule: **an axis declaration must be at module scope, and it must not be inside a conditional.** A
declaration the bootstrap compile cannot see is an axis the cooker never enumerates, and nothing
reports it. Reject a conditional declaration. Name the file and the line.

### Inheritance is also the explosion vector

A common header that declares five axes multiplies into every module that imports it. This is the
Slipspace failure mode: a static branch in a central file, and a combinatorial cost nobody sees until
the build is slow.

Three defenses exist. Keep all three, and add a fourth.

| Mechanism | When | Exact | Cost |
|---|---|---|---|
| Canonicalization | Before the compile | No, a guess | Free |
| **Symbol reachability (new)** | Before the compile | No, conservative | One text scan |
| Interning | After the compile | Yes | Already paid |
| Influence matrix | After the compile | Yes, and it reports | Integer compares |

The new one: after the bootstrap compile, test whether the axis symbol appears in the source text
reachable from the entry points of the module. If it does not appear, the axis cannot affect the
module, and the cooker drops it before it compiles anything. This test is conservative, so it never
removes an axis that matters.

---

## 6. The constraint language

Extend `SizeExpression` with a comparison level and a logical level. The grammar gains two rows below
`shift`:

```
logical    := comparison (( '&&' | '||' ) comparison)*
comparison := shift (( '==' | '!=' | '<' | '<=' | '>' | '>=' ) shift)*
shift      := sum (( '<<' | '>>' ) sum)*
```

`unary` gains `!`. The result stays `int64_t`, and a nonzero result means true. This matches C and
Slang. It adds no type, and it keeps one evaluator. A size expression and a constraint expression are
then one function called in two places.

About 80 new lines. The existing tests stay green.

### Two constraint kinds, and no more

Kconfig has `depends on`, `select`, `visible if`, and `range`. Take two of them.

- **`ActiveWhen`** — the axis is active only while the expression holds. When it does not hold, the
  axis is inactive and canonicalization fills its first value. This generalizes today's `ParentIndex`
  and `RequiredParentValue` from one parent to an expression.
- **`Require`** — an assignment is invalid when the expression fails. This prunes the enumeration.

**Do not implement `select`.** In kconfig, `select` forces a value and bypasses `depends on`. It
causes most kconfig defect reports, and it exists there for backward compatibility.

### One requirement that is easy to miss

An `ActiveWhen` expression can name other axes, and those axes can carry `ActiveWhen` expressions of
their own. **The axes therefore form a directed graph, and canonicalization must evaluate them in
topological order.** A cycle must fail when the registry loads, and not during a cook. The error must
name every axis in the cycle.

`ParentIndex` is a single index today, so a cycle is nearly impossible to write by accident. With
expressions it is easy. Add the cycle check in the same step as the constraint language.

---

## 7. Enumeration, and the index

### Counting is not needed

Validity is SAT. Counting is #SAT. Both are hard in general, and neither one is the problem here.

**A counting oracle is unnecessary, because the cooker already enumerates.** Stage 2 walks the whole
valid space and produces every `VariantDescriptor`. A depth-first walk with constraint propagation
does the same job with constraints in place. Once the sorted list of valid assignments exists, the
rank of an assignment is its position in that list. Counting answers the rank **without** the list,
and the list is already there.

The sizes:

- 20 boolean axes give a nominal space of about one million.
- Constraints usually reduce that to hundreds or low thousands.
- A depth-first walk with propagation visits the valid space and the pruned branches. That is
  thousands of nodes, and each node costs a few integer compares.

**Enumeration is never the bottleneck. Compilation is, by about five orders of magnitude.** A cook of
400 variants spends milliseconds on enumeration and minutes on compilation. The policy file of
section 10 therefore matters far more than the choice of algorithm.

One guard: **enforce `MaxVariants` during the walk, and not after it.** A constraint set that admits
ten million assignments must fail in a second and name the module.

### Mixed radix stops working

A space with holes needs a million slots for four hundred variants, and the emitted tables carry
every hole.

### Form 1: a sorted key table and a binary search. **Use this form**

Pack each canonical assignment into one `uint64_t`, with the existing mixed-radix arithmetic. Sort
the packed keys. **The dense index is the position in the sorted array.**

- Lookup: `std::lower_bound` over a `constexpr` array. About 9 steps for 400 variants, and 17 for
  100,000.
- Memory: 8 bytes for each variant. 400 variants cost 3.2 KiB, against 4 MiB of holes today.
- `constexpr`: yes. A sorted array and a binary search are both `constexpr` in C++20.

**Canonicalization does not change.** The packed mixed-radix key stays the canonical form. It stops
being the storage index and becomes the search key. A partial-assignment lookup still works, because
it works on the canonical form.

### Form 2: prefix counts and true ranking

Bake the number of valid completions of each prefix. Ranking is then a sum over the axes, with no
search:

```
rank = sum over axes i, of  sum over w < v_i, of  N(v_0..v_{i-1}, a_i = w)
```

This costs O(axes) instead of O(log variants), and it needs the #SAT counts at cook time. It is the
more elegant answer. It is not worth the cost yet. Consider it if a module ever passes about
100,000 variants.

### Form 3: mixed radix with the holes

What exists today. It fails as described above.

### One place implements the index. It was three

The removal of `ShaderLibraryEmitter` on 2026-09-01 deleted two of the three sites. Only the
cooker's own arithmetic is left.

| Site | What it is | State |
|---|---|---|
| `ComputeVariantIndex`, `src/permute/PermutationSpace.cpp:221` | The cooker's own arithmetic | The one site |
| `EmitVariantIndex`, in the C++ emitter | Emitted a `constexpr` C++ function | Deleted |
| `EmitCanonicalize`, in the C++ emitter | Emitted the `constexpr` canonical form | Deleted |

The emitter held a comment reading "This must match ComputeVariantIndex in". That was a copy of the
one rule the whole index rests on, kept in step by hand. It is gone.

**This makes E4 much smaller.** E4 used to change the cooker arithmetic, the emitted C++, and the
manifest variant table at one time, and the step is marked high risk for that reason. It now changes
the arithmetic and the manifest.

**Form 1 removes the copy.** The emitted side becomes a sorted array and a search, and the search
encodes no radix rule. The cooker computes the keys, and the header only reads them. The rule then
lives in one place.

### The input to the index has a type

Phase D step D8c gave the canonical form its own type, `CanonicalAssignment`. Only
`PermutationSpace::CanonicalizeAssignment` builds one, so a partial assignment cannot reach
`ComputeVariantIndex` and return a plausible wrong index.

Form 1 replaces the arithmetic, and not the guarantee. Whatever computes a ranking index still needs
an input it can trust.

### One invariant to record now

**A dense variant index is stable only inside one cook.** A new axis, a new value, or a new interface
conformance renumbers it. Mixed radix behaves this way today, and ranking behaves the same way, so
this is not a regression. Phase E makes the space much easier to change.

State the rule where a reader finds it: **a material, a save file, or any asset outside the cook must
reference a variant by its assignment, and never by its index.** The index is a cook-internal handle.

---

## 8. Interface axes. The spike is complete

**Step E0 ran on 2026-09-01. `docs/phase-e-interface-spike.md` holds the answers and the
citations.** Read it before you start E7. This section records what it decided.

| Question | Answer |
|---|---|
| Can link-time specialization fill an `extern` of interface type? | **Yes** |
| Can reflection enumerate the conforming types, with a stable name? | **Yes**, but no one API does it |
| Can an interface carry its own bindings? | **No** |

**Take the interface axis. The enum fallback is not needed.** This section used to keep the fallback
because the spike might have found no capability. It found the capability.

`extern struct Sampler : ISampler;` in one module, and `export struct Sampler : ISampler = FooSampler;`
in another, link and generate code. **That is the mechanism the constant axis already uses.** The
cooker writes one synthetic module for each active axis value today. A constant axis emits
`export static const uint X = 5;`, and a type axis emits `export struct X : IFoo = Concrete;`.

Three findings change the work. Each one costs time if it is found late.

1. **An interface axis can never carry a resource.** A conformance that declares a resource member
   fails code generation, and the layout never changes. So resource presence, which
   `docs/phase-f-vocabulary.md` §2 lists as an axis kind, must stay a constant axis with a null test
   or an index test. An interface axis carries behaviour alone.
2. **Enumeration takes a declaration walk and a conformance test.** Slang has no API that lists the
   types conforming to an interface. An interface also reflects as an unsupported declaration kind,
   so `findTypeByName` must find the interface itself.
3. **`getFullName` is not module qualified.** Section 4 states the sort key, and the spike shows it
   is not unique. Build the key from the module name and `getFullName` together.

E7 also owes one check. A conformance that declares a resource member must fail the cook with a
message that names the conformance. Slang reports it as a code generation error against the module
that used the type, which names the wrong file. Step E0c built the shape this check takes.

---

## 9. Policy in a file

Declaring a space is authorship. Cooking a space is policy. Unity separates `multi_compile` from
`shader_feature` for this reason, and Unreal uses `ShouldCompilePermutation` for the same reason. A
declared axis is not always a cooked axis.

**Format: JSON.** TOML reads better for a person who edits by hand, and a good TOML parser is several
times the size. Do not use YAML.

### The reader is a third-party library, behind a facade

**A tech artist edits this file, so parse error quality is a user-facing feature.** A library gives
byte offsets and clear messages. A quick hand-written parser gives whatever somebody wrote. The
author of this repository then answers the question "it says invalid and I do not know why".

A writer is easy to hand-write. A parser must handle escapes, Unicode, number edge cases, and every
malformed input a person can type. The two are not the same job.

**Do not name the library in this document.** Choose it at E5 against these criteria:

| Criterion | Why |
|---|---|
| No exception on the error path, or a non-throwing entry point | Every error path in this repository is `std::expected` |
| Small compile-time cost | A very large single header costs every translation unit that sees it |
| Byte offset and a readable message on a parse failure | The tech artist is the user |

**Hide it behind a facade.** Use a pointer-to-implementation reader with a small key-and-value accessor. It keeps the
third-party type out of every header. It keeps the include in one translation unit. It limits a
later replacement to one file. `include/compile/SlangCompiler.hpp` is the standing example, because it
names no Slang type.

### The writer and the reader must not be one implementation

Keep `JsonWriter` for writing. Take the library for reading. Two independent implementations that
agree form **an asymmetry, and not a redundancy**. This is the shape of the WGSL scanner that checks
Slang's reflection. A round trip through one hand-written implementation proves only that it repeats
its own defects.

### Contents

| Key | Meaning | Source today |
|---|---|---|
| `MaxVariants` | The variant budget | `k_OceanFftPolicy` in C++ |
| `ExpectedInfluence` | Which axes must be inert for which entry point | `k_OceanFftPolicy` in C++ |
| `CookValues` | The subset of the values of an axis to cook | New. This is `shader_feature` |
| `CookWhen` | A predicate that removes an assignment from the cook | New. This is `ShouldCompilePermutation` |

`CookValues` and `CookWhen` use the evaluator of section 6. One grammar, three uses.

The `todo.md` item about parameter domain and device properties belongs in `CookWhen`. A predicate over
a named platform profile removes the assignments the target cannot run. It needs no mechanism beyond
a few symbols in the table of the evaluator.

### One section for each target profile

**The policy file needs a section for each target profile.** This is where the lowering decision from
`docs/phase-f-vocabulary.md` §3 is recorded. A section states, for each axis, the binding time that
the target really uses. An axis whose earliest sound binding time is `Draw` stays a cooked axis on a target with no push
constants. It collapses to one variant on a target that has them.

`MaxVariants` then caps each target separately. That is the honest way to state the cost: one shader
can be four variants on a desktop target and forty on WebGPU.

### One field in the manifest, and not in the policy

**Each variant must record the capability requirement it was cooked for.** A client queries the live
adapter, maps the result to a target profile, and asks the manifest which variants that profile can
run. Without the field, the client parses extensions by hand.

This is a small schema addition. It is far cheaper while the manifest already changes in E4.
`ManifestVariant` gained two index fields in phase D step D8b, so the record already moved once, and
nothing outside this repository reads it. `k_IsManifestRecord` replaced the `sizeof` assertions.

---

## 10. What phase D left for phase E

**Phase D is complete. Read this as a checklist.** D4, D5b, and D6 are done. D-wide holds. D1 is half
done. D2 is settled, and section 10 D2 records what it still owes E2.

### D2. Dump the space stage with full fidelity. **Settled 2026-09-01**

The riskiest change in phase E replaces `k_ModuleSpaces` with data. A stable JSON shape for the
stage 1 dump turns that job into "produce the same JSON from a different source".

**The author chose the first answer. The three fields are written now, as nulls.** `WriteAxis` in
`src/emit/StageDump.cpp` emits `kind`, `earliestBindingTime`, and `valueDomain` between the name and
the values. E2 fills them. E2 therefore changes a value and never the shape of the object, and the
known good dump stays a byte comparison for the step that most needs one.

`tests/known_good/OceanFft.stage-space.json` holds the new shape. The change added nine lines, which
is three fields for each of three axes, and nothing else moved.

**The byte comparison already exists, and it is not a file.** `CheckSpaceDump` in
`tests/StageDumpTests.cpp` holds one golden literal in the source. Its comment states the job: it
pins the JSON shape, the key names, the key order, and the four space indent, so a change to any of
those fails there rather than in a large diff. It failed on the three new fields at once, and the
literal now carries them.

**An inline literal is the better home for this check.** It needs no file, so it cannot disagree with
a checkout about line endings, and it names the failing claim rather than printing a diff.

**`tests/known_good/` now has a reader.** `scripts/check-known-good.py` cooks a module, compares all
six stage dumps, and prints a unified diff for each one that differs. `--accept` copies the new dumps
over the accepted ones, and it exists so that a schema change is accepted on purpose and never in
passing. All six files are current as of 2026-09-01, and `.gitattributes` keeps JSON at LF so the
comparison never fails on a line ending.

**The two checks answer different questions, and both are needed.** The inline literal pins the shape
of one small space, and it fails fast with no build artifact. The dump files pin the whole content of
a real module, across every stage. The literal cannot see a reflection regression in `OceanFft`, and
the script cannot fail before a cook runs.

**The stale dumps were refreshed on 2026-09-01, and one of the differences was a defect.** Four
changes were schema moves: the `ScopeName` field that E0a and E0b added, `globalBindings` renamed to
`bindings`, the binding kind `SampledTexture` renamed to `Texture`, and the hash name `xxHash3`
renamed to `xxHash3_64`. The fifth was a reflection regression that made every texture report an
invalid sample type. `docs/agent-handoff.md` §13 records it. **Read a difference before accepting
it.**

### D4. Separate the module-level stage 3 call from the per-variant one. **Done**

`SlangCompiler::PrepareRawModule(space)` returns a `RawModule` with the module facts. It runs once for
each module. `SlangCompiler::Compile(variants, sink)` returns one `CookResult<RawVariant>` for each
variant, and a `ThreadPool` spreads them across workers.

The bootstrap compile that E6 needs is an attribute reader added to `PrepareRawModule`. It is not a
restructure.

### D1. Name the JSON target for both directions. **Half done**

The alias `lodestone::json` is right. The target behind it is `lodestone_json_writer`, so
`JsonReader.cpp` would land in a target whose name says it only writes. Rename the target and keep
the alias, at the start of E5.

### D-wide. Do not add a fourth index site. **Holds**

Section 7 listed three sites. The removal of the C++ emitter deleted two, so one is left. Nothing
new computes a mixed-radix index. The hand-maintained copy that the warning comment named went with
the emitter, so phase E no longer has to delete it.

### D5b. The diagnostic sink. **Done, and phase E makes it pay**

A diagnostic is a record with a severity, a code, a file, a range, and a message. A sink decides what
becomes of it.

**Phase E owes that sink a source location.** `VerifyAxisNamesAreDeclared` searches the raw source
text for a string today, so the best it reports is an axis name. When each axis declaration moves onto the `extern const static` as an attribute, the check becomes
a walk of the declaration tree. Slang gives a location for each declaration through
`ISession::getDeclSourceLocation(DeclReflection*, SourceLocation*)`.

Two results follow, and both are phase E work:

- Remove the string search. It cannot report a location. A search that reads source text cannot
  separate a declaration from a comment that quotes one.
- An axis name that matches nothing stops being a message on a terminal and becomes a mark on the
  line that holds the mistake. That failure leaves the shader on its default and reports nowhere
  else, so it is the failure that most deserves a location.

Walk the declaration tree once, here.

### D6. Test the evaluator through its interface. **Done**

Section 6 adds two grammar levels. Write the stage 4 tests against `EvaluateSizeExpression` and the
resolve entry point, and never against a parse tree. The new levels then do not churn the tests.

### Small, and only while the code is open

Do not harden `PermutationValue` against new alternatives. Phase E adds `Enum` and `Type`. Read
`ModulePolicy` through `FindPolicyForModule` rather than the static table, so phase E replaces the
source and not the call sites.

---

## 11. Steps

E1 to E4 change **how**. Each one is verifiable byte for byte against a phase D golden dump. E5 to E7
change **what**. Each one adds capability that no golden file covers.

| Step | Work | Verified by | Risk |
|---|---|---|---|
| E0a | The entry point scope walk, from §3a. **Done 2026-08-21** | A probe module cooks and the cross-check agrees | low |
| E0b | The `ParameterBlock` sub-object walk, from §3b. **Done 2026-08-21** | A probe module cooks and the cross-check agrees | low |
| E0c | Reject a pointer type under a bound access model, from §3c. **Done 2026-09-01** | `AccessModelRejectTest`, on `PointerMember.slang` | low |
| E0 | Slang interface spike, with citations. **Done 2026-09-01** | `docs/phase-e-interface-spike.md` | none |
| E1 | Comparison and logical levels in `SizeExpression` | The existing tests, plus new ones | low |
| E2 | `AxisValueDomain`, `AxisKind`, `EarliestBindingTime`, `ActiveWhen`, `Require`, axis DAG, cycle check. `k_ModuleSpaces` stays the source | The space dump, **once §10 D2 is settled** | medium |
| E3 | Depth-first enumeration with constraint propagation | **The variants dump is byte identical** | medium |
| E4 | Sorted key table and binary search, in place of the storage index. Add the per-variant capability requirement to the manifest | Round trips, and the emitted tables shrink | **high** |
| E5 | Rename the JSON target, then the reader, the policy file, per-target sections, `CookValues`, `CookWhen` | Round trip against `JsonWriter` | medium |
| E6 | Axis attributes and the bootstrap compile. Delete `VerifyAxisNamesAreDeclared` | A cook of `OceanFft` with no registry entry | **high** |
| E7 | Interface axes. E0 removed the enum fallback | A new test shader | medium |
| E8 | Documents, and the measured numbers again | — | none |

**E0c and E0 are complete.** E1 is the next step, and everything in the constraint path depends on it.

**E4 needs care.** It changes the emitted C++, the manifest variant table, and the arithmetic of the
cooker at one time. The round trips find an error, and the stage dumps say where.

**Run E6's acceptance test once before E6 starts.** A module with no registered space reached
`space.front()` on an empty vector and aborted the cook until 2026-08-20. `EnumerateActiveCombinations`
is fixed, and `PermutationIndexTest` covers the empty space. Run the cook anyway, so a failure during
E6 belongs to E6.

E6 is the step that justifies the phase. After it, an axis name cannot drift from its declaration,
because only one name exists.

---

## 12. What this gives

An author declares an axis where the constant lives, and it propagates to every module that imports
the file.

A tech artist edits one checked-in file to decide what cooks, and never opens a C++ file.

A constraint that no tree can express is one line, and an assignment that breaks it never reaches the
compiler.

Four hundred variants cost 3.2 KiB of index, in place of 4 MiB of holes.

The failure that rule 6 of `CLAUDE.md` exists to catch cannot be written down.
