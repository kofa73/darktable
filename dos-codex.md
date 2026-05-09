# Diffuse Or Sharpen Review: darktable vs Ansel

## Scope

Reviewed:

- `src/iop/diffuse.c` in this repo
- `data/kernels/diffuse.cl` in this repo, because the Ansel history includes CPU/GPU consistency fixes and the live CPU math cannot be reviewed in isolation
- `dos-maths/Ansel _ The mathematics of diffuse or sharpen.html`
- `/workspace/ansel/src/iop/diffuse.c`
- `/workspace/ansel/data/kernels/diffuse.cl`
- recent Ansel history affecting the live code path, especially:
  - `d7b0cfabf236c7fdf43902a3bfafcb9a88c88a1e` `Fix diffuse & sharpen bugs`
  - `85188c39584bf06bf75db86be7918aa2e5869b71` `Optimize D or S`
  - `6fbe2a4167ec3657031605506ed94059f780893d` `Fix D & S GPU vs. CPU inconsistencies for real`
  - `c10ae1976f81622489981debd88582af319d260c` `diffuse or sharpen: fix post-optimization regression on GPU, better align CPU-GPU code`
  - `f24440d58873426e49f34e0540866e3fef884c9c` `Fix diffuse again`
  - `1565deb3e1af679c5ce9a575eaf9129d2df30b7a` `Diffuse or sharpen: minor FMA nudging`
  - `42d12c96be25993bb4def2ad67094c57c7fbf4e5` `Implement target clones everywhere relevante`

`DIFFUSE_V3` was ignored as requested.

## Which version matches the article better?

The Ansel fork is the better match for the article, clearly.

The article explicitly describes:

- a scale-normalized local high-frequency band-energy regularizer,
- additional normalization by local low-frequency energy,
- identical CPU and OpenCL mathematics,
- a wavelet-domain solver whose physical support comes from the a trous stride rather than a larger PDE stencil.

That is what the live Ansel code now does. Our darktable code still implements the older regularizer:

- CPU: local raw HF variance `sum(HF^2)` scaled by `regularization * current_radius_square / 9`
- OpenCL: same older raw HF variance model

The article instead describes what Ansel now implements:

- local `sum((HF / LF)^2)` band energy,
- guarded against divide-by-zero,
- then multiplied by a scale-normalized factor based on the physical blur radius.

So on article alignment, Ansel wins on both the math and the CPU/GPU consistency story.

## Main findings

### 1. Our live regularizer is not the one documented in the article

darktable currently computes:

- CPU: `variance[c] += sqf(neighbour_pixel_HF[k][c])`
- then `variance[c] = variance_threshold + variance[c] * regularization_factor`
- with `regularization_factor = regularization * current_radius_square / 9.f`

That is the old raw HF-variance penalty.

Ansel changed this in `d7b0cfabf236c7fdf43902a3bfafcb9a88c88a1e` and then cleaned/corrected it in `6fbe2a4167ec3657031605506ed94059f780893d`:

- accumulate `energy += sqf(HF / safe_LF)` over the 3x3 support,
- define `safe_LF = max(LF - FLT_MIN, 0) + FLT_MIN`,
- then use `energy = variance_threshold + energy * normalized_regularization`.

This is the single most important math delta.

Why it matters:

- It matches the article.
- It makes the regularizer relative to the local LF signal level instead of absolute HF magnitude.
- It decouples the penalty from raw scene intensity better than `sum(HF^2)`.
- It is applied identically in CPU and OpenCL in Ansel.

Conclusion: this should be imported.

### 2. Our OpenCL path is also on the old math, so CPU/GPU behavior remains coupled to the old model

darktable `data/kernels/diffuse.cl` still mirrors the old variance model:

- raw `variance += HF^2`
- scale with `regularization * current_radius_square / 9`

Ansel `data/kernels/diffuse.cl` was updated alongside the CPU path, mainly in:

- `d7b0cfabf236c7fdf43902a3bfafcb9a88c88a1e`
- `6fbe2a4167ec3657031605506ed94059f780893d`

This is not optional if the goal is to bring in the important fix. Porting only the CPU part would reintroduce a CPU/OpenCL semantic split.

Conclusion: any import of the math fix must include `data/kernels/diffuse.cl` and the OpenCL argument setup in `src/iop/diffuse.c`.

### 3. Ansel also fixed the OpenCL wavelet blur pass ordering to match the CPU path

Our OpenCL decomposition still does:

- horizontal pass into `HF[s]`
- vertical pass into `buffer_out`

Ansel now does:

- vertical pass into `HF[s]`
- horizontal pass into `buffer_out`

The relevant history is:

- `c10ae1976f81622489981debd88582af319d260c`
- `f24440d58873426e49f34e0540866e3fef884c9c`

The final Ansel comment is explicit: keep the same separable order as the CPU path.

Given that the same blur operator is used to build the wavelet ladder that drives the PDE update, this is a real correctness issue, not a cosmetic refactor.

Conclusion: this should be imported together with the regularizer work.

### 4. The Ansel SIMD refactor is real, but it is not a direct cherry-pick into darktable

