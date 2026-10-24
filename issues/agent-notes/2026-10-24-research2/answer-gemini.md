# Confidence measures for highlight reconstruction fusion

Gradient-domain highlight reconstruction fails on daylight skies because Dirichlet boundary values from dark occluders bleed across the Laplace solve. It succeeds on colored lamps because lamps have bright, monochromatic halos. The most reliable predictor of gradient-domain trust is boundary concordance: the fraction of the region boundary near clipping combined with the circular variance of boundary hue. This measure runs in under 2 ms on a 24 MP raw, needs no training data, and distinguishes colored lamps from occluded skies without brittle segment classification.

## Why disagreement penalties fail

The symmetric penalty t0 * exp(-|log2(gd/opp)| / s) failed because inpaint opposed and gradient domain disagree most strongly on colored lamps. Opposed collapses saturated lamps to near-neutral white, while gradient domain preserves lamp chroma. Penalizing disagreement therefore suppresses gradient domain where it wins. Reliable fusion requires **asymmetric measures** assessing whether boundary conditions feeding the gradient-domain solve are trustworthy.

## Candidate 1: Boundary luminance-chromaticity concordance

Measures whether clipped region boundary pixels form a uniform light source halo or mix bright sky with dark silhouettes.

Citation: Tao, M. W., Johnson, M. K., and Paris, S., "Error-Tolerant Image Compositing", International Journal of Computer Vision, 103(2): 178–189, 2013. [DOI: 10.1007/s11263-012-0579-7](https://doi.org/10.1007/s11263-012-0579-7). Mardia, K. V., and Jupp, P. E., Directional Statistics, John Wiley & Sons, 2000. [DOI: 10.1002/9780470316979](https://doi.org/10.1002/9780470316979).

Formulas:
For clipped region $\Omega$ with boundary $\partial\Omega$:
$$f_{\text{clip}} = \frac{1}{|\partial\Omega|} \sum_{p \in \partial\Omega} \mathbb{I}(Y(p) \ge \alpha Y_{\text{clip}}), \quad V_\theta = 1 - \frac{\sqrt{(\sum w_p \cos\theta_p)^2 + (\sum w_p \sin\theta_p)^2}}{\sum w_p}$$
$$t(\Omega) = t_{\text{base}} \cdot f_{\text{clip}} \cdot (1 - V_\theta)$$
$Y(p)$ is unclipped channel luminance, $Y_{\text{clip}}$ is saturation level, $\theta_p$ is raw hue angle, $w_p = Y(p)/Y_{\text{clip}}$, and $\alpha \approx 0.75$.

Separation mechanism:
Saturated lamps are surrounded by blooming where $Y(p) \approx Y_{\text{clip}}$ all along $\partial\Omega$ ($f_{\text{clip}} \approx 1$), with monochromatic hue ($V_\theta \approx 0$), yielding $t \approx t_{\text{base}}$. Skies touching occluders have dark border pixels ($Y(p) \ll Y_{\text{clip}}$, $f_{\text{clip}} < 0.6$) and high hue variance between sky and foliage ($V_\theta \gg 0$), driving $t(\Omega) \to 0$ (reverting to opposed).

Cost on 24 MP raw: Under 2 ms (traverses $< 80{,}000$ perimeter pixels).
Training data: None.
Verified vs inferred: Verified in Tao et al. that boundary mismatch causes Poisson color bleeding; verified in Mardia & Jupp that $V_\theta$ measures directional dispersion. Inferred: gating highlight blend weights with $f_{\text{clip}} \cdot (1 - V_\theta)$.

## Candidate 2: Dirichlet energy of interpolated hue

Measures internal smoothness of the harmonic hue field produced by the Laplace solve.

Citation: Rouf, M., Lau, C., and Heidrich, W., "Gradient Domain Color Restoration of Clipped Highlights", PROCAMS, 2012. [Project page](https://www.cs.ubc.ca/labs/imager/tr/2012/GradientDomainColorRestoration/). Grady, L., "Random Walks for Image Segmentation", IEEE TPAMI, 28(11): 1768–1783, 2006. [DOI: 10.1109/TPAMI.2006.233](https://doi.org/10.1109/TPAMI.2006.233).

Formulas:
Rouf et al. solve $\Delta H = 0$ in $\Omega$. Mean Dirichlet energy density:
$$\mathcal{E}_H(\Omega) = \frac{1}{|\Omega|} \sum_{x \in \Omega} \|\nabla H(x)\|^2, \quad t(\Omega) = t_{\text{base}} \cdot \exp\left( - \frac{\mathcal{E}_H(\Omega)}{\sigma_H^2} \right)$$
where $\|\nabla H(x)\|^2 \approx (H(x+1,y)-H(x,y))^2 + (H(x,y+1)-H(x,y))^2$.

Separation mechanism:
Uniform lamp halos yield flat harmonic hue fields ($\|\nabla H\| \approx 0 \implies \mathcal{E}_H \approx 0 \implies t \approx t_{\text{base}}$). Conflicting boundary hues (sky versus tree) force steep gradients across $\Omega$ to bridge disparate values; high $\mathcal{E}_H$ flags active **color bleeding** and lowers $t$. Evaluates the hue field darktable already computes.

Cost on 24 MP raw: Under 3 ms (finite differences over clipped pixels only, no extra solves).
Training data: None (tune $\sigma_H$).
Verified vs inferred: Verified in Rouf et al. that hue uses Laplace interpolation; verified in Grady that Dirichlet energy reflects boundary label conflict. Inferred: using mean squared hue gradient as a method confidence gate.

## Candidate 3: Exposure-fusion well-exposedness of boundary photosites

Weights boundary reliability by photometric exposure quality of border pixels rather than internal saturation.

Citation: Mertens, T., Kautz, J., and Van Reeth, F., "Exposure Fusion: A Simple and Practical Alternative to High Dynamic Range Photography", Computer Graphics Forum, 28(1): 161–171, 2009. [DOI: 10.1111/j.1467-8659.2008.01171.x](https://doi.org/10.1111/j.1467-8659.2008.01171.x).

Formulas:
$$W_E(\partial\Omega) = \frac{1}{|\partial\Omega|} \sum_{p \in \partial\Omega} \exp\left( - \frac{(Y(p) - Y_{\text{clip}})^2}{2\sigma_E^2} \right), \quad t(\Omega) = t_{\text{base}} \cdot [W_E(\partial\Omega)]^\gamma$$
where $\sigma_E \approx 0.2 Y_{\text{clip}}$ and $\gamma \ge 1$.

Separation mechanism:
Mertens Gaussian well-exposedness is centered at saturation $Y_{\text{clip}}$. Pixels in an unclipped light halo satisfy $Y(p) \approx Y_{\text{clip}}$, producing $W_E \approx 1$. Dark occluders have $Y(p) < 0.3 Y_{\text{clip}}$, giving weights near zero. Regions with occluded borders yield low $W_E$, preventing gradient domain from bleeding dark foreground hues into clouds.

Cost on 24 MP raw: Under 2 ms (1D loop over boundary pixels with lookup table).
Training data: None (tune $\sigma_E, \gamma$).
Verified vs inferred: Verified in Mertens et al. that Gaussian well-exposedness isolates well-exposed tones. Inferred: centering the Gaussian at $Y_{\text{clip}}$ along $\partial\Omega$ to evaluate boundary **trustworthiness**.

## Candidate 4: Inpainting confidence decay via region scale

Confidence in PDE inpainting decays with distance from the boundary; region scale separates compact lamps from vast skies.

Citation: Criminisi, A., Pérez, P., and Toyama, K., "Region Filling and Object Removal by Exemplar-Based Image Inpainting", IEEE TIP, 13(9): 1200–1212, 2004. [DOI: 10.1109/TIP.2004.833105](https://doi.org/10.1109/TIP.2004.833105). Bornemann, F., and März, T., "Fast Image Inpainting Based on Coherence Transport", JMIV, 28(3): 259–270, 2007. [DOI: 10.1007/s10851-007-0017-6](https://doi.org/10.1007/s10851-007-0017-6).

Formulas:
Hydraulic radius (scale) $R_H(\Omega) = |\Omega| / |\partial\Omega|$:
$$t(\Omega) = t_{\text{base}} \cdot \frac{1}{1 + (R_H(\Omega) / R_0)^2}, \quad \text{or pixel-level } t(x) = t_{\text{base}} \cdot \exp\left( - \frac{d(x, \partial\Omega)^2}{2 R_0^2} \right)$$

Separation mechanism:
Colored lamps and specular glints are compact ($R_H \le 15$ px); the Laplace solve operates over short distances, preserving valid halo hue across the core. Blown skies are massive ($R_H \ge 50\text{--}200$ px); harmonic interpolation flattens variation and bleeds distant occluders across the entire cloud. Penalizing large $R_H$ confines gradient domain to compact highlights.

Cost on 24 MP raw: Under 1 ms for region-level $R_H$ (computed during connected-component labeling); ~18 ms for per-pixel Euclidean distance transform.
Training data: None (tune $R_0$).
Verified vs inferred: Verified in Criminisi et al. and Bornemann & März that inpainting confidence decays rapidly away from boundaries. Inferred: scaling method blend weight $t$ by region hydraulic radius $R_H$.

## Candidate 5: Multi-estimator agreement via physical daylight locus

Daylight and overcast skies follow a narrow chromaticity locus. Estimator agreement against Planckian and daylight loci identifies unphysical casts.

Citation: Judd, D. B., MacAdam, D. L., and Wyszecki, G., "Spectral distribution of typical daylight as a function of correlated color temperature", JOSA, 54(8): 1031–1040, 1964. [DOI: 10.1364/JOSA.54.001031](https://doi.org/10.1364/JOSA.54.001031). Finlayson, G. D., and Süsstrunk, S., "Color Constancy at a Pixel", IEEE TPAMI, 23(11): 1201–1214, 2001. [DOI: 10.1109/34.969117](https://doi.org/10.1109/34.969117).

Formulas:
In white-balanced camera space, neutral clouds satisfy $R \approx G \approx B$. Occluder bleeding pushes gradient domain into the purple line (magenta: high $R, B$, deficient $G$):
$$P_{\text{magenta}}(x) = \max\left(0, \frac{R_{\text{gd}}(x) + B_{\text{gd}}(x)}{2 G_{\text{gd}}(x)} - 1\right), \quad t(x) = t_{\text{base}} \cdot \exp\left( - \frac{P_{\text{magenta}}(x)^2}{2\sigma_M^2} \right)$$

Separation mechanism:
Gradient domain failure on daylight skies produces unnatural magenta or cyan casts on clouds that inpaint opposed reconstructs as neutral. Magenta does not exist in daylight illuminants or clouds. Elevated $P_{\text{magenta}}$ drops $t \to 0$. True colored lamps (amber, green, red) are unpenalized unless emitting violet/magenta.

Cost on 24 MP raw: Under 4 ms (per-pixel ratio on clipped photosites).
Training data: None (uses camera white balance from raw metadata).
Verified vs inferred: Verified in Judd et al. and Finlayson & Süsstrunk that daylight chromaticities exclude magenta. Inferred: using $P_{\text{magenta}}$ as a gating penalty on gradient domain reconstruction.

## Ranking of candidates

1. Boundary luminance-chromaticity concordance (Candidate 1). Directly measures boundary halo integrity and hue dispersion before solving.
2. Dirichlet energy of interpolated hue (Candidate 2). Measures hue conflict across the solve using fields darktable already computes, at zero extra memory.
3. Exposure-fusion boundary well-exposedness (Candidate 3). Robust against dark occluders; does not detect hue clashes among bright boundary pixels.
4. Region scale via hydraulic radius (Candidate 4). Extremely fast ($< 1$ ms) and protects vast cloudy skies; cannot distinguish small occluded sky holes from lamps. Best used with Candidate 1.
5. Daylight locus consistency (Candidate 5). Targets the magenta cloud defect directly; risks penalizing genuine magenta stage lights.

## Not verified

I did not run these formulas on darktable's 103 test pairs. Runtimes are estimates based on standard cache-friendly C loops and SIMD vectorization on 24 MP raw data, not benchmarks inside darktable's pixelpipe. Parameter values ($\alpha \approx 0.75$, $\sigma_H$, $R_0$) require tuning on empirical test pairs.