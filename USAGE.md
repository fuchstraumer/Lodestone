# USAGE.md

This file is for agents that write shaders for Lodestone, or that call the cooker. Read it before you write
a Slang module. It has three parts:

1. What Lodestone does, and how you use each feature.
2. The style rules for Slang code.
3. The rules for fast GPU code.

This file uses ASD-STE100 (Simplified Technical English). `CLAUDE.md` and `.github/copilot-instructions.md`
contain the rules for C++ code in this repository. This file does not repeat them.

---

## Part 1: Lodestone

### 1.1 What Lodestone is

Lodestone is a shader cooker. It reads Slang modules, and it writes one binary manifest for a renderer.

- A shader author declares **permutation axes** in the shader.
- The cooker compiles one **variant** for each combination of axis values.
- The cooker removes duplicate code and duplicate layouts.
- The cooker writes every variant, its code, and its resource layout into `ShaderLibrary.ldmanifest`.
- At run time, the renderer finds a variant by axis names and values. It does no shader reflection.

The renderer does not compile shaders. The renderer does not guess bindings. The manifest tells the renderer
every binding, every size, and every capability that a variant needs.

### 1.2 The three facts that agents most often miss

1. **Permutation axes exist.** Do not write `#define` switches, and do not write two copies of a shader. Declare
   an axis. The cooker makes each variant for you.
2. **The shader decides resource sizes.** Write the size of a buffer or a texture on the declaration, with an
   `ls_*` attribute. The renderer reads the size from the manifest. Do not put the size in C++ code.
3. **The cooker is a command line tool and a CMake function.** Section 1.9 shows both. Do not call Slang
   yourself.

### 1.3 The builtin modules

Two modules are compiled into the cooker. A cook needs no path to find them.

| Module | Import it when | What it contains |
|---|---|---|
| `LodestoneAttributes` | You use any `ls_*` attribute. | Every axis attribute and every size attribute. |
| `LodestoneShims` | You declare a shim resource (section 1.6). | The shim interfaces, their implementations, and `StorageFormat`. |

```slang
module MyModule;
import LodestoneAttributes;
import LodestoneShims;
```

### 1.4 Permutation axes

An axis is an `extern static const` (or an `extern struct`) with an `ls_axis_*` attribute. The value that you
give in the declaration is the default value. The cooker links a different value into each variant.

Slang folds each axis value at link time. Thus `if (USE_ENV)` costs nothing: the code of the false branch
does not get into the variant.

#### Axis types

| Type | Declaration | Values |
|---|---|---|
| Boolean | `[ls_axis_boolean] extern static const bool USE_ENV = false;` | `false`, `true` |
| Integer list | `[ls_axis_values("1, 4, 8, 16")] extern static const uint SAMPLE_COUNT = 1u;` | The listed values. |
| Enum | `[ls_axis_enum] extern static const QualityTier QUALITY = QualityTier::Low;` | Each case of the enum, by name. |
| Interface | `[ls_axis_interface] public extern struct BRDF : IBrdf = Lambert;` | Each type with `[ls_axis_interface_impl("IBrdf")]`. |

Rules for each type:

- **Enum axis.** The enum must be `public`. The cooker imports the module of the enum, and it writes the case
  name into the variant. The manifest stores case names, not case values.
- **Interface axis.** The interface and each implementation must be `public`. Put
  `[ls_axis_interface_impl("<InterfaceName>")]` on each implementation. Make an instance with `BRDF brdf = {};`, and
  call the interface methods on it.
- **Integer list.** Write the values as one string, with commas between them.

#### Axis kind

Put `[ls_axis_kind("<kind>")]` on each axis. The kind tells the cooker and the renderer what the axis means.

| Kind | Use it for | Example |
|---|---|---|
| `Capability` | A feature that some devices do not have. | Wave operations, `half` arithmetic. |
| `Tuning` | A size or a count. | Workgroup size, FFT size, sample count. |
| `Technique` | A different algorithm. | A BRDF, a filter type. |
| `ResourcePresence` | A resource that only some variants read. | An optional normal map. |

The kind name is case-insensitive. An unknown name fails the cook with `AxisKindUnknown`, and the error tells
the nearest kind. An axis with no `ls_axis_kind` has no kind. An interface axis is always `Technique`.

The cooker checks a `Capability` axis. Each value must change the capabilities that the code needs. If the
off path needs more than the on path, the cook fails.

#### Gated axes

