Waiting for the investigation subagent to complete research.
Investigating published highlight reconstruction methods and patent claims.
Researching highlight reconstruction methods and patent landscape.
The primary reason gradient domain fails on skies is that spatial boundary interpolation (Laplace Dirichlet solve) bleeds the hues of dark occluders, such as trees and roofs, across clipped highlights. The most promising route to beat inpaint opposed on skies while keeping gradient domain's gains on colored lights is to estimate missing channel ratios in color covariance space rather than through spatial boundary bleeding, specifically via local Bayesian declipping (adapting Zhang & Brainard 2004) or Color Lines projection (adapting Elboher & Werman 2010). Both methods operate on color distributions rather than geometric contours, exclude dark occluders via luminance gating, preserve saturated chromaticities on compact lights, and avoid the claims of Dolby patent US8861851B2.

# Published methods

### Bayesian estimation of saturated pixel values
Reference: Xuemei Zhang and David H. Brainard, "Estimation of saturated pixel values in digital color imaging", Journal of the Optical Society of America A, Vol. 21, No. 12, pp. 2301-2310, 2004 [verified in source].
- Core idea: Models local sensor responses as a multivariate Gaussian prior distribution. When one or two channels saturate ($X_c \ge C$), the missing values are predicted by the conditional expectation under the truncated multivariate normal distribution given the surviving channels $X_u = x_u$. For a single clipped channel, the conditional mean simplifies to:
$$\hat{x}_c = \mu_{c|u} + \sigma_{c|u} \lambda\left(\frac{C_c - \mu_{c|u}}{\sigma_{c|u}}\right), \quad \lambda(\alpha) = \frac{\phi(\alpha)}{1 - \Phi(\alpha)}$$
where $\phi$ is the standard normal probability density function, $\Phi$ is the cumulative distribution function, and $\lambda$ is the inverse Mills ratio. For two clipped channels, sequential univariate conditioning (conditioning channel 1 on $X_u$, then channel 2 on $X_u$ and $\hat{x}_1$) yields a closed-form approximation that enforces both saturation floors [inferred].
- Failure case addressed: Color shifts and cross-talk in partially clipped pixels without relying on spatial boundary diffusion. Guarantees the physical lower bound $\hat{x}_c \ge C_c$.
- Cost: Low. Under 200 ms on CPU for 24 MP. Closed-form scalar operations per pixel and small matrix inversions ($2 \times 2$ or $3 \times 3$) per region.
- Data needed: Raw mosaic or half-size RGB grid, sensor clipping thresholds. The mean vector and covariance matrix are estimated locally from unclipped pixels in the same segment or surrounding band.
- Patent status: Academic paper (2004), unpatented. Published seven years before Dolby US8861851B2. Contains no spatial boundary hue interpolation and no Poisson solve.
- Performance: On skies, gating border samples by luminance ($L > 0.6 \cdot C_{max}$) excludes dark foliage and roof tiles from the covariance, avoiding cyan and pink casts [inferred]. On compact lights, the local covariance preserves the chromaticity of the light source.

### Color lines projection
Reference: Elhanan Elboher and Michael Werman, "Recovering Color and Details of Clipped Image Regions", Proceedings of the International Conference on Computer Graphics, Visualization, Computer Vision and Image Processing (CGVCVIP 2010), pp. 1-8, 2010; expanded in International Journal of Computer Information Systems and Industrial Management Applications (IJCISIM), Vol. 3, pp. 812-819, 2011 [verified in source].
- Core idea: Natural image patches form elongated 1D lines $\mathbf{c}(t) = \mathbf{c}_0 + t \mathbf{v}$ in RGB color space (Omer & Werman, CVPR 2004). Saturation bends these lines when channels clip. The algorithm clusters unclipped pixels in the neighborhood, fits 1D lines via principal component analysis, and projects clipped pixels along the fitted line using the unclipped channel coordinate $t = (x_u - c_{0,u}) / v_u$.
- Failure case addressed: Eliminates color bleeding across fine geometric boundaries because clustering occurs in color space rather than spatial coordinate space.
- Cost: Moderate. Local clustering plus $3 \times 3$ singular value decomposition per cluster, followed by scalar projection per clipped pixel. Estimated at 1 to 2 seconds on CPU for 24 MP [inferred].
- Data needed: Half-size RGB grid or raw mosaic, saturation thresholds.
- Patent status: Academic publication (2010), unpatented. Clear of US8861851B2.
- Performance: Tree foliage and sky form distinct clusters in color space, preventing green occluders from bleeding into bright skies. Preserves colored light sources if unclipped halo pixels define the chromaticity line.

