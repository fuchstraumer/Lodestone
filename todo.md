# Phase F preparation/edge case fixes
- add query presets, which are module-agnostic
- make the query system very clearly single-module-only. it can reuse a preset across modules, though
- make it clear that variant keys are per-session use only. to save or persist a variant, save something 
  like `DecodedVariant` from the index, which can be expanded back into a valid variant key even if
  the manifest changed underneath us (invalidating previous keys)
    - a load of persisted variants will then need to first decode it's `Values` vector, to compute
      the key, and see if it matches. The `Key` field is somewhat redundant, I suppose, at least in
      persisted data
- Decide the persisted form of a variant. `DecodedVariant` holds string views into one manifest and
  no axis names, so it cannot outlive that manifest. A persisted form needs (axis name, value) pairs
  with owned strings, and a load that re-keys them through a query.
- Done 2026-09-24: `AxisNames()` and `AxisValues(axis)` on the index. `QueryFor(profile)` is obsolete,
  because an index now reads one environment (one module, one profile).
# SlangCompiler
- Right now, we map to a super small subset of formats and features. We should support the full range, and extract them untouched
    - Then, during output format mapping we collapse to what that platform
    actually supports
    - Alternatively, use foresight about output target to fail builds if unsupported
# Resource Layer
- Record the capabilities that each entry point needs. Today no cook records them, because Slang warning
  41012 is off (phase F plan, O7). Read them from the target output (SPIR-V `OpCapability`). Collate them
  for each variant.
- A query takes the device capability set and removes each variant that needs more. This filter is the
  base preset. Every other query builds on it.
- We should provide a way for clients to call something like `SetDeviceLimits` or `SetApiLimits` - we can use this to validate resource sizing expressions when being run as a live compiler,
  or we can use it against cooked content (in the device form) to make sure we don't try to create a shader a device can't support
# Permutation system
- Policy file target keys are flat (`targets.wgsl`, later `targets.dxil`, `targets.spirv`). A platform split is coming: one target profile likely needs several device presets under it, e.g. `minspec`, `recommended`, `mobile`. A preset picks a different `MaxVariants`, `CookValues`, and binding-time lowering. Decide whether a preset nests under a target (`targets.wgsl.mobile`) or forms its own axis in the policy schema. Do this after E5 lands the flat form, so the schema change has a working baseline to move from.
- Let a size expression name a *derived* constant, not only an axis or an `extern static const`. Today the resolve namespace (`MakeResolveContext`) holds axis values plus captured undriven `extern` defaults, and nothing else. An `internal static const` that is computed from axes (e.g. `VTF_CLUSTER_COUNT = GRID_X * GRID_Y * GRID_Z`) is invisible, so `[ls_element_count("VTF_CLUSTER_COUNT")]` fails with `AttributeExpressionUnknownSymbol`. The author must inline the product or promote the constant to `extern`, which is a usability wart: the shader already states the relationship once, and we make them state it again.
  - Approach: capture each module-scope `const` name together with its defining *expression string* (not a folded value) during the same source scan that reads the extern defaults. The `SymbolTable` already tokenizes every source line (it absorbed the old `ExternConstantScanner`), so it is the natural place to grab these too.
  - Add them to the evaluator as derived symbols. When a name resolves to a derived symbol, evaluate its stored expression recursively against the same context, so the leaves bottom out at axes and extern defaults and the value tracks the per-variant axis values. A folded value captured from reflection would be wrong: it freezes at the declaration defaults and ignores the axes.
  - Guard against a cycle in the derived-symbol graph (a derived const that names another), and cache a name's evaluated result per variant so a diamond is not recomputed.
- Let a module policy section inherit another target section, for example `inherit = "wgsl"`. Phase F
  step F1.5 makes a missing section fail the cook.
# Cook driver and manifest
- Resolve a synthetic module's required import statements for type and enum axes as the declaring
  module for the type being used. Currently `RawAxisDeclaration::RootModule` holds that, but it's
  just captured as where the variable was found during the bootstrap (not where type is defined).
    - We can find this by reflecting the type as a DeclReflection, then walking that chain up
      until we hit the declaring module.
    - This should be what both enum and interface axes use for finding the definitions.
- Clean up cookerdriver. This is getting a bit ridiculous: it's a hugely complex.cpp, all the anonymous
  namespace functions are declared and defined together, it could all be condensed considerably. Might be
  worth using the FSM approach we tried in VeloxRhi, where we use a variant of discrete states and step
  through them. Then each state and it's functionality could go in a file, and it would help make control
  flow more clear.
- Add a guard that fails loudly if a cook emits no manifest, so this validator cannot be orphaned again
  without a test going red. (2026-09-24: `--target` is now required, which closes the zero-target
  case. A guard in the emit path is still open.)
- Verify that the cook actually wrote the files it claims. Part of a bug hid behind stale artifacts
  on disk, not missing ones: a reader saw an old file and thought the cook succeeded. After a cook,
  check with `std::filesystem::last_write_time` that each expected artifact is newer than the cook's
  start time, and that every expected artifact exists. `check-known-good.py` should also refuse to
  compare an artifact whose timestamp predates the cook it just ran.
- Make `RunCookOnce` and the determinism path emit through one code path. The determinism branch
  replays memory artifacts to the real sink with its own `sink.Write`/`WriteArtifact` loop instead of
  the shared emit, so the two paths can diverge (they did). Route both through the same emit.