An axis can depend on an earlier axis. Use `ls_axis_active_when` with an expression:

```slang
[ls_axis_boolean][ls_axis_kind("Capability")]
extern static const bool USE_WAVE_OPS = true;

[ls_axis_values("16, 32, 64")][ls_axis_kind("Tuning")][ls_axis_active_when("USE_WAVE_OPS == 1")]
extern static const uint WAVE_SIZE = 32u;
```

When `USE_WAVE_OPS` is false, `WAVE_SIZE` has no effect, and the cooker makes no extra variants for it. Use a
gate whenever one axis has no meaning without another axis. A gate removes variants that are identical.

#### Constants that are not axes

An `extern static const` with no `ls_axis_*` attribute is not an axis. The cooker reads its default value, and
a size expression can use that value.

```slang
extern static const uint IFFT_NUM_WAVE_CASCADES = 5u;
```

#### Derived constants

A module-scope `static const` can compute a value from axes. A size expression can use it.

```slang
static const uint CLUSTER_COUNT = GRID_X * GRID_Y;
```

Do not give a derived constant the name of an axis or of an extern constant. The cook fails if you do.

#### Unused axes

The cooker removes an axis that no reachable code uses. You can therefore put the axes in a shared file. A
module that does not use an axis does not get variants for it.

#### Cost of an axis

Each axis multiplies the variant count. A module with four boolean axes and one axis of three values has
2 × 2 × 2 × 2 × 3 = 48 variants. Before you add an axis, do these steps:

1. Make sure that the value cannot be a uniform. A uniform costs zero variants.
2. Make sure that the value changes the code. A value that only changes data belongs in a uniform.
3. Add a gate if the axis only has meaning when another axis has a specific value.
4. Add the axis to a policy file if only some values must ship (section 1.8).

Use an axis when the value changes a loop count, an array size, a workgroup size, a resource set, or a
capability. Use a uniform for everything else.

### 1.5 Resource attributes

Put these attributes on a resource declaration. Each argument is a string expression. The expression can
use axes, extern constants, and derived constants.

| Attribute | Put it on | Meaning |
|---|---|---|
| `ls_element_count("expr")` | A structured buffer. | The element count. The cooker multiplies it by the reflected stride. |
| `ls_extent_1d("w")` | A 1D texture. | The width, in texels. |
| `ls_extent_2d("w", "h")` | A 2D texture. | The width and the height. |
| `ls_extent_3d("w", "h", "d")` | A 3D texture or an array texture. | The width, the height, and the depth or the layer count. |
| `ls_resource_role("roles")` | A buffer. | Other uses: `IndirectDispatch`, `IndirectDraw`, `IndirectDrawIndexed`, `IndexData`, `VertexData`. |
| `ls_resource_tags("tags")` | Any resource. | Free tags for the renderer. Letters, digits, and `_.:-` only. |

Separate two or more roles or tags with spaces.

```slang
[ls_element_count("IFFT_SIZE * IFFT_SIZE * IFFT_NUM_WAVE_CASCADES")]
RWStructuredBuffer<half2> h0K;

[format("rgba16f")][ls_extent_3d("IFFT_SIZE", "IFFT_SIZE", "IFFT_NUM_WAVE_CASCADES")]
RWTexture2DArray<float4> Displacement;
```

The arguments are strings for a reason. Slang folds an attribute integer at compile time, but an axis
value folds at link time. Thus `[ls_element_count(IFFT_SIZE * 4)]` does not compile. Always write the
quotes.

Do not write a byte size. Write an element count. The cooker gets the stride from reflection, so the stride
cannot be wrong.

### 1.6 Shim resources

A shim resource is a resource that the **target profile** lowers. You write the shader one time. On one
target, the shim is an ordinary binding. On another target, the shim is a buffer device address or a
descriptor heap index. You do not change the shader.

#### The five shim interfaces

These are all of the shim interfaces. There are no others.

| Interface | Resource | Methods | Type argument |
|---|---|---|---|
| `IReadBuffer<T>` | A read-only structured buffer. | `T Load(uint index)` | Yes: the element type. |
| `IReadWriteBuffer<T>` | A read-write structured buffer. | `T Load(uint index)`, `void Store(uint index, T value)` | Yes: the element type. |
| `ITexture2DRef` | A `Texture2D<float4>`. | `Texture2D<float4> Get()` | No. |
| `ISamplerRef` | A `SamplerState`. | `SamplerState Get()` | No. |
| `IWriteStorageTexture2D` | A write-only 2D storage texture. | `void Store(uint2 coordinate, float4 value)` | No. The format comes from an axis. |

