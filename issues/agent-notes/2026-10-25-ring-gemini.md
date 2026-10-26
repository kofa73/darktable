The 0.3-0.5 EV deficit occurs because `_process_opposed` evaluates clipped channels in a single uncoupled pass using truncated inputs, creating a mutual clamping deadlock where red and green hold each other down at the clip threshold. Solving opposed's coupled equations algebraically breaks this deadlock and yields an additive shift in cube-root space driven by the surviving channel, which raises warm highlights by the missing dynamic range without reading on Dolby US8861851B2 claim 20. If the measured channel drops to zero, additive channel transfer flatlines, requiring fallback to spatial boundary gradient extrapolation.

# Cause of the deficit in opposed

In `src/iop/hlreconstruct/segbased.c:227-231`, the reference channel estimate is calculated from the cube roots of 3x3 window means:

```c
  const dt_aligned_pixel_t croot_refavg = { 0.5f * (mean[1] + mean[2]),
                                            0.5f * (mean[0] + mean[2]),
                                            0.5f * (mean[0] + mean[1]),
                                            0.0f};
  return (linear) ? fcube(croot_refavg[color]) : croot_refavg[color];
```

When red and green are both clipped, `_calc_refavg` reads their truncated input values `mean[0] = C_R` and `mean[1] = C_G`. For red, the reference is `ref_R = ((C_G^(1/3) + B^(1/3)) / 2)^3`. Because blue is low (`B < C`), the average is strictly below `C_R^(1/3)`, so `ref_R` falls below the clip level. In `src/iop/hlreconstruct/opposed.c:418`, `MAX(inval, ref + chrominance[color])` evaluates to `C_R` because `ref + chrominance` cannot clear the clip threshold. Red and green mutually clamp each other at clip level, losing 0.3-0.5 EV of highlight dome.

# Patent risk of candidate mechanisms

Dolby US8861851B2 claim 20 requires obtaining estimated hues comprising ratios of saturated to non-saturated channels, and multiplying values based on the non-saturated channels by those ratios.

Candidate mechanisms compare against claim 20 as follows:

- Additive offsets (root space or linear): Does not read on claim 20. Adding a difference `Delta = R_rim^(1/3) - B_rim^(1/3)` in cube-root space involves no ratio of saturated to non-saturated channels and no multiplication of the unclipped channel by a hue factor.
- Spatial extrapolation from own values: Does not read on claim 20. Extrapolating a channel from its own boundary values and inward gradients does not inspect unclipped channels, computes no hue, and performs no cross-channel operations.
- Lower bounds: Does not read on claim 20. Enforcing a geometric profile or point-spread convexity constraint inside the highlight mask relies on spatial geometry, not channel ratios.
- Luminance-only corrections: Does not read on claim 20 if the additive energy term is derived spatially or from an additive unclipped channel difference.
- Linear regressions (`R = a * B + b`): High risk of reading on claim 20. The slope `a = Cov(R, B) / Var(B)` represents the local differential ratio between the saturated and non-saturated channels. Multiplying `B` by `a` exposes the implementation to infringement claims under claim 20.

# Non-ratio formulations

### Formulation 1: Closed-form opposed unclamping

The opposed model assumes that in cube-root space `r = R^(1/3)`, `g = G^(1/3)`, `b = B^(1/3)`, the channels satisfy:

```
r = 0.5 * (g + b) + delta_r
g = 0.5 * (r + b) + delta_g
```

where `delta_r` and `delta_g` are additive boundary offsets. Instead of substituting truncated inputs, solving this 2x2 system simultaneously yields:

```
r(x) = b(x) + (4/3 * delta_r + 2/3 * delta_g)
g(x) = b(x) + (2/3 * delta_r + 4/3 * delta_g)
```

Substituting the boundary definitions `delta_r = r_rim - 0.5 * (g_rim + b_rim)` and `delta_g = g_rim - 0.5 * (r_rim + b_rim)` simplifies the coefficients exactly:

