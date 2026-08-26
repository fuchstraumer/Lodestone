# VTF shader handoff

Written 2026-08-18. Rewritten 2026-08-23. This document records the state of the volume tiled forward
shaders and of the measurement tools that go with them. Read it before you continue that work.

**Scope.** `tests/assets/` and `tests/scripts/` only. It does not cover the C++ cooker.

**Terms.** A *check* is one assertion in a test script. A *defect* is an error in code. An *axis* is a
cooked permutation axis. A *constant* is an `extern const static` value. An *arm* is one value of an
axis. A *wave* is a hardware SIMD group. A *cluster* is one cell of the screen grid. A *leaf* is the
node above the lights, and it holds `VTF_BVH_BRANCHING` of them.

**Why the shaders exist.** They are the load for the permutation explosion, and not a product. The
work below went further than that goal needs, because the measurements were useful. Do not let the
shader work displace the cooker work.

---

## 1. State

The Slang port compiles and runs. Three test scripts pass, and together they give **58 checks**.

| Script | Checks | Needs a device |
|---|---|---|
| `test_vtf_compute.py` | 33 | yes |
| `test_vtf_curves.py` | 21 | no |
| `test_vtf_raster.py` | 4 | yes |

Run each one from `tests/scripts/`. Use `py`, which is Python 3.14 and holds slangpy, numpy, and
matplotlib. `python` is a different interpreter and holds none of them.

```bash
cd tests/scripts && py test_vtf_compute.py
```

Compile one entry point to WGSL:

```bash
build/ninja-msvc/third_party/slang/slang-2026.14.1-windows-x86_64/bin/slangc.exe -target wgsl -I tests/assets -I tests/assets/common -I tests/assets/materials -I tests/assets/render -I tests/assets/compute/VolumeTiledForwardShading -entry VtfRadixSort -stage compute tests/assets/compute/VolumeTiledForwardShading/VolumeTiledForwardShading.slang -o out.wgsl
```

Each correction in the shaders carries a comment that starts with `fix:`. Search for that prefix.

---

## 2. The tools, and how to use them

`vtf_support.py` holds the harness. Each other script imports it. Nothing else holds device code.

### What `vtf_support.py` supplies

| Part | Use |
|---|---|
| `VtfDevice` | the device, the loaded module, and the entry points |
| `VtfDevice.kernel(name, constants)` | one compute entry point, linked against chosen axis values |
| `VtfDevice.raster(vs, fs, constants)` | one vertex stage and one fragment stage, linked the same way |
| `VtfDevice.buffer` / `zeros` / `read` / `upload` | buffers, and the bytes in them |
| `VtfDevice.texture` / `render_target` / `point_sampler` | the raster resources |
| `VtfDevice.time_dispatch(...)` | the device clock, in milliseconds |
| `ClusterGrid` | the grid and the camera together, plus `cluster_aabbs(ctx)` |
| `make_lights_realistic(grid, count)` | a scene that behaves like a level |
| `make_lights_in_frustum(grid, count)` | the older scene. Read section 5 before you use it |
| `morton_from_cells`, `morton_from_cells_anisotropic`, `hilbert_from_cells` | the sort keys |
| `count_cluster_overlaps`, `count_clusters_per_light` | the two cost measures |
| `Results` | the check counter that each test script shares |

### The measurement scripts

Each one prints numbers and asserts nothing. Run them from `tests/scripts/`.

| Script | Question it answers |
|---|---|
| `vtf_leaf_overlap.py` | Which sort key gives the least traversal work? Add `random` on the command line for the older scene |
| `vtf_grid_tuning.py` | What does the near plane cost, and what does the slice count cost? |
| `vtf_size_binning.py` | Does a size class help? Also: what does the count of lights in one leaf cost? |
| `vtf_timing.py` | What does the device clock say, and does the model agree with it? |

### The two cost measures, and which to trust

* **`count_clusters_per_light`** gives the *irreducible* cost. It is the count of (cluster, light)
  pairs that really touch, and no method can make it smaller. It mirrors `SphereInsideAABBFast`.
* **`count_cluster_overlaps`** gives the count of (cluster, node) pairs. It mirrors
  `AABBIntersectAABB`. Multiply it by `VTF_BVH_BRANCHING` to get the count of sphere tests the
  traversal performs.
* **The ratio of the two** is the tree efficiency. A ratio of 1.00 would mean every sphere test
  succeeds. Today it is 7.19.

`vtf_timing.py` proved that the sphere test count predicts the measured time. Section 4 gives the
numbers. So the model can rank an arm that no device has run.