### Opponent chrominance desaturation blend and directional diffusion
Reference: David Coffin, `dcraw.c` (`blend_highlights()`, `recover_highlights()`), 1997-2015; LibRaw `postprocessing_aux.cpp:276-370` [verified in source].
- Core idea:
  - `-H 2` (`blend_highlights`): Converts raw RGB to an opponent color space ($L = R+G+B$, $C_1 = \sqrt{3}(R-G)$, $C_2 = -R-G+2B$). For pixels exceeding the clipping threshold, it computes the chroma vector of the unclipped input and that of the clamped input, and scales the unclipped chroma vector by their ratio:
$$\text{ratio} = \sqrt{\frac{C_{1,\text{clamp}}^2 + C_{2,\text{clamp}}^2}{C_{1,\text{raw}}^2 + C_{2,\text{raw}}^2}}$$
This smoothly attenuates chromaticity toward zero as luminance increases, enforcing a roll-off toward neutral white while keeping surviving channel luminance gradients intact.
  - `-H 3-9` (`recover_highlights`): Iteratively diffuses channel ratios from unclipped neighbors into clipped pixels along eight spatial directions.
- Failure case addressed: Prevents unnatural pink and magenta tinting in overexposed areas without spatial color bleeding.
- Cost: Negligible for `-H 2` (<50 ms on CPU for 24 MP, single pass, no auxiliary memory). Moderate for directional diffusion (`-H 3-9`).
- Data needed: Raw Bayer data, white balance multipliers, saturation levels.
- Patent status: Open source (GPL / public domain). Unpatented.
- Performance: Highly reliable on daylight skies because overexposed clouds and skies naturally desaturate toward white at peak irradiance. Fails on colored lights (neon, candles) by bleaching saturated highlights to pure white.

### Multiscale color ratio propagation
Reference: Emil Martinec, RawTherapee `rtengine/hilite_recon.cc` (2008-2011), optimized by Ingo Weyrich (2019); also `librtprocess` `HLRecovery_inpaint` [verified in source].
- Core idea: Decomposes channels into low frequencies using fast separable box filters and recursive downsampling, alongside high frequencies in difference buffers. Color ratios from unclipped low-frequency areas are propagated inward into clipped areas across pyramid scales, while high-frequency textures from the least-clipped channel are preserved.
- Failure case addressed: Smooth color transitions across broad clipped areas without posterization.
- Cost: Moderate. $O(N)$ with box blurs; approximately 0.5 to 1.5 seconds on CPU for 24 MP [inferred].
- Data needed: Raw mosaic data or half-size RGB, clipping thresholds, white balance coefficients.
- Patent status: GPLv3 open source. Unpatented.
- Performance: Recovers colored lights effectively. On skies, standard isotropic box blurring can bleed magenta or purple when blur kernels sweep across high-contrast edges such as dark tree branches.

