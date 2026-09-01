# Phase E step E0: the Slang interface spike

Written on 2026-09-01. This document reports step E0 of
`docs/phase-e-data-driven-permutations.md` §8. It answers three questions about interface axes.

Text in this file follows ASD-STE100.

Each answer comes from a measurement, and not from a reading alone. The probe is
`interface_spike.cpp` in the scratchpad. It links `slang.h` and nothing from this repository. Slang
is at `v2026.16.1-2-g28c755b09`, and the target is WGSL.

---

## The answers, in one table

| Question | Answer |
|---|---|
| Q1. Can link-time specialization fill an `extern` of interface type? | **Yes.** The mechanism is the one the constant axis already uses. |
| Q2. Can reflection enumerate the conforming types, with a stable name? | **Yes, but no API does it.** A decl walk plus a conformance test does it. The name is not module qualified. |
| Q3. Can an interface carry its own bindings? | **No.** A resource member in a conformance fails code generation. |

**Take interface axes for step E7.** Q1 is the question that decided it, and the answer removes the
need for the enum fallback. Q2 and Q3 add three requirements, and section 5 lists them.

---

## Q1. Link time specialization fills an `extern` type

**Yes.** Slang documents the form, and the probe links it.

`third_party/slang/docs/user-guide/10-link-time-specialization.md:120` opens the section
"Link-time Types". Line 146 declares the extern, and line 163 supplies the definition:

```slang
// the module that uses the type
extern struct Sampler : ISampler;

// a separate module that chooses it
export struct Sampler : ISampler = FooSampler;
```

The probe built both a `FooSampler` program and a `BarSampler` program from one root module. Both
linked, and the `FooSampler` program generated 589 bytes of WGSL.

**This is the same mechanism as `extern static const`.** The cooker already writes one synthetic
module for each active axis value, and links it. A constant axis emits
`export static const uint X = 5;`. A type axis emits `export struct X : IFoo = Concrete;`. The
difference is the text of one line in `SlangVariantCompiler`.

Two rules from the Slang document apply to any axis built this way.

- The name **and** the conformance clause must match between the `export` and the `extern`
  (line 167 of that document). A mismatch links nothing and reports nothing, which is the failure
  class rule 6 of `CLAUDE.md` already guards for constants.
- An `extern` may carry a default (line 174), so a module with no chooser still compiles.

## Q2. Enumeration works, and the name is weaker than the plan assumed

**Slang has no API that lists the types conforming to an interface.** The probe found none, and the
header has none. Enumeration therefore takes two steps.

**Step one: walk the declarations.** `IModule::getModuleReflection()` (`slang.h:5736`) returns the
module declaration, and `getChild` walks it. The probe read this tree for the probe module:

| Declaration | `getKind()` | `getFullName()` | Conforms |
|---|---|---|---|
| `common` | `Unsupported` | none | not asked |
| `ISampler` | `Unsupported` | none | not asked |
| `FooSampler` | `Struct` | `FooSampler` | yes |
| `BarSampler` | `Struct` | `BarSampler` | yes |
| `NotASampler` | `Struct` | `NotASampler` | no |
| `Deep` | `Namespace` | none | not asked |
| `Deep::NestedSampler` | `Struct` | `Deep.NestedSampler` | yes |

**Step two: test each `Struct` for conformance.**
`ISession::createTypeConformanceComponentType` (`slang.h:4652`) returns `SLANG_FAIL` when the type
does not conform (`slang.h:4650`). The probe used it as a predicate, and it separated the three
conforming types from `NotASampler` correctly.

Four measured facts follow, and each one costs work if it is found late.

1. **An interface reflects as `Unsupported`.** `SLANG_DECL_KIND` (`slang.h:2190` to `slang.h:2197`)
   holds eight kinds, and none of them is an interface. So the decl walk cannot find the interface
   the axis names. Use `ProgramLayout::findTypeByName` instead, which the probe confirmed works.
2. **`getType()` on a decl that is not a `Struct` crashes the process.** The probe faulted on the
   first `Unsupported` child. Test `getKind()` first. This is a precondition and not a defensive
   check, because the answer belongs to Slang and this code did not establish it.
3. **A namespace holds its own children.** `Deep::NestedSampler` is a child of `Deep`, and not of
   the module. A walk that reads one level finds fewer conformances than exist, and reports nothing.
4. **`getFullName` qualifies a namespace, and not a module.** It gave `Deep.NestedSampler`, and it
   gave `FooSampler` rather than `common.FooSampler`. `getFullName` is at `slang.h:2682`.

### What the conformance test decides

