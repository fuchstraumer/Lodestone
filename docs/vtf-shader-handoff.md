# VTF shader handoff

Written 2026-08-18. Rewritten 2026-08-23. Rewritten 2026-08-29, after a machine change destroyed the
Python environment and after every magnitude was measured again. This document records the state of
the volume tiled forward shaders and of the measurement tools that go with them. Read it first.

**Scope.** `tests/assets/` and `tests/scripts/` only. It does not cover the C++ cooker. The agent who
owns `src/` reads `docs/agent-handoff.md`.

**Terms.** A *check* is one assertion in a test script. A *defect* is an error in code. An *axis* is a
cooked permutation axis. A *constant* is an `extern static const` value. An *arm* is one value of an
axis. A *wave* is a hardware SIMD group. A *cluster* is one cell of the screen grid. A *leaf* is the
lowest node level of the BVH, and it holds `VTF_BVH_BRANCHING` lights. A *turn* is one pass of the
traversal loop, and the traversal takes one turn for each node it enters.

**Why the shaders exist.** They are the load for the permutation explosion, and not a product. The
work below went further than that goal needs, because the measurements were useful. Do not let the
shader work displace the cooker work.

---

## 1. State on 2026-08-29

The Slang port compiles and runs. Three test scripts pass, and together they give **58 checks**.

| Script | Checks | Needs a device |
|---|---|---|
| `test_vtf_compute.py` | 33 | yes |
| `test_vtf_curves.py` | 21 | no |
| `test_vtf_raster.py` | 4 | yes |

All 58 checks passed on 2026-08-29 on the desktop. No shader changed since 2026-08-26. Two comments
in `vtf_leaf_size.py` changed, because they held a price that a different machine measured.

**No shader work is in progress.** The last session finished a measurement and wrote no shader.

---

## 2. The environment, and how to rebuild it

**The machine changed.** The repository moved from a laptop to a desktop. The desktop holds an
**NVIDIA GeForce RTX 4080** and an AMD integrated part. slangpy selects the NVIDIA part through
D3D12, and each script prints the name it selected. Read that line before you trust a time.

**The Python install was lost.** The earlier environment was Python 3.14 and held numpy, matplotlib,
and slangpy. That interpreter is gone. The machine now holds **Python 3.13.15** at `C:\Python313`,
and `py` selects it.

Rebuild the packages with:

```bash
py -m pip install -r tests/scripts/requirements.txt
```

`tests/scripts/requirements.txt` pins the three versions that gave 58 checks passed on 2026-08-29:
numpy 2.5.2, matplotlib 3.11.1, and slangpy 0.43.1. slangpy carries the Slang compiler and the device
layer, so it is the package that decides which shaders build. Nothing else in this folder needs a
package that the standard library does not hold.

Run each script from `tests/scripts/`:

```bash
cd tests/scripts && py test_vtf_compute.py
```

Compile one entry point to WGSL:

```bash
build/ninja-msvc/third_party/slang/slang-2026.14.1-windows-x86_64/bin/slangc.exe -target wgsl -I tests/assets -I tests/assets/common -I tests/assets/materials -I tests/assets/render -I tests/assets/compute/VolumeTiledForwardShading -entry VtfRadixSort -stage compute tests/assets/compute/VolumeTiledForwardShading/VolumeTiledForwardShading.slang -o out.wgsl
```

Each correction in the shaders carries a comment that starts with `fix:`. Search for that prefix.

### Which numbers the machine change moves

The scene generator is deterministic, and it uses numpy alone. So **every model number in this
document is machine independent**, and it reproduces exactly. Only a clock number moves.

The desktop is faster than the laptop that produced the earlier document. The table below gives the
same measurement on both machines.

| Quantity | Laptop, 2026-08-23 | Desktop, 2026-08-29 |
|---|---|---|
| `VtfAssignLightsToClusters`, Morton, near 1.0 | 0.2322 ms | **0.1616 ms** |
| one sphere test | 0.0382 ns | **0.0265 ns** |
| one append | 0.1066 ns | **0.0762 ns** |
| append against sphere test | 2.8 times | **2.9 times** |
| Hilbert against Morton, on the clock | −3.1% | **−3.8%** |
| near plane 0.1 against 1.0, on the clock | +9.3% | **+9.6%** |

