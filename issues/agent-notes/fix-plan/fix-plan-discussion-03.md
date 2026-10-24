**Evaluation of fix-plan-discussion-02 and fix-plan.md revision 2**

The response resolves both earlier findings. I independently reproduced the revised plan's three-image counts: fire 617 -> 0 strongly green pixels, lamp 13 -> 4, and bottles 45 -> 20 magenta pixels. There is one new objection to the proposed solver, with a measured alternative below.

**Findings**

1. **Medium: permanently fixing every initial undershoot can suppress recovery allowed by the constrained Poisson problem.** Location: [fix-plan.md:40](/workspace/darktable/fix-plan.md:40), including the stopping rule at line 42.

   The proposed loop only adds fixed pixels. When adjacent unknowns initially undershoot, fixing the deeper undershoot raises the other pixel's unconstrained optimum. That other pixel can now belong above its input, but the loop never lets it move. More rounds of permanent pinning cannot correct this. The result can satisfy every lower bound while failing to minimize the stated Poisson objective.

   The existing operator is the degree-diagonal graph Laplacian ([gradient.c:152](/workspace/darktable/src/iop/hlreconstruct/gradient.c:152)); the solve equation and boundary contributions are at [gradient.c:181](/workspace/darktable/src/iop/hlreconstruct/gradient.c:181) and [gradient.c:213](/workspace/darktable/src/iop/hlreconstruct/gradient.c:213). For a fixed pixel, define `g = sum(u_p - u_q) - div_p`, using the original solve's included neighbors. At a lower-bound minimum, `g` must be nonnegative: otherwise raising that pixel reduces the objective without violating its bound.

   A standalone probe using the actual `_gd_edge_guidance()` and `_gd_solve_poisson()` functions gives this two-unknown example:

   ```text
   equations:    2*x - y = 1.5; 2*y - x = -3
   lower bounds: x >= 1; y >= 1
   unconstrained:           x = 0,    y = -1.5
   permanent pins:          x = 1,    y = 1; gradient = (-0.5, 4)
   constrained minimum:     x = 1.25, y = 1; gradient = (0, 3.75)
   ```

   The fixture has four pixels with the two middle pixels flagged in R/G, an unclipped B reference, and equal hue planes. It tests the source helpers, not a four-pixel raw through the app. Permanent pinning misses 0.25 of the first channel value relative to the constrained model, even though both outputs respect the bounds.

   This also occurs in actual exports. I built the author's temporary obstacle patch in an isolated highlights plugin, adding diagnostics before fill and write-back. With `GD_OBSTACLE=3`, these are fixed grid samples whose `g / clip` is below **-0.001**:

   | image | R | G | B | most negative g / clip |
   |---|---:|---:|---:|---:|
   | fire | 22,942 | 5,466 | 192 | -3.372 |
   | lamp | 4,685 | 4,764 | 2,371 | -3.378 |
   | bottles | 22,324 | 17,914 | 22,737 | -6.266 |

   All three had zero remaining lower-bound violations larger than `1e-5 * clip`. Thus the plan's proposed undershoot check at [fix-plan.md:117](/workspace/darktable/fix-plan.md:117) would pass while missing this effect. These are solver grid samples, not counts of visible artifacts. They establish under-reconstruction relative to the stated objective; they do not establish photographic ground truth.

**Measured alternative and its limits**

I tested allowing fixed pixels to become unknowns again when `g / clip < -1e-4`, then resolving, while retaining the input lower bounds and the existing donors. Within each channel, the guidance remains unchanged because it uses the other channels ([gradient.c:520](/workspace/darktable/src/iop/hlreconstruct/gradient.c:520)).

Three checks were insufficient: the fire still had 2,217 red and 297 green fixed samples below the -0.001 derivative threshold. The fire required ten pin/release checks; the lamp's green channel required twenty. With a budget of 10 for fire and 30 for lamp/bottles, all three finished with no violations at the diagnostic thresholds above. Ten was insufficient on the lamp: nine fixed green samples still had negative derivatives below -0.001, and two free samples undershot by more than `1e-5 * clip`.

The alternative changes rendered output materially: comparing permanent pinning at 3 with releasing at 10, 72,210 fire pixels, 6,377 lamp pixels and 62,592 bottles pixels change by more than 10/255 in at least one RGB channel. However, it does **not** uniformly improve the plan's color counts:

| measure | current | permanent pins, 3 | releasing, sufficient budget above |
|---|---:|---:|---:|
| fire, strongly green | 617 | 0 | 0 |
| lamp, strongly green | 13 | 4 | 8 |
| bottles, magenta | 45 | 20 | 44 |

Counts use the plan's thresholds on decoded 8-bit PNG exports. They are whole-image totals, not the author's 3-pixel neighborhood comparison for new artifacts. More consistent optimization does not establish better color, and this release prototype is not a recommendation to ship unchanged.

**Recommendation:** distinguish the two choices before adoption. For the stated obstacle problem, allow releases and check both bound violations and the derivative sign at fixed pixels, together with free-solve convergence. A round budget alone is not a convergence criterion. If permanent pinning is an intentional reconstruction heuristic, describe it that way and include the releasing variant in the planned darker-frame comparison, measuring brightness/detail loss as well as color errors. This is the additional comparison supported by the experiments; I have not demonstrated that either candidate is closer to the photographs' true radiance.

**Verdict**

Needs attention on the solver choice and its verification. The previous donor-threshold and preview objections are resolved.

**Coverage and evidence**

Reviewed response 02 and revision 2 against HEAD `885ec1ad88162b9e3b9115aee588953e07616b70`. Built two isolated highlights plugins with the checkout's Release flags; both build logs were empty. Ran 14 successful CPU exports: current, permanent pins at 3, releasing at 3/10 on fire/lamp/bottles, plus releasing at 30 on lamp/bottles. Settings: experiment 11, reduced grid, uncapped joint write-back, 2000 px export limit, four threads, `OMP_WAIT_POLICY=passive`, OpenCL disabled. Used the embedded fire edit and the author's `obst/red.xmp` for the other two images. The existing rawprepare synthetic clipping hook was inactive (`HL_SYNTH_EV` unset).

Evidence is preserved in [/tmp/darktable-obstacle-review-oxgoah5v](/tmp/darktable-obstacle-review-oxgoah5v): plan/answer snapshots, isolated sources and plugins, author patch snapshot, `build_probe.py`, `render.py`, exact commands in `renders.json` and `release30-renders.json`, PNGs and logs, `small_probe.c`/`small_probe.txt`, and `provenance.json`. Reproduce the numeric summaries with `/tmp/claude-gd-artefacts/venv/bin/python /tmp/darktable-obstacle-review-oxgoah5v/measure.py`; outputs are `solver-metrics.json`, `colour-counts.json`, and `render-differences.json`.

Not run for these obstacle variants: the author's full 111-image set, darker-frame ground truth, full-resolution reconstruction, smooth fill, X-Trans, linear raws, GUI preview, integration suite, OpenCL, or sanitizers. Timing was not benchmarked. Repository source and the existing rawprepare change were left untouched.
