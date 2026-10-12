# Gradient domain highlight reconstruction

**gradient domain** is method 6 of the **highlight reconstruction** module. It rebuilds a clipped
camera channel from the gradients of the channels that are still measured, scaled by an estimate of
the local hue. The method follows M. Rouf, C. Lau and W. Heidrich, "Gradient Domain Color Restoration
of Clipped Highlights" (2012).

The code is a proof of concept on the branch `gradient-hl`. It is an alternative for users to pick
on images where **inpaint opposed** and **segmentation based** fail, not a new default. Two
experimental parameters, **write-back** and **experiment**, select variants of single steps. Both
will change or go away before a merge request.

This page describes commit `010dbe5876`, with experiment values 0 to 15.

| | |
|---|---|
| Code | `src/iop/hlreconstruct/gradient.c`, included by `src/iop/highlights.c` |
| Entry point | `_process_gradient()`, called from `process()` for raw and linear input |
| Mode | `DT_IOP_HIGHLIGHTS_GRADIENT = 6`, clipping threshold factor 0.98 (`highlights_clip_magics[6]`) |
| Params | `full_resolution`, `writeback` and `experiment` at the end of `dt_iop_highlights_params_t`, version 6 |
| Devices | CPU only, with no OpenCL path and no tiling. It always processes the whole image and also runs in fast pipes |

---

## The method rebuilds clipped channels in five steps

The method relies on the camera channels clipping independently. Where one channel is clipped, the
others usually still carry the structure of the scene. `_gd_solve()` runs steps 2 to 4.

1. `_gd_to_rgb()` converts the photosites into RGB pixels on a grid, with one clipped flag per
   channel. Pixels with any channel flagged form the region U. Unflagged pixels 4-adjacent to U
   form its border B.
2. `_gd_smooth_border()` smooths the colors on B along B, and a Laplace solve interpolates them over
   U. The result is the hue estimate `rho` (paper section 3.2).
3. `_gd_edge_guidance()` builds a guidance field for each channel inside its clipped region, from
   the other channels where they are measured and the hue ratio (paper eq. 7 and 8, with the
   changes listed below).
4. One Poisson solve per channel over its clipped region turns the guidance into values, with the
   unclipped neighbors as boundary values (paper eq. 6).
5. `_gd_write_back()` gives each clipped photosite the larger of its input and the solution.

### Photosites become RGB pixels with clipped flags

The sensor type and **full resolution** decide how `_gd_to_rgb()` builds the RGB pixels:

| Input | **full resolution** off | **full resolution** on |
|---|---|---|
| Bayer | 2x2 blocks | 3x3 window per photosite |
| X-Trans | 5x5 plane fit at the center photosite of each 3x3 block | 5x5 plane fit per photosite |
| linear raw | the RGB input, with the checkbox hidden | the RGB input |

- In a block, each channel is the mean of the block's photosites of that color.
- In a window, the photosite's own color keeps its value. Each other color is the mean of its
  photosites in the window. On Bayer, that equals bilinear interpolation.
- A channel is flagged if any photosite that contributes to it is clipped. The own color of a
  window takes its flag from the center photosite alone.
- The clipping level of channel `c` is `clip * 0.98 * processed_maximum[c]`. Guided laplacians uses
  a level of the same form with its own factor, 0.995 (`highlights_clip_magics[]`). With `clip` at
  its default of 1, a value of 0.99 of `processed_maximum` counts as clipped here but not there.
- At the image edges, windows move inward until they fit. X-Trans then uses the mean instead of the
  plane fit, because the fit would extrapolate.

X-Trans needs the plane fit (`_gd_xtrans_fit()`). A 3x3 mean samples red and blue up to one
photosite off center, and the offset changes from one photosite to the next. On a neutral ramp, two
neighboring blocks then come out as (0.25, 0.30, 0.35) and (0.35, 0.30, 0.25). The method would treat
that difference as a gradient and a hue. The plane fit is exact on linear ramps at every pattern
phase, and all its weights are positive.