Every direction held. Every magnitude moved, and the ratio of an append to a test held at about 2.9.
This is the rule in section 15 item 8, and it now has evidence.

---

## 3. The tools, and how to use them

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
| `make_lights_in_frustum(grid, count)` | the older scene. Read section 7 before you use it |
| `morton_from_cells`, `morton_from_cells_anisotropic`, `hilbert_from_cells` | the sort keys |
| `reference_bvh_leaves` | the lowest node level alone |
| `build_bvh_levels` | **every** node level, with the leaf size held apart from the branching factor |
| `count_traversal_tests` | every test at every level, plus the count of turns |
| `count_cluster_overlaps`, `count_clusters_per_light` | the two cost measures |
| `Results` | the check counter that each test script shares |

`build_bvh_levels` and `count_traversal_tests` are the newest parts. Section 6 states what they
corrected, and it is the most important section of this document.

### The measurement scripts

Each one prints numbers and asserts nothing. Run them from `tests/scripts/`.

| Script | Question it answers |
|---|---|
| `vtf_leaf_size.py` | What does the count of lights in one leaf cost, over the **whole** tree? |
| `vtf_leaf_overlap.py` | Which sort key gives the least traversal work? Add `random` on the command line for the older scene |
| `vtf_grid_tuning.py` | What does the near plane cost, and what does the slice count cost? |
| `vtf_size_binning.py` | Does a size class help? **Its leaf size sweep is superseded.** See section 6 |
| `vtf_timing.py` | What does the device clock say, and does the model agree with it? |
| `vtf_visualize.py` | Draws the GPU buffers that `test_vtf_compute.py` produced |

### The two cost measures, and which to trust

* **`count_clusters_per_light`** gives the *irreducible* cost. It is the count of (cluster, light)
  pairs that really touch, and no method can make it smaller. It mirrors `SphereInsideAABBFast`.
* **`count_cluster_overlaps`** gives the count of (cluster, node) pairs for one level. It mirrors
  `AABBIntersectAABB`.
* **`count_traversal_tests`** sums the tests over **every** level, and it also counts the turns.
  Use this one. The two measures above are its parts.
* **The ratio of the tests to the irreducible cost** is the tree efficiency. A ratio of 1.00 would
  mean every sphere test succeeds. Today it is 7.14 at near 0.1, and 7.19 at near 1.0.

`vtf_timing.py` proved that the test count predicts the measured time. Section 5 gives the numbers.
So the model can rank an arm that no device has run.

---

## 4. Benchmark discipline

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

## 5. What the clock says

Every number below comes from `make_lights_realistic`, 4096 lights, a 24 by 16 by 24 grid, and the
desktop RTX 4080 through D3D12, on 2026-08-29. The baseline arm is Morton at near 1.0.

### Every arm, over 5 interleaved rounds

| arm | predicted tests | best ms | appends | ns for each test |
|---|---|---|---|---|
| unsorted, near 1.0, control | 25 391 200 | 0.7188 | 606 549 | 0.0283 |
| **Morton, near 1.0** | 4 358 112 | **0.1616** | 606 549 | 0.0371 |
| Hilbert, near 1.0 | 4 134 912 | 0.1554 | 606 549 | 0.0376 |
| Morton, near 0.1 | 4 783 200 | 0.1771 | 670 254 | 0.0370 |
| Morton, near 0.5 | 4 517 728 | 0.1678 | 637 307 | 0.0372 |
| Hilbert, near 0.1 | 4 594 528 | 0.1731 | 670 254 | 0.0377 |

### The model agrees with the clock

| arm | model | clock |
|---|---|---|
| unsorted, the control | +482.6% | +344.7% |
| Morton, near 1.0 | — | — |
| Hilbert, near 1.0 | −5.1% | **−3.8%** |
| Morton, near 0.1 | +9.8% | **+9.6%** |
| Morton, near 0.5 | +3.7% | **+3.8%** |
| Hilbert, near 0.1 | +5.4% | +7.1% |

Correlation with the control: 1.0000. Without it: **0.9898**. The cost of one predicted test held
between 0.0370 and 0.0377 ns across every sorted arm, so the test count is the cost driver and not a
proxy that happens to rank correctly.