### Clipping-aware wavelet pyramid
Reference: Johannes Hanika, `vkdt` `src/pipe/modules/hilite/` (`reduce.comp`, `assemble.comp`, `readme.md`), 2022 [verified in source].
- Core idea: A multiscale wavelet or Gaussian pyramid downsamples and recombines channels in two passes. The downsampling blur ignores clipped pixels and renormalizes weights to unclipped pixels only. Recombination upsamples the coarse buffer, rescales brightness to match surviving channels at full resolution, and fills in clipped channels with optional user desaturation.
- Failure case addressed: Halo-free highlight reconstruction on GPU and CPU without boundary Poisson solves.
- Cost: Very fast. Under 10 ms in Vulkan on GPU; approximately 200 to 400 ms on CPU [inferred].
- Data needed: Raw mosaic data rescaled to $[0, 1]$, channel saturation levels.
- Patent status: BSD / GPLv3 open source. Unpatented.
- Performance: Clipping-aware normalization prevents clipped pixels from dragging down average brightness, but spatial downsampling without range weighting still blends dark occluders with adjacent bright sky.

### Decoupled lightness and confidence-weighted chromaticity diffusion
Reference: Dong Guo, Yuan Cheng, Shaojie Zhuo, Terence Sim, "Correcting Over-exposure in Photographs", IEEE CVPR, pp. 515-521, 2010 [verified in source].
- Core idea: Separates lightness estimation from color recovery. Lightness is reconstructed via an overexposure likelihood map. Chromaticity $(r, g) = (R/\Sigma, G/\Sigma)$ receives confidence scores based on margin to saturation; chromaticity is then diffused inward using an edge-stopping partial differential equation.
- Failure case addressed: Reconstructs areas where all three channels are clipped.
- Cost: Moderate. Iterative diffusion requires approximately 2 to 4 seconds on CPU [inferred].
- Data needed: Linear RGB image, clipping mask.
- Patent status: Academic paper (2010), unpatented.
- Performance: Edge-stopping weights reduce color leakage across sky and occluder borders, but fine branches can still leak without an explicit luminance cutoff.

### Deep CNN single-raw highlight reconstruction
Reference: Yunhao Zou, Chenggang Yan, Ying Fu, "RawHDR: High Dynamic Range Image Reconstruction from a Single Raw Image", IEEE/CVF ICCV, pp. 12334-12344, 2023; Gabriel Eilertsen et al., "HDR image reconstruction from a single exposure using deep CNNs", ACM Transactions on Graphics (SIGGRAPH Asia), Vol. 36, No. 6, 2017 [verified in source].
- Core idea: Operates in the linear raw domain using exposure masks to separate severely clipped from partially clipped pixels. Dual-intensity guidance uses unclipped channels to reconstruct structure, while convolutional feature extraction hallucinates missing texture and color.
- Failure case addressed: Joint recovery of non-linear color expansion and texture in deep, fully clipped regions.
- Cost: Moderate. A lightweight ONNX network (~60,000 parameters) takes 1 to 3 seconds on desktop CPU via AVX2 [inferred]. Memory footprint ~400 MB.
- Data needed: Must be trained on external paired raw datasets, such as the RealRawHDR dataset (1,500 bracketed raw scenes) or synthetic saturation applied to public raw collections like MIT-Adobe FiveK. darktable's 103 usable pairs must be kept strictly for testing.
- Patent status: Open source (MIT license). Unpatented.
- Performance: High semantic selectivity distinguishes skies from foreground occluders, eliminating boundary bleeding. Produces plausible curved domes in fully clipped light sources.

### Smartphone ISP pipelines
Reference: Samuel W. Hasinoff et al., "Burst photography for high dynamic range and low-light imaging on mobile cameras", ACM Transactions on Graphics (SIGGRAPH Asia), Vol. 35, No. 6, 2016 (Google HDR+); Suk Hwan Lim et al., US Patent 10,070,109 B2 (Apple Inc., 2018); Maxim Smirnov, US Patent 10,825,154 B2 (Apple Inc., 2020) [verified in source].
- Core idea: Google HDR+ underexposes bursts during capture to prevent sensor clipping, and clamps chroma to zero in bilateral filtering for residual clipped highlights. Apple's ISP patents estimate a target hue map for clipped pixels using low-frequency previews and adjust clipped channels under monotonic lightness constraints.
- Patent status: Apple US10070109B2 and US10825154B2 remain active until approximately 2037. Apple's methods present patent exposure and should be avoided.