The method leaves the image unchanged in five cases:
- a clipping level is 0;
- the image is smaller than one block or window;
- the Bayer pattern lacks a color in its 2x2 block;
- an X-Trans color does not span a plane in some 5x5 window;
- every grid pixel has at least one flagged channel, so U has no border.

In the last case, `_gd_solve()` returns before any solve, whatever the experiment value. That
includes neutral hue and opposed's estimate, which do not need the border hue. An image whose red
channel is clipped everywhere stays unchanged, even where green and blue are measured.

### The hue estimate comes from the border of the clipped region

`_gd_smooth_border()` replaces each border color with a bilateral mean of the border pixels in the
square around it of half-width 2 sigma_s, rounded up. The spatial sigma `sigma_s` is `GD_BORDER_SIGMA_S`, 5
full-resolution pixels, divided by the grid step and multiplied by `roi_in->scale / piece->iscale`,
with a minimum of 0.5 grid pixels. The range sigma is `GD_BORDER_SIGMA_R`, 0.25, on color
differences with each channel divided by its clipping level. The bilateral filter keeps noise on
the border from turning into streaks of hue.

`_gd_pullpush_fill()` spreads the border colors into U through a pull-push pyramid. The result is
only the starting point of a Laplace solve, which then interpolates the border colors over U. The
solver, `_gd_solve_poisson()`, runs conjugate gradients with a Jacobi preconditioner. It stops when
the residual falls below `1e-5` times the larger of the right-hand side and the initial residual, or
after 5000 iterations.