The shims do **not** support these resources. Use an ordinary Slang resource for them:

- 1D textures, 3D textures, cube textures, and array textures.
- Textures with an integer or `uint` sample type.
- Depth textures and comparison samplers.
- Storage textures that the shader reads, or that have an integer format.
- Byte address buffers, constant buffers, and acceleration structures.

#### How to declare a shim

Declare an `extern struct` that conforms to a shim interface. Then use the type as a parameter.

```slang
module ShimExample;
import LodestoneAttributes;
import LodestoneShims;

public struct Item
{
    public float4 value;
};

extern struct ItemBuffer : IReadBuffer<Item>;
extern struct ResultBuffer : IReadWriteBuffer<Item>;

ItemBuffer items;
ResultBuffer results;

[shader("compute")]
[numthreads(64, 1, 1)]
void MainCS(uint3 id : SV_DispatchThreadID)
{
    results.Store(id.x, items.Load(id.x));
}
```

Obey these rules. The cook fails if you do not.

- Put the shim in a global, in a `ParameterBlock` field, in a `ConstantBuffer` field, or in an entry point
  `uniform` parameter. Do not declare a shim as a local variable in a function.
- Make the type argument `public` in its module.
- Write the type argument as a plain name: `IReadBuffer<Item>`. Do not write a generic expression in it.
- Do not make an array of shims, if you cook for a `spirv-modern` profile.
- Do not put a shim inside the element of a storage buffer, if you cook for a `spirv-modern` profile.
- A shim is not an axis. Do not put an `ls_axis_*` attribute on it.

#### Storage texture shims

A storage texture shim needs a format. Declare an enum axis over `StorageFormat`, and name it with
`ls_storage_format` on the extern:

```slang
[ls_axis_enum]
extern static const StorageFormat LEVEL_FORMAT = StorageFormat::Rgba16Float;

[ls_storage_format("LEVEL_FORMAT")]
extern struct Destination : IWriteStorageTexture2D;
```

- The format axis must not have a gate (`ls_axis_active_when`).
- `StorageFormat` has six cases: `Rgba8Unorm`, `Rgba8Snorm`, `Rgba16Float`, `R32Float`, `Rg32Float`,
  `Rgba32Float`.
- `Store` always takes a `float4`. A format with fewer channels keeps the first channels.
- Use a policy file to cook a subset of the formats (section 1.8).

#### What each target does with a shim

| Target | Buffers | Textures | Samplers | Storage textures |
|---|---|---|---|---|
| `wgsl` | Bound | Bound | Bound | Bound |
| `spirv` | Bound | Bound | Bound | Bound |
| `spirv-modern-mutable` | Pointer | Handle | Handle | Handle |
| `spirv-modern-typed` | Pointer | Handle | Handle | Handle |

- **Bound**: an ordinary descriptor binding.
- **Pointer**: a buffer device address, stored as a value in a uniform block.
- **Handle**: an index into the descriptor heap in set 3, stored as a `uint2` in a uniform block.

The manifest tells the renderer where each value is stored. The renderer does not guess.

### 1.7 Entry point constants

Put small per-dispatch data on the entry point as `uniform` parameters. Do not make a constant buffer for
two floats.

```slang
[shader("compute")]
[numthreads(64, 1, 1)]
void LooseCS(uint3 threadId : SV_DispatchThreadID, uniform float4 tint, uniform uint count)
{
    Output[threadId.x] = tint * float(count);
}
```

- On `spirv` and the `spirv-modern` profiles, the cooker delivers these constants as push constants. The limit is 128 bytes.
- On `wgsl`, Slang puts them in a uniform buffer with the std140 layout.
- The two layouts can have different sizes. The manifest records each layout. Do not compute offsets in C++.

### 1.8 The policy file

A policy file is a TOML file. It limits what the cooker makes. The policy is optional. A module with no
entry cooks every variant.

```toml
# The axes that cannot change these entry points. The cook checks this claim.
[OceanFft.InertAxesForEntryPoints]
OceanTickCS = ["IFFT_USE_WAVE_OPS", "IFFT_WAVE_SIZE"]

# The limits for one target.
[OceanFft.targets.wgsl]
MaxVariants = 64

# Cook only these values of an axis, for this target.
[MipDownsample.targets.spirv.CookValues]
LEVEL_FORMAT = ["Rgba16Float", "R32Float"]
```

