The research report has been written to `/tmp/hl-rules-study/tasks/2026-10-24-research2/answer-gemini.md` (1364 words).

### Summary of findings

1. **Why `exp(-|log2(gd/opp)| / s)` failed**: Inpaint opposed forces clipped channels near the average of unclipped channels (white), whereas gradient domain reconstructs high chroma on colored lamps. The two estimators disagree most on colored lamps; penalizing disagreement suppresses gradient domain on the exact scenes where it wins.
2. **Top candidate — Boundary Concordance**: Combines boundary clip fraction $f_{\text{clip}}(\partial\Omega)$ with luminance-weighted circular variance of hue $V_\theta(\partial\Omega)$. For colored lamps, blooming ensures $f_{\text{clip}} \approx 1$ and $V_\theta \approx 0$ ($t \approx t_{\text{base}}$). For occluded skies, dark silhouettes drop $f_{\text{clip}}$ and hue jumps between sky and occluder raise $V_\theta$, driving $t \to 0$. Runs in under 2 ms on a 24 MP raw with no training data.
3. **Second candidate — Dirichlet Hue Energy**: $\mathcal{E}_H = \frac{1}{|\Omega|} \sum \|\nabla H\|^2$ measures the smoothness of the Laplace-interpolated hue field that darktable already calculates. Monochromatic lamp halos produce flat fields ($\|\nabla H\| \approx 0$); conflicting sky/occluder boundaries force steep gradient ramps across $\Omega$, flagging color bleeding in under 3 ms.
4. **Additional evaluated candidates**: Exposure-fusion boundary well-exposedness (Mertens et al.), region scale via hydraulic radius ($|\Omega|/|\partial\Omega|$), and daylight locus consistency.
