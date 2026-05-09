# Code Review & Mathematical Analysis: Diffuse or Sharpen (Ansel fork vs. Darktable)

## Conclusion on the Best Match for the Article
The Ansel fork (`/workspace/ansel/src/iop/diffuse.c` and `/workspace/ansel/data/kernels/diffuse.cl`) is a **much better match** for the mathematical model described in the article "The mathematics of diffuse or sharpen".

The article explicitly highlights that because photographs are digital reproductions where exposure scaling affects signal variance, a standard statistical variance over the High-Frequency (HF) layer is insufficient to normalize the diffusion. Instead, it defines an **exposure-invariant band energy**: 
`Energy = sum ( HF^2 / max(LF, FLT_MIN)^2 )` 
This matches the implementation found in the Ansel fork (introduced in `d7b0cfab` and refined in `85188c39` and `6fbe2a41`), while the current Darktable implementation incorrectly relies on the naive variance `sum(HF^2)`.

## Specific Changes to Port to Our Codebase
To bring our `diffuse.c` and `diffuse.cl` up to parity with the correct mathematical formulation and the optimizations from the Ansel fork (excluding the `DIFFUSE_V3` backward-compatibility logic), the following specific changes must be applied:

### 1. Update the Regularization Metric (Math & CPU/GPU Consistency)
- **Change**: Replace the naive `variance` calculation (`sqf(neighbour_pixel_HF[k])`) with the scale-normalized `energy` ratio calculation: `sqf(hf_value / fmaxf(lf_value, FLT_MIN))`.
- **Change**: Adjust the regularization accumulation equation from `variance = variance_threshold + variance * regularization_factor` to `energy = variance_threshold + energy * normalized_regularization`.
- **Change**: Update the final accumulation divisor from `acc / variance` to `acc / energy`.
- **References**: `d7b0cfab` (initial implementation), `6fbe2a41` (fixing GPU vs CPU inconsistencies for real), `c10ae197`.

### 2. SIMD Vectorization & OpenCL Optimizations
- **Change**: Refactor `find_gradients`, `find_laplacians`, `rotation_matrix_isophote`, `rotation_matrix_gradient`, `build_matrix`, and `compute_kernel` in the CPU path to be `__attribute__((always_inline))` and use the `dt_aligned_pixel_simd_t` types (e.g., `dt_simd_set1()`, `dt_fast_hypotf()`, `dt_fast_expf()`). 
- **Change**: In `diffuse.cl`, refactor `compute_kern` to directly take gradient arrays instead of individual scalar/vector pairs and utilize `hypot` and branchless evaluation for `cos_theta` and `sin_theta` to avoid division-by-zero errors.
- **Change**: In `diffuse.cl`, perform the convolution kernel application iteratively with `acc += kern[k] * input[k]` instead of unrolled variables.
- **References**: `85188c39` ("Optimize D or S"), `c10ae197`.

### 3. Minor FMA (Fused Multiply-Add) Nudging
- **Change**: Ensure that the compiler correctly generates FMA instructions during the accumulation of kernel derivatives. Instead of `a += b * c`, explicitly write `a = b * c + a`.
- **References**: `1565deb3` ("Diffuse or sharpen: minor FMA nudging").

### 4. Module Scaling Fixes
- **Change**: Correct the radius and scaling calculations to accurately represent `zoom` instead of `1.f / roi_in->scale`. The scaling factor needs to incorporate `dt_dev_get_module_scale(pipe, roi_in)` to avoid over/under magnification issues on downsampled mipmap inputs.
- **References**: `3f03ad57` ("Fix module scaling issue"), later adjusted in `d7b0cfab`.

### 5. B-Spline CPU Optimization
- **Change**: Port the `reduce_2D_Bspline` and `expand_2D_Bspline` optimizations along with the decimated `_bspline_horizontal_decimated` functions added to `src/common/bspline.h` and OpenCL kernels. This enhances the multiscale wavelet ladder generation performance.
- **References**: `d7b0cfab` ("Fix diffuse & sharpen bugs").

By integrating these specific commits, the `diffuse` module will become physically accurate according to the described heat equation/wavelet scheme, visually consistent across zoom levels, and significantly faster on both CPU and GPU.