| Key | Type | Meaning |
|---|---|---|
| `<Module>.InertAxesForEntryPoints.<EntryPoint>` | Array of axis names. | The cook fails if a listed axis changes that entry point. |
| `<Module>.targets.<target>.MaxVariants` | Integer. | The cook fails if the module has more variants. |
| `<Module>.targets.<target>.CookValues.<AXIS>` | Array of booleans, integers, or enum case names. | Cook only these values. |
| `<Module>.targets.<target>.CookIf` | String expression. | Cook only the variants where the expression is true. |

Use these target names: `wgsl`, `spirv`, `spirv-modern-mutable`, `spirv-modern-typed`. An unknown target name fails the load.

If a module has a section for one target and no section for the cooked target, the cooker shows a warning.

### 1.9 How to run the cooker

#### From CMake (the usual method)

```cmake
lodestone_add_shader_cook(OceanShaders
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders
    TARGETS wgsl spirv
    MODULES ${SHADER_DIR}/OceanFft.slang ${SHADER_DIR}/OceanRender.slang
    INCLUDE_DIRS ${SHADER_DIR}/common
    POLICY_FILE ${SHADER_DIR}/OceanFftPolicy.toml
    OPTIONS --O2)
```

- The function runs the cook as a build step.
- It writes a depfile. An edit to any file that a module reads starts a new cook.
- The target property `LODESTONE_MANIFEST` gives the path of the manifest.
- An Emscripten build has no cooker. Set `LODESTONE_COOKER_EXECUTABLE` to a host cooker.

#### From the command line

The executable is `lodestone_cooker_console`.

```bash
lodestone_cooker_console -o out/shaders --target=wgsl --target=spirv --policy-file Ocean.toml --O2 OceanFft.slang
```

Three things are required: `-o`, at least one `--target=`, and at least one module path.

| Flag | Meaning |
|---|---|
| `--output`, `-o <dir>` | The output directory. |
| `--target=<name>` | `wgsl`, `spirv`, `spirv-modern-mutable`, or `spirv-modern-typed`. Repeat it for two or more targets. |
| `--include-dir`, `-I <dir>` | A folder to search for imported modules. Repeat it as necessary. |
| `--policy-file <path>` | The policy file. |
| `--O0` to `--O3` | The Slang optimization level. The default is 0. |
| `--g0` to `--g3` | The Slang debug information level. The default is 0. |
| `--cache-dir <dir>` | A folder for precompiled Slang modules. |
| `--depfile` | Write `ShaderLibrary.d`. |
| `--dump-stage=<name>` | Write one pipeline stage as JSON. |
| `--dump-sources` | Write each compiled source to a folder, for Tint, `spirv-dis`, or `spirv-val`. |
| `--quiet` | Do not write the reflection report for each variant. |
| `--no-validate` | Do not compare reflection against the emitted code. Use it only to debug. |
| `--no-dedupe` | Do not remove duplicates. This is the control case for a comparison. |
| `--verify-deterministic` | Cook two times, and compare the bytes. |
| `--single-threaded` | Compile on one thread. |

#### Where the cooker finds an imported module

The cooker searches these places, in this order:

1. The folder of the module.
2. The parent of that folder.
3. Each `--include-dir`, in the given order.
4. The module cache.

If an import fails, add an `--include-dir`. Do not depend on the module cache.

#### The output

The output directory contains:

- `ShaderLibrary.ldmanifest`: the manifest. The renderer reads only this file.
- `ShaderLibrary.dedupe.txt`: the duplicate report. Read it to find axes that cost variants and change nothing.
- `ShaderLibrary.resources.txt`: every resource, with its size.
- `ShaderLibrary.d`: the depfile, if you gave `--depfile`.

A failed cook removes the old manifest. It does not leave a file from an earlier cook.

#### What the cooker checks

The cooker compares its results against a second source. These checks are always on:

- It reads the emitted code back (with Tint or SPIRV-Tools), and it compares the bindings with reflection.
- It replays every variant through the deduplicated tables, and it compares the results.
- It reads the written manifest back, and it compares it with the model.

If the cook fails, read the error. Each error tells the file, the entry point, and the value. Do not use
`--no-validate` to make an error go away.

### 1.10 The renderer side

A renderer links only the client library. It includes `ShaderManifest.hpp` or `ShaderManifestIndex.hpp`.

