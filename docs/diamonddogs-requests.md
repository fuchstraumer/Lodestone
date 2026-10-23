# Requests from DiamondDogs

DiamondDogs (`C:\SoftwareDev\DiamondDogs`) embeds Lodestone as a sibling checkout (`add_subdirectory`) and uses
it as its only shader path: build-step cooks (`lodestone_add_shader_cook`, `spirv` profile) and the client
(`rhi::ShaderLibrary` in `modules/RhiSystem`). Its runtime Slang compiler is being removed. This file lists what
DiamondDogs needs from Lodestone. Text follows ASD-STE100. Ask the author before each change, as usual.

## Embedding fixes (small, DiamondDogs works around each one today)

| # | Issue | Fix |
|---|---|---|
| E1 | **Done 2026-10-21.** `tools/cooker_console`, `tools/manifest_dump`, and `tests/` CMakeLists use `${CMAKE_SOURCE_DIR}`. Embedded, that is the parent's root. | Use `${PROJECT_SOURCE_DIR}`. Then remove the include workaround in `DiamondDogs/cmake/ConfigureLodestone.cmake`. |
| E2 | **Done 2026-10-21.** `LodestoneCooker.cmake` prepends `cmake/` (FindVulkanHeaders) only when `Vulkan::Vulkan` does not exist. A parent that found Vulkan first breaks Slang's `find_package(VulkanHeaders)`. | Prepend unconditionally. |
| E3 | **Done 2026-10-21.** Set both options `OFF` before `add_subdirectory`; Lodestone keeps a defined value. `ConfigureSlang.cmake` forces `SLANG_ENABLE_MIMALLOC` and `SLANG_ENABLE_SPIRV_TOOLS_MIMALLOC` with plain `set()`. A parent with its own `mimalloc-static` gets a duplicate target. | Respect a parent value (`if(NOT DEFINED ...)`), or skip when `TARGET mimalloc-static` exists. Note: with several mimalloc copies per process, `MI_WIN_USE_FIXED_TLS` must stay OFF (it crashed `slang-bootstrap`). |

## Capabilities

