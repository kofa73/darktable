**Findings**

- `medium` [fix-plan.md:64](/workspace/darktable/fix-plan.md:64): Raising both donor limits produces new colored specks on a second Bayer image.

  I built the proposed formula in an isolated highlights plugin and reproduced its improvement on the fire image. The reduced-resolution export drops from 608 strongly green pixels to 10, with donors increasing from 7,655 to 16,575. This corroborates the plan's central measurement.

  However, the same change regresses `Water bottle & lamp clipped.NEF`. With gradient domain, experiment 11, reduced resolution, and uncapped joint write-back, strongly green pixels increase from 13 to 95. In the glass crop at `x=1025..1384, y=409..768`, they increase from 7 to 81. The full-resolution version of the same edit has 5 across the image and 4 in that crop. The candidate adds visible cyan speckling to the glass. Donors increase from 3,830 to 5,798 on its 1519x1007 grid.

  [The comparison crop](/tmp/darktable-fixplan-review-eo7supds/lamp-before-plan-full.png) shows current reduced resolution, proposed reduced resolution, and current full resolution, left to right. All exports use a 2000-pixel limit. Counts use the plan's exact condition, `G - max(R, B) > 100`, on decoded 8-bit RGB.

  The continuity decision controls both the hue donors and the channel-solve boundary. See [gradient.c:408](/workspace/darktable/src/iop/hlreconstruct/gradient.c:408), [gradient.c:1436](/workspace/darktable/src/iop/hlreconstruct/gradient.c:1436), and [gradient.c:1631](/workspace/darktable/src/iop/hlreconstruct/gradient.c:1631). Increasing the threshold therefore changes which measured neighbors anchor reconstruction, as well as which colors guide it. The successful fire result does not establish that interpreting both thresholds as per-photosite limits is safe for other boundaries.

  Add this counterexample to the verification set and reconsider the uniform relaxation before choosing the fix. Raising only the measured-neighbor ratio also fails this case, as the measurements below show.

- `low` [fix-plan.md:89](/workspace/darktable/fix-plan.md:89): The claimed full-resolution invariance does not hold for the preview pipe.

  This is a measured scope objection, not a claim that every changed preview pixel is incorrect. The full-resolution export stays pixel-identical, but the full-resolution preview changes substantially and matches the unchanged main image less closely on this test.

  On GTK3/X11, with `highres_preview_mip=false`, the fire image's preview reaches highlights with `piece->iscale=3.646457` and `roi_in->scale=1`. With full resolution enabled, `step=1`, so the proposed exponent is 3.646457, not 1. The threshold becomes 1.664685 instead of 1.15. Donors increase from 3,465 to 7,398 out of 8,314 border pixels. Source loading explicitly supplies the mipmap scale at [develop.c:719](/workspace/darktable/src/develop/develop.c:719).

  Captured input grids and clipping flags are identical between runs. After reconstruction, 107,672 RGB-grid pixels differ by more than 1% of a channel's clipping level. These are intermediate grid values, before per-photosite write-back. The final main-canvas screenshots are pixel-identical, while the navigator differs. Against the resized main image, navigator RGB mean absolute error rises from 6.54 to 7.70 levels out of 255, and RMS error from 14.76 to 15.86. This is a display-consistency comparison, not ground truth for reconstruction. It excludes the navigator's controls and outer border.

  With full resolution disabled, the same preview has spacing 7.292914 and threshold 2.771176, substantially beyond the 1.3225 tested by the original export experiment. It accepts 3,082 of 3,570 border pixels, compared with 1,044 currently.

  Qualify the invariance claim as applying to exports and full sensor inputs, and include preview verification before adopting the mipmap-dependent exponent. A step-only exponent is a separately tested way to limit this extension.

**Measured alternatives**

I tested the alternatives in temporary source copies. No production code changed.