Ansel `85188c39584bf06bf75db86be7918aa2e5869b71` rewrites the inner solver around:

- `dt_aligned_pixel_simd_t`
- `dt_simd_set1`
- aligned SIMD loads/stores
- nontemporal stores on the final reconstruction pass
- branch-reduced normalization and accumulation

That is a meaningful performance improvement, and `1565deb3e1af679c5ce9a575eaf9129d2df30b7a` further nudges expression shape for FMA-friendly codegen.

However, this repo does not appear to expose the same helper layer under the same names. A straight transplant of the Ansel SIMD code is therefore unlikely to compile unchanged.

Conclusion:

- the optimization ideas are relevant,
- but they are a second step after the math/bug-fix import,
- and they need adaptation to darktable's current SIMD abstraction layer instead of a blind copy.

### 5. `42d12c96be25993bb4def2ad67094c57c7fbf4e5` is worth considering, but only after the functional fixes

Ansel adds `__DT_CLONE_TARGETS__` on the hot helpers and solver entry points.

That is relevant for performance, but it is not part of the article/math alignment and not a bug fix by itself.

Conclusion: optional follow-up, not first priority.

## Concrete changes needed in our code

These are the changes I would make to bring in the important Ansel fixes.

### A. Replace the old HF variance regularizer with the Ansel HF/LF band-energy regularizer

In `src/iop/diffuse.c`:

1. Change `heat_PDE_diffusion()` so it no longer takes:
   - `regularization`
   - `current_radius_square`

2. Instead pass a precomputed:
   - `normalized_regularization`

3. Inside the 3x3 neighborhood loop:
   - load both HF and LF neighbor samples,
   - compute `safe_lf = max(lf - FLT_MIN, 0) + FLT_MIN`,
   - accumulate `energy += sqf(hf / safe_lf)`.

4. After the neighborhood fetch:
   - compute `energy = variance_threshold + energy * normalized_regularization`.

5. Remove the old `variance` accumulation over raw HF values.

This change comes from:

- introduction: `d7b0cfabf236c7fdf43902a3bfafcb9a88c88a1e`
- corrected CPU/GPU-safe form with `safe_lf`: `6fbe2a4167ec3657031605506ed94059f780893d`

### B. Compute the scale factor using physical blur radius, not `current_radius_square`

In `wavelets_process()` in `src/iop/diffuse.c`, compute:

- `real_radius = current_radius * zoom`
- `normalized_regularization = regularization / 9.f * sqf(real_radius)`

and pass that to `heat_PDE_diffusion()`.

This is the live non-`DIFFUSE_V3` Ansel behavior from:

- `d7b0cfabf236c7fdf43902a3bfafcb9a88c88a1e`

This matters because the article frames the normalization in terms of physical scale, not just the discrete wavelet step index.

### C. Mirror the same regularizer change in OpenCL

In both:

- `src/iop/diffuse.c`
- `data/kernels/diffuse.cl`

make the OpenCL interface and kernel do the same thing as the CPU path:

- pass `normalized_regularization` instead of `regularization` and `current_radius_square`,
- accumulate `energy += sqf(HF / safe_LF)`,
- use the same guarded `safe_LF`,
- compute `energy = variance_threshold + energy * normalized_regularization`,
- use that in the update denominator.

Relevant Ansel commits:

- `d7b0cfabf236c7fdf43902a3bfafcb9a88c88a1e`
- `6fbe2a4167ec3657031605506ed94059f780893d`

### D. Fix the OpenCL separable blur pass ordering

In our `wavelets_process_cl()`:

- run the vertical B-spline pass first into the temporary,
- then the horizontal pass into the low-pass output,
- matching the CPU decomposition ordering.

Relevant Ansel commits:

- `c10ae1976f81622489981debd88582af319d260c`
- `f24440d58873426e49f34e0540866e3fef884c9c`

This should be treated as part of the bug-fix import, not as an optimization.

### E. Then, optionally, port the inner-loop optimization strategy

After the math is matched, consider adapting the ideas from:

- `85188c39584bf06bf75db86be7918aa2e5869b71`
- `1565deb3e1af679c5ce9a575eaf9129d2df30b7a`
- `42d12c96be25993bb4def2ad67094c57c7fbf4e5`

That means:

- cache the 3x3 neighborhood once,
- accumulate derivative terms without reloading center pixels,
- use branch-reduced normalization of gradient directions,
- consider nontemporal stores for the final reconstruction pass,
- consider target clones on the hottest helpers.

But this is a follow-up. It should not block importing the functional fixes above.

## Recommendation

Priority order:

1. Import the regularizer/math change from `d7b0cfab` plus the consistency corrections from `6fbe2a41`.
2. Import the OpenCL wavelet-pass ordering fix from `c10ae197` and `f24440d5`.
3. Only then consider adapting the SIMD/perf work from `85188c39`, `1565deb3`, and optionally `42d12c96`.

If the question is "which codebase currently reflects the article?", the answer is Ansel.

If the question is "what should we bring over first?", the answer is:

- the HF/LF band-energy regularizer,
- its OpenCL mirror,
- and the OpenCL pass-order consistency fix.