---

## 3. Benchmark discipline

**Read this before you write any timing code.** Each trap below gives a wrong number that looks
correct, and two of them cost a large part of one session.

1. **Reset the counters between the timed runs.** `PointLightIndexCounter` rises across the runs
   otherwise. The output list then overflows, and each run after the second measures a shader that
   ran out of room. `time_dispatch` takes a `reset` list for this.
2. **Allocate every buffer one time.** A set of buffers rebuilt for each arm made the same dispatch
   report 0.33 ms on the first pass and 2.34 ms on the fifth.
3. **Put every arm in one round robin, and take the smallest time.** This device runs at two clock
   states, and they are up to 5 times apart. It holds one state for a while. So a group of arms timed
   together shares a state, a group timed later does not, and the two groups cannot be compared. One
   configuration read 0.23 ms in the first group and 1.16 ms in the fourth, with the same work.

   Inside one state the median holds to 0.3 percent. So the noise looks tight and trustworthy while
   the number is wrong. This is the trap that is hardest to see.

`vtf_timing.py` obeys all three. Copy its shape rather than writing a new one.

---

## 4. What the measurements say

Every number below comes from `make_lights_realistic`, 4096 lights, a 24 by 16 by 24 grid, and an
NVIDIA RTX 4080 laptop through D3D12.

### The cost today

```
irreducible (cluster, light) pairs        606 549
sphere tests the traversal performs     4 358 112     ratio 7.19
measured time of VtfAssignLightsToClusters   0.2322 ms
```

**86 percent of sphere tests fail.** That share is the budget every idea competes for.

### The model agrees with the clock

| arm | model | clock |
|---|---|---|
| unsorted, the control | +482.6% | +346.2% |
| Morton, near 1.0 | — | — |
| Hilbert, near 1.0 | −5.1% | −3.1% |
| Morton, near 0.1 | +9.8% | +9.3% |
| Morton, near 0.5 | +3.7% | +4.1% |

Correlation without the control: **0.969**. The cost of one predicted test holds at 0.053 ns for each
sorted arm, so the test count is the cost driver and not a proxy that happens to rank correctly.

### The shader agrees with the reference

Each timed arm reported an append count equal to `count_clusters_per_light` exactly: 606549, 670254,
637307. This ties the numpy reference to the shader, and it is why the model can be trusted.

### The levers, ranked

| change | evidence | state |
|---|---|---|
| lights in one leaf, 32 to 4 | model, −51% net | needs `VtfBuildBVH` and traversal changes |
| direct write, no tree | estimate, up to −72% | structural. The estimate is optimistic |
| grid near plane 0.1 to 1.0 | **clock, −9.3%** | one constant |
| Hilbert in place of Morton | **clock, −3.1%** | small. Keep it as an axis, not as an optimization |

Leaf size, near plane, and curve are independent, so they multiply.

### What one operation costs

```
one sphere test   0.0382 ns
one append        0.1066 ns     2.8 times a sphere test
```

An append is **2.8 times** a sphere test, and not an order of magnitude. So a method with no
traversal and the same appends would take about 0.065 ms against 0.232 ms today.

Treat this as an estimate. It rests on two points, and it assumes the control differs from Morton in
the test count alone. The control also diverges more, so the cost of one test here is too large and
the share left for the appends is too small. A microbenchmark that varies the appends alone settles
it.

### Wave operations against shared memory

`VtfRadixSort`, both arms, round robin, best of 6: wave 0.0122 ms, shared memory 0.0255 ms,
**2.10 times**. The cause is the barrier count and not the shared memory traffic. That shader runs 3
barriers for each bit against 6, which is 96 sync points against 192, and the measured ratio follows
the barriers.

This is one entry point. No other pair of arms has been timed.

---

## 5. Ideas that failed. Do not repeat them

| Idea | Result |
|---|---|
| Quantize z in the log space the grid uses | 11.82 to 11.71 slices for each leaf. The maximum became worse |
| Give z more bits, 10/10/12 | +0.5% of the traversal work |
| Give z fewer bits, 10/10/8 | **+11.2%**. An early proxy favoured it, and the proxy was wrong |
| A size class in the high bits of the sort key | best arm −2.1%, and several arms worse |

**Why the size class failed, because the reason is useful.** The expectation was that a leaf mixing
one large light with 31 small ones is what costs. The per class table says the opposite: the tree is
best on the largest lights (ratio 2.9) and worst on the smallest (ratio 240).

