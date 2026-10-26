Biharmonic extrapolation produces runaway domes because 4th-order PDEs lack a maximum principle, scale peak height with region radius ($O(s R)$), and turn step edges at occluder silhouettes into steep inward slopes. A **screened Poisson solve** $((-\Delta + \mu^2) u = \mu^2 u_{base} + c)$ solves the problem: it obeys the maximum principle, naturally caps peak height at $s / \mu$ without ad-hoc limits, and runs in under 50 ms in C. Across the 69 bracketed pairs, true lamp brightness exceeds clipping by at most 0.8 EV, so the dome ceiling should never exceed 1.0 EV.

# Occluder edges and boundary conditions

In `/tmp/hl-rules-study/tasks/2026-10-24-brightness/lift.py:82-83`, the right-hand side couples clipped pixels $u \in \Omega$ to outside pixels $k \notin \Omega$:
```python
    A = (Du.T @ Du).tocsc()
    b = -(Du.T @ (Dk @ L.ravel()[k]))
```
When an outside pixel $k$ belongs to a dark occluder (tree branch, roof, statue), $L_k \ll Y_{clip}$. The discrete Laplacian treats this step of 1 to 3 EV across 1 pixel as an inward slope of the light source, projecting a massive dome into the sky.

## Which rim pixels constrain slope

A rim pixel $p \in \partial \Omega$ belongs to a true optical falloff (flare or glare) only if:
- Slope magnitude bound: $|\nabla L(p)| \le g_{max} \approx 0.08$ EV/px in log2 luminance. Silhouette step edges (0.5 to 3.0 EV/px) fail this test.
- Outward transition support: Luminance must decrease monotonically outward along the normal $-\hat{n}_{in}$ over at least 3 to 5 pixels ($W_{trans} \ge 3$ px). At an occluder, luminance drops within 1 pixel and levels off or fluctuates.
- Proximity to clipping: $Y(p) \ge \tau Y_{clip}$ with $\tau \approx 0.85$ (within 0.25 EV of clipping). Dark occluders fail immediately.
- Segment continuity: Valid rim pixels must form contiguous segments of at least 5 pixels along the boundary. Isolated pixels caused by foliage gaps or sensor noise are discarded.

If raw data is accessible, optical falloffs have a multi-pixel band of partially clipped photosites (1 or 2 clipped channels), whereas occluders drop from 3 clipped channels to 0 clipped channels within 1 photosite.

## Boundary conditions elsewhere

At occluder boundaries and rejected rim pixels, the highlight must decouple from the foreground:
- Homogeneous Neumann condition ($\partial u / \partial n = 0$): Sever stencil connections between clipped pixel $u \in \Omega$ and occluder pixel $k \notin \Omega$. In the discrete Laplacian for pixel $u$, replace $(L_k - L_u)$ with 0. Occluder pixels do not contribute rows. This removes dark values from $b$, so the surface meets the occluder with zero normal flux.
- Zero curvature condition ($\Delta u = 0$): If a 4th-order solve is used, setting $\Delta u = 0$ at severed edges eliminates bending moments.
- Fallback: If a region has fewer than 10% valid rim pixels (such as sky seen through dense foliage), pure Neumann leaves the solve singular. Fall back to Dirichlet $u = Y_{clip}$ (flat lift at clipping level, zero dome).

# Distinguishing lights from flat surfaces and occluded skies

Lamps differ from flat surfaces and skies in the width and coherence of their unclipped transition zone, not just their raw boundary gradient:
- An occluded sky has dark silhouette edges that create high local boundary steps, but its unclipped sky areas have near-zero gradient ($S < 0.005$ EV/px).
- A flat bright surface (moon disk, white car, paper sign) has either a uniform interior and exterior ($S < 0.005$ EV/px), or a sharp material boundary with dark surroundings where falloff width is at most 1 to 2 pixels.
- A light with falloff has an optical glare halo that extends over multiple pixels outside the clipped core, with gradients pointing inward across most directions.

## Decision tree per region

From data available in the clipped image and raw clipping masks, evaluate four checks per region:
1. Valid rim fraction: Compute $f_{valid} = |\partial \Omega_{valid}| / |\partial \Omega|$. If $f_{valid} < 0.30$, classify as occluded sky or silhouette-bounded region; use flat lift or zero lift.
2. Outward transition width ($W_{trans}$): Measure the median outward radial distance over which unclipped luminance decreases monotonically. If $W_{trans} < 3$ pixels, classify as a sharp object boundary (such as the moon disk); use flat lift.
3. Transition slope magnitude ($S_{trans}$): Measure median inward slope across the halo $[R, R + W_{trans}]$. If $S_{trans} < 0.008$ EV/px, classify as a diffuse surface (white wall, overcast sky); use flat lift.
4. Angular gradient coherence ($C_{angle}$): Measure the fraction of boundary directions where the gradient points inward. If $C_{angle} < 0.60$, classify as non-radiating; use flat lift.

Enable the dome only if all four checks pass.

# Principled limit for dome height

