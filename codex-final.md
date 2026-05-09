# CPU-Path Verification Summary for Future Planning

## Scope of this summary

This summary is intentionally limited to:

- `src/iop/diffuse.c` in darktable,
- `src/iop/diffuse.c` in `/workspace/ansel`,
- the mathematics article in `dos-maths/Ansel _ The mathematics of diffuse or sharpen.html`.

It explicitly excludes OpenCL for now. The stated goal is that OpenCL should later be aligned 1:1 with the CPU path, so this document focuses on deciding what the CPU reference should be first.

This is not yet a plan. It is planning input derived from direct comparison of:

- current darktable CPU code,
- current Ansel CPU code,
- the article text.

## Executive summary

The core conclusion remains:

- darktable CPU still implements the older regularization model based on raw HF variance;
- Ansel CPU implements the newer exposure-invariant HF/LF band-energy model;
- the article clearly supports the HF/LF exposure-invariant regularizer and clearly does not support the old raw-HF-variance model.

However, one important nuance emerged from checking the article directly:

- the article also explicitly says that extra scale normalization of the regularization term was tried and then abandoned;
- Ansel’s current CPU code still multiplies the new HF/LF energy by `sqf(real_radius)`;
- therefore Ansel is a better match than darktable on the main mathematical point, but it is not an exact implementation of the final article wording on scale normalization.

That is the main issue future planning will need to resolve carefully.

## 1. What darktable CPU currently does

### 1.1 Regularization model

darktable’s `heat_PDE_diffusion()` still takes:

- `regularization`,
- `variance_threshold`,
- `current_radius_square`.

It computes:

- `regularization_factor = regularization * current_radius_square / 9.f`,
- accumulates `variance += sqf(neighbour_pixel_HF[k])`,
- then applies `variance = variance_threshold + variance * regularization_factor`,
- and uses `acc / variance` in the final update.

Relevant code:

- [src/iop/diffuse.c](/workspace/darktable/src/iop/diffuse.c#L950)
- [src/iop/diffuse.c](/workspace/darktable/src/iop/diffuse.c#L985)
- [src/iop/diffuse.c](/workspace/darktable/src/iop/diffuse.c#L1099)

This is the old raw-HF-variance denominator.

### 1.2 Radius and zoom handling

In the CPU process path, darktable computes:

- `scale = fmaxf(piece->iscale / roi_in->scale, 1.f)`,
- `final_radius = (data->radius + data->radius_center) * 2.f / scale`.

In wavelet synthesis it computes:

- `current_radius = equivalent_sigma_at_step(B_SPLINE_SIGMA, s)`,
- `real_radius = current_radius * zoom`,
- but still passes `sqf(current_radius)` into `heat_PDE_diffusion()`, not `sqf(real_radius)`.

Relevant code:

- [src/iop/diffuse.c](/workspace/darktable/src/iop/diffuse.c#L785)
- [src/iop/diffuse.c](/workspace/darktable/src/iop/diffuse.c#L1238)
- [src/iop/diffuse.c](/workspace/darktable/src/iop/diffuse.c#L1276)
- [src/iop/diffuse.c](/workspace/darktable/src/iop/diffuse.c#L1383)

### 1.3 Else-branch reconstruction behavior

In the CPU `else` branch, darktable simply copies:

- `out = HF + LF`

without clamping to non-negative values.

Relevant code:

- [src/iop/diffuse.c](/workspace/darktable/src/iop/diffuse.c#L1133)

## 2. What Ansel CPU currently does

### 2.1 Regularization model

Ansel’s `heat_PDE_diffusion()` no longer uses raw HF variance.

Instead it:

- loads both HF and LF neighbor samples,
- computes `safe_lf = max(lf_value - FLT_MIN, 0) + FLT_MIN`,
- computes `ratio = hf_value / safe_lf`,
- accumulates `energy += ratio * ratio`,
- then computes `energy = variance_threshold + energy * normalized_regularization`.

Relevant code:

- [ansel src/iop/diffuse.c](/workspace/ansel/src/iop/diffuse.c#L803)
- [ansel src/iop/diffuse.c](/workspace/ansel/src/iop/diffuse.c#L817)
- [ansel src/iop/diffuse.c](/workspace/ansel/src/iop/diffuse.c#L825)

This is the exposure-relative HF/LF band-energy denominator.

### 2.2 Radius and zoom handling

Ansel computes:

- `zoom = fmaxf(dt_dev_get_module_scale(pipe, roi_in), 1.f)`,
- `final_radius = (data->radius + data->radius_center) * 2.f / zoom`,
- `real_radius = current_radius * zoom`,
- `normalized_regularization = regularization / 9.f * sqf(real_radius)` in the non-`DIFFUSE_V3` path.

Relevant code:

- [ansel src/iop/diffuse.c](/workspace/ansel/src/iop/diffuse.c#L1045)
- [ansel src/iop/diffuse.c](/workspace/ansel/src/iop/diffuse.c#L1054)
- [ansel src/iop/diffuse.c](/workspace/ansel/src/iop/diffuse.c#L1163)

So Ansel differs from darktable in two distinct ways:

- it uses the HF/LF energy instead of raw HF variance;
- it also ties the normalization term to `real_radius`, not just `current_radius`.

### 2.3 Else-branch reconstruction behavior

In the CPU `else` branch, Ansel reconstructs with a non-negativity clamp.

Relevant code:

- [ansel src/iop/diffuse.c](/workspace/ansel/src/iop/diffuse.c#L928)

## 3. What the article clearly supports

### 3.1 Exposure-invariant regularization

The article explicitly explains:

- signal variance scales with the square of exposure;
- both HF and LF scale linearly with exposure;
- therefore the HF/LF ratio is exposure-invariant;
- the regularization should be based on exposure-invariant band energy over the 3x3 support.

Relevant documentation region:

- [dos-maths article](/workspace/darktable/dos-maths/Ansel%20_%20The%20mathematics%20of%20diffuse%20or%20sharpen.html#L309)

This directly supports Ansel’s switch from raw `sum(HF^2)` to a local `sum((HF/LF)^2)`-style energy estimate and directly contradicts darktable’s current denominator.

### 3.2 Full-resolution / zoom-stable radius interpretation

The article also explicitly states that:

- the weighting scheme should be stable no matter the zoom level;
- radii are taken in raw-image, full-resolution space;
- downscaled previews should be reweighted according to equivalent full-resolution radius.

Relevant documentation region:

- [dos-maths article](/workspace/darktable/dos-maths/Ansel%20_%20The%20mathematics%20of%20diffuse%20or%20sharpen.html#L220)

This supports the need for correct full-resolution-aware scale handling in the CPU path.

Importantly, this supports a semantic requirement, not a specific helper function from Ansel.

### 3.3 No additional scale normalization of the Laplacian term

This is the point that matters most for future planning.

In the “Normalizing scale and spatial coverage” section, the article says:

- the Laplacian support already expands with the same stride as the wavelet blur;
- because of that, the Laplacian already follows the variance spreading across scales;
- therefore no additional scale normalization is needed there.

Then it adds that:

- an earlier `sigma_s^2` boost on the regularization parameter was the initial implementation;
- another attempt at scale-invariant energy normalization was also tried;
- both attempts were abandoned.

Relevant documentation region:

- [dos-maths article](/workspace/darktable/dos-maths/Ansel%20_%20The%20mathematics%20of%20diffuse%20or%20sharpen.html#L317)

This directly affects how we should interpret Ansel’s current `normalized_regularization = regularization / 9 * sqf(real_radius)`.

## 4. Where darktable and Ansel differ in ways that matter for CPU planning

### 4.1 Difference A: raw HF variance vs HF/LF band energy

This is a real, article-backed mathematical difference.

Assessment:

- darktable is on the old model;
- Ansel is on the corrected model;
- the article clearly sides with Ansel here.

Planning implication:

- this is the strongest CPU-side candidate for bring-back;
- it should be treated as a functional correction, not an optimization.

### 4.2 Difference B: `current_radius_square` vs `sqf(real_radius)`

This is also a real code difference, but its status is more subtle.

darktable:

- scales the denominator by `sqf(current_radius)`.

Ansel:

- scales the denominator by `sqf(real_radius) = sqf(current_radius * zoom)`.

The article:

- supports zoom/full-resolution stability for radius selection;
- but also says no additional scale normalization is needed and explicitly says earlier extra scale-normalization attempts were abandoned.

Assessment:

- the article supports “radii must be interpreted in full-resolution space”;
- the article does not clearly support Ansel’s current extra `sqf(real_radius)` normalization term;
- therefore it would be unsafe to assume that Ansel’s exact scaling expression should be imported without further judgment.

Planning implication:

- this item needs explicit decision-making later;
- it should not be bundled under “obviously article-correct” in the future plan.

### 4.3 Difference C: darktable zoom expression vs Ansel zoom helper

The reports previously disagreed here in a way that can now be cleaned up.

darktable uses:

- `piece->iscale / roi_in->scale`

Ansel uses:

- `dt_dev_get_module_scale(pipe, roi_in)`.

Assessment:

- this is not fundamentally a dispute about “which helper function to port”;
- it is a dispute about whether darktable’s existing zoom computation has the same semantics Ansel now relies on;
- the article only requires correct full-resolution-aware behavior, not adoption of Ansel’s specific helper.

Planning implication:

- future planning should phrase this as “verify and, if needed, fix darktable’s CPU zoom/full-resolution radius mapping”;
- it should not phrase it as “import `dt_dev_get_module_scale()`”.

### 4.4 Difference D: non-negativity clamp in the CPU else branch

This is not documented in the maths article, but it is a real CPU behavior difference.

darktable:

- `out = HF + LF`

Ansel:

- clamps the reconstruction to non-negative values in the else branch too.

Assessment:

- this looks like a correctness/consistency fix rather than a math-model change;
- the article does not settle it directly;
- it is still likely relevant for parity and robustness.

Planning implication:

- this should remain on the candidate list for later planning,
- but it should be clearly labeled as a CPU correctness/parity fix, not as article-derived math.

## 5. Items that should not be treated as CPU math decisions

### 5.1 SIMD refactor

Ansel rewrites much of the CPU path around:

- `dt_aligned_pixel_simd_t`,
- SIMD loads/stores,
- branch-reduced normalization,
- target clones,
- optional non-temporal stores.

Relevant code:

- [ansel src/iop/diffuse.c](/workspace/ansel/src/iop/diffuse.c#L822)
- [ansel src/iop/diffuse.c](/workspace/ansel/src/iop/diffuse.c#L900)

The article does not require any of that implementation strategy.

Assessment:

- these are implementation and performance choices;
- they should not be conflated with the mathematical deltas.

Planning implication:

- keep them out of the first CPU functional discussion unless they are needed to make a later port practical.

### 5.2 FMA-friendly accumulation rewrite

Ansel uses `a = b * c + a`-style accumulation and a staged `update` expression.

darktable uses the simpler `acc += derivative * coeff` style.

Relevant code:

- [ansel src/iop/diffuse.c](/workspace/ansel/src/iop/diffuse.c#L907)
- [darktable src/iop/diffuse.c](/workspace/darktable/src/iop/diffuse.c#L1119)

Assessment:

- this is performance/codegen shaping;
- it is not article-mandated CPU behavior.

Planning implication:

- treat as low-priority follow-up only.

### 5.3 Broader B-spline optimization work

Ansel’s `src/common/bspline.h` contains extra helpers such as:

- `reduce_2D_Bspline()`,
- `expand_2D_Bspline()`,
- `_bspline_horizontal_decimated()`.

Relevant code:

- [ansel src/common/bspline.h](/workspace/ansel/src/common/bspline.h#L165)

But the active CPU `diffuse.c` path in both repos still calls `decompose_2D_Bspline()` directly:

- [darktable src/iop/diffuse.c](/workspace/darktable/src/iop/diffuse.c#L1214)
- [ansel src/iop/diffuse.c](/workspace/ansel/src/iop/diffuse.c#L1023)

Assessment:

- those broader B-spline helpers are not the active point of divergence for the CPU path under review here;
- they should not be mistaken for required diffuse/sharpen CPU math changes.

Planning implication:

- keep them out of the core bring-back discussion unless later work broadens scope beyond `diffuse.c`.

## 6. Recommended framing for future planning

The future plan should probably separate CPU items into three buckets.

### 6.1 Article-backed functional changes

These have the strongest support:

- replace raw HF variance regularization with the exposure-invariant HF/LF band-energy model;
- keep the 3x3 local support structure described in the article;
- preserve full-resolution-aware radius interpretation across zoom levels.

### 6.2 CPU behavior changes that are relevant but not directly settled by the article

These should be evaluated, but labeled correctly:

- whether to clamp the CPU else-branch reconstruction to non-negative values;
- whether any CPU consistency cleanups around reconstruction should be imported alongside the main denominator change.

### 6.3 Explicit design decisions requiring judgment

These are the real unresolved planning questions:

- whether the future CPU target should include Ansel’s `sqf(real_radius)` normalization term as-is;
- or whether the article’s “no additional scale normalization is needed” wording means that Ansel’s current code should not be copied literally here;
- whether darktable’s current zoom computation is semantically sufficient, or whether it needs adjustment to match the article’s full-resolution-radius requirement.

## 7. Bottom line

The CPU-side findings can be reduced to five practical statements:

- darktable’s current denominator is definitely outdated relative to the article and relative to Ansel.
- Ansel’s HF/LF band-energy denominator is the right direction and is clearly closer to the article than darktable.
- Ansel’s additional `sqf(real_radius)` normalization is not clearly validated by the final article text; the article appears to argue against extra scale normalization.
- the zoom/full-resolution semantics matter, but that is a semantic requirement, not a mandate to copy Ansel’s exact helper interface.
- SIMD/FMA/B-spline optimization work should stay separate from the first CPU functional decisions.

That should be the baseline for later planning.