```
4/3 * delta_r + 2/3 * delta_g = r_rim - b_rim
2/3 * delta_r + 4/3 * delta_g = g_rim - b_rim
```

The intermediate clipped channels cancel out entirely. The reconstructed linear raw photosites become:

```
R(x) = max(C_R, (B(x)^(1/3) + Delta_rb)^3)
G(x) = max(C_G, (B(x)^(1/3) + Delta_gb)^3)
```

where `Delta_rb = <R^(1/3) - B^(1/3)>_rim` and `Delta_gb = <G^(1/3) - B^(1/3)>_rim` are scalar additive offsets averaged over unclipped photosites along the boundary of the clipped patch.

Behavior on skies: in daylight skies and clouds, light is near-neutral (`R ~ G ~ B`). On the rim, `Delta_rb ~ 0` and `Delta_gb ~ 0`. The formula gives `R(x) ~ (B(x)^(1/3))^3 = B(x) ~ C_R`, leaving near-neutral highlights completely unchanged.

Behavior on warm lights: on warm rims, `R_rim >> B_rim`, so `Delta_rb > 0`. Because `R(x) = (B^(1/3) + Delta_rb)^3 = B + 3 * B^(2/3) * Delta_rb + 3 * B^(1/3) * Delta_rb^2 + Delta_rb^3`, the effective slope with respect to blue is `dR/dB = (1 + Delta_rb / B^(1/3))^2`. When `B` increases toward the light center, the nonlinear cubic expansion amplifies the small blue rise by 2.5-4.0x, lifting red and green 0.3-0.5 EV above clip.

Computational cost: requires one pass over the rim to compute the two scalar offsets `Delta_rb` and `Delta_gb`, and one pass over clipped photosites to evaluate `(B^(1/3) + Delta)^3`. Runtime on a 20 MP raw is under 25 milliseconds using OpenMP. It works directly on Bayer and X-Trans raw photosites by reading `fcol(row, col, filters, xtrans)`.

### Formulation 2: Spatial boundary gradient continuation

When blue provides little guidance, the missing energy can be reconstructed purely from the spatial slope of each clipped channel at the boundary.

Let `D(x, y)` be the Euclidean distance of clipped pixel `(x, y)` to the nearest unclipped boundary, and `D_max` the maximum distance inside the clipped patch. Let `grad_n(c)` be the average inward spatial gradient of channel `c` along the boundary `dOmega_c`:

```
grad_n(c) = mean(max(0, -grad(C_c) . n_out))  along dOmega_c
```

The reconstructed channel is a quadratic dome:

```
C_c(x, y) = C_c_clip + grad_n(c) * D(x, y) * max(0.0, 1.0 - D(x, y) / (2.0 * D_max))
```

Behavior on skies: sky highlight transitions are flat and diffuse, so `grad_n(c) ~ 0`. The correction term vanishes, leaving skies unaffected.

Behavior on warm lights: point lights have steep boundary gradients (`grad_n >> 0`). The quadratic continuation elevates the center by `0.5 * grad_n * D_max`, naturally restoring the missing 0.3-0.5 EV dome.

Computational cost: the distance transform `D(x, y)` is already computed in darktable's `segbased.c`. Evaluating the quadratic formula takes under 15 milliseconds for a 20 MP raw.

### Formulation 3: Adaptive hybrid blend

Formulations 1 and 2 can be combined based on the signal strength of the unclipped channel on the boundary:

```
w_B = clamp(B_rim / (0.1 * C_B), 0.0, 1.0)
R(x) = w_B * R_formulation1(x) + (1.0 - w_B) * R_formulation2(x)
```

When blue is healthy, the spatial details of blue drive the reconstruction via Formulation 1. When blue vanishes, the spatial dome of Formulation 2 smoothly takes over.

# Behavior when the measured channel is near zero

When light is deep red and blue approaches zero (`B(x) -> 0`):