| Reduced-resolution variant | Fire: strongly green pixels | Lamp: strongly green pixels |
|---|---:|---:|
| Current code | 608 | 13 |
| Proposed spacing formula, both limits | 10 | 95 |
| `powf(GD_DONOR_RATIO, step)`, both limits | 10 | 95 |
| Step-only exponent, measured-neighbor ratio only | 28 | 96 |
| Original donor test, all measured channel boundaries | 0 | 2 |
| Current full-resolution reference | 5 | 5 |

The step-only formula produces exactly the same decoded fire export pixels as the proposed formula. On its reduced preview, it uses 1.3225 and retains 1,761 donors, instead of the proposal's 2.771176 and 3,082 donors. It preserves the original threshold when `step=1`, including full-resolution previews and linear inputs. This is an alternative to the preview extension; it shares the lamp regression and is not a complete fix.

The measured-boundary experiment keeps the existing hue-donor test, but passes `NULL` as the channel solver's `cknown` mask. Unclipped channel measurements then remain boundary values even when their hue is rejected. It directly tests the missing-anchor diagnosis without changing hue-donor classification. A version restricted to grids with `step > 1` gives the same 0 and 2 counts and preserves every decoded pixel of the fire's full-resolution export.

That experiment also has a cost. On `Bottles Clipped.NEF`, it introduces 39 pixels with `min(R, B) - G > 80` that are absent from both the current reduced-resolution and full-resolution outputs. [This crop](/tmp/darktable-fixplan-review-eo7supds/bottles-baseline-plan-anchors.png) shows current, proposed, and measured-boundary outputs. The experiment improves some aggregate metrics but creates new magenta spots. I do not recommend replacing the boundary mask globally on this evidence. It does show that the fire result does not uniquely support relaxing donor classification.

**Verdict**

`needs-attention`: the proposed export change has a measured second-image regression. Its preview extension also exceeds the evidence and compatibility claim in the plan. The tested alternatives need further selection and validation.

**Coverage**

- Reviewed `fix-plan.md` against HEAD `885ec1ad88162b9e3b9115aee588953e07616b70`, following grid conversion, donor classification, hue interpolation, channel boundaries, write-back, and mipmap scaling. The plan remained unchanged during the review. Production source and the existing unstaged `rawprepare.c` change remained untouched.
- Provenance is `artifact-assisted`. Used the supplied plan and its full-resolution sidecars, then generated fresh exports and GUI measurements. Earlier WB review material is known from this conversation but does not support these findings. Historical boundary-confidence research notes were seen during the memory pass. No prior green-speck review reports were consulted.
- Built temporary baseline, proposed, step-only, ratio-only, measured-boundary, and reduced-grid measured-boundary plugins with the current Release compiler flags. All final prototype builds passed. The executable and other modules stayed the same. `HL_SYNTH_EV` was unset in every render.
- Export comparisons covered the fire image, storm clouds, plants, graffiti wall, bottles, and lamp. The strongest second-image objection above is backed by a full-resolution comparison. Other differences and aggregate color counts are not treated as proof of correct reconstruction. No unclipped capture was registered as ground truth.
- Actual GTK3/X11 runs covered reduced and full-resolution previews. The first GUI setup lacked the lighttable plugin directory and crashed before image processing; corrected runs used the complete module set and finished. All private GUI sessions have ended. No Wayland, GTK4, GPU, X-Trans, linear-raw runtime, sanitizer, or complete evaluation-set run was performed.
- CPU integration tests `0094-highlight-segmentation` and `0095-highlight-opposed` passed with the reduced-grid prototype, with maximum dE 1.230 and 1.321. These check neighboring methods, not the proposed gradient-domain change.

Reproduction scripts, source copies, sidecars, compiler logs, export commands, screenshots, captured preview grids, and measurements are in [the review evidence directory](/tmp/darktable-fixplan-review-eo7supds). `measure_all.py` regenerates the main color-count and navigator-comparison tables. `provenance.json` records the reviewed revision and source hashes.