The test answers "does this type conform to this interface". It does **not** answer "does this type
implement every member", because a type that fails to do so never reaches reflection.

The probe declared a struct that names `ISampler` and implements one of its two methods. The module
did not load:

```
error[E38100]: missing interface member
 --> brokenmod.slang:4:31
  | public struct BrokenSampler : ISampler
  |                               ^^^^^^^^ type 'BrokenSampler' does not provide
  |                               required interface member 'sample'
error[E39999]: import failed due to compilation error
```

Slang enforces full conformance in the front end, at module load. So a partial conformance cannot
exist in a module the cooker can reflect on. By the time
`createTypeConformanceComponentType` can be called, a type that declares an interface already
satisfies it, and the two questions have one answer.

**This matters for the cooker in one way.** A broken conformance fails the bootstrap compile of
stage 3, and it fails with a Slang diagnostic that names the file, the line, and the missing member.
The cooker needs no check of its own for this case, and must not add one.

Fact 4 weakens a defense that §4 of the phase E document states. That section says to sort
conformances by fully qualified type name, and never by discovery order, because discovery order
comes from a file system walk. The reason is right. The key is not unique. Two modules that each
declare `FooSampler` produce one key. The sort then has a tie, and the file system breaks it.

**Build the key from the module name and `getFullName` together.** The cooker knows which module it
walked, so this costs nothing. Record it as a rule, because a bare `getFullName` looks correct.

## Q3. An interface cannot carry its own bindings

**No, and the failure is worse than a wrong layout.** The probe gave `BarSampler` a
`Texture2D<float4>` member and linked it in place of `FooSampler`. Two things happened.

**The layout did not change.** Both programs reported one global parameter, `output`, at space 0 and
binding 0. The texture appeared nowhere.

**Code generation failed.**

```
fatal error[E56003]: use of uninitialized opaque handle
  --> mainmod.slang:14:34
   | output[tid.x] += s.sample(i);
   |                          ^ use of uninitialized opaque handle 'Texture2D'.
```

The header states the layout half of this in advance. `slang.h:5441` to `slang.h:5445` says that a
component type which uses interface-type specialization parameters alone keeps a layout compatible
with the unspecialized layout, and that every parameter holds the same offset and binding. So the
placement cannot move. The probe adds what the header does not say: a resource inside the
conformance does not become a parameter at all, and the compile then stops.

Three results follow.

- **§8 of the phase E document asks the wrong follow-up.** It says that if an interface can carry
  bindings, the layout depends on the axis and the layout interner handles that. The layout never
  depends on the axis, so the interner is not involved.
- **An interface axis cannot express resource presence.** `docs/phase-f-vocabulary.md` §2 lists
  resource presence as one of the four axis kinds. That kind must stay a constant axis with a null
  test or an index test. An interface axis carries behaviour, and never resources.
- **The failure has no good report today.** A conformance with a resource member produces a Slang
  code generation error that names a line in the module that used the type, and not the conformance
  that caused it. Section 5 makes this a check.

---

## 5. What step E7 must do

Q1 says take the interface axis. Q2 and Q3 say it carries three requirements.

1. **Enumerate with a decl walk, a namespace recursion, and a conformance test.** Find the interface
   with `findTypeByName`, because a decl walk cannot see it. Test `getKind()` before `getType()`.
2. **Key each value on the module name and `getFullName` together.** Sort on that key. Never sort on
   discovery order.
3. **Reject a conformance that declares a resource member.** Stage 3 sees the type, so it can walk
   the fields and report the conformance by name. Without the check, the author reads a Slang error
   that names the wrong file.

Requirement 3 has the shape of step E0c, which is complete: Slang states a fact about a type, the
cooker holds a rule the target cannot break, and the check reports at the ingestion surface. Use the
same `ReportError` path.

**The enum fallback is no longer needed.** §8 of the phase E document keeps it because the spike
might have found no capability. Q1 found the capability, so E7 takes the type-level form. Keep the
fallback in the document as a record of the decision, and not as work.

## 6. What this spike did not measure

State these plainly, because a later reader must not think they were answered.

- **A generic interface.** The Slang document says a link-time type may be generic and may conform
  to a generic interface. The probe used a plain interface alone.
- **The cost of the conformance test.** `createTypeConformanceComponentType` builds a component
  type, so it is not a free predicate. A module with hundreds of structs pays it once for each. The
  probe had five.
- **Two modules that declare one type name.** Fact 4 predicts a key collision. The probe measured
  the name, and it did not build the collision.
- **A conformance added by a module linked later.** §4 of the phase E document names this hazard.
  The spike confirms nothing about it, and the policy file stays the defense.