The method fails most often in this step. The border of a clipped sky often consists of trees,
roofs or lamp posts, and the interpolation carries their colors into the sky. The experiments in
[Experiment values](#experiment-values) change this step.

### Measured channels guide each clipped channel

`_gd_edge_guidance()` gives the target difference of channel `j` across the edge between pixels `p`
and `q`. Each usable reference channel `k` contributes `r(q) * u_k(q) - r(p) * u_k(p)`, where
`r = rho_j / rho_k` is the hue ratio at that pixel. The guidance is the mean of these contributions,
weighted by `_gd_weight()` (after paper eq. 8). The weight lowers the confidence in values near black
and near clipping, and it is the smaller of its values at `p` and `q`.

Three rules depart from the paper:

- **The weight is smoothstep, not the polynomial printed as eq. 8.** Let `x` be the channel value
  divided by its clipping level, scaled so that `x` is 0 at black and at clipping and 1 at the peak
  `GD_WEIGHT_PEAK`, 0.65. The paper prints `3x^3 - 2x^2 + epsilon`, which is negative below
  `x = 2/3`. At a value of 0.325 of clipping, `x = 0.5`, it gives -0.124. The code uses
  `3x^2 - 2x^3 + epsilon`, which gives 0.501 there and has the zero slopes at black, clipping and
  the peak that the paper asks for. `epsilon` is `GD_WEIGHT_EPS`, 0.001.
- **The flags decide which references count, not the values.** A reference channel flagged at
  either end of the edge is left out, unless an earlier solve (experiment 3) or the smooth fill
  (experiment 4) has restored it there (`_gd_usable()`). A block or window mean that includes a
  clipped photosite can lie far below the clipping level, where eq. 8 would give it full
  confidence. If no reference remains, the edge gets no guidance, as in the paper's fully clipped
  regions.
- **The guidance is the change of `r * u_k`, not `r` times the change of `u_k`** as in eq. 7. Where
  `r` varies, eq. 7 is not a gradient field, and its error grows with `u_k`. With a smooth fill far
  above clipping as the reference, eq. 7 pushed red down to -4.5 times the clipping level, and the
  joint write-back then multiplied green by up to 74,000. Without the smooth fill, the change moved
  the exported test images by at most 0.09 on a 0 to 1 scale.

### Each clipped channel gets one Poisson solve

For channel `j`, the unknowns are the pixels flagged in `j`. Their neighbors that are not flagged in
`j` supply measured boundary values, with the exception described for the continuous border.
Neighbors outside the image are left out.

The initial guess extends the paper's eq. 4 to several references. It is the mean of `r * u_k` over
the usable references, each weighted by `_gd_weight()` at the pixel. If that mean is below the
pixel's own value, or no reference is usable, the guess is the pixel's own value.

Each solve writes its result into the RGB buffer in place. That is safe because the flags never
change, so no solve reads a flagged value of another channel unless an experiment asks for it.

### Fully clipped regions have no guidance

Where all three channels are flagged, no channel guides another, so the Poisson solve reduces to a
Laplace solve there. The solutions in such a region interpolate the solutions around it. With the
basic write-back, many of these pixels end up at their input, which is the clipping level of each
channel. Under white balance those levels differ, so the result is pinkish. Several experiment
values fill these regions differently.

### The write-back raises only clipped photosites

`_gd_write_back()` gives each clipped photosite of color `c` the larger of its input value and the
solution of `c` at its position. At reduced resolution, it interpolates the solution bilinearly
between block centers. Beyond the outermost centers, it clamps each grid coordinate on its own and
still interpolates along the other axis. Only at the corners does a single center apply. Unclipped
photosites keep their input. The write-back runs only after all solves succeed. If an allocation
fails, the output is a copy of the input.

The **write-back** parameter can scale the flagged channels of a pixel together before this step,
see [Write-back values](#write-back-values).

---

## Parameters

### full resolution

The `full_resolution` checkbox is off by default and visible for Bayer and X-Trans raws only. When
it is on, every photosite becomes an RGB pixel. When it is off, the grid is 2 times (Bayer) or 3
times (X-Trans) coarser in each direction. That is several times faster and loses accuracy near the
image corners. The evaluation used it on.

### Write-back values

The values are those of `dt_iop_highlights_writeback_t`. `_gd_joint_scale()` implements values 1 to 4.

| Value | UI name | Effect |
|---|---|---|
| 0 | basic | each clipped channel takes the larger of its input and its solution |
| 1 | joint, all channels clipped | in fully clipped pixels, the flagged channels are multiplied by one factor, the smallest one of at least 1 that lifts each of them to its input |
| 2 | joint, any channel clipped | the same in every pixel with a flagged channel |
| 3 | joint, all channels clipped, capped | as 1, with the factor capped at `GD_JOINT_MAX`, 2; the write-back then lifts channels still below their input one by one |
| 4 | joint, any channel clipped, capped | as 2, capped the same way |

The basic write-back lifts each flagged channel on its own. That changes the ratios among the
flagged channels wherever one of their solutions falls below its input. The joint write-back
multiplies all flagged channels of a pixel by the same factor, so their ratios stay those of the
solution. It does not preserve the hue of the whole pixel in every case:
- it leaves the measured channels unscaled, so in a partly clipped pixel the ratio between flagged
  and measured channels can still change;
- with the cap, `_gd_write_back()` afterwards raises any channel still below its input on its own.

A positive solution close to zero asks for a huge factor, which then multiplies the other flagged
channels too. With all channels flagged, input (1, 1, 1) and solution (0.00001, 1, 1), the uncapped
factor is 100,000, and the pixel becomes (1, 100000, 100000). The cap limits the factor to 2. A
solution at or below zero has no defined factor. Uncapped, such a channel is left out of the factor.
Capped, it sets the factor to the cap. The evaluation used value 4.

### Experiment values

The values are those of `dt_iop_highlights_experiment_t`. Values 1 to 5 change one step of the basic
method. From value 6 on, each value combines a hue source for partly clipped pixels with a fill for
fully clipped pixels.

| Value | UI name | What changes |
|---|---|---|
| 0 | basic | nothing |
| 1 | neutral hue | the hue estimate is white (`_gd_white()`) in all of U, instead of the border hue |
| 2 | hue estimate in fully clipped | fully clipped pixels take the hue estimate (`_gd_hue_fill()`) |
| 3 | solved channels as references | channels solved earlier guide later ones, largest clipped region first |
| 4 | smooth fill (paper 3.4) | one channel gets a smooth bump in fully clipped regions and guides the others (`_gd_fill_fully_clipped()`) |
| 5 | block-wide own-color flag (X-Trans) | at reduced resolution, the center color takes its flag from the whole 3x3 block |
| 6 | hue from measured channels | pixels with one flagged channel take their hue from a model fitted on the border (`_gd_model_hue()`) |
| 7 | hue from measured channels, hue estimate in fully clipped | as 6, with the fill of 2 |
| 8 | hue from measured channels, white in fully clipped | as 6, and fully clipped pixels become white |
| 9 | opposed's estimate as hue, hue estimate in fully clipped | partly clipped pixels take the hue of inpaint opposed's estimate (`_gd_opposed_hue()`), with the fill of 2 |
| 10 | continuous border, hue estimate in fully clipped | as 7, with untrusted border pixels left out (`_gd_continuous()`, `_gd_hue_domain()`) |
| 11 | continuous border, white in fully clipped | as 8, with the continuous border |
| 12 | opposed's estimate as hue, white in fully clipped | as 9, with the white fill |
| 13 | hue from measured channels, edge hue to white in fully clipped | as 6, with the edge fill (`_gd_edge_fill()`) |
| 14 | continuous border, edge hue to white in fully clipped | as 10, with the edge fill |
| 15 | opposed's estimate as hue, edge hue to white in fully clipped | as 9, with the edge fill |

The hue fill of value 2 and the white fill (`_gd_hue_fill()`) give a fully clipped pixel a target
color `r`. For the hue fill, `r` is the hue estimate. For the white fill, it is white. The pixel
becomes `r` times the smallest factor that keeps every channel at or above both its solution and its
input.

**White** (`_gd_white()`), for the white fill, the edge fill and neutral hue, is the color that
renders neutral in the scene-referred workflow. There, colorin takes data balanced with the D65
(reference) multipliers and adapts it from D65 to D50. Color calibration then derives the scene
illuminant from the as-shot multipliers and adapts from it to D50:
- with white balance "as shot to reference", it has equal channels. The module's input is balanced
  with the as-shot multipliers, so equal channels are the scene illuminant. The late correction in
  colorin returns the data to the D65 multipliers only after this module;
- with "camera reference", channel `c` is `D65coeffs[c] / as_shot[c]`, from `dev->chroma`: the
  scene illuminant under the D65 multipliers. Equal channels would be the D65 white, which the
  adaptation from the scene illuminant shifts. On a fire photo with a warm as-shot white balance,
  the white fill turned bright blue;
- with any other white balance, it has equal channels, the white the user set. In the
  display-referred workflow no color calibration follows.

The white is divided by its largest channel. "camera reference" and "as shot to reference" are meant
to be used with color calibration, which does the chromatic adaptation. Its default illuminant is
"as shot in camera", or daylight or black body at the as-shot temperature when that is within 0.005
in CIE 1960 uv of the as-shot white (`_check_if_close_to_daylight()` in `channelmixerrgb.c`). The
fill is then neutral to within that distance. With any other illuminant, the fill is not neutral
with either white balance. Without color calibration, which is not the intended use, "camera
reference" gives the fill the color of the scene illuminant.

**Hue from measured channels** (`_gd_model_hue()`) works per 4-connected region of U and per
channel `j`. It fits `c_j = a * c_k1 + b * c_k2` by least squares on the region's border pixels with
all three channels at least 5% of clipping:
- the fit is ridge regularized toward `a = 0.5 * sum(c_j) / sum(c_k1)` and
  `b = 0.5 * sum(c_j) / sum(c_k2)`, with a strength of 0.01 times the mean of `sum(c_k1^2)` and
  `sum(c_k2^2)`. A border of one color cannot separate `a` from `b`;
- a negative coefficient is set to 0, and the other one is fitted alone;
- regions smaller than `GD_MODEL_MIN_REGION` (64 pixels), with fewer than 16 border samples, or
  with an rms residual above `GD_MODEL_MAX_RESIDUAL` (0.1) times the rms of `c_j` keep the filled
  hue.

A pixel with only channel `j` flagged then gets `rho_k = max(c_k, 0)` for its two measured channels
and `rho_j` equal to the larger of the model and its own `c_j`. The Laplace fill of the border colors
cannot follow a hue that changes inside the region, such as a sunlit cloud warmer and brighter than
its edge. The measured channels show that change.

**Opposed's estimate as hue** (`_gd_opposed_hue()`) builds the hue of a partly clipped pixel the way
inpaint opposed estimates a clipped channel:
- measured channels keep their values;
- a flagged channel `j` gets the larger of its value and a reference plus an offset. The reference
  is the cube of the mean of the cube roots of the pixel's two other channels. The offset is one
  value per channel for the whole image;
- the offset is the mean difference between `j` and that reference over the pixels where `j` is
  unflagged and lies strictly between 0.2 and 1 times its clipping level, and that lie in the square
  of half-width 2 sigma_s (rounded up, at least 1) around a pixel flagged in `j`. With 100 such
  pixels or fewer, the offset is 0;
- before the reference, the other channels are multiplied by opposed's white balance correction,
  taken from `dev->chroma` when `late_correction` is set, as in `opposed.c`.

Inpaint opposed writes this estimate into the channel directly. Here it only sets the hue ratio that
guides the solve.

**The continuous border** trusts a border pixel only if it continues each of its clipped neighbors
without a jump (`_gd_continuous()`). For each clipped 4-neighbor and each channel:
- a channel measured in the neighbor must lie within a factor of `GD_DONOR_RATIO`, 1.15, of the
  neighbor's value, unless both values are below `GD_DONOR_DARK`, 5% of clipping;
- a channel flagged in the neighbor must reach the clipping level divided by 1.15.

The test does not use brightness alone, because sunlit trees can be as bright as the sky next to
them. Untrusted border pixels fix no values:
- In the hue solve, they become unknowns, so their hue comes from the trusted ones however far
  away. The solve leaves out the unclipped pixels behind them, as it does at the image edge.
- In the channel solves, they are left out like the image edge, together with their guidance terms.
  A roof or lamp next to a clipped sky would otherwise pull the sky's clipped channels toward its
  own values.
- A region with no trusted pixel next to it, such as sky enclosed by a lamp post, keeps the hue that
  the pull-push pyramid spreads from the nearest trusted pixels, smoothed by the solve.
- An image without any trusted border pixel keeps its whole border as fixed values, as without the
  continuous border.

With the continuous border, the model of value 6 is fitted on trusted pixels only.

**The edge fill** (`_gd_edge_fill()`) looks at the square of side `2 * rad + 1` around a fully
clipped pixel, cut at the image edges, where `rad` is 2 sigma_s, rounded up, and at least 1:
1. Its target hue `h` is the mean over the partly clipped pixels in the square. Each contributes
   its solution raised to its input, divided by its largest channel. Without a partly clipped pixel
   in the square, `h` is the hue estimate divided by its largest channel, or white where that is 0.
2. Let `t` be the share of fully clipped pixels in the square, mapped from 0.8
   (`GD_EDGE_WHITE_FROM`) to 1 onto 0 to 1 and clamped. The white weight is `w = 3t^2 - 2t^3`.
3. The target color is `(1 - w) * h + w * white`, which is white where `w` is 1.
4. As with the other fills, the pixel is that color times the smallest factor that keeps every
   channel at or above its solution and its input.

The edge fill reads the partly clipped pixels before the joint write-back changes them.

---

## Memory and run time

`tiling_callback()` reports the memory in `tiling->factor`, as a multiple of the input buffer, and
leaves `tiling->overhead` at 0. The overhead field is an `unsigned`, and 66 bytes per pixel would
exceed its range at about 65 MP.

The base cost is 66 bytes per RGB pixel. It covers the RGB pixels, the state, border, hue and index
planes, and the solver arrays, with every pixel counted as clipped. That is about 1 GB for a 16 MP
raw at full resolution. The experiments add these amounts per RGB pixel:

| Experiment | extra bytes per RGB pixel |
|---|---|
| input copy, for the fills and the joint write-backs | 12 |
| hue from measured channels | 13 |
| continuous border | 16 |
| edge fill | 20 |
| smooth fill | 18 |
| opposed's estimate | 2 |

Linear raws add 1 to the factor for their temporary buffer.

The conjugate gradient solves take most of the run time. Large clipped regions need thousands of
iterations. On a sky that covers most of a 6 MP image, the hue solve took nearly 3000. `-d perf`
prints a line per run with the grid size, the clipped and border pixel counts, the iterations of
each solve, and the parameters.

---

## What the evaluation showed

A harness outside the tree scored the method against ground truth on 180 images:
- 41 with synthetic clipping;
- 73 bracketed pairs from SI-HDR;
- 62 bracketed pairs from Fairchild's HDR Photographic Survey;
- 4 pairs from Iain's collection of hard cases.

The first score scales each render so that the clipping level sits at luminance 1, applies the
tone curve `rgb / (1 + Y)`, and takes the mean ΔE2000 over the clipped pixels. A second score runs
the renders through sigmoid's "smooth" preset with exposure set for the mid-tones. These results
predate experiment values 10 to 15.

- **Averages favor inpaint opposed.** Its mean is 7.64, against 8.08 for experiment 7 with
  write-back 4. On partly clipped pixels it is 5.67 against 6.67.
- **The method is an alternative where the defaults fail.** On 52 images, the better of inpaint
  opposed and segmentation based scores above ΔE 10. Experiment 7 with write-back 4 is better by at
  least 2 ΔE and 20% on 31 of them.
- **It wins on colored light and colored objects,** such as candles, neon, lamps and a magenta
  flower.
- **It keeps lightness detail in deep clipped areas.** There, its local contrast is 0.74 of the
  truth's, against 0.56 for inpaint opposed. In the maintainer's side-by-side comparisons, this
  showed as detail in faces, hair and lit walls.
- **It loses on nearly neutral skies next to trees, roofs and rock.** The hue estimate takes the
  colors of those objects, and the sky turns pink or cyan.
- **A tone mapper hides most of the wins.** Under sigmoid "smooth", they shrink to 1 or 2 ΔE, while
  the sky artifacts stay visible.

---

## Known problems

- **Colors of other objects in the hue estimate.** The continuous border helps where two channels
  are measured. Where the sky has only one measured channel, the test compares only that channel and
  the clipping level, so a bright object next to the sky can pass.
- **One measured channel.** In a sky with green and blue clipped, red alone says nothing about the
  hue. The border hue may be wrong, and opposed's estimate turns pink as red rises toward the
  horizon.
- **Speckles where partly and fully clipped pixels mix.** Noise scatters fully clipped pixels along
  the edge of a clipped sky. The white fills make them visible. The edge fill gives them the hue
  around them, but they stay about 5 to 10% brighter, because their clipped red cannot go below its
  input.
- **A darker white with opposed's estimate.** Near the Sun, the estimate keeps the solution near the
  clipping level, and the fully clipped core is interpolated from that level. On one test image the
  core came out 1.9 EV darker than with the other white fills.
- **White in large light sources.** The edge fill and the white fills remove their color, and candle
  flames turn white.
- **Shallow clipping.** On one barely clipped sky, the hue ratio from the border raised the clipped
  green by 30%, while the truth was only 2% above the unreconstructed value.
- **Hard edges without a tone mapper.** The white and edge fills give a fully clipped area nearly
  equal channels. Without a tone mapper, once the area passes display white, all three channels
  clip together, and it turns flat white with a sharp edge against the partly clipped pixels next to
  it. Inpaint opposed leaves unequal channels there, so the same area keeps a tint and has no such
  edge.

---

## Related code

- **inpaint opposed** (`src/iop/hlreconstruct/opposed.c`) and **segmentation based**
  (`src/iop/hlreconstruct/segbased.c`) are the defaults the method is compared with.
  `_gd_opposed_hue()` mirrors opposed's estimate.
- **guided laplacians** (`src/iop/hlreconstruct/laplacian.c`) uses the same bilinear window
  interpolation, and a clipping level of the same form with factor 0.995 instead of 0.98.
