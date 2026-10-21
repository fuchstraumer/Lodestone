# Phase F preparation/edge case fixes
- Allow declaration of presets in the policy TOML and cook them into the manifest. Data is easier to iterate on than compiled code. Currently we build these as objects/value types at the client level, inside compiled client code.
- Decide the persisted form of a variant. `DecodedVariant` holds string views into one manifest and
  no axis names, so it cannot outlive that manifest. A persisted form needs (axis name, value) pairs
  with owned strings, and a load that re-keys them through a query.
# Resource Layer
- The WGSL cross-check does not compare a storage texture format. Slang `ac945e5` reflected `RWTexture3D<float>` (no `[format]`) as `Invalid` while the emitted WGSL said `r32float`, and nothing failed. Slang `e9fc02c8d` reflects `R32Float`. Compare the format against Tint's inspector, so the next disagreement fails the cook.
- Reflection joins a scope with `_` (`JoinScopeName` in `SlangReflector.cpp`), which is the WGSL
  spelling. SPIR-V uses `.`. Store `.`, and let each validator make its own form. `SpvValidator` compares
  the last name segment only until then.
- `SpvValidator` reads more than it compares. Pass the `CompiledEntryPoint` to the validator, then compare
  the LocalSize, the member layout (Offset, ArrayStride, MatrixStride, and the inverted matrix major), the
  vertex inputs, the color targets, and `WritesFragDepth`. Add a per-variant hook for the vertex-to-fragment
  link check.
- Record the capabilities that each entry point needs. Today no cook records them, because Slang warning
  41012 is off (phase F plan, O7). Read them from the target output (SPIR-V `OpCapability`). Collate them
  for each variant. `src/target/impl/SpvModuleReader` already parses each module and lists its
  `OpCapability` names (`ParseState::Capabilities`), so this can read them without the comparison.
- A query takes the device capability set and removes each variant that needs more. This filter is the
  base preset. Every other query builds on it.
- We should provide a way for clients to call something like `SetDeviceLimits` or `SetApiLimits` - we can use this to validate resource sizing expressions when being run as a live compiler,
  or we can use it against cooked content (in the device form) to make sure we don't try to create a shader a device can't support
# Resource Metadata
- In most shader markup languages, it's possible to specify default values, mins, maxes, etc for each uniform parameter expected to be used as a content-facing editable parameter. 
- Add this capability to our resource system, using the existing annotation system but expanded with the aforementioned features
- Add an additionally "clamp mode" parameter: artists often want to push values beyond the min max we specify, so we should use this loose clamping as default. Sometimes, going outside
  of the range could cause UB or break a shader though - so we want to have a `HardClamp` sort of attribute
- Lodestone doesn't take action on these, it should just pack them into the manifest. The most elegant solution may be a typeless attribute system of some kind, just a bundle of tagged bytes.
# Permutation system
- Policy file target keys are flat (`targets.wgsl`, later `targets.dxil`, `targets.spirv`). A platform split is coming: one target profile likely needs several device presets under it, e.g. `minspec`, `recommended`, `mobile`. A preset picks a different `MaxVariants`, `CookValues`, and binding-time lowering. Decide whether a preset nests under a target (`targets.wgsl.mobile`) or forms its own axis in the policy schema. Do this after E5 lands the flat form, so the schema change has a working baseline to move from.
- Let a module policy section inherit another target section, for example `inherit = "wgsl"`. Phase F
  step F1.5 makes a missing section fail the cook.
# Cook driver and manifest
- Add a guard that fails loudly if a cook emits no manifest, so this validator cannot be orphaned again
  without a test going red. (2026-09-24: `--target` is now required, which closes the zero-target
  case. A guard in the emit path is still open.)
- Verify that the cook actually wrote the files it claims. Part of a bug hid behind stale artifacts
  on disk, not missing ones: a reader saw an old file and thought the cook succeeded. After a cook,
  check with `std::filesystem::last_write_time` that each expected artifact is newer than the cook's
  start time, and that every expected artifact exists. `check-known-good.py` should also refuse to
  compare an artifact whose timestamp predates the cook it just ran.
- Tune the worker thread count against memory, and measure it. `ThreadPool::Initialize` uses
  `hardware_concurrency` and ignores `SlangCompilerCreateInfo::ExpectedBatchSize`. Nothing measures the
  count against anything.
    - Each worker has its own global session and its own copy of the core module (the
      `k_UseSlangWorkaround` path). Measured on 2026-10-12, KitchenSink, RelWithDebInfo, 16 workers: a
      one-target cook peaks at 1.62 GB of commit. `--single-threaded` peaks at 0.89 GB. That is about
      45 MB for each worker, before any variant work.
    - Slang does not check `malloc` (`source/core/slang-allocator.h`). Out of commit, a cook does not
      fail with an error. It crashes with 0xC0000005 (a null write in `List::reserve`), 0xC0000409
      (`bad_alloc` to `std::terminate`), or 0xC00000FD (a stack page that cannot commit). On 2026-10-12
      a PC with no page file and about 8 GB of free commit gave all three, and three parallel cooks
      crashed in most runs.
    - Threading across environments (several targets or modules at once) multiplies the sessions. Decide
      the count from a memory budget and the variant count of the batch, not from the core count alone.
      Measure peak commit and wall time against the count first: 1, 2, 4, 8, 16.
    - Consider one global session and core module shared by the workers, if Slang allows it. Measure
      the global-session convoy again first (handoff fault 9).
- Entry point constants in the merged layout. Slang puts the push constant block of every SPIR-V entry point at
  offset 0, so two stages of one pipeline that each have constants read the same bytes. `MergedLayoutView` must
  give one push constant range for a pipeline, and fail when two stages state different members at one offset.
  WGSL needs nothing: each stage's uniform buffer has its own binding.
- Make `RunCookOnce` and the determinism path emit through one code path. The determinism branch
  replays memory artifacts to the real sink with its own `sink.Write`/`WriteArtifact` loop instead of
  the shared emit, so the two paths can diverge (they did). Route both through the same emit.
# Error reporting
- `CookResult<T>`'s error type could be refactored to effectively be a `Diagnostic` entry, meaning that failures can be passed up the chain... but would also complicate having something like `MaxErrorCount`. Evaluate this space to see if it could potentially clean up the code, and help create more unified (but still expressive) error reporting
- Better error reporting out of the attribute expression evaluation system
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