# Error reporting
- `CookResult<T>`'s error type could be refactored to effectively be a `Diagnostic` entry, meaning that failures can be passed up the chain... but would also complicate having something like `MaxErrorCount`. Evaluate this space to see if it could potentially clean up the code, and help create more unified (but still expressive) error reporting
- Better error reporting out of the attribute expression evaluation system
# Found in review
- SlangCompiler.cpp, Line 1234: We extract the raw global bindings not once, but individually for each variant. We should be able to do this at a higher level, even if this specific variant doesn't actually use all of the entrypoints. We will need to identify further axes for data reuse like this to scale to much higher variant counts without terrible performance.
# Testing
- Done: the KitchenSink set (`tests/assets/KitchenSink/`) replaced OceanFft, and it is the known-good
  reference. The original item follows.
- Build a purpose-made "kitchen-sink" test shader that exercises the whole capability space in one
  module, and retire the borrowed content shaders (`OceanFft`, `Vtf`) from the test tree. Those are
  real content meant for other projects, and each capability now needs its own asset (a separate shader
  just for enum axes, another for interface axes). One synthetic module should cross an interface axis,
  an enum axis, a boolean axis, and a tuning axis, with an `ActiveWhen` gate and a `Require` constraint,
  so the cook tests exercise the axis interactions the single-purpose assets cannot. It should carry a
  size expression that names an axis, so the resolve path is covered too. Do this when the query and
  cook code is otherwise finished; it is a test-asset consolidation, not a blocker.
## Testing findings to resolve
- Done 2026-09-24: the stale `CookTest` script line is retired. `OceanFft.slang` is gone, and
  `KitchenSinkCookTest` is the end-to-end coverage.
- Done (verified 2026-09-24): `HashReflectedBinding` now hashes each member's whole `Data`, so the item
  below is closed.
- `HashReflectedBinding` (`src/model/ShaderDataSchema.cpp`) hashes each member's `Offset`, `Size`, and
  `ArrayCount`, but not `ElementStride` or `MatrixLayout`. Dedup stays correct, because
  `ReflectedUniformMember::operator==` includes both and the interner decides equality by byte
  comparison, not the hash. The cost is extra bucket collisions for structured buffers that differ only
  in layout, plus a landmine if the hash is ever treated as equality. Close it or leave it, but know it.
# Phase F: portable geometry (vertex/index pulling)
Design explored 2026-09-22. This is Phase F lowering work, not scheduled yet. The goal is a shader
that reads vertices and indices the same way whether the target binds a vertex buffer through the
input assembler or pulls from a storage buffer, so one shader is truly portable across the fixed
pipeline and a GPU-driven one.
- Add `IVertexSource` and `IIndexSource` as library builtins, in a new `.slang` file beside
  `LodestoneAttributes.slang`. The reflector reads them by name, the same way it reads the attributes.
- Keep the interface stage-agnostic: `Vertex load(uint index)` and `uint load(uint i)`. The caller
  supplies the index (the input assembler / `SV_VertexID` in a vertex shader, a thread-computed index
  in compute). Do NOT bake `SV_VertexID` into the interface, or it cannot run in compute. This is the
  factoring that lets the same mesh-decode code run in a vertex shader and a compute geometry pass, so
  it is the enabler for a Nanite-like software-raster / meshlet system (the access floor, not the
  system).
- Realize the choice by selection, not transformation. Vertex sourcing is a technique axis backed by
  link-time specialization (interface conformances), which Slang already composes for us
  (`IComponentType::specialize` / `link`, `ITypeConformance`). Slang exposes composition, not AST
  mutation: there is no public AST-rewrite API, so text substitution and AST surgery are both off the
  table. This is the same mechanism the interface axes already use.
- Add an attribute (e.g. `ls_vertex_attribute`) that marks the pullable fields. One declaration, two
  consumers: the bound arm emits the input-assembler layout, the pulled arm synthesizes the buffer's
  element layout. Both reflection substrates already exist (`ReflectedVertexInput`, and the
  structured-buffer member walk with stride and matrix layout).
- Derisk first: the bound (IA) arm needs the varying inputs on the entry point, because Slang ties
  varyings to entry-point parameters, not to an interface implementation. So the bound arm needs either
  a generated thin wrapper entry point (built from the marked struct) or a generic entry-point template
  the body plugs into. Prototype this seam on a trivial mesh, both arms, before baking
  `ls_vertex_attribute` into the manifest schema. Everything else here is cheap or proven; this is the
  one unknown.
- Do not require the interface globally. Make it an opt-in portability capability/tier. Inside the tier
  the interface is mandatory and raw vertex access cannot be written down; outside it, raw varyings stay
  legal. This keeps the escape hatch and does not tax simple raster shaders.
- The enforcement is an ingestion-surface contract check ("does this pull-capable shader obey the
  contract?"), not one of the four cross-check validators. It is checkable on the reflected entry-point
  inputs: a pull-capable entry point must not declare a raw vertex-semantic varying that did not come
  from the builtin. Slang gives the interface and the conformance; it does not enforce "no raw
  varyings" — that policy is ours.
- The bound source is vertex-stage-only (there is no input assembler in compute), so it is a natural
  `ActiveWhen` gate (stage == vertex) that drops into the constraint engine. Compute geometry is always
  pulled.
- Index / EBO pulling is the same pattern one level up (`IIndexSource`). It interacts with indirect draw
  arguments and with base-vertex / base-instance. Note Slang rebases `SV_VertexID` / `SV_InstanceID`
  (`gl_VertexIndex - gl_BaseVertex`, needs the `DrawParameters` capability on SPIR-V), so the pulled
  index keeps HLSL zero-based semantics across targets, but the base offset and that capability are a
  per-profile concern.
- This does not disturb the frozen client contract: the pulled arm is an ordinary structured-buffer
  binding, the bound arm is vertex inputs, and both are already modeled.
- The pulled buffer's layout (interleaved vs planar, AoS vs SoA) is device-sensitive, so by the phase F
  rule it is a Lodestone tuning knob. Drive it from policy later, once the selection machinery exists.