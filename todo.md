# SlangCompiler
- Right now, we map to a super small subset of formats and features. We should support the full range, and extract them untouched
    - Then, during output format mapping we collapse to what that platform
    actually supports
    - Alternatively, use foresight about output target to fail builds if unsupported
# Resource Layer
- We should provide a way for clients to call something like `SetDeviceLimits` or `SetApiLimits` - we can use this to validate resource sizing expressions when being run as a live compiler,
  or we can use it against cooked content (in the device form) to make sure we don't try to create a shader a device can't support
# Permutation system
- Give enum axes real value names, end to end. Enum axis values are integers under the hood (stored in
  the manifest `AxisValues` table as int64, same as Integral), but authors will want to write and read
  them as names, not magic numbers. Needs three pieces: (1) a shader attribute that preserves the enum
  value names alongside the axis declaration, so the cooker can carry them through; (2) query and policy
  surfaces that accept the names and map them to the underlying integer; (3) the reverse map for decode
  and error reporting. `magic_enum` should make the name<->int mapping cheap and avoids the string-table
  dance that Type axes need (there the value *is* a string index; an enum's value stays an int and only
  its *name* is a string). Until this lands, an Integral value on an Enum axis is accepted by number
  only (see `ManifestQueryBuilder::where`, which already treats Integral as valid for an Enum axis), and
  `ValueNotInAxis` on an enum carries no name suggestion because there are no names to suggest yet.
- Policy file target keys are flat (`targets.wgsl`, later `targets.dxil`, `targets.spirv`). A platform split is coming: one target profile likely needs several device presets under it, e.g. `minspec`, `recommended`, `mobile`. A preset picks a different `MaxVariants`, `CookValues`, and binding-time lowering. Decide whether a preset nests under a target (`targets.wgsl.mobile`) or forms its own axis in the policy schema. Do this after E5 lands the flat form, so the schema change has a working baseline to move from.
- Let a size expression name a *derived* constant, not only an axis or an `extern static const`. Today the resolve namespace (`MakeResolveContext`) holds axis values plus captured undriven `extern` defaults, and nothing else. An `internal static const` that is computed from axes (e.g. `VTF_CLUSTER_COUNT = GRID_X * GRID_Y * GRID_Z`) is invisible, so `[ls_element_count("VTF_CLUSTER_COUNT")]` fails with `AttributeExpressionUnknownSymbol`. The author must inline the product or promote the constant to `extern`, which is a usability wart: the shader already states the relationship once, and we make them state it again.
  - Approach: capture each module-scope `const` name together with its defining *expression string* (not a folded value) during the same source scan that reads the extern defaults. The `SymbolTable` already tokenizes every source line (it absorbed the old `ExternConstantScanner`), so it is the natural place to grab these too.
  - Add them to the evaluator as derived symbols. When a name resolves to a derived symbol, evaluate its stored expression recursively against the same context, so the leaves bottom out at axes and extern defaults and the value tracks the per-variant axis values. A folded value captured from reflection would be wrong: it freezes at the declaration defaults and ignores the axes.
  - Guard against a cycle in the derived-symbol graph (a derived const that names another), and cache a name's evaluated result per variant so a diamond is not recomputed.
# Cook driver and manifest
- Clean up cookerdriver. This is getting a bit ridiculous: it's a hugely complex.cpp, all the anonymous namespace functions are declared
  and defined together, it could all be condensed considerably. Might be worth using the FSM approach we tried in VeloxRhi, where we use
  a variant of discrete states and step through them. Then each state and it's functionality could go in a file, and it would help
  make control flow more clear.
- Store axis values in the manifest as `uint32`, not `int64`. The `AxisValues` table is `int64` today
  only because the emitter fills it through `PermutationValueToInt64` (`PermutationValue.hpp`), a helper
  that exists to widen a value to the **size-expression evaluator's** working type. The evaluator does
  signed 64-bit arithmetic and should keep `int64`; the manifest has no such need. Every axis value is
  `uint32` at the source (`PermutationValue::uintValue`, interface ordinals, and Type-axis string
  indices are all `uint32`), and every consumer already casts back down to `uint32` (`decodeAxis`,
  `integralValueIndices`, the view accessors), so the `int64` wastes half the table and adds a lossy
  downcast. Fix: emit `value.AsUInt()` / the Type string index directly instead of routing through the
  evaluator's widener.
  - Touch points: `AppendLiteralValues`/`AppendStringValues` + `AxisTables::Values` (`int64`->`uint32`)
    in `ShaderManifestEmitter.cpp`; the `AxisValues` section record size in `ValidateTablesInRange`; the
    view accessors `AxisValues`/`AxisValue`/`AllAxesValues` (`int64`->`uint32`); `ValidateAxes` (its Type
    string-index check drops the signed `cmp_greater_equal`); the downcasts in `ManifestIndex`. Bump
    `k_ShaderManifestVersion` (format change; no clients yet, so free to do now).
  - Signedness: unsigned is correct, because `PermutationValue` is unsigned (`UInt`). A signed axis
    value (a bias/offset tuning knob) would be a `PermutationValue` model change first; the manifest
    width follows from there, not the other way around.
  - Do NOT wrap it in a strong enum the way `VariantKey` is. `VariantKey` earned its enum by
    disambiguating two identically-typed roles (a dense index vs a packed key). An axis raw value's
    meaning is `Domain`-dependent (a literal for Bool/Integral/Enum, a string index for Type), which a
    single enum can't encode: a reader must consult `Domain` regardless, so a wrapper prevents no real
    bug and only adds `to_underlying` ceremony at every use. Keep it a plain `uint32`.