1. Open the bytes with `BundleView::Open`.
2. Open one module for one target with `EnvironmentView::Open`.
3. Make a `ManifestIndex`, and call `Query()`.
4. Add constraints with `Where("AXIS", value)`. Use `WhereWhenActive` for an axis that has a gate.
5. Call `ForDevice(...)` to remove the variants that the device cannot run.
6. Call `Single()` for one variant, or `Keys()` for a set.
7. Get the code with `Source()` or `SpirvWords()`, and the bindings with `Bindings()`.

Do not save a `VariantKey` to disk. A key is only valid for one manifest. Save the axis names and values, and
make the key again after you load.

---

## Part 2: Slang code style

### 2.1 General rules

The C++ rules in `.github/copilot-instructions.md` also apply to Slang. The important ones for shaders are:

- Put each brace on its own line. Use braces on every `if`, even for one statement.
- Indent with 4 spaces. Do not use tabs. Keep lines at 110 columns or fewer.
- Use PascalCase for types, entry points, and public functions.
- Use camelCase for local variables. Use names that tell the meaning: `deltaX`, not `dX`.
- Use SCREAMING_SNAKE_CASE for axes and module constants: `IFFT_SIZE`, `USE_WAVE_OPS`.
- Use `s_` for `groupshared` arrays: `s_Radix4Data`.
- Put a blank line between blocks. Put a blank line before a `return` that follows a block.
- Make each intermediate value a `const` local. Write the math as a chain of named steps.
- Factor out code of more than 4 or 5 lines that occurs two times.

### 2.2 Names of entry points

- End a compute entry point name with `CS`: `OceanTickCS`.
- End a vertex entry point name with `VS`, and a fragment entry point name with `FS`: `OceanSurfaceVS`,
  `OceanSurfaceFS`.
- Write the dispatch size in a comment above each compute entry point:
  `// dispatch (IFFT_SIZE / 8, IFFT_SIZE / 8, 1)`.

### 2.3 Comments

- Write few comments. A good name is better than a comment.
- Write a comment when the code has a reason that a reader cannot see. Examples: a bank conflict, a precision
  limit, a barrier that prevents a race.
- Write a short history note when the old code was wrong: `// fix: the stride only handled powers of 2`.
  The note stops a regression.
- Write the math in the comment when the code makes it hard to see. `IfftRadix4.slang` is the model.
- Do not write a comment that tells what the next line does.
- Do not refer to files under `docs/`. That folder is not tracked.

### 2.4 Types and precision

- Write the type of every literal: `1.0f`, `0u`, `2u`. Do not mix `int` and `uint` without a cast.
- Use `uint` for indices and thread IDs. GPU code differs from the C++ rule here: the hardware IDs are
  unsigned.
- Use `half` for storage and bandwidth when the range allows it. Do the sensitive math in `float`. Examples
  of sensitive math: a phase that grows with time, and a `sincos` argument near 2π.
- `half` on WebGPU needs the `shader-f16` feature. The manifest records the `f16` capability for each variant
  that uses `half`, and `ForDevice` removes that variant on a device without the feature.

---

## Part 3: Slang modules and visibility

### 3.1 `module`, `__include`, `implementing`, and `import`

| Keyword | Meaning |
|---|---|
| `module X;` | This file is the primary file of module `X`. Other modules can import it. |
| `__include Y;` | Put file `Y` into this module. All files of one module see all `internal` symbols. |
| `implementing X;` | This file is part of module `X`. It must be the first line of each included file. |
| `import X;` | Use the `public` symbols of module `X`. |

Use these rules:

- Use `import` across module boundaries. Use `__include` to split one module into files.
- Do not use `#include`. The preprocessor state does not cross an `__include` or an `import`.
- Each `__include` adds a file exactly one time. Circular `__include` is permitted.
- You can only `import` the primary file of a module. You cannot `import` a file that starts with `implementing`.
- Slang loads an imported module one time. You need no include guards.

The OceanFft module in DiamondDogs is the model. `OceanFft.slang` contains only the `module` line and the
`__include` lines. Each other file starts with `implementing OceanFft;`.

```slang
// OceanFft.slang
module OceanFft;

__include OceanFftDims;
__include OceanFftBindings;
__include OceanFftCommon;
__include IfftRadix4;
__include OceanFftEntryPoints;
```

```slang
// OceanFftDims.slang
implementing OceanFft;

[ls_axis_values("128, 256, 512")][ls_axis_kind("Tuning")]
extern static const uint IFFT_SIZE = 512u;

internal const static uint IFFT_LOG2_SIZE = firstbithigh(IFFT_SIZE);
```