The control is the one arm the model overstates. It predicts +482.6% and the clock gives +344.7%. An
unsorted tree diverges more, so it wastes work in a second way that the test count does not hold.

### The shader agrees with the reference

Each timed arm reported an append count equal to `count_clusters_per_light` exactly: 606 549 for each
near 1.0 arm, 670 254 for each near 0.1 arm, and 637 307 for the near 0.5 arm. This ties the numpy
reference to the shader, and it is why the model can be trusted.

### What one operation costs

```
one sphere test   0.0265 ns
one append        0.0762 ns     2.9 times a sphere test
```

Of the 0.1616 ms that Morton at near 1.0 takes, 0.1155 ms goes to 4 358 112 sphere tests and
0.0462 ms goes to 606 549 appends. So a method with no traversal and the same appends would take
about 0.0462 ms, which is **3.5 times less** than the tree takes today.

Treat this as an estimate. It rests on two points, and it assumes the control differs from Morton in
the test count alone. The control also diverges more, so the cost of one test here is too large and
the share left for the appends is too small. A microbenchmark that varies the appends alone settles
it.

### Wave operations against shared memory

`VtfRadixSort`, both arms, round robin, best of 6, on the **laptop**: wave 0.0122 ms, shared memory
0.0255 ms, **2.10 times**. The cause is the barrier count and not the shared memory traffic. That
shader runs 3 barriers for each bit against 6, which is 96 sync points against 192, and the measured
ratio follows the barriers.

This is one entry point, and nobody has run it on the desktop. No other pair of arms has been timed.

---

## 6. The leaf size result, and why it reversed

**This is the headline of the 2026-08-26 session, and the earlier version of this document stated the
opposite.** Read the whole section before you act on the leaf size.

### What the earlier claim was

`vtf_size_binning.py` swept the count of lights in one leaf and reported that a leaf of 4 costs
**51 percent less** than a leaf of 32. This document ranked that as the largest lever.

That model counted the lowest level alone. A narrow leaf needs more node levels, and the model
charged nothing for them. So it was wrong in one direction, and it always favoured a narrow leaf.

`build_bvh_levels` and `count_traversal_tests` in `vtf_support.py` replace it. They build every level
from the lights up to the root, and they count each test and each turn.

### What the whole tree says

Every row below holds the interior branching factor at 32 and moves the leaf size alone. The scene is
`make_lights_realistic`, 4096 lights, near 0.1, 670 254 irreducible pairs.

| lights for each leaf | levels | nodes | total tests | ratio | against 32 | turns | against 32 |
|---|---|---|---|---|---|---|---|
| 2 | 4 | 2115 | 4 704 400 | 7.02 | −10.1% | 790 490 | **+360.7%** |
| 4 | 3 | 1057 | 4 286 028 | 6.39 | −18.1% | 567 892 | **+230.9%** |
| 8 | 3 | 529 | 4 110 992 | 6.13 | **−21.4%** | 388 114 | +126.2% |
| 16 | 3 | 265 | 4 461 040 | 6.66 | −14.8% | 260 879 | +52.0% |
| 32, today | 3 | 133 | 5 233 248 | 7.81 | 0.0% | 171 603 | 0.0% |
| 64 | 3 | 67 | 6 386 816 | 9.53 | +22.0% | 113 330 | **−34.0%** |

The old model, on the same data, gives −49.7% for a leaf of 4. The whole tree gives −18.1%. **The
missing 31 points are the levels the old model did not charge.**

### The two costs move in opposite directions

A narrow leaf lowers the tests and raises the turns. A wide leaf does the reverse. So neither count
alone ranks the arms.

One turn holds two group barriers and one pop that thread 0 performs alone. **Nobody has measured
what a turn costs.** The table below gives the total time under a range of prices for one turn, with
the price of one test held at the measured 0.0265 ns. Each row is a uniform tree, where the leaf size
and the branching factor are one number.

