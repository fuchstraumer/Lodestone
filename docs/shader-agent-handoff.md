# Shader measurement history

Written 2026-08-19 as a session handoff. Rewritten 2026-08-29 and given a new purpose.

**This document is no longer a handoff.** `docs/vtf-shader-handoff.md` holds the state, the current
numbers, and the next steps. Read that one first. This document holds the history: what each session
believed, which measurement broke that belief, and why the wrong belief looked correct.

**Why keep it.** Four proxy metrics in this repository gave a confident wrong answer. Each one was
reasonable. A list of them is the cheapest way to stop a later session from building the fifth.

---

## 1. The four proxies, and how each one failed

Each row is a measurement that somebody trusted, then replaced.

| Proxy | What it measured | Why it failed |
|---|---|---|
| leaf diagonal over scene diagonal | the size of one leaf box | A box that binds light **spheres** was compared against an ideal for light **centres**. The two are not the same quantity |
| depth slices for each leaf | how far one leaf reaches in z | It counts the grid axis and not the grid. A cluster is not a cube, and its shape changes with depth |
| sphere tests at the lowest level | the traversal cost | It counts one level of a tree that holds three. A narrow leaf needs more levels, and this charges nothing for them |
| (cluster, node) pairs at the leaf level | the traversal cost | Better than the row above, and still one level |

The measure that replaced all four is `count_traversal_tests` in `vtf_support.py`. It counts every
test at every level, and it also counts the turns of the traversal loop. `vtf_timing.py` then tied it
to the device clock at a correlation of 0.9898.

**The pattern.** Each failed proxy measured a *property of the data structure*. The measure that
worked counts *the operations the shader performs*. When you propose a new metric, ask which of the
two it is.

---

## 2. Two claims that were corrected inside one session

Correct these if you read them in an older message or an older document.

### The leaf boxes were much better than the first report said

The first report said the leaf boxes were 1.75 times the ideal size. That comparison was wrong, for
the reason in row 1 of the table above.

| Measurement | Median leaf diagonal over scene diagonal |
|---|---|
| centres only | 0.223 |
| equal split ideal, 128 boxes | 0.198 |
| as the shader builds them, spheres | 0.356 |

The Morton partition is 1.13 times the ideal. **There is almost nothing to gain from a better
partition.** The step from 0.223 to 0.356 is light radius, and nothing else.

`vtf_leaf_overlap.py` says the same thing in the units that matter. With the light radius at zero the
tree meets 22 792 (cluster, node) pairs. With the radius as authored it meets 149 475. So 85 percent
of the leaf level work is radius.

### The two locality measurements do not agree

The first report said the Morton walk ratio and the leaf box ratio both gave about 1.8 times ideal.
That agreement was an artifact of the error above. The two numbers do not agree, and no conclusion
follows from them together.

---

## 3. The bit budget experiments, and the reasoning that survives

Three sessions tried to change how the Morton code spends its bits. `docs/vtf-shader-handoff.md`
section 7 holds the current results. The reasoning below is the part worth keeping, because it
predicts the result of any similar proposal.

### Extra low bits cannot help, and the arithmetic says so

The module holds 4096 lights in a space of 2^30 cells. That is one light for each 260 000 cells, so
no two lights share a cell. The low bits separate only lights that share a cell. Therefore the low
bits separate nothing.

The top bits decide which lights fall in one leaf. A leaf covers about 1/128 of the code range, so
the top 7 bits decide it. The z axis already takes 3 of those 7 bits, because z is the highest axis
of each group of three. The x axis and the y axis take 2 each.

**Test a bit budget change at the top of the code, or do not test it.**

### The bit budget must follow the physical extent

The code maps each axis to the range 0 to 1, whatever its true size. The scene extent is 94.7 by 94.9
by 53.1. So one z bit covers 53.1/2^k world units and one x bit covers 94.7/2^k. **The z axis already
holds 1.8 times more resolution than x, in world units.**

This is why a 10/10/8 code looked correct. It was the first proposal that pointed the right way. It
still lost, at +34.1% of the traversal work, because the loss it causes at the top of the code is
larger than the gain it wins in world units.

### The z axis is loose, and resolution is not the cause

The z axis is the loosest axis, at a mean extent of 0.413 against 0.404 for x and 0.350 for y. One
arm gave z 12 splits through the whole bit schedule, and the z extent moved from 0.413 to 0.415.
**More splits do not make z tighter.** The cause is that z is the short axis of the scene, so a
radius of 4.49 covers a larger fraction of it.

### The worst boxes are Z-order straddles

| Diagonal | x | y | z |
|---|---|---|---|
| 0.903 | 0.973 | 0.911 | 0.591 |
| 0.715 | 0.940 | 0.452 | 0.556 |

These two boxes hold runs of lights that cross a high bit boundary of the Morton code. Light radius
does not cause them. A Hilbert curve has no long jump at a bit boundary, so it is the one change that
targets them, and it gives −3.9% of the traversal work.

---

## 4. Two kinds of axis

This distinction came out of the leaf size discussion. It is the phase F binding time idea with a
real example, and `docs/phase-f-vocabulary.md` holds the vocabulary.

**An axis that stays inside the shader.** `VTF_SORT_CURVE` is one. It changes emitted text. No code
outside the shader learns the value. This kind is safe to add today.

**An axis that the host must also know.** `VTF_BVH_BRANCHING` is one. The host computes node counts,
level offsets, and dispatch sizes from it. `VtfConfig.slang:44` states that it stays 32 for this
reason, and it is `internal static const` and not `extern` so that nobody drives it by accident. A
leaf size axis has the same problem in a smaller form, because it changes the dispatch the host
issues.

The second kind needs a design before it needs code. The manifest can carry the value, and that is
probably the correct answer. Nobody must add a table entry before the design exists.

---

## 5. Where each proposal from this document ended

| Proposal | State on 2026-08-29 |
|---|---|
| Replace the Morton curve with a Hilbert curve | Measured. −3.9% of the traversal work, −3.8% on the clock. Implemented in Python only |
| Make the curve an axis, `VTF_SORT_CURVE` | Open. Step 7 of the current handoff |
| Separate the leaf size from the branching factor | **Measured, and the answer reversed.** A narrow leaf loses. See section 6 of the current handoff |
| Measure clusters overlapped for each leaf | Done. It became `count_cluster_overlaps`, then `count_traversal_tests` |
| Draw one raster pass and capture the image | Open. Step 6 of the current handoff |
| Add spot lights to each compute test | Open. Step 4 of the current handoff |
| Test the three untested compute entry points | Open. Step 5 of the current handoff |

The leaf size row is the important one. This document proposed a narrow leaf on the strength of a
one level model, and it stated that "the usual objection to small leaves does not apply at this light
count". The whole tree model says the objection does apply, and that the turn count is the reason.

---

## 6. The rule this history produces

**Do not tune a data structure against a property of that data structure.** Count the operations the
shader performs, and price each one on a device.

Every failed experiment in this document obeyed the first half of the repository rule, "correctness
is proved by comparison". Each one compared two arms. None of them compared the metric itself against
a second opinion, and the metric was the part that was wrong.