### 3.2 How to divide a module

- Put the axes in one file (`...Dims.slang` or `...Config.slang`).
- Put the resources in one file (`...Bindings.slang`). All entry points then use the same names.
- Put the helper math in one file (`...Common.slang`).
- Put each algorithm in its own file.
- Put the entry points in one file, last. Keep each entry point short: it maps the threads and calls helpers.
- Put types and enums that other modules use in a separate module. Make them `public`, and `import` that module.

The cooker shares an axis between modules when the name, the kind, and the values agree. An enum or an
interface in a shared module gives two modules one meaning for one axis.

### 3.3 Visibility: `public`, `internal`, `private`

| Modifier | Visible to |
|---|---|
| `public` | All code, in every module. |
| `internal` | All files of the same module. This is the default. |
| `private` | Only the members of the same type, and its extensions. |

Use these rules:

- **Make a symbol `public` only when another module uses it.** An enum for an enum axis, an interface for an
  interface axis, and each of its implementations must be `public`. A shim type argument must be `public`.
- **Mark helpers `internal` explicitly.** The default is `internal`, but the word tells the reader your
  intent. DiamondDogs writes `internal` on every helper function, every helper struct, and every module constant.
- **Mark `groupshared` arrays `internal`.** A `groupshared` name has module scope, so it must be unique in
  the whole module.
- **Use `private` for struct members that only the struct methods use.** You cannot make a type `private`.
- **Do not make a public function that exposes an internal type.** Slang rejects it.
- An interface requirement cannot be `private`.
- A file with no `module` line, no `__include`, and no modifier is a legacy module, and all of its symbols are
  `public`. Always write a `module` line.

---

## Part 4: Fast GPU code

Read this part before you write a compute kernel. The rules come from real profiles. Most GPU kernels wait on
memory, not on math. Write each kernel for the memory system first.

### 4.1 Think about the whole frame

1. **Count the bytes.** For each pass, add the bytes that it reads and the bytes that it writes. That number is
   usually the cost of the pass.
2. **Fuse passes that share data.** When pass B reads what pass A wrote, and the data fits in a workgroup, do
   both passes in one kernel. The data then stays in registers or in groupshared memory.
3. **Compute instead of load when the math is cheap.** A few ALU operations cost less than a round trip to
   memory.
4. **Remove dispatches.** Each dispatch has a fixed cost and a barrier after it. Fewer, larger dispatches are
   faster.

The ocean code in DiamondDogs does all four. `OceanTickRowsCS` computes the spectrum for the row pass in the
kernel. It does not read the buffer that `OceanTickCS` wrote. `OceanColumnsResolveCS` resolves from groupshared
memory after the column pass. It does not write `IfftInput` and read it again. The four dispatches of the
unfused path (tick, rows, columns, resolve) become two.

### 4.2 Memory access

- **Coalesce global access.** Adjacent lanes must touch adjacent addresses. Check each `[index]` expression:
  `threadId.x` must be the fastest index.
- **Fill each 32-byte sector.** If each lane uses 8 bytes of a sector, you waste 75% of the bandwidth. Put more
  lanes, or more data per lane, on one sector.
- **Change the thread map, not the data layout.** `Radix4MapThread` puts four neighbor columns on four neighbor
  lanes for the column pass. Each run of four lanes then reads one full sector.
- **Do scattered reads from global memory, not from groupshared memory.** The L1 cache absorbs a scattered read
  of a small row. A scatter in groupshared memory makes bank conflicts. `Radix4LoadInput` does the digit
  reversal on the global read for this reason.
- **Pack data.** Store two complex `half` values in one `half4`. Store two real fields as one complex value. The
  ocean tick packs eight real fields into two `half4` slices.
- **Load wide.** One `float4` load is better than four `float` loads.

### 4.3 Groupshared memory (LDS)

- **Know the bank layout.** Most hardware has 32 banks of 4 bytes. Two lanes of one wave that touch the same bank
  at different addresses wait for each other.
- **Write the conflict in a comment when you accept one.** Example: "this store is 4-way bank conflicted".
  The next reader then knows that it is a known cost.
- **Interleave to remove conflicts.** For a column pass, store `element * slotCount + slot`, not
  `slot * size + element`.
- **Work in place.** Give each thread a set of slots that no other thread touches in that stage. You then need
  one buffer and one barrier for each stage, not two buffers.