The ratio follows how much larger a leaf box is than one light inside it. Thirty-two small lights
span a box far larger than any one of them, so almost every sphere test fails. Thirty-two large
lights span a box barely larger than one of them. A size class does not change that quantity. **The
count of lights in one leaf does**, and that is why it is the first lever in section 4.

**A warning about the scene.** The same measurement gave a ratio of 2.76 on one scene and 7.19 on
another. `make_lights_in_frustum` draws depth and radius without letting them know about each other,
so 15.7 percent of its lights hold the camera inside them. A level does not.
`make_lights_realistic` gives 2.2 percent. Every direction in section 4 is robust. **No magnitude is.**

---

## 6. Facts that are easy to lose

### The SlangPy binding trap

Each VTF entry point takes `uniform ParameterBlock<T> resources`, which is an entry point parameter.
`ComputeKernel.dispatch(vars=...)` cannot reach one. The call raises no error, the dispatch runs, and
every resource reads as zero. Use `LinkedKernel.dispatch`, which goes through
`ShaderCursor(shader_object).find_entry_point(0)` and raises on a wrong name.

`test_ifft.py` uses the `vars=` form, and that is correct there. OceanFft declares module scope
globals. The two shapes need two different binding paths.

### Three SlangPy raster traps

Each one stops the process with no message and no python traceback.

1. **A render pass is not a context manager.** `with encoder.begin_compute_pass()` works.
   `with encoder.begin_render_pass(...)` does not. Call `end()`.
2. **Hold `create_view()` in a local.** The attachment does not keep it alive, so a view built inline
   in the dict dies before the pass runs.
3. **A descriptor struct takes a dict and not keyword arguments.** This one raises a `TypeError`.

The fragment stage holds the `ParameterBlock`, so the raster cursor asks for entry point **1**.

### The permutation override

`VtfDevice.constant_module` builds a one-line Slang module that holds
`export static const <type> <NAME> = <value>;`, and links it next to the real module. This is the
mechanism `MakeExportedConstantSource` in `src/PermutationSpace.cpp` uses. Pass `constants=` to
`VtfDevice.kernel` or `VtfDevice.raster` to cook one variant.

### The wave size contract

`VTF_WAVE_SIZE` is an **upper bound** and not an exact width. A variant cooked for 128 lanes is
correct on a device of 32 lanes. A variant cooked for 32 lanes is **not** correct on a device of 64.
Only the ballot count in `VtfAssignLightsToClustersBVH` reads the constant. Every other algorithm
reads `WaveGetLaneCount()`.

### The branching factor is not the wave width

`VTF_BVH_BRANCHING` is 32 and must stay 32. It is a property of the tree that CPU code shares.
`VTF_WAVE_SIZE` varies alone.

### The leaf holds one light

A leaf node of this tree is **one light**. The level above it holds 32. So the 128 boxes that
`reference_bvh_leaves` builds are that upper level, and sphere tests equal (cluster, node) pairs
multiplied by 32. This is the arithmetic behind every table in section 4.

### The grid near plane need not equal the camera near plane

A first person camera wants 0.1. The cluster grid does not. It can start at 1.0 and let slice 0
absorb everything closer. That is where the −9.3% comes from.

### The Hilbert curve

`hilbert_from_cells` and `cells_from_hilbert` follow David Walker, "Algorithms for Encoding and
Decoding 3D Hilbert Orderings", UTC Research Institute, August 2023. Table 3 is the encode and
Table 4 is the decode. `test_vtf_curves.py` checks both worked examples of the paper, then checks
that the map is one to one and that **every step of the walk has length 1** at depths 2, 3, and 4.

That last check is the one that matters. A round trip proves only that the two tables are inverses,
and two tables with the same transcription error still round trip.

The paper gives an optimization that skips the leading zero digits. This code does not use it,
because the count of steps it skips depends on the data and would diverge a wave.

### An uncalled function in the tree

`CalculateMortonCodeWithAnisotropy` in `VtfComputeMortonCodes.slang` has no caller. Its bit order is
correct, but `QuantizePosition` scales all three axes by one scalar, so `zExtra` is always zero and
the function returns what the isotropic one returns. Per axis quantization is needed to make it do
anything. Section 5 says it is not worth calling.

### Reference material

| Source | Location |
|---|---|
| The thesis, as markdown | `C:\Users\fuchs\Downloads\VolumeTiledForwardShading.md` |
| The 3D Hilbert paper, as markdown | `C:\Users\fuchs\Downloads\hilbert_algorithms_3d.md` |
| Frame sequencing, and the merge loop | `D:\DiamondDogs\modules\VtfModule\src\vtfTasksAndSteps.cpp` |
| GLSL progenitors | `tests/assets/volumetric_forward/` |

