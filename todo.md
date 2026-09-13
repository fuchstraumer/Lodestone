# SlangCompiler
- Right now, we map to a super small subset of formats and features. We should support the full range, and extract them untouched
    - Then, during output format mapping we collapse to what that platform
    actually supports
    - Alternatively, use foresight about output target to fail builds if unsupported
# Resource Layer
- We should provide a way for clients to call something like `SetDeviceLimits` or `SetApiLimits` - we can use this to validate resource sizing expressions when being run as a live compiler,
  or we can use it against cooked content (in the device form) to make sure we don't try to create a shader a device can't support
# Permutation system
- Policy file target keys are flat (`targets.wgsl`, later `targets.dxil`, `targets.spirv`). A platform split is coming: one target profile likely needs several device presets under it, e.g. `minspec`, `recommended`, `mobile`. A preset picks a different `MaxVariants`, `CookValues`, and binding-time lowering. Decide whether a preset nests under a target (`targets.wgsl.mobile`) or forms its own axis in the policy schema. Do this after E5 lands the flat form, so the schema change has a working baseline to move from.
- Let a size expression name a *derived* constant, not only an axis or an `extern static const`. Today the resolve namespace (`MakeResolveContext`) holds axis values plus captured undriven `extern` defaults, and nothing else. An `internal static const` that is computed from axes (e.g. `VTF_CLUSTER_COUNT = GRID_X * GRID_Y * GRID_Z`) is invisible, so `[ls_element_count("VTF_CLUSTER_COUNT")]` fails with `AttributeExpressionUnknownSymbol`. The author must inline the product or promote the constant to `extern`, which is a usability wart: the shader already states the relationship once, and we make them state it again.
  - Approach: capture each module-scope `const` name together with its defining *expression string* (not a folded value) during the same source scan that reads the extern defaults. The E7 loose end that folds `ExternConstantScanner` into `SymbolTable` is the natural place to grab these too, since the tokenizer already visits every line.
  - Add them to the evaluator as derived symbols. When a name resolves to a derived symbol, evaluate its stored expression recursively against the same context, so the leaves bottom out at axes and extern defaults and the value tracks the per-variant axis values. A folded value captured from reflection would be wrong: it freezes at the declaration defaults and ignores the axes.
  - Guard against a cycle in the derived-symbol graph (a derived const that names another), and cache a name's evaluated result per variant so a diamond is not recomputed.
# Error reporting
- Route more errors through the diagnostic sink, especially all of the `std::println` statements. Those would just be plain better handled by diagostic sinks
- `CookResult<T>`'s error type could be refactored to effectively be a `Diagnostic` entry, meaning that failures can be passed up the chain... but would also complicate having something like `MaxErrorCount`. Evaluate this space to see if it could potentially clean up the code, and help create more unified (but still expressive) error reporting
- Better error reporting out of the attribute expression evaluation system
# Found in review
- SlangCompiler.cpp, Line 1234: We extract the raw global bindings not once, but individually for each variant. We should be able to do this at a higher level, even if this specific variant doesn't actually use all of the entrypoints. We will need to identify further axes for data reuse like this to scale to much higher variant counts without terrible performance.