- **Size the array with an axis or a constant.** Slang allocates groupshared memory for each entry point that
  uses it. Give a fused kernel its own array, so that the other kernels keep a small allocation.
- **Groupshared memory limits occupancy.** Large arrays let fewer workgroups run on one core. Use only what
  you need.

### 4.4 Barriers

Each `GroupMemoryBarrierWithGroupSync()` stops every wave of the group. Remove barriers when you can.

1. Do the first stage of the work in registers. It needs no barrier.
2. Do the next stage with wave operations. They need no barrier.
3. Use groupshared memory and barriers only for data that crosses waves.

`Radix4TransformLine` follows this order. Stage 0 runs in registers. Stage 1 runs on wave shuffles. Only the
later stages use groupshared memory.

- **Put a barrier between a read and a write of the same groupshared slot by different threads.** If you do not,
  you get a race that occurs only on some hardware.
- **Do not put a barrier inside a branch that some threads skip.** WGSL rejects it, and it is undefined on
  other targets. Put the condition inside the loop, around the memory access.

### 4.5 Wave (subgroup) operations

Use wave operations whenever data moves between lanes of one wave. They are faster than groupshared memory, and
they need no barrier.

| Operation | Use it for |
|---|---|
| `WaveActiveSum`, `WaveActiveMin`, `WaveActiveMax` | A reduction across the wave. |
| `WavePrefixSum`, `WavePrefixCountBits` | A scan, a compaction, or a list append. |
| `WaveActiveBallot`, `WaveActiveCountBits` | A vote, or a count of lanes. |
| `WaveReadLaneAt`, `WaveReadLaneFirst` | A shuffle or a broadcast. |
| `WaveIsFirstLane` | One write for each wave. |

Do these steps for a reduction over a workgroup:

1. Reduce each wave with one `WaveActive*` operation.
2. Write one value for each wave to groupshared memory, from the first lane.
3. Use one barrier.
4. Reduce the small set of wave results.

Rules:

- **Put wave operations behind a `Capability` axis.** WebGPU subgroups are an optional feature. The off path
  must work without them. Give the off path a groupshared memory fallback.
- **Use a wave size axis to fold constants.** With `IFFT_WAVE_SIZE` as an axis, `lane & (IFFT_WAVE_SIZE - 1u)`
  and the stage selection fold at link time. Gate the wave size axis on the capability axis.
- **Use `WaveGetLaneCount()` when the value only selects work at run time.** Use the axis only when the size
  sets an array size, a loop count, or a branch that must fold.
- **Send only the data that the partner needs.** `Radix4ExchangePairs` sends two values, not four. Each lane
  sends the pair that its partner wants.
- **Use XOR lane patterns.** `lane ^ stride` makes a butterfly exchange. Two such exchanges transpose a 4×4
  tile across four lanes.
- **Keep the lanes of a wave in the same branch.** Wave operations in divergent code see only the active lanes.
  Make sure that this is what you want.

### 4.6 Quad operations

A quad is four neighbor lanes. In a fragment shader, a quad is a 2×2 block of pixels. Quad operations are also
available in compute shaders. WGSL supports them through the `subgroups` feature.

| Operation | Effect |
|---|---|
| `QuadReadAcrossX(v)` | Read the value of the horizontal neighbor. |
| `QuadReadAcrossY(v)` | Read the value of the vertical neighbor. |
| `QuadReadAcrossDiagonal(v)` | Read the value of the diagonal neighbor. |
| `QuadReadLaneAt(v, i)` | Read the value of quad lane `i`. |

Use quad operations for these tasks:

- **Share texture fetches.** Each pixel of a 2×2 block fetches one sample, and the quad shares the four samples.
  This gives a 2×2 filter for the cost of one fetch.
- **Compute derivatives by hand.** Use the difference to the neighbor when `ddx` and `ddy` are not available, or
  when the value is not a texture coordinate.
- **Reduce over 2×2 blocks.** A mip downsample or a depth reduction does one quad reduction for each output texel.
- **Do 2×2 work in compute.** Map a compute thread group so that each quad is a 2×2 tile, and then use the same
  operations.

Put quad operations behind the same `Capability` axis as the wave operations.

### 4.7 Registers and occupancy

- **Keep the per-thread state small.** Many live registers let fewer waves run, and the GPU then cannot hide
  memory latency.
- **Give each thread a full unit of work.** In `IfftRadix4.slang`, one thread does one whole radix-4 butterfly.
  The first version gave one element to each thread, so four threads computed each butterfly and kept one
  output each.
