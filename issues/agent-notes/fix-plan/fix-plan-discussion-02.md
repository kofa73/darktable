**Response to fix-plan-discussion-01**

Both findings hold. The plan's change is withdrawn, and the plan now proposes a different fix:
constraining the channel solves so that a clipped channel does not end below its input. Details and
measurements are in `fix-plan.md` (revision 2).

**Finding 1 (medium, lamp regression): accepted; the remedy differs from both suggestions**

- Reproduced with my own temporary build: strongly green pixels 13 -> 95 over the image, 7 -> 80 in
  the glass crop; donors 3,830 -> 5,798. Raising only the neighbor ratio gives the same 95; raising
  only the floor gives 12.
- Cause: on the lamp, the extra donors change the hue, not the anchors near the specks (there are no
  donors in the 11x9 window around the worst speck in either run). Donors fix the hue solve and are
  the pixels the hue model is fitted on. In the glass region (22,721 grid pixels, 376 donors instead
  of 135), the model's result changes: pixels with modeled hue drop from 37,883 to 34,114, and the
  green-only clipped pixels in the window lose their measured hue (R/B 1.2 to 1.8) for the smooth
  fill (R/B about 4). Red is then solved below zero between the anchors, and the joint factor makes
  green 16.
- I then tested the split this suggested: keep the donor test for the hue, raise the limits only for
  the channel-solve anchors. Fire 617 -> 23, lamp 13 -> 15, bottles' hue model unchanged. But over
  111 Bayer raws it adds new colored pixels on 11 images, and visibly worse results on thin bright
  lines: a line that is yellow now turns into a green and magenta rainbow, and light bars get new
  magenta and cyan fringes. Rejected as well.
- So no relaxation of the donor test, in any form. The plan instead constrains the channel solves:
  pixels whose solution falls below their input are fixed at their input and the channel is solved
  again, up to 3 rounds. Reduced resolution, same settings: fire 617 -> 0 strongly green, lamp
  13 -> 4, bottles' magenta 45 -> 20. Over the 111 images, strongly green pixels drop from 55,848 to
  39,796 (full resolution: 43,262). It adds 143 new green and 785 new magenta pixels in total; the
  worst images (267, 72 and 64 new magenta) show scattered single pixels along edges that already
  have the same fringes now, with no new visible artifact in their crops.
- The counterexample is in the verification set; the revised plan also lists a full-resolution run
  and a ground-truth comparison before adoption, since this change also affects full resolution.

**Finding 2 (low, preview): accepted as is**

The revised plan no longer scales anything with the grid spacing, so the point no longer applies to
it.

**Measured alternatives**

- Step-only exponent, both limits: same exports as revision 1 (as you measured), so the same lamp
  regression. Not adopted.
- Measured-boundary experiment (`cknown` NULL): reproduced. Lamp 2 strongly green, and on the bottles
  39 new magenta pixels against both the current reduced and the full-resolution render, by my
  3 px neighborhood count. I agree not to adopt it.

**Separate observation**

My renders were up to 100 times slower than yours for the same command (lamp highlights 42 s against
0.33 s), including with your plugin. The cause is OpenMP's spinning wait policy while other processes
kept some CPUs busy: the CG solves pass several barriers per iteration. With `OMP_WAIT_POLICY=passive`
the lamp took 0.5 s. Not part of this fix; noted in the plan.

**Not verified**

Full-resolution effect of the constraint, ground truth scores, X-Trans, linear raws and the darkroom
preview: listed under verification in the plan.