| arity | 0 ns for each turn | 1 ns | 5 ns | 20 ns |
|---|---|---|---|---|
| 2 | −18.1% | +627.0% | +986.0% | +1103.0% |
| 4 | **−32.7%** | +214.0% | +351.3% | +396.0% |
| 8 | −28.7% | +82.3% | +144.1% | +164.2% |
| 16 | −15.3% | +27.2% | +50.8% | +58.5% |
| 32, today | 0.0% | 0.0% | 0.0% | 0.0% |
| 64 | +27.3% | **−9.5%** | **−30.1%** | **−36.7%** |

A turn holds two group barriers. A group barrier on this hardware costs far more than 1 ns. So the
0 ns column is a bound that no hardware reaches, and every other column ranks a **wide** tree first.

### Lane use says the same thing

The traversal gives one child to each thread of a 32-thread group. So an arity below 32 leaves lanes
idle at every turn: 6% of the group works at arity 2, 12% at arity 4, and 25% at arity 8. A narrow
tree therefore pays twice, in turns and in idle lanes, and the turn price table does not hold the
second cost at all. The true ranking is worse for a narrow tree than the table states.

### Where a narrow tree spends

| leaf of 32 | nodes | turns | tests |
|---|---|---|---|
| level 0, the root | 1 | 9 216 | 36 864 |
| level 1 | 4 | 12 912 | 413 184 |
| level 2, the leaves | 128 | 149 475 | 4 783 200 |

| leaf of 4 | nodes | turns | tests |
|---|---|---|---|
| level 0, the root | 1 | 9 216 | 294 912 |
| level 1 | 32 | 62 729 | 2 007 328 |
| level 2, the leaves | 1024 | 495 947 | 1 983 788 |

A leaf of 4 more than halves the sphere tests, from 4 783 200 to 1 983 788. It then spends 2 007 328
box tests on the level above, which is almost the whole saving. The old model saw the first number
and not the second.

### What to conclude

1. **Do not narrow the leaf.** The model gives it a win only when a turn is free, and a turn is not
   free.
2. **A wider tree is the open question, and nobody has tested one.** Arity 64 raises the tests by
   22% and lowers the turns by 34%. It also needs `VTF_ASSIGN_LIGHTS_NUM_THREADS` at 64, or a group
   of 32 threads that visits two children for each turn.
3. **Measure the turn price before either change.** That measurement decides every row of the table,
   and it is one dispatch with a known node count. Section 13 lists it as step 1.

### What this does not measure

The build cost and the memory of the tree. A narrow bottom holds more nodes, and `VtfBuildBVH` writes
each one. The node count is in the first table for that reason. A wider tree holds fewer nodes, so
the build cost moves the same way as the traversal cost, and that helps a wide tree again.

---

## 7. Ideas that failed. Do not repeat them

| Idea | Result |
|---|---|
| Quantize z in the log space the grid uses | 11.82 to 11.71 slices for each leaf. The maximum became worse |
| Give z fewer bits, 10/10/8 | **+34.1%** of the traversal work. An early proxy favoured it, and the proxy was wrong |
| A size class in the high bits of the sort key | best arm −2.1%, and several arms worse |
| A leaf of fewer than 32 lights | See section 6. The model that favoured it charged nothing for the extra levels |

**One earlier result did not reproduce.** The 2026-08-23 document reported that a 10/10/12 Morton code
costs +0.5% of the traversal work. `vtf_leaf_overlap.py` on 2026-08-29 gives **−3.0%**. The two runs
did not use the same near plane, and −3.0% sits close to the −3.9% that Hilbert gives. Treat 10/10/12
as unsettled and not as a lever. An earlier session explained why extra low bits cannot help: 4096
lights sit in 2^30 cells, so no two lights share a cell, and the low bits separate nothing.

**Why the size class failed, because the reason is useful.** The expectation was that a leaf mixing
one large light with 31 small ones is what costs. The per class table says the opposite: the tree is
best on the largest lights (ratio 2.9) and worst on the smallest (ratio 240).

The ratio follows how much larger a leaf box is than one light inside it. Thirty-two small lights
span a box far larger than any one of them, so almost every sphere test fails. Thirty-two large
lights span a box barely larger than one of them. A size class does not change that quantity.

**A warning about the scene.** The same measurement gave a ratio of 2.76 on one scene and 7.19 on
another. `make_lights_in_frustum` draws depth and radius without letting them know about each other,
so 15.7 percent of its lights hold the camera inside them. A level does not.
`make_lights_realistic` gives 2.2 percent. Every direction in section 5 is robust. **No magnitude is.**