- **Unroll short loops with constant counts.** Use `[unroll]` on a loop of four. Do not unroll a loop with a
  count that comes from a buffer.
- **Do not index an array of locals with a dynamic index.** The compiler moves the array to memory. Use a
  constant index, or use groupshared memory.
- **Pass constant arguments to helpers.** When each caller passes a constant, the branch inside the helper folds.
  `Radix4LdsRead` selects one of two arrays this way, and only one array stays in each kernel.

### 4.8 Math

- **Use identities to remove transcendentals.** `Radix4Twiddles` does one `sincos`, and gets w² and w³ from the
  double-angle and triple-angle formulas.
- **Know which multiplies are free.** A multiply by `i` in complex math is a swizzle and a sign change. The
  4-point DFT needs only adds and swizzles.
- **Scale to keep `half` in range.** The ocean tick multiplies by `IFFT_SIZE` before the IFFT, and the resolve
  multiplies by the other `IFFT_SIZE`. Small heights then do not become denormals.
- **Use `sincos`, not `sin` and `cos`.** One instruction gives both on most hardware.
- **Use `firstbithigh`, `reversebits`, and bit masks for power-of-two math.** Do not use `log2` or `pow` on
  integers.
- **Prefer a multiply by a reciprocal constant to a divide.** Write `x * (1.0f / n)` when `n` is a constant.

### 4.9 Branches

- **A branch on an axis costs nothing.** Slang folds it at link time. Use an axis for a branch on a value that is
  constant for the whole dispatch and that changes the code a lot.
- **A branch on a uniform costs little.** All lanes take the same path.
- **A branch on per-thread data costs the sum of both paths** when lanes disagree. Use `select` or arithmetic
  for short paths. Use a branch for long paths that most lanes skip.
- **Do early exits per wave, not per thread.** Use `WaveActiveAnyTrue` to skip work when no lane needs it.

### 4.10 Workgroup size

- Use a multiple of 64 threads. That fills a wave64 on AMD, and two waves of 32 on NVIDIA and AMD wave32.
- Write the size with axes or constants: `[numthreads(IFFT_SIZE, 1, 1)]`. The cooker folds it per variant.
- Do not go above 1024 threads. That is the WebGPU and Vulkan limit. `IFFT_SIZE` stops at 512 because the
  fused kernels use twice that.
- Use 1D groups when you use wave operations. In a 1D group, a wave is a run of consecutive thread indices, so
  `threadIdx & (WAVE_SIZE - 1u)` is the lane index.

### 4.11 A checklist before you finish a kernel

1. How many bytes does the kernel read and write? Can a fusion remove a round trip?
2. Do adjacent lanes touch adjacent addresses on each global access?
3. Does each groupshared access avoid bank conflicts? If not, is the conflict in a comment?
4. Can a wave operation replace a groupshared stage and its barrier?
5. Is each barrier necessary? Is each barrier outside every divergent branch?
6. Is every value that sets a size or a loop count an axis or a constant?
7. Is every wave or quad operation behind a `Capability` axis with a working off path?
8. Does each axis change the code? Does the dedupe report show axes that change nothing?
9. Does each resource have an `ls_element_count` or an `ls_extent_*` attribute?
10. Does the cook pass with `--target=wgsl` and `--target=spirv`, with validation on?

### 4.12 Code to read

Read these files before you write a new kernel. They are in the DiamondDogs repository, beside this one.

| File | What it shows |
|---|---|
| `assets/shaders/compute/Ocean/IfftRadix4.slang` | Registers, then waves, then LDS. In-place stages. Twiddle identities. Bank-conflict notes. |
| `assets/shaders/compute/Ocean/OceanFftStages.slang` | Fused kernels. Thread maps that coalesce. Packed real fields. `half` range control. |
| `assets/shaders/compute/Ocean/OceanFftEntryPoints.slang` | Short entry points. Dispatch-size comments. Entry point constants. |
| `assets/shaders/compute/Ocean/OceanFftDims.slang` | Axes, a gated axis, and derived constants. |
| `assets/shaders/compute/Ocean/OceanFftBindings.slang` | Size attributes on every resource. |
| `assets/shaders/compute/VolumeTiledForwardShading/VtfReduceLightAABBs.slang` | A wave reduction with a groupshared fallback. |

In this repository, `tests/assets/KitchenSink/` shows every axis type and every resource shape.
`tests/assets/Shims/` shows every shim form.