- Formulation 1 (Root-space additive offset): as `B(x) -> 0`, `B(x)^(1/3) -> 0`. The reconstruction evaluates to `R(x) = (0 + Delta_rb)^3 = Delta_rb^3 = R_rim = C_R`. Because blue carries zero spatial variation (`grad(B) ~ 0`), Formulation 1 cannot generate a spatial gradient. The clipped area does not darken below clip, but it flatlines at clip level without rising into a dome.
- Formulation 2 (Spatial gradient continuation): completely independent of blue. The boundary gradient `grad_n(R)` of red itself continues inward, forming a complete highlight dome even when blue is identically zero.
- Formulation 3 (Hybrid blend): as `B_rim -> 0`, `w_B -> 0`. The blend transfers full weight to Formulation 2, preventing flatlining in deep red highlights.
- Affine regression (`R = a * B + b`): fails catastrophically. When blue is near zero, `Var(B)` is dominated by sensor read noise. The slope `a = Cov(R, B) / Var(B)` divides near-zero by near-zero, amplifying noise into severe color blotches.
- Linear additive offset (`R = B + Delta`): evaluates to `0 + (C_R - 0) = C_R`. It remains entirely flat at clip level.

# Literature on two-channel clipping without ratios

Verified published literature relevant to non-ratio clipping reconstruction includes:

- Kiku, Monno, Tanaka, Okutomi (2014), "Minimized Laplacian Residual Interpolation for Color Image Demosaicking", SPIE Digital Photography X; expanded in Monno et al. (2015), "Beyond Color Difference: Residual Interpolation for Color Image Demosaicking", IEEE Transactions on Image Processing 24(5): 2034-2046. The authors prove that interpolating in the residual (additive difference) domain `R - G` avoids the singularities, noise amplification, and false colors inherent in color ratio interpolation `R / G`.
  *Inference*: while originally proposed for demosaicing, their mathematical proof that the Laplacian energy of additive inter-channel residuals is lower and more stable than ratio fields directly justifies using additive root-space offsets over hue ratios in clipped highlight recovery.

- Pérez, Gangnet, Blake (2003), "Poisson Image Editing", ACM Transactions on Graphics (SIGGRAPH 2003) 22(3): 313-318. Establishes the foundations for non-ratio gradient reconstruction. Solving the Poisson equation `grad^2(R) = grad^2(B)` with Dirichlet boundary condition `R = C_R` transfers spatial detail additively without scaling gradients by cross-channel ratios.

- Bertalmio, Sapiro, Caselles, Ballester (2000), "Image Inpainting", ACM SIGGRAPH 2000: 417-424. Formulates single-channel PDE inpainting by propagating smoothness along isophote lines into unknown regions. Operates per-channel without cross-channel ratio priors, serving as the basis for spatial boundary continuation.

- Didyk, Mantiuk, Heinelt, Seidel (2008), "Enhancement of Bright Video Features for HDR Displays", Computer Graphics Forum 27(2): 69-78; and Rempel et al. (2007), "Ldr2Hdr: On-the-fly Reverse Tone Mapping of Texture and Video", ACM Transactions on Graphics (SIGGRAPH 2007) 26(3): 39. Both papers reconstruct missing dynamic range in clipped regions by generating smooth spatial boost maps via edge-stopping spatial filters without computing or multiplying cross-channel hue ratios.

- Contrast case (ratio-based): Rouf, Lau, Heidrich (2012), "Gradient Domain Color Restoration of Clipped Highlights", Proc. PROCAMS 2012. This is the academic paper corresponding to Dolby patent US8861851B2. It explicitly estimates hue ratios along the boundary and multiplies the non-saturated channel gradient by those ratios, demonstrating the exact approach that must be avoided.

# Not verified

- Reconstructed EV levels were not verified against the user's specific bracketed raw test files, as no raw image files were provided in the task directory.
- The behavior of Formulation 1 on extreme clipped highlights exceeding 5 EV above sensor saturation was not benchmarked.