- Strong-type the variant key so an index can never be passed where a key is expected. `VariantKey` is
  `using VariantKey = uint64_t` today, an alias with no type safety. That let `ManifestShaderSourceProvider::Source/Bindings/Workgroup`
  forward a dense `variant_index` straight into `ShaderManifestView::FindSlot`, which interprets its
  second argument as a packed key (`lower_bound` + exact match on `variantKeys`). A small ordinal never
  equals a packed key, so `FindSlot` returned `nullptr` for every variant and `Source` returned empty
  text. Make `VariantKey` a distinct type (`enum class VariantKey : uint64_t` is zero-overhead, keeps
  `<`/`==` for `lower_bound`, and stays a valid manifest record); consider a distinct `VariantIndex`
  too, so neither direction can be confused. The packing site (`ComputeVariantKey`) is the one place
  allowed to mint a key. This change makes the current bug a compile error.
- Runtime variant retrieval is by key, and the key comes from desired axis values. A renderer asks for
  "the variant with these axis values" (capabilities, tuning set), not "variant number 37". So the
  path is: desired axis values -> canonical assignment -> `VariantKey` -> `FindSlot`. The dense index
  stays an internal storage rank and leaves the public API. For this to work the manifest must carry
  the axis schema (axis names, each value's ordinal, the packing order), and the library should offer a
  `KeyFor(desired axis values)` that builds and validates the key, so the cooker and the runtime share
  one packing algorithm and cannot drift.
- `VerifyManifestRoundTrip` was not running. `EmitLibraryModules` (which emits each manifest and runs
  the round trip) had no caller, so the manifest validator was silently dead, against rule 3 of
  `CLAUDE.md` ("the round trip checks are never optional"). It is wired back in now. Add a guard that
  fails loudly if a cook emits no manifest, so this validator cannot be orphaned again without a test
  going red.
- Verify that the cook actually wrote the files it claims. Part of this bug hid behind stale artifacts
  on disk, not missing ones: a reader saw an old file and thought the cook succeeded. After a cook,
  check with `std::filesystem::last_write_time` that each expected artifact is newer than the cook's
  start time, and that every expected artifact exists. `check-known-good.py` should also refuse to
  compare an artifact whose timestamp predates the cook it just ran.
- Own cook-scoped resources in a `CookContext`/`CookSession`. The `PermutationSpace` was a local in
  `CookModule`, but the interned module and then the cooked module hold pointers into it, so the
  pointers dangled the moment `CookModule` returned. The temporary fix owns the space in a `unique_ptr`
  reset between cooks. The real fix is an object that owns the per-cook resources (symbol table,
  permutation space, ...) for the span of one cook, with the pointer holders scoped to its lifetime.
  `CookerDriver` likely becomes `CookSession` in the process.
- Tear out the dead primary-content write. `first.GetContent()` / `sink.Write(...)` is the old
  compiled-in header path; the C++ emitter is gone, so the primary content is empty and the `-o` target
  is really an output directory that `FileOutputSink` still treats as a file name. Reconcile the sink
  around a directory target, and remove the empty primary write.
- Make `RunCookOnce` and the determinism path emit through one code path. The determinism branch
  replays memory artifacts to the real sink with its own `sink.Write`/`WriteArtifact` loop instead of
  the shared emit, so the two paths can diverge (they did). Route both through the same emit.
# Error reporting
- Route more errors through the diagnostic sink, especially all of the `std::println` statements. Those would just be plain better handled by diagostic sinks
- `CookResult<T>`'s error type could be refactored to effectively be a `Diagnostic` entry, meaning that failures can be passed up the chain... but would also complicate having something like `MaxErrorCount`. Evaluate this space to see if it could potentially clean up the code, and help create more unified (but still expressive) error reporting
- Better error reporting out of the attribute expression evaluation system
# Found in review
- SlangCompiler.cpp, Line 1234: We extract the raw global bindings not once, but individually for each variant. We should be able to do this at a higher level, even if this specific variant doesn't actually use all of the entrypoints. We will need to identify further axes for data reuse like this to scale to much higher variant counts without terrible performance.