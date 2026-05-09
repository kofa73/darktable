<summary>
### Things they agree on
- **Core Mathematical Model:** The Ansel fork accurately implements the article "The mathematics of diffuse or sharpen", whereas darktable uses an outdated model. 
- **Exposure-Invariant Band Energy:** All three reports emphasize that the most critical fix is replacing the raw high-frequency variance (`sum(HF^2)`) with an exposure-invariant ratio (`sum((HF / max(LF, FLT_MIN))^2)`).
- **Zoom-Aware Normalization:** The regularization scale factor must account for the physical blur radius (incorporating zoom) and be pre-calculated.
- **CPU/GPU Parity:** Both the CPU (`diffuse.c`) and OpenCL (`diffuse.cl`) code paths must be updated simultaneously to maintain behavioral consistency.
- **FMA Optimization:** All reports note the minor optimization of rewriting accumulation loops to be Fused-Multiply-Add (FMA) friendly (i.e., `a = b * c + a` instead of `a += b * c`).

### Things not all of them mention, and their relevance
- **OpenCL Separable Blur Pass Ordering (Codex):** Only Codex notes that Ansel fixed the OpenCL wavelet blur pass ordering (vertical then horizontal) to match the CPU path. *Relevance: High.* Since the wavelet ladder drives the PDE update, an inconsistent pass order is a correctness issue and should be brought back at high priority.
- **GPU/CPU Consistency Bugs (Claude):** Claude uniquely identifies that the GPU `else` branch is missing an `fmax(..., 0.f)` clamp (causing potential negative pixel values to leak), and that the GPU re-reads the center pixel instead of using the already-fetched `neighbour_pixel[4]`. *Relevance: High.* These are direct correctness and structural parity fixes that should be included in the primary math port.
- **`native_sqrt` vs `dtcl_sqrt` on GPU (Claude):** Claude mentions switching to `native_sqrt` for OpenCL gradient magnitude. *Relevance: Low.* This is a straightforward micro-optimization but not critical for mathematical correctness. It can be deferred or included as a low-risk bonus.

### Things where they conflict
- **SIMD Abstractions (`dt_aligned_pixel_simd_t`) and OpenMP Macros:** Gemini suggests directly porting Ansel's SIMD abstractions and `__DT_CLONE_TARGETS__`. Claude explicitly argues *against* this, pointing out that Darktable has its own SIMD/autovectorization patterns (`for_each_channel`, `dt_aligned_pixel_t`). Codex takes the middle ground: the optimization ideas are valid but require careful adaptation to Darktable's codebase rather than a blind copy.
- **Pipeline API Scaling (`dt_dev_get_module_scale`):** Gemini recommends porting `dt_dev_get_module_scale` to fix zoom scaling. Claude notes this is a proprietary Ansel API change and that Darktable must use its existing `piece->iscale / roi_in->scale` mechanics to achieve the same zoom-aware radius.
- **B-Spline CPU Optimizations:** Gemini suggests porting Ansel's multiscale B-spline optimizations. Claude explicitly flags this as out of scope, noting it is a separate optimization targeting wavelet decomposition rather than the diffuse PDE math itself.
</summary>