# Ranked candidate methods

### Rank 1: Local Bayesian declipping with luminance-gated border covariance
- Motivation: Adapts Zhang & Brainard (2004) to segmented image regions. Instead of interpolating colors spatially across the clipped area, it fits a Gaussian color distribution to local unclipped pixels. On skies, border pixels below a luminance threshold ($L \le 0.6 \cdot C_{max}$) are discarded, completely excluding dark trees and roofs from the covariance matrix. When one channel clips, the value is calculated analytically via the inverse Mills ratio. When two channels clip, sequential univariate conditioning is applied. For compact lights, local covariance captures the specific lamp chromaticity.
- Strengths: Closed-form scalar solution (no iterative solver, multigrid, or conjugate gradient). Strictly enforces $\hat{x}_c \ge C_c$. Runs under 200 ms on CPU. Fully clear of Dolby US8861851B2.

### Rank 2: Color lines projection with occluder-filtered clustering
- Motivation: Adapts Elboher & Werman (2010). Natural surface patches form 1D lines in RGB space. Trees, roofs, and skies form distinct, widely separated clusters in color space. Segmenting the unclipped border into local color clusters (via SLIC on half-size RGB) and fitting a line $\mathbf{c}_0 + t \mathbf{v}$ per cluster via $3 \times 3$ PCA allows clipped pixels to be projected along the line using the unclipped channel coordinate.
- Strengths: Prevents occluder color bleeding into skies without requiring geometric normal estimation. Preserves saturated light sources. Avoids Dolby claims.

### Rank 3: Luminance-gated guided filter detail transfer with opponent chrominance roll-off
- Motivation: Replaces the Poisson solve in gradient domain with Kaiming He's $O(N)$ Guided Filter, and replaces Laplace hue interpolation with a luminance-gated color donor search. Texture from unclipped channels is transferred to the clipped channel via local linear regression. Color donors are restricted to unclipped neighbors within 1 EV of the clipping threshold, preventing dark occluders from donating color. Fully clipped cores transition smoothly to neutral white using dcraw's opponent-color roll-off curve.
- Strengths: Fast ($<500$ ms on CPU), halo-free, avoids flat cores on lamps, and avoids Poisson solves and Dirichlet boundary interpolation.

### Rank 4: Photometric edge-stopped multiscale color propagation
- Motivation: Adapts RawTherapee's `HLRecovery_inpaint` (`librtprocess`) by introducing a range weight $w(\Delta L) = \exp(-(\Delta L)^2 / 2\sigma_L^2)$ into the downsampling box filters. The large brightness step between sky and tree branches ($>2-4$ EV) forces range weights to zero, blocking tree green from entering the sky propagation buffer.
- Strengths: Reuses mature, optimized open-source code from `librtprocess` (GPLv3). Retains strong performance on colored specular highlights while fixing the occluder-bleeding flaw on skies.

### Rank 5: Single-raw CNN autoencoder (RawHDR architecture)
- Motivation: Employs a compact CNN (~60,000 parameters) based on the RawHDR architecture within darktable's optional ONNX pipeline, operating on half-size raw grids.
- Strengths: Learned semantic context distinguishes skies from foreground occluders without heuristic thresholds. Hallucinates smooth domes on fully clipped cores.
- Trade-offs: Requires an ONNX runtime dependency and model weights (~1.5 MB), plus training on external datasets (keeping darktable's 103 pairs strictly held out).

# Concrete experiments

### Experiment 1: Local Bayesian declipping (Candidate 1)
- Implementation:
  1. Downsample raw data to a half-size RGB grid (one pixel per $2 \times 2$ Bayer quad).
  2. Segment connected clipped components and identify a surrounding border band (width 16 to 32 pixels).
  3. Filter border pixels with a luminance floor ($L > 0.6 \cdot C_{max}$) to reject dark occluders.
  4. Compute the mean vector $\mu \in \mathbb{R}^3$ and covariance matrix $\Sigma \in \mathbb{R}^{3 \times 3}$ on the filtered border pixels.
  5. For one clipped channel, evaluate $\hat{x}_c = \mu_{c|u} + \sigma_{c|u} \lambda((C_c - \mu_{c|u}) / \sigma_{c|u})$. For two clipped channels, apply sequential univariate conditioning so both channels exceed their clipping thresholds.
  6. Transfer reconstructed values back to the raw mosaic grid before demosaicing.
- Test pairs: All 103 usable pairs, evaluated separately on the 49 pairs where gradient domain lost to inpaint opposed (skies with trees and roofs) and the 26 pairs where gradient domain won (colored lamps, candles, neon).
- Success criteria:
  - Net positive win/loss against inpaint opposed across the full 103 pairs ($\ge 55$ wins, $<20$ losses).
  - SSIMULACRA 2 score increase $\ge +0.5$ on the 49 sky/occluder pairs compared to gradient domain.
  - Qualitative verification confirms zero pink, cyan, or olive color casts in skies adjacent to trees or roofs.
  - Execution time under 250 ms on CPU for a 24 MP raw image.
- Expected effort: 3 to 5 days of developer effort (pure C, zero external dependencies).

### Experiment 2: Color lines projection (Candidate 2)
- Implementation:
  1. On the half-size RGB grid, compute SLIC superpixels (nominal tile size $32 \times 32$).
  2. For each superpixel containing clipped pixels, gather unclipped pixels. Discard pixels whose chromaticity distance from the superpixel median exceeds $2\sigma$ to eliminate edge contamination.
  3. Perform singular value decomposition on the $3 \times 3$ scatter matrix to obtain primary color line axis $\mathbf{v}$ and center $\mathbf{c}_0$.
  4. For each clipped pixel with surviving channel $u$, compute $t = (x_u - c_{0,u}) / v_u$ and set $\hat{x}_c = c_{0,c} + t v_c$. Clamp $\hat{x}_c \ge C_c$.
- Test pairs: The 49 sky failure pairs and the 26 colored light pairs.
- Success criteria:
  - Eliminates color bleeding across foliage and rooflines.
  - Matches or exceeds gradient domain's SSIMULACRA 2 scores on neon and candle highlights.
  - CPU runtime under 1.5 seconds on 24 MP.
- Expected effort: 5 to 7 days of developer effort.

### Experiment 3: Luminance-gated guided filter detail transfer (Candidate 3)
- Implementation:
  1. Construct a luminance guide $I_{guide} = \sum w_c X_c$ from unclipped channels.
  2. Filter color donors around each clipped segment using a luminance cutoff ($L_{donor} > C_{min} - 1.0\text{ EV}$) to reject occluders.
  3. Average the filtered donors to obtain base chromaticity $(r_0, g_0, b_0)$.
  4. Apply a fast Box-Guided Filter using $I_{guide}$ to transfer high-frequency spatial gradients: $\hat{X}_c = a_k I_{guide} + b_k$.
  5. For pixels where $I_{guide}$ reaches peak saturation, blend chromaticity toward neutral white using dcraw's opponent matrix ratio.
- Test pairs: All 103 pairs, with visual inspection of lamp cores (confirming dome behavior) and daylight skies.
- Success criteria:
  - No flat, posterized white discs in lamp cores.
  - Zero color cast in skies.
  - Runtime under 500 ms on CPU.
- Expected effort: 4 to 6 days of developer effort.

### Experiment 4: Edge-stopped multiscale color propagation (Candidate 4)
- Implementation:
  1. Adapt `HLRecovery_inpaint` from `librtprocess` (`hilite_recon.cc`).
  2. Modify blur kernels (`boxblur2`, `boxblur_resamp`) to include a range weight $w_{range} = \exp(-(\Delta Y)^2 / 2\sigma_Y^2)$, making the box filter edge-preserving.
  3. Set $\sigma_Y \approx 1.5$ EV so smooth sky gradients pass while tree/roof steps are blocked.
- Test pairs: Benchmark against the 49 sky loss pairs and the 26 light win pairs.
- Success criteria:
  - Reverses the sky losses without degrading performance on smooth light highlights.
  - Peak memory consumption under 1 GB.
- Expected effort: 3 to 4 days of developer effort.

### Experiment 5: Single-raw CNN autoencoder (Candidate 5)
- Implementation:
  1. Implement a 4-stage convolutional autoencoder with depthwise-separable convolutions (~60,000 parameters, receptive field $65 \times 65$).
  2. Train on an external paired raw dataset (RealRawHDR dataset by Zou et al. ICCV 2023 or synthetic clipping on MIT-Adobe FiveK raw files) with simulated exposure clipping. Do not train on darktable's 103 pairs.
  3. Export to ONNX and integrate into darktable's optional ONNX pipeline on the half-size Bayer grid.
- Test pairs: All 103 held-out bracketed pairs.
- Success criteria:
  - Outperforms inpaint opposed by $\ge +1.0$ SSIMULACRA 2 on average across held-out pairs.
  - Model weights $\le 2$ MB, zero run-time downloads.
  - Inference under 2 seconds on desktop CPU via ONNX Runtime.
- Expected effort: 2 to 3 weeks of developer effort.

# Patent analysis regarding US8861851B2 (Dolby)

US Patent 8,861,851 B2 ("Color highlight reconstruction", Wolfgang Heidrich and Mushfiqur Rouf, assigned to Dolby Laboratories Licensing Corp., filed 2011, active to 2032) claims an automated process requiring both of the following elements in combination:
1. Estimating hues for pixels inside a saturated region based on hues along the boundary of the saturated region (Dirichlet/Laplace boundary interpolation).
2. Estimating gradients for pixel values in saturated color channels based on gradients in non-saturated channels, and reconstructing pixels using those estimated gradients (Poisson solve / gradient domain reconstruction).

Patent exposure assessment for the proposed candidates:
- Candidates 1 and 2 (Bayesian estimation and Color Lines) do not interpolate boundary hues across regions, do not estimate cross-channel gradients, and do not solve Poisson or gradient-domain systems. Furthermore, Zhang & Brainard (2004) was published seven years prior to Dolby's priority date. Both are free of US8861851B2 claims.
- Candidate 3 (Guided filter transfer) transfers gradients via local linear regression (He et al. 2010), not by solving a Poisson equation from boundary conditions, and does not perform Laplace boundary hue interpolation. It avoids the independent claims of US8861851B2.
- Candidate 4 (Edge-stopped Color Propagation) is a multiscale box-blur ratio inpainting method (Martinec 2008), with no gradient-domain solve or Poisson equation.
- Candidate 5 (ONNX CNN) is an end-to-end neural autoencoder, completely outside the scope of analytical gradient-domain reconstruction claims.

# Not verified

- The candidate algorithms were not compiled or executed against the 103 bracketed raw test pairs during this research session; SSIMULACRA 2 score improvements and execution runtimes are inferences based on algorithm complexity and published results.
- In Candidate 1, whether a single covariance matrix per connected clipped component is sufficient for wide sky expanses with subtle color temperature gradients (such as sunset horizons transitioning to zenith blue) was inferred rather than tested; tiling large segments may be necessary.
- In Candidate 2, the exact clustering stability of SLIC superpixels on noisy raw sensor data was inferred from literature rather than measured directly on camera CFA arrays.