In `/tmp/hl-rules-study/tasks/2026-10-24-brightness/lift.py:89`, biharmonic extrapolation on a circular disk of radius $R$ with rim slope $s$ produces an exact peak height of $\Delta u_{peak} = \frac{1}{2} s R$. Height scales with $R$, so large regions extrapolate gentle slopes into huge domes.

## Physical and empirical limits

1. Flare penetration depth ($R_{eff} = \min(R, W_{trans})$): A large clipped region is an extended emitter (lamp globe, bright cloud), not an unresolved point source. Extended emitters have roughly uniform surface radiance. Slope exists only in the glare halo of width $W_{trans}$ outside the source. Slope cannot physically extrapolate deeper into the emitter than $W_{trans}$. The geometric height bound is $\Delta L_{geom} = \frac{1}{2} s R_{eff}$. For an extended source with $W_{trans} = 15$ px and $s = 0.04$ EV/px, $\Delta L_{geom} \approx 0.3$ EV, which prevents runaway.
2. Empirical ceiling from bracketed pairs: Measured across all 69 bracketed pairs, the maximum observed deficit on lamps is 0.787 EV (median 0.20 EV across all scenes, 0.35 EV on lamps). Caps of 2 to 4 EV permit highlights to become 4 to 16 times brighter than reality. An empirical ceiling of $H_{max} = 1.0$ EV safely encloses all measured lamps.
3. Smooth algebraic compression: Hard clamping produces a flat plateau with sharp derivative discontinuities. Smooth algebraic saturation rounds the peak without flattening:
   $\Delta L_{capped} = \frac{\Delta L}{\sqrt{1 + (\Delta L / H_{cap})^2}}$, with $H_{cap} = \min(H_{max}, \frac{1}{2} s R_{eff})$.
   When $\Delta L \ll H_{cap}$, the slope is preserved. Near $H_{cap}$, the profile rounds off smoothly to a horizontal tangent.

# Better brightness priors

Biharmonic extrapolation requires solving a 4th-order PDE whose condition number is $O(N^2)$. On large regions, algebraic multigrid is slow, complex in C, and lacks a maximum principle. Distance-transform domes ($1 - \exp(-s D(x) / H)$) have derivative discontinuities along the medial axis, forming sharp ridges on elongated highlights and conical points on circular highlights.

## Recommended prior: screened Poisson equation

Solve the second-order screened Poisson equation on the clipped domain $\Omega$:
$(-\Delta + \mu^2) u = \mu^2 u_{base} + c$
with Dirichlet boundary condition $u = L$ on valid rim pixels $\partial \Omega_{valid}$ and homogeneous Neumann $\partial u / \partial n = 0$ on occluder edges.
- $u_{base}$ is the harmonic solution ($\Delta u_{base} = 0$), interpolating uneven boundary values.
- Screening parameter $\mu = 1 / R_{eff}$, where $R_{eff} = \min(R, W_{trans})$.
- Source term $c = s \cdot \mu / \tanh(\mu R_{eff})$, matching the measured rim slope $s$.

Properties:
- Automatic height saturation: In radial coordinates, the peak height of the screened Poisson solve approaches $s / \mu = s R_{eff}$ as $R \to \infty$. On a test disk with $s = 0.05$ EV/px, increasing radius from $R=40$ to $R=300$ increases the biharmonic peak from 1.0 EV to 7.5 EV, whereas screened Poisson saturates at exactly 1.000 EV.
- Maximum principle: As a second-order elliptic PDE, the solution is strictly concave. It produces zero ringing, zero boundary undershoot, and no medial-axis ridges.
- Execution speed in C: The discrete matrix is a 5-point Laplacian plus a constant diagonal damping term $\mu^2$. Its condition number is $O(N)$ (and $O(1)$ for large $\mu$). A 30-line Red-Black Gauss-Seidel solver converges in 15 to 25 iterations, running in under 30 ms in C for 50,000 pixels.
- Patent compliance: Operates strictly on scalar luminance ($Y$ or $\log_2 Y$) without hue ratios, boundary filters, or hue transfer. It is completely outside US8861851B2.

## Alternative prior: raw-guided ring reconstruction

Partially clipped pixels have 1 or 2 unclipped raw channels. In darktable's raw pipeline, unclipped raw channels can reconstruct luminance across the partly clipped ring using sensor white balance multipliers before any PDE solve. This raises boundary values by 0.4 to 0.5 EV to their true level, leaving only a tiny fully clipped core. A second-order Laplace solve ($\Delta u = 0$) on that smaller core then reconstructs the highlight with minimal extrapolation distance.

# Not verified

We verified mathematical derivations, peak scaling, and dataset statistics on the 69 bracketed pairs, but did not implement or benchmark the C code inside darktable's `opposed.c`. The numerical thresholds ($g_{max} = 0.08$ EV/px, $H_{max} = 1.0$ EV, $W_{trans} \ge 3$ px) are our inferences from sample scenes in the dataset and have not been validated across all 69 pairs in a batch run.