---

## 8. Where the work sits, and what that suggests

`vtf_leaf_overlap.py` on 2026-08-29, near 0.1, 4096 lights, 670 254 irreducible pairs.

### The sort keys

| arm | sphere tests | ratio | wasted tests | against Morton |
|---|---|---|---|---|
| unsorted, the control | 24 153 568 | 36.04 | 23 483 314 | +405.0% |
| Morton 10/10/10, today | 4 783 200 | 7.14 | 4 112 946 | 0.0% |
| Morton 10/10/8 | 6 413 888 | 9.57 | 5 743 634 | +34.1% |
| Morton 10/10/12 | 4 638 720 | 6.92 | 3 968 466 | −3.0% |
| Hilbert 10 | 4 594 528 | 6.85 | 3 924 274 | −3.9% |

**86 percent of sphere tests fail.** That share is the budget every idea competes for. A better curve
takes about 4 points of it, and section 6 says a better arity may take more.

### A few leaves hold most of the work

| the worst leaves of 128 | share of the traversal work |
|---|---|
| 1 | 4.6% |
| 4 | 17.4% |
| 8 | 33.8% |
| 16 | **65.1%** |
| 32 | 84.9% |

### A few lights hold most of the irreducible cost

| the largest lights of 4096 | share of the irreducible cost |
|---|---|
| 1 | 0.9% |
| 16 | 13.5% |
| 64 | **49.7%** |
| 256 | 79.4% |

The average light touches 163.6 clusters and the median light touches 20. The largest light touches
5988 clusters of 9216.

**Light radius sets a floor that no tree can pass.** With the radius as authored the tree meets
149 475 (cluster, node) pairs at the leaf level. With the radius at zero it meets 22 792, which is
15.2 percent. So 85 percent of the leaf level work is radius and not partition quality.

Both tables above say the same thing in two ways: the cost is not spread evenly, and 64 lights of
4096 decide half of it. Nobody has tested a method that treats those lights differently from the
rest. A size class in the **sort key** failed, and that is not the same idea as a separate list for
the largest lights. Treat the second one as untested and not as failed.

---

## 9. The near plane and the slice count

`vtf_grid_tuning.py` on 2026-08-29. The model alone, so these numbers are machine independent.

### The near plane, at 24 slices and 9216 clusters

| near | k | slices used | depth of slice 1 | irreducible | tests | ratio | against near 0.1 |
|---|---|---|---|---|---|---|---|
| 0.10, today | 1.426 | 20 of 24 | 0.143 | 670 254 | 4 783 200 | 7.14 | 0.0% |
| 0.50 | 1.334 | 19 | 0.667 | 637 307 | 4 517 728 | 7.09 | −4.9% |
| **1.00** | 1.296 | 19 | 1.296 | 606 549 | 4 358 112 | 7.19 | **−9.5%** |
| 2.00 | 1.259 | 18 | 2.517 | 556 804 | 4 168 480 | 7.49 | −16.9% |
| 5.00 | 1.212 | 17 | 6.058 | 410 368 | 3 711 936 | 9.05 | −38.8% |

The clock confirmed the 1.00 row at −9.6%. A near plane of 0.1 wastes 4 slices of 24 on depths that
hold no light, and the exponential division spends its resolution there.

**This is one constant and it needs no new code.** It is the cheapest confirmed win in this document.

### The slice count, at near 1.0

| slices | clusters | irreducible | tests | ratio | lights for the average cluster |
|---|---|---|---|---|---|
| 16 | 6 144 | 475 183 | 3 121 728 | 6.57 | 77.3 |
| 24, today | 9 216 | 606 549 | 4 358 112 | 7.19 | 65.8 |
| 32 | 12 288 | 750 128 | 5 593 056 | 7.46 | 61.0 |
| 48 | 18 432 | 1 043 464 | 8 124 096 | 7.79 | 56.6 |

Read this table with care. More slices give **more** total work, because the cluster count rises with
the slice count and each cluster walks the tree. Fewer slices give less work and a longer light list
for each cluster. So the slice count trades this dispatch against the shading dispatch, and no
measurement in this repository covers the second one. **Do not change the slice count on this table
alone.**

