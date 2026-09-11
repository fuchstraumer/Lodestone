# Phase E step E6: the axis attribute spike

Written on 2026-09-11. This document reports two probes for step E6 of
`docs/phase-e-data-driven-permutations.md` §5 and §11. It answers whether Slang reflection can read an
axis declaration off a shader, and whether an axis in an imported module reaches the shader that
imports it.

Text in this file follows ASD-STE100.

Each answer comes from a measurement, and not from a reading alone. Two probes measured them.
`axis_attr_probe.cpp` reads one module. `import_probe.cpp` reads two modules and an `import`. Each probe
links `slang.h` and nothing from this repository, and each lives in the scratchpad. Slang is at
v2026.16.1, and the target is WGSL.

---

## The answers, in one table

| Question | Answer |
|---|---|
| Q1. Does an `extern static const` reflect, with its attributes? | **Yes.** It reflects as a `Variable` decl, and reflection reads each attribute string. |
| Q2. Does reflection give the declaration a source location? | **Yes.** `getDeclSourceLocation` gives a file, a line, and a column. |
| Q3. Does an axis in an imported module reach the importing module? | **Yes, but not through the importer's own reflection.** The session holds every loaded module, and the axis stays in the module that declared it. |

**The heritability holds, and the walk changes.** An axis in a common header reaches every shader that
imports it, because the session loads the whole import closure. A reader must not walk the root module
alone. It must iterate the loaded modules.

---

## Q1. An `extern static const` reflects with its attributes

**Yes.** Slang's own source states the attribute is legal on a constant. `core.meta.slang:4736`
defines `_AttributeTargets.Var` as "Global and local variables and constants". So an attribute declared
with `[__AttributeUsage(_AttributeTargets.Var)]` applies to an `extern static const`.

The probe declared two attributes and three constants, and it read them back:

```
  [2] kind=5 name=IFFT_SIZE
       userAttributeCount=1
       vx_axis_values arg0='128, 256, 512, 1024'
  [3] kind=5 name=IFFT_WAVE_SIZE
       userAttributeCount=2
       vx_axis_values arg0='32, 64'
       vx_axis_active_when arg0='IFFT_USE_WAVE_OPS != 0'
```

Four facts follow.

1. **An `extern static const` is `DeclReflection::Kind::Variable` (value 5).** `getModuleReflection()`
   (`slang.h:5736`) returns the module decl, `getChild` walks it, and `asVariable()` (`slang.h:3969`)
   returns the `VariableReflection`.
2. **Reflection reads each attribute string.** `VariableReflection::findAttributeByName(globalSession,
   "vx_axis_values")` (`slang.h:3251`) returns the attribute, and `Attribute::getArgumentValueString`
   (`slang.h:2514`) returns the author's exact string. The spaces stay, so the axis-values parser
   splits the string itself.
3. **A declaration holds more than one attribute.** `IFFT_WAVE_SIZE` carried two, `getUserAttributeCount`
   reported two, and both read.
4. **`extern` and defined behave the same.** A defined `static const` read the same way as the
   `extern` form.

This is the same pair the reflector already uses on struct fields
(`src/compile/impl/SlangReflector.cpp:708`). The probe proves it also works on a module-scope decl.

**One precondition.** The child walk returns mixed kinds. It returned the attribute struct definitions
(`Struct`) and the generated `attribute_syntax` entries (`Unsupported`), beside the constants
(`Variable`). Test `getKind() == Variable` before `asVariable()`. The E0 spike found the same rule for
`getType()`, which crashes on a decl that is not a `Struct`.

## Q2. The declaration has a source location

**Yes.** `ISession::getDeclSourceLocation(decl, &loc)` (`slang.h:4736`) returned `SLANG_OK`.
`SourceLocation` is `{ filePath, line, column }` (`slang.h:4485`). For `IFFT_SIZE` it gave
`probe.slang`, line 9, column 26, and the column points at the identifier.

This is what §10-D5b of the phase E document needs. The old check searches the source text for the axis
name, so the best it reports is a name and never a location. A decl walk reports the exact line. Remove
the text search.

## Q3. An imported axis reaches the importer through the session, not its reflection

This is the question that changes a plan. The probe built two modules. `common` declares the attributes
and the axis constants. `main` imports `common` and uses the constants.

**`main`'s own reflection does not hold the imported axes.**

```
=== does main's own reflection surface the imported axes? ===
  main: 3 children
      [0] kind=0 name=(null)          # the import, as an Unsupported decl
      [1] kind=5 name=gOut
      [2] kind=2 name=csMain
```

**The session holds both modules, and `common` still holds its axes.**