---

## 7. What the tests do not prove

Read this before you state that the port works.

1. **No raster pass draws the scene.** `test_vtf_raster.py` covers the harness and `VtfDebugTexture`
   only. Seven other raster entry points compile and nothing more.
2. **Spot lights are untested.** Each test supplies zero spot lights.
3. **Three compute entry points are untested**: `VtfFindUniqueClusters`, `VtfUpdateLights`, and
   `VtfUpdateClusterIndirectArgs`.
4. **`VTF_REDUCTION_TYPE` value 1 is untested.**
5. **Wave widths above 32 are untested.** The test device holds 32 lanes.
6. **The `SerialMerge` bounds guards are not proven.** A mutation removed them and the suite still
   passed. They are hardening. Keep them, because the read is undefined by specification, but do not
   call them a defect fix.
7. **Every magnitude in section 4 rests on a synthetic scene.** See the warning in section 5.

---

## 8. The test must be able to fail

Each correction carries a mutation test:

1. Return the defect to the shader.
2. Run the suite.
3. Confirm the check that covers that defect fails.
4. Return the shader to its corrected state.
5. Run the suite again.

This found a weakness in a test. A panel drew `np.sort(codes)`, so it was monotone by construction
and could not fail. It now draws three buffers the GPU wrote.

The raster checks carry two mutations. A flipped triangle fails the texel comparison and passes the
coverage check. A triangle at half scale fails both, at 2016 pixels of 4096.

**Do not add a check without a mutation that makes it fail.**

---

## 9. Defects in the C++ cooker

Another agent owns `src/`. Report these again if nobody has corrected them.

1. **`VerifyAxisNamesAreDeclared` accepts one undeclared axis.** `src/PermutationSpace.cpp:464` sets
   the counter to −1, and line 490 tests for a value above zero.
2. **The variant budget applies after every variant compiles.** `CheckVariantBudget` takes a
   `CookedModule`, and `EnumerateVariants` never receives the policy.

The second one blocks any test that must exceed a variant budget.

`docs/replay-harness-and-cook-cost.md` holds the cook cost list. Item 6a, the quadratic
`ComputeAxisInfluence` that runs twice, is still the largest one.

---

## 10. Next steps, in order

| Step | Work | Blocked by |
|---|---|---|
| 1 | Separate the leaf size from `VTF_BVH_BRANCHING`, then measure 4 and 8 on the clock | nothing |
| 2 | Move the grid near plane to 1.0 | nothing |
| 3 | Add spot lights to each compute test | nothing |
| 4 | Test the three untested compute entry points | nothing |
| 5 | Draw the scene with a raster pass | nothing |
| 6 | Implement Hilbert in the shader as `VTF_SORT_CURVE`, with a control arm | nothing |
| 7 | A microbenchmark that varies the appends alone | nothing |
| 8 | Correct the two cooker defects in section 9 | the agent who owns `src/` |
| 9 | Register `VolumeTiledForwardShading` in `k_ModuleSpaces` | step 8 |

Step 1 is the largest lever and it needs no C++ change. `ReduceBranchingSegment` in
`VtfBuildBVH.slang` already performs a segmented reduction over aligned runs, which is what a narrow
leaf needs.

Step 6 gives 3 percent. Do it because it is the safest new axis and gives the influence matrix a true
statement to check, and not because it is fast.

Step 9 follows `docs/permutation-explosion-plans.md`.

---

## 11. Rules for the next session

1. Change files under `tests/assets/` and `tests/scripts/` only.
2. Do not change `src/`, `include/`, or `client/`. Another agent owns them.
3. Do not change `docs/shader-cooker-handoff.md`. It has uncommitted edits.
4. Write each comment and each document in ASD-STE100.
5. Give each subagent prompt the same register.
6. Report what a test proves, and report what it does not prove.
7. Do not add a check without a mutation that makes it fail.
8. Establish that a cost exists before you correct it. This work produced four failed optimizations,
   and each one looked correct before the measurement.
9. State the direction and the magnitude apart. A direction here survives a change of scene. A
   magnitude does not.

---

## 12. `docs/` is in `.gitignore`

Line 16 of `.gitignore` holds `docs/`. Only `shader-cooker-change-summary.md` and
`shader-cooker-handoff.md` are tracked. This document is not. Force add it to keep it:

```bash
git add -f docs/vtf-shader-handoff.md
```
