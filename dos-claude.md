# Diffuse or Sharpen: darktable vs Ansel Fork Code Review

## Summary

The Ansel fork of `diffuse.c` contains several significant mathematical
improvements, bug fixes, and CPU/GPU consistency fixes that are directly
relevant to darktable. The companion article ("The mathematics of diffuse
or sharpen") documents the mathematical model that the Ansel code now
implements. Our (darktable) code is the *older* version and does not match
the article in the key area of the regularization/energy metric.

---

## 1. Exposure-Invariant Band Energy Regularization (BUG FIX / MATH)

**This is the single most important change.**

### What darktable does (current, WRONG per the article)

In `heat_PDE_diffusion()` (darktable `diffuse.c:1097-1118`):

```c
// Accumulate raw HF energy:
variance[c] += sqf(neighbour_pixel_HF[k][c]);

// Scale by radius^2:
variance[c] = variance_threshold + variance[c] * regularization_factor;
// where regularization_factor = regularization * current_radius_square / 9.f
```

And the final update divides by this `variance`:
```c
acc[c] = (HF[index + c] * strength + acc[c] / variance[c]);
```

### What Ansel does (CORRECT per the article)

In `heat_PDE_diffusion()` (Ansel `diffuse.c:805-825`):

```c
// Accumulate exposure-normalized HF/LF energy ratio:
const dt_aligned_pixel_simd_t safe_lf = dt_simd_max_zero(lf_value - flt_min) + flt_min;
const dt_aligned_pixel_simd_t ratio = hf_value / safe_lf;
energy += ratio * ratio;

// Pre-normalized regularization (computed in caller):
energy = variance_threshold_v + energy * normalized_regularization_v;
// where normalized_regularization = regularization / 9.f * sqf(real_radius)
```

### Why this matters

The article (Section "Defining well-behaved frequency filters for photographs",
equations 50-54) explains that:

- Signal variance scales with the square of the exposure factor.
- Using raw `sum(HF^2)` makes the regularization exposure-dependent: the
  same scene exposed differently gets different sharpening/diffusion behavior.
- Normalizing by `LF` (i.e., `sum((HF/LF)^2)`) makes the metric
  exposure-invariant.

**Practical consequence**: In darktable, dark regions get over-sharpened
compared to bright regions because the raw HF^2 is much smaller in
dark areas (lower denominator), while in Ansel the HF/LF ratio is
exposure-invariant.

### Ansel commit

`d7b0cfab` ("Fix diffuse & sharpen bugs") introduced this change on both
CPU and GPU paths. The commit message references the article directly:
`See https://ansel.photos/en/resources/diffuse-or-sharpen-math/`

---

## 2. CPU/GPU Consistency: Use neighbour_pixel[4] Instead of Re-reading Center Pixel

### What darktable does (INCONSISTENT)

In the OpenCL kernel (`diffuse.cl:282-287`), darktable re-reads the center
pixel from the image for the final update:

```c
float4 hf = read_imagef(HF, samplerA, (int2)(x, y));
acc = (hf * strength + acc / variance);
float4 lf = read_imagef(LF, samplerA, (int2)(x, y));
out = fmax(acc + lf, 0.f);
```

Meanwhile, the CPU path (`diffuse.c:1011-1020`) fetches these into the
`neighbour_pixel_HF[4]` / `neighbour_pixel_LF[4]` arrays. Since the
center pixel IS `neighbour_pixel[4]` (index `[1][1]` in the 3x3 grid),
re-reading it from the image is redundant at best and could differ if
the GPU texture cache returns stale data.

### What Ansel does (CORRECT)

After commit `6fbe2a41` ("Fix D & S GPU vs. CPU inconsistencies for real"),
Ansel uses `neighbour_pixel_HF[4]` and `neighbour_pixel_LF[4]` in both the
CPU and GPU paths:

```c
// GPU (diffuse.cl):
const float4 acc
    = neighbour_pixel_HF[4] * strength
      + (...) / energy;
out = fmax(acc + neighbour_pixel_LF[4], 0.f);
```

This ensures identical arithmetic on CPU and GPU.

### Ansel commits

- `c10ae197` ("diffuse or sharpen: fix post-optimization regression on GPU, better align CPU-GPU code")
- `6fbe2a41` ("Fix D & S GPU vs. CPU inconsistencies for real")

---

## 3. GPU `else` Branch Missing `fmax(..., 0.f)` Clamp

### What darktable does (BUG)

In the GPU kernel's `else` (no-mask/opacity=0) branch (`diffuse.cl:291-294`):

```c
float4 hf = read_imagef(HF, samplerA, (int2)(x, y));
float4 lf = read_imagef(LF, samplerA, (int2)(x, y));
out = hf + lf;
```

No `fmax(out, 0.f)` clamp is applied. The CPU path similarly lacks the clamp
in the else branch (`diffuse.c:1137`):
```c
out[index + c] = HF[index + c] + LF[index + c];
```

### What Ansel does (CORRECT)

Ansel applies `fmax(..., 0.f)` in the `else` branch of **both** CPU and GPU:

```c
// GPU (diffuse.cl:289):
out = fmax(hf + lf, 0.f);

// CPU (diffuse.c:932-936):
dt_store_simd_aligned(out + index, dt_simd_max_zero(
    dt_load_simd_aligned(HF + index) + dt_load_simd_aligned(LF + index)));
```

Without this clamp, negative pixel values can leak through the `else`
path, which is a correctness bug when the module operates in
scene-referred linear RGB.

### Ansel commit

`6fbe2a41` ("Fix D & S GPU vs. CPU inconsistencies for real")

---

## 4. Regularization Pre-computation (normalized_regularization)

### What darktable does

Darktable computes the regularization factor inside the PDE kernel:

```c
// diffuse.cl line 189:
const float4 regularization_factor = regularization * current_radius_square / 9.f;
```

And passes `regularization`, `variance_threshold`, and `current_radius_square`
as separate kernel arguments.

### What Ansel does

Ansel pre-computes `normalized_regularization = regularization / 9.f * sqf(real_radius)`
on the host side and passes it as a single parameter. This:

1. Reduces the number of kernel arguments (from 15 to 14 in the PDE kernel).
2. Moves computation out of the per-pixel GPU kernel.
3. Makes the CPU and GPU code structurally identical.

### Ansel commits

- `d7b0cfab` ("Fix diffuse & sharpen bugs")
- `f24440d5` ("Fix diffuse again") -- final cleanup of kernel signature

---

## 5. GPU Uses `native_sqrt` Instead of `dtcl_sqrt`

### What darktable does

```c
const float4 magnitude_grad = dtcl_sqrt(sqf(gradient[0]) + sqf(gradient[1]));
```

### What Ansel does

```c
const float4 magnitude_grad = native_sqrt(sqf(gradient[0]) + sqf(gradient[1]));
```

`native_sqrt` is faster on GPU hardware (uses hardware intrinsics) and is
sufficiently accurate for the gradient magnitude computation. This is a
straightforward performance improvement with no accuracy concern in this
context (the result is only used for direction normalization and exp
damping).

### Ansel commit

Part of the GPU alignment in `c10ae197` / `f24440d5`.

---

## 6. FMA-Friendly Accumulation Rewrite

### What darktable does

```c
for(size_t k = 0; k < 9; k++)
{
    for_each_channel(c, ...)
    {
        derivatives[0][c] += kern_first[k][c] * neighbour_pixel_LF[k][c];
        // ...
        variance[c] += sqf(neighbour_pixel_HF[k][c]);
    }
}
// ...
for(size_t k = 0; k < 4; k++)
{
    for_each_channel(c, ...)
        acc[c] += derivatives[k][c] * ABCD[k];
}
```

### What Ansel does

```c
// FMA-friendly form: a*b + c (accumulator last)
derivatives[0] = kern_first[k] * neighbour_pixel_LF[k] + derivatives[0];
// ...
dt_aligned_pixel_simd_t update = derivatives[0] * ABCD[0];
update = derivatives[1] * ABCD[1] + update;
update = derivatives[2] * ABCD[2] + update;
update = derivatives[3] * ABCD[3] + update;
const dt_aligned_pixel_simd_t acc = neighbour_pixel_HF[4] * strength_v + update / energy;
```

The `a * b + accumulator` form is the canonical FMA pattern that compilers
can map to hardware FMA instructions. The `accumulator += a * b` form is
equivalent mathematically but some compilers fail to recognize it as FMA.

### Ansel commit

`1565deb3` ("Diffuse or sharpen: minor FMA nudging")

---

## 7. Which Version Matches the Article?

**Ansel matches the article. Darktable does not.**

The critical equation from the article is equation (53):

> We use the exposure-invariant band energy:
> `E_s(x) = sum_k (HF_s(x_k) / LF_s(x_k))^2`

And equation (54):

> And its local average:
> `<E_s> = (1/9) * sum over 3x3 stencil of E_s`

Darktable uses `sum(HF^2)` -- the raw squared high-frequency energy
without the LF normalization. This is the formula that the article
explicitly calls out as the *old* approach that was replaced.

Additionally, the article section "Normalizing scale and spatial coverage"
discusses how the `sqf(real_radius)` factor arises naturally from the
Gaussian variance increment between scales, matching Ansel's
`normalized_regularization = regularization / 9.f * sqf(real_radius)`.
In darktable, `current_radius` (without zoom) is used, which means the
regularization doesn't properly account for the zoom level during
darkroom preview.

---

## Recommended Changes to darktable's `diffuse.c`

### Priority 1 (Bug fixes)

1. **Change the energy metric from `sum(HF^2)` to `sum((HF/LF)^2)`**
   in `heat_PDE_diffusion()` (~line 1097-1118).
   This is the main mathematical fix. Both CPU and GPU paths need updating.
   
   *Ansel commit: `d7b0cfab` ("Fix diffuse & sharpen bugs")*

2. **Pre-compute `normalized_regularization` in the caller** instead of
   passing raw `regularization` + `current_radius_square` to the PDE.
   Use `real_radius` (i.e., `current_radius * zoom`) instead of
   `current_radius` in the regularization factor.
   
   This requires:
   - Changing the `heat_PDE_diffusion()` signature
   - Changing the OpenCL `diffuse_pde` kernel signature (removing
     `current_radius_square` argument, changing `regularization` to
     `normalized_regularization`)
   - Updating `wavelets_process()` and `wavelets_process_cl()` to compute
     `normalized_regularization = regularization / 9.f * sqf(current_radius * zoom)`
   
   *Ansel commits: `d7b0cfab`, `f24440d5`*

3. **Add `fmax(0.f)` clamp in the `else` branch** of both CPU and GPU
   paths (when opacity=0, the passthrough should still clamp negatives).
   
   *Ansel commit: `6fbe2a41` ("Fix D & S GPU vs. CPU inconsistencies for real")*

4. **Use `neighbour_pixel_HF[4]` / `neighbour_pixel_LF[4]`** instead of
   re-reading the center pixel in the GPU kernel's final update.
   
   *Ansel commits: `c10ae197`, `6fbe2a41`*

### Priority 2 (Performance / consistency)

5. **Use `native_sqrt` instead of `dtcl_sqrt`** in the GPU kernel for
   gradient magnitude computation.
   
   *Ansel commit: part of `c10ae197` / `f24440d5`*

6. **Rewrite accumulation loops for FMA friendliness**: use `a*b + acc`
   form instead of `acc += a*b`. This is a minor optimization that helps
   on CPUs with hardware FMA.
   
   *Ansel commit: `1565deb3` ("Diffuse or sharpen: minor FMA nudging")*

### Not recommended to port (Ansel-specific infrastructure)

- `dt_aligned_pixel_simd_t` / `dt_simd_set1()` / `dt_load_simd_aligned()` --
  Ansel's SIMD abstraction layer. darktable uses `for_each_channel()` and
  `dt_aligned_pixel_t` which achieve the same via compiler autovectorization.
  Porting would require bringing in Ansel's SIMD infrastructure headers.

- `dt_dev_get_module_scale()` / `dt_pixelpipe_cache_alloc_*()` -- Ansel's
  pipeline API changes. darktable uses `piece->iscale / roi_in->scale`.

- `__DT_CLONE_TARGETS__` / `__OMP_PARALLEL_FOR__()` -- Ansel's OpenMP
  macro refactoring. Cosmetic differences.

- Local-memory OpenCL B-spline kernels (`kernel_filmic_bspline_*_local`) --
  these are a separate optimization in the B-spline decomposition, not in
  the diffuse PDE itself.

- DIFFUSE_V3 / `normalize_band_energy` parameter -- explicitly out of scope
  per instructions, and disabled (`#define DIFFUSE_V3 0`) in Ansel anyway.

---

## Ansel Commit Reference Summary

| Commit | Description | Relevance |
|--------|-------------|-----------|
| `d7b0cfab` | Fix diffuse & sharpen bugs | **Critical**: exposure-invariant energy, normalized_regularization, zoom-aware scaling |
| `c10ae197` | Fix post-optimization regression on GPU | GPU alignment: store LF/HF arrays, use neighbour_pixel[4] |
| `6fbe2a41` | Fix D & S GPU vs. CPU inconsistencies for real | GPU/CPU parity: neighbour_pixel[4], fmax clamp in else branch |
| `f24440d5` | Fix diffuse again | Final GPU kernel cleanup, revert to explicit compute_kern signature |
| `1565deb3` | Minor FMA nudging | FMA-friendly accumulation rewrite |
| `3f03ad57` | Fix module scaling issue | `dt_dev_get_module_scale()` -- Ansel API, not directly portable |
| `85188c39` | Optimize D or S | SIMD type migration (`dt_aligned_pixel_simd_t`) -- Ansel infrastructure |