```
=== session->getLoadedModule enumeration ===
getLoadedModuleCount = 2
  loadedModule[0] name=common: 5 children
      [2] kind=5 name=IFFT_SIZE        <-- carries vx_axis_values
      [3] kind=5 name=IFFT_WAVE_SIZE   <-- carries vx_axis_values
  loadedModule[1] name=main: 3 children
```

Three results follow.

1. **A module reflection is module-local.** `getModuleReflection()` returns the decls that module
   declares, and never an imported decl. An `import` shows as one `Unsupported` child with no name, so
   the decl tree cannot even name the imports. Use the session, not the decl tree, to find them.
2. **The import closure is reachable through the session.** After the cooker loads the root, every
   module the root imports is a loaded module, and each module holds its own axis decls.
   `ISession::getLoadedModuleCount` and `getLoadedModule` (`slang.h:4667` and `slang.h:4668`) return
   them. The Slang core module did not appear in the count. The count was two, and both were user
   modules.
3. **The reader iterates loaded modules.** It collects the axis-carrying `Variable` decls from each
   loaded module. It does not walk the root alone.

**The heritability the plan wanted is real.** §5 of the phase E document states that an axis declared in
`CommonLighting.slang` reaches every module that imports it. The probe confirms it. An axis in `common`
reaches `main`, because loading `main` loads `common` into the session. The mechanism is the session's
loaded-module set, and not a transitive decl walk.

**The explosion vector the plan warned about is now certain.** §5 lists a common header of five axes
that multiplies into every importer. Because the reader collects axes from the whole closure, every
importer inherits every axis in its closure, whether it uses the axis or not. So the symbol-reachability
defense of §5 is no longer optional. Keep only an axis whose symbol appears in the source reachable from
the shader's entry points. This test also scopes the axis set to one shader, for the case where the
session holds several shaders at once.

---

## Facts for E6, with the API each one used

- `IModule::getModuleReflection()` returns the module's own decls only. `slang.h:5736`.
- `DeclReflection::Kind::Variable` is value 5, and it is the kind of an `extern static const`.
  `slang.h:3945`.
- `DeclReflection::asVariable()` gives the `VariableReflection`. `slang.h:3969`. Test the kind first.
- `VariableReflection::findAttributeByName(globalSession, name)` and `getUserAttributeCount()` read the
  attributes. `slang.h:3251` and `slang.h:3239`.
- `Attribute::getArgumentValueString(index, &size)` returns the argument string, untouched.
  `slang.h:2514`.
- `ISession::getDeclSourceLocation(decl, &loc)` fills `{ filePath, line, column }`. `slang.h:4736` and
  `slang.h:4485`.
- `ISession::getLoadedModuleCount()` and `getLoadedModule(index)` return the import closure.
  `slang.h:4667` and `slang.h:4668`.
- `_AttributeTargets.Var` covers a constant, so the attribute is legal on the declaration.
  `core.meta.slang:4736`.

## What step E6 must do

1. **Read axes from the loaded-module set, not the root.** Iterate `getLoadedModuleCount` and
   `getLoadedModule`. For each module, walk `getModuleReflection` children, and keep the `Variable`
   decls that carry a `vx_axis_*` attribute.
2. **Gate on `getKind() == Variable` before `asVariable()`.** The walk returns `Struct` and
   `Unsupported` decls beside the constants.
3. **Read each attribute with `findAttributeByName` and `getArgumentValueString`.** This is the path the
   size attributes already use.
4. **Prune by symbol reachability.** Keep only an axis whose symbol appears in the source reachable from
   the shader's entry points. This is the §5 defense, and the import result makes it required, because
   every importer inherits every axis in its closure. The prune also scopes the axis set to one shader.
5. **Report a bad axis at `getDeclSourceLocation`.** This replaces the text search and
   `VerifyAxisNamesAreDeclared`.

## What this spike did not measure

State these plainly, because a later reader must not think they were answered.

- **The cooker's own session.** The probes built a fresh session. The cooker's bootstrap
  (`PrepareRawModule`) loads the root and its imports into a `SlangModuleContext`. Confirm
  `getLoadedModule` returns the same closure there, and confirm the core module still does not appear.
- **A multi-module cook.** If one session cooks several shaders, `getLoadedModule` returns every closure
  at once. The reachability prune scopes the axes to one shader, but the probe did not build that case.
- **An `ActiveWhen` reference across a module boundary.** `IFFT_WAVE_SIZE` named `IFFT_USE_WAVE_OPS` in
  its `active_when`. The probe read the string, but it did not resolve the reference across the two
  modules. E2's backward-only rule assumes one declaration order. Confirm the order and the rule when
  the axes arrive from several modules.
- **A conditional or nested declaration.** §5 requires an axis at module scope, and not inside a struct
  or a function. The probe declared every axis at module scope. Reject a conditional declaration, and
  name the file and the line.