| # | Need | Why | DiamondDogs today |
|---|---|---|---|
| C1 | **Done** (format 11, task L14; pipelines 2026-10-20). Read `EntryPointInstanceView::Constants()`, or `MergedLayoutView::PushConstants()` for one range from offset 0 for every stage of a pipeline. A SPIR-V block above 128 bytes fails the cook. Push constants in the manifest (task L14). | Per-dispatch data for noise kernels and passes. | Programs are created with no push constant ranges. Shaders use a `ConstantBuffer`. |
| C2 | **Done 2026-10-21.** The cook sets Slang's `VulkanUseEntryPointName`, so `OpEntryPoint` has the source name. The cook fails when it differs from the manifest entry point name, so `ModuleView` entry point names are the SPIR-V names. Stop reading `OpEntryPoint`. The SPIR-V entry point name on the entry point record. | Slang names every SPIR-V entry point `main`. | Read from `OpEntryPoint` in the words. |
| C3 | **Deferred** (2026-10-21) until DiamondDogs needs hot reload. In-process cooks work now through `RunCook` with `MemoryOutputSink` (the Lodestone tests use them), but no public header supports this yet. Open-world interface axes are a separate design. A live cook path: cook one module (or one type expression) in-process or as a subprocess, and return a bundle in memory. | Editor hot reload, and fused noise-graph kernels (L19 in the noise plan: open-world interface axes from client-supplied type expressions). | None. Only build-step cooks. |
| C4 | **Deferred** to phase F step F4, which resumes after the VeloxRhi rendergraph works on WebGPU. Placement forms beyond Bound on SPIR-V (phase F F4: Pointer buffers, Handle textures). | Buffer device address and bindless in DiamondDogs' RhiResources. | `ShaderLibrary` rejects non-Bound placements (`UnsupportedResourcePlacement`). |
| C5 | **Mostly done 2026-10-22** (phase F slices F3.1 to F3.4c, format 15). F3.4d (bindings per stage, bind groups) is left. F3.5 is done: see the notes below. See the notes below. Device capability filter (F3). | Pick variants by device features. | Not used. |
| C6 | Generated host layout headers. The cook writes one C++ header per module. The header has each uniform block, push constant block and struct as a C++ type, with the exact std140/std430 offsets, padding members, `static_assert(sizeof)` and `offsetof` checks. | The host mirrors are written by hand, so they drift from the shader. A generated header removes the mirrors and moves the check from run time to compile time. | OceanFft mirrors `SpectrumParameters`, `CascadeParameters`, `OceanViewParameters` and `OceanGridParameters` by hand, and checks some of them at run time against the manifest (`CheckUniformLayouts`). That check had a name bug (see C7). |
| C7 | **Done 2026-10-22.** Exact member lookup. `UniformMemberView` lookup by path (for example `FindMember("WindSpeed")`) with exact matching. Document how member names are qualified. Use `UniformMemberRange::Find(path)`: `instance.Constants().Members().Find("Swell.WindSpeed")`, or `resource.Members().Find(...)`. It returns `std::optional<UniformMemberView>`. See the notes below. | Member names in the manifest can be qualified. A client that matches by suffix gets the wrong member. | A hand-written suffix match took `SwellWindSpeed` (offset 52) for `WindSpeed` (offset 8). DiamondDogs now compares the last name component whole. |
| C8 | **Done 2026-10-22.** Include search paths. `INCLUDE_DIRS` on `lodestone_add_shader_cook`, and `--include-dir <path>` (or `-I <path>`) on the cooker. See the notes below. | Build-generated Slang must be able to stay in the build directory. | The cooker searches only the module's folder, so `OceanFftGenerated.slang` is written into the DiamondDogs source tree (only when its contents change). |
| C9 | **Done 2026-10-22** (the warning text and the context; the cache message is explained below). Shorter diagnostics. The "no specific policy coverage" warning prints a full C++ function signature on every build. Print the module and the target. Leave out the source location, or show it only with `--verbose`. Warnings now print no code location, and an error prints `File.cpp:line` in place of the function signature. | The build log is hard to read. | Not applicable. Separately, "cannot write the module cache file" appeared once during a concurrent build. |
| C11 | **Done 2026-10-23** (phase F steps C11.0 to C11.4; see the notes below). Not an `ls_format` string: an enum axis over `StorageFormat` in `LodestoneShims` (six float formats) selects the type argument of a write-only storage shim, `[ls_storage_format("AXIS")] extern struct Next : IWriteStorageTexture2D;`. A policy `CookValues` list of case names cooks a subset. The manifest records each variant's format as `StorageFormat`, as before. | Slang's `[format(...)]` takes a literal, so an axis cannot pick the format of a storage image. A shader that works on several formats (a mip generator, a blur, a copy) must then write without a format. Without any `[format]`, Slang guesses `rgba32f` from `float4`, which is wrong for every other format: validation reports `Undefined-Value-StorageImage-FormatMismatch-ImageView`. | `NvproPyramid` (`assets/shaders/compute/Pyramid`) declares its level bindings `[format("unknown")]`, and `pyramid::MipGenerator` requires `shaderStorageImageWriteWithoutFormat`. The WGSL target cannot cook it, because WGSL storage textures need a format. |
| C12 | **Done 2026-10-22** (SPIR-V; see the notes below). Binding arrays in the SPIR-V validator: a fixed-size `OpTypeArray` of images, samplers or buffers. The validator now rejects them ("a binding array, which this validator does not read yet", `SpvModuleReader.cpp` `ClassifyVariable`), so the cook fails with `ReflectionMismatch`. The client already reads `ResolvedResource::ArrayCount()`. | A shader that indexes one of several views by a uniform index, such as the mip levels of a texture (nvpro_pyramid's `imageMipLevels[16]`). | `NvproPyramid` declares sixteen bindings, `PyramidLevel0` to `PyramidLevel15`, and switches on the level. `WriteManifestDescriptors` (DiamondDogs RhiResources) already writes array bindings from `ArrayCount()`, so the switch can go when C12 lands. |
| C13 | **Done 2026-10-22** (one fault, fixed by C8; see the notes below). Cross-module imports in one cook (added 2026-10-22). Two parts: (1) when module B imports module A and both are in `MODULES`, the cook fails in worker setup with `WorkerSetupFailed (ModuleLoadFailed)`, also with a 4-line test module, so it is not specific to one shader; (2) an imported module is found only in the importer's folder or that folder's parent. C8 (include search paths) may solve part 2, but part 1 is a separate failure. | Shared shader libraries. `ddColor` (`assets/shaders/common/Color`) is the color math that every HDR pass and opt-in material shader imports, in other folders. | `ColorMathTest/CMakeLists.txt` copies the kernel, `ddColor.slang` and `ddColorGenerated.slang` into one build folder, and cooks only the kernel. OceanFft cannot import `ddColor` until this is fixed. |

## Proof that DiamondDogs uses

`DiamondDogs/tests/integration_tests/ComputeManifestTest` cooks `HashFill.slang` (one boolean axis), selects each
variant with `Where(...).SingleFor(...)`, reads uniform offsets and bindings from the manifest, dispatches, and
compares every output word with the CPU. A Lodestone change that breaks the client contract fails this test.

## Notes for the DiamondDogs agent

### C11: a storage format for each variant

Declare the axis and the shim extern, and write through the shim:

```slang
import LodestoneAttributes;
import LodestoneShims;

[ls_axis_enum]
extern static const StorageFormat LEVEL_FORMAT = StorageFormat::Rgba16Float;

extern struct Previous : ITexture2DRef;
[ls_storage_format("LEVEL_FORMAT")]
extern struct Next : IWriteStorageTexture2D;

struct MipLevel { Previous previous; Next next; };
ParameterBlock<MipLevel> level;
// level.next.Store(id.xy, value); takes a float4, narrowed to the texel of the format
```

- The accepted formats are the cases of `StorageFormat`: `Rgba8Unorm`, `Rgba8Snorm`, `Rgba16Float`, `R32Float`, `Rg32Float`,
  `Rgba32Float`. The comment on the enum in `builtins/LodestoneShims.slang` lists the formats that are not accepted, and why.
- Each variant binds a storage texture of its format on both targets: `texture_storage_2d<rgba16float, write>` on WGSL, and an
  image with that format on SPIR-V, so no variant needs `StorageImageWriteWithoutFormat`. Select the variant with
  `Where("LEVEL_FORMAT", AxisValueDomain::Enum, "Rgba16Float")`, and read the format back with `ResolvedResource::StorageFormat()`.
- The shim is write-only. Read the previous level through `ITexture2DRef` (a sampled texture). A `read_write` storage texture
  is portable for `R32Float` only, because core WebGPU allows no other format there.
- A subset of formats is policy, not shader code: `[Module.targets.spirv.CookValues]` with `LEVEL_FORMAT = ["Rgba16Float", "R32Float"]`.
  A misspelled case fails the cook with a suggestion.
- The axis must not be gated by `ls_axis_active_when`. A storage extern with no attribute, an attribute that names no axis,
  and an axis over another enum each fail the cook with their own error (`ShimStorageFormatMissing` = 180,
  `ShimStorageFormatAxisUnknown` = 181, `ShimStorageFormatAxisInvalid` = 182).
- **Not covered: an array of shim externs.** `NvproPyramid` binds its levels as one array (`imageMipLevels[16]`, C12). A shim
  extern cannot be an array element yet, so this form serves a kernel that writes one level for each dispatch (the shape of
  `tests/assets/StorageFormatAxis/MipDownsample.slang`). Tell the author if the pyramid needs the array form with a format
  axis.
- On this Slang a plain `RWTexture2D<float4>` with no `[format]` gets `rgba32f` from its element type, so it does not need
  `StorageImageWriteWithoutFormat` either. That is the wrong format for any other image view, which is the fault C11 removes.

`tests/StorageFormatAxisTest` is the proof: for both targets and every format, the variant binds the same resources at the
same places, with the same format, as a control written with the literal type.

### C13 and C8: imports across folders

Both reports were one fault. The cooker searched for an imported module in three places only: the folder of the module
being cooked, that folder's parent, and the module cache. When a module such as `ddColor` sat anywhere else:

- cooking the kernel alone failed with `cannot open file 'ddColor.slang'`, which is the part 2 report; and
- cooking `ddColor` first, in the same run, wrote `ddColor.slang-module` into the cache folder, so the kernel then found
  `ddColor` through the cache. The cook's own session loaded it, but the worker sessions could not, and the cook failed
  with `WorkerSetupFailed (ModuleLoadFailed)` and no reason. That is the part 1 report, and it is why a 4-line test module
  in another folder failed too.

The fix is the search path. Pass the folder of each imported module (and of each `__include`d fragment that is not beside
its module) as an include directory:

```cmake
lodestone_add_shader_cook(color_math_test_shaders
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/cooked"
    TARGETS spirv
    INCLUDE_DIRS "${COLOR_PIPE_SHADER_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated"
    MODULES "${CMAKE_CURRENT_SOURCE_DIR}/ColorMathKernels.slang")
```

On the command line it is `--include-dir <path>`, repeated for each folder. Relative CMake paths are made absolute from the
current source directory. Include directories come after the module's own folder and its parent, and before the module
cache. Measured on 2026-10-22 with copies of `ddColor.slang`, `ddColorGenerated.slang`, and `ColorMathKernels.slang`:

- the kernel alone, with `ddColor` in another folder, cooks;
- `ddColor` and the kernel in one run cook, with the order of the module list either way;
- `ddColorGenerated.slang` in a third folder (the build folder) is found for the `__include` in `ddColor.slang`, when that
  folder is an include directory.

So the staging in `ColorMathTest/CMakeLists.txt` can go, and `OceanFftGenerated.slang` can be written into the build
folder. The shared module does not have to be in `MODULES`. List it only when you want it in the bundle as a module of its
own. Tell the author if a case still fails: a failure of this kind now says `a worker cannot load the module 'X' ... pass
the folder of its source with --include-dir`.

`tests/CrossModuleCookTest` is the proof: two modules in two folders, cooked in one run.

### C7: member names

A member name is the dotted path of field names from the root of the block's struct. The name of the block itself is not
part of it. For `struct Params { float WindSpeed; Swell Swell; }` with `struct Swell { float Height; float WindSpeed; }`:

| Member | Name in the manifest |
|---|---|
| `Params.WindSpeed` | `WindSpeed` |
| `Params.Swell.Height` | `Swell.Height` |
| `Params.Swell.WindSpeed` | `Swell.WindSpeed` |

`Members().Find("WindSpeed")` returns the first row only. A suffix, a prefix, or a parent path (`Swell`) matches nothing, and
the result is `std::nullopt`. Compare the whole name yourself if you iterate. An array of scalars or vectors is one
member with `ArrayCount()` elements. `CheckUniformLayouts` can use `Find` for each name it checks.

### C9: the module cache warning

The warning `cannot replace the module cache file '...'` is written when the rename of the temporary file onto
`<Module>.slang-module` fails. On Windows that happens when another process has the destination open, which two concurrent
builds that cook the same module can do. The old file stays, and the next cook may recompile the module. Nothing is wrong
with the output of the cook that printed it. It was not reproduced here, so tell the author if it appears with a single
build running.

### C5: the device filter, as it stands

What exists (format 15, 2026-10-22):

- Each source states the capability names its code needs, in the names of its target: SPIR-V `OpCapability` and
  `OpExtension` names, or WGSL `enable` names. `EntryPointInstanceView::RequiredCapabilities`, `RequiresCapability`, and
  `WorkgroupMemoryBytes` read them. A name that Vulkan 1.2 guarantees (`Shader`) is not stored.
- `client/include/VulkanCapabilityTable.hpp` (generated from `vk.xml`) maps each SPIR-V name to the Vulkan support that
  allows it: any one of a version, an extension, a feature, or a property bit. It names no Vulkan type. Your
  `ExtensionWrangler` resolves the dependencies; Lodestone does not.
- `ManifestQueryBuilder::ForDevice(supports, user, limits)` removes the variants your device cannot run. `supports` is a
  plain function pointer, `bool (*)(void* user, std::string_view capability_name)`; return true when the device supports
  that SPIR-V name. `DeviceLimits` has the workgroup memory, the invocations, the size on each side, and the push constant
  bytes. A limit left at 0 rejects everything that has a size.
- `Keys`, `Variants`, and `Single` test every entry point of the module. `SingleFor(names)` tests the named ones, which is
  what a pipeline wants.
- No match gives `QueryErrorCode::NoVariantFitsDevice`, with the reason in `Detail` (a `manifest::DeviceMismatch`) and the
  capability name in `AxisName` for a missing capability.
- A profile whose cook read no facts returns `DeviceFactsUnavailable` (`EnvironmentView::HasCodeFacts()` is false). The
  `spirv` and `wgsl` profiles of this build both read them.
- `EnvironmentView::MakeDeviceCapabilities` is called twice, as in Vulkan: an empty span returns the word count, and a span
  of that size is filled. The builder does this for you. Use it directly only if you build your own filter on `Fit`.

Not built yet: the limits of bindings per stage and bind groups (F3.4d).

**Capability axes (F3.5).** An axis with `[ls_axis_kind("capability")]` now has a check, and you declare nothing more.
When no other axis gates it, a pair of variants that differ only in that axis must need different capability names in some
entry point. A boolean axis must need no more when off than when on. An `IFFT_USE_WAVE_OPS` whose off path still calls a
wave operation now fails the cook with `CapabilityAxisInert`, naming the module, the profile, and the axis. A gated axis
such as `IFFT_WAVE_SIZE` is not checked. If a cook of OceanFft now fails with this error, the axis is declared as a
capability axis and changes no capability: either its off path still needs the capability, or its kind should be `tuning`.

The format number changes often now. A manifest from an older build fails to open with `VersionMismatch`: recook.

### C12: binding arrays

A fixed-size array of images, samplers, or buffers cooks for `spirv` now, and the manifest stores it as one binding with
`ResolvedResource::ArrayCount()` set to the length. Measured on 2026-10-22, with the index an entry point `uniform`:

| Declaration | Result |
|---|---|
| `[format("rgba8")] RWTexture2D<float4> Levels[4]` | Cooks. Kind `StorageTexture`, `ArrayCount()` 4. |
| `[format("unknown")] RWTexture2D<float4> imageMipLevels[16]` (the pyramid shape) | Cooks. `ArrayCount()` 16, and the capabilities `StorageImageReadWithoutFormat`, `StorageImageWriteWithoutFormat`, and (for the dynamic index) `StorageImageArrayDynamicIndexing`. |
| `Texture2D<float4> Views[3]` | Cooks. `ArrayCount()` 3. |
| `StructuredBuffer<float> Weights[2]`, `RWStructuredBuffer<float> A[2]` | Cook. |
| `ConstantBuffer<float4> Tints[2]` | Cooks. |
| `SamplerState Samps[2]` | Cooks. |
| `Texture2D<float4> A[2][3]` (an array of arrays) | Fails: `spirv-val` rejects it, because Vulkan allows only an array of one opaque type. |
| `Texture2D<float4> Views[]` (unbounded) | Fails with `ReflectionMismatch`: "an unbounded binding array, which needs the Indexed access model". This is bindless, which phase F step F4 will read. |
| Any array, for `wgsl` | Fails with `CodeGenerationFailed`. Slang rejects an array of resources for WGSL (E56105). |

`WriteManifestDescriptors` can write these from `ArrayCount()` as it is, so the `PyramidLevel0` to `PyramidLevel15` bindings
and the switch can become one array indexed by the level.

**The index decides a device feature, and Slang does not state it.** A dynamic index into a binding array needs a Vulkan
device feature: `shaderSampledImageArrayDynamicIndexing` (sampled images, samplers, and combined image samplers),
`shaderStorageImageArrayDynamicIndexing`, `shaderUniformBufferArrayDynamicIndexing`, or
`shaderStorageBufferArrayDynamicIndexing`. Slang emits the access chain and no capability for it. The cook adds the matching
SPIR-V capability name when the index of an access chain on a binding array is not a constant, so
`EntryPointInstanceView::RequiredCapabilities` and the device filter see it. A constant index adds none. The switch with 16
bindings needed none of these features, so the array form adds a device requirement that the switch did not have. Enable the
feature, and let your `ForDevice` predicate answer `true` for these names only when the device has it.

Not covered: an array of texel buffers (`Buffer<T>[N]`), which needs the `...TexelBufferArrayDynamicIndexing` features, and
a non-uniform index (`NonUniformResourceIndex`), which needs the `ShaderNonUniform` family. Tell the author if you need either.

`tests/BindingArrayTest` is the proof.