---

## 10. Facts that are easy to lose

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
mechanism `MakeExportedConstantSource` uses in the cooker. Pass `constants=` to `VtfDevice.kernel` or
`VtfDevice.raster` to cook one variant.

### The wave size contract

`VTF_WAVE_SIZE` is an **upper bound** and not an exact width. A variant cooked for 128 lanes is
correct on a device of 32 lanes. A variant cooked for 32 lanes is **not** correct on a device of 64.
Only the ballot count in `VtfAssignLightsToClustersBVH` reads the constant. Every other algorithm
reads `WaveGetLaneCount()`.

### The branching factor is not the wave width

`VTF_BVH_BRANCHING` is 32, and it is `internal static const` and not `extern`. It is a property of
the tree that CPU code shares, so a change to it changes host code as well. `VTF_WAVE_SIZE` varies
alone. `VtfConfig.slang:44` states this, and it also states that the constant sits apart from the
wave size so that somebody can drive it later.

### The leaf holds one light

A leaf node of this tree is **one light**. The level above it holds 32. So the 128 boxes that
`reference_bvh_leaves` builds are that upper level. `build_bvh_levels` and `count_traversal_tests`
use the same convention: the lights are not a level, and a test against the lowest node level is a
sphere test.

### The grid near plane need not equal the camera near plane

A first person camera wants 0.1. The cluster grid does not. It can start at 1.0 and let slice 0
absorb everything closer. That is where the −9.6% comes from.

### The Hilbert curve

`hilbert_from_cells` and `cells_from_hilbert` follow David Walker, "Algorithms for Encoding and
Decoding 3D Hilbert Orderings", UTC Research Institute, August 2023. Table 3 is the encode and
Table 4 is the decode. `test_vtf_curves.py` checks both worked examples of the paper, then checks
that the map is one to one and that **every step of the walk has length 1** at depths 2, 3, and 4.

That last check is the one that matters. A round trip proves only that the two tables are inverses,
and two tables with the same transcription error still round trip.

The paper gives an optimization that skips the leading zero digits. This code does not use it,
because the count of steps it skips depends on the data and would diverge a wave.

**The curve exists in Python only.** No shader computes a Hilbert code today.

### An uncalled function in the tree

`CalculateMortonCodeWithAnisotropy` in `VtfComputeMortonCodes.slang` has no caller. Its bit order is
correct, but `QuantizePosition` scales all three axes by one scalar, so `zExtra` is always zero and
the function returns what the isotropic one returns. Per axis quantization is needed to make it do
anything. Section 7 says it is not worth calling.

### Reference material

| Source | Location |
|---|---|
| The thesis, as markdown | `C:\Users\fuchs\Downloads\VolumeTiledForwardShading.md` |
| The 3D Hilbert paper, as markdown | `C:\Users\fuchs\Downloads\hilbert_algorithms_3d.md` |
| Frame sequencing, and the merge loop | `D:\DiamondDogs\modules\VtfModule\src\vtfTasksAndSteps.cpp` |
| GLSL progenitors | `tests/assets/volumetric_forward/` |

The first three paths point at the earlier machine. Confirm that each one exists before you cite it.

---

## 11. What the tests do not prove

Read this before you state that the port works. This list did not change on 2026-08-29.

1. **No raster pass draws the scene.** `test_vtf_raster.py` covers the harness and `VtfDebugTexture`
   only. Seven other raster entry points compile and nothing more.
2. **Spot lights are untested.** Each test supplies zero spot lights.
3. **Three compute entry points are untested**: `VtfFindUniqueClusters`, `VtfUpdateLights`, and
   `VtfUpdateClusterIndirectArgs`.
4. **`VTF_REDUCTION_TYPE` value 1 is untested.**
5. **Wave widths above 32 are untested.** The NVIDIA part holds 32 lanes. The desktop also holds an
   AMD integrated part, which usually holds 64. Nobody has run the suite on it. That part is the one
   device on this machine that can test the contract above.
6. **The `SerialMerge` bounds guards are not proven.** A mutation removed them and the suite still
   passed. They are hardening. Keep them, because the read is undefined by specification, but do not
   call them a defect fix.
7. **Every magnitude in section 5 rests on a synthetic scene.** See the warning in section 7.
8. **No measurement covers the shading dispatch.** Section 9 needs it, and section 6 would use it.
9. **Nobody measured what one turn of the traversal loop costs.** Section 6 depends on that number.

---

## 12. The test must be able to fail

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

## 13. Next steps, in order

The order changed on 2026-08-29, because section 6 removed the step that used to be first.

| Step | Work | Blocked by |
|---|---|---|
| 1 | **Measure what one turn of the traversal loop costs** | nothing |
| 2 | Move the grid near plane to 1.0. It is one constant, and the clock confirmed −9.6% | nothing |
| 3 | Test a **wider** tree, arity 64, against the clock | step 1 |
| 4 | Add spot lights to each compute test | nothing |
| 5 | Test the three untested compute entry points | nothing |
| 6 | Draw the scene with a raster pass | nothing |
| 7 | Implement Hilbert in the shader as `VTF_SORT_CURVE`, with a control arm | nothing |
| 8 | A microbenchmark that varies the appends alone | nothing |
| 9 | Run the suite on the AMD integrated part, at 64 lanes | nothing |
| 10 | Correct the two cooker defects in section 14 | the agent who owns `src/` |
| 11 | Register `VolumeTiledForwardShading` in `k_ModuleSpaces` | step 10 |

**Step 1 decides step 3, and it decides whether section 6 is finished.** Every row of the turn price
table depends on one number that nobody has measured. The measurement is one dispatch: run the
traversal on a tree whose node count is known, then run it again on a tree with a different node
count and about the same test count. `count_traversal_tests` supplies both counts, so the model can
choose the pair of trees before the device runs either one.

Step 2 is the only confirmed win that needs no new measurement. Do it first if you want a result.

Step 7 gives about 4 percent. Do it because it is the safest new axis and gives the influence matrix
a true statement to check, and not because it is fast.

Step 11 follows `docs/permutation-explosion-plans.md`.

### One proposal, and it is not started

`vtf_leaf_size.py` models the tests and the turns. It does not model the appends, because every arm
of that script performs the same count of them. So each percentage in section 6 is a share of the
traversal alone, and not a share of the dispatch. Section 5 gives the append price, so the script can
hold the whole dispatch. That change makes each percentage smaller and more honest. It changes no
ranking, so it is a clarity change and not a correction.

---

## 14. Defects in the C++ cooker

Another agent owns `src/`. Report these again if nobody has corrected them. Neither one was checked
on 2026-08-29, and the file names in the earlier document come from before the compiler split, so
confirm the location before you report it.

1. **`VerifyAxisNamesAreDeclared` accepts one undeclared axis.** The counter starts at −1 and the
   test asks for a value above zero.
2. **The variant budget applies after every variant compiles.** `CheckVariantBudget` takes a
   `CookedModule`, and `EnumerateVariants` never receives the policy.

The second one blocks any test that must exceed a variant budget.

`docs/replay-harness-and-cook-cost.md` holds the cook cost list. Item 6a, the quadratic
`ComputeAxisInfluence` that runs twice, was the largest one when that document was written.

---

## 15. Rules for the next session

1. Change files under `tests/assets/` and `tests/scripts/` only.
2. Do not change `src/`, `include/`, or `client/`. Another agent owns them.
3. Write each comment and each document in ASD-STE100.
4. Give each subagent prompt the same register.
5. Report what a test proves, and report what it does not prove.
6. Do not add a check without a mutation that makes it fail.
7. Establish that a cost exists before you correct it. This work produced five failed optimizations,
   and each one looked correct before the measurement.
8. State the direction and the magnitude apart. A direction here survives a change of machine and a
   change of scene. A magnitude survives neither. Section 2 gives the evidence.
9. **A model that counts one level is not a model of a tree.** Section 6 is the example. When you
   build a cost model, name the part of the cost it leaves out. Put that name in the doc comment of
   the function that holds the model.

---

## 16. `docs/` is in `.gitignore`

Line 16 of `.gitignore` holds `docs/`. A document survives only when somebody force adds it. Run
`git ls-files docs/` to see which ones did. This document is tracked. Add it again after a rename:

```bash
git add -f docs/vtf-shader-handoff.md
```
