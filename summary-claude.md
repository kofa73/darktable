# Synthesis: Diffuse or Sharpen Code Reviews

Cross-referencing `dos-claude.md` (Claude), `dos-codex.md` (Codex), and `dos-gemini.md` (Gemini).

## Things All Three Agree On

1. **Ansel matches the article; darktable does not.** No ambiguity here — all three reports reach this conclusion independently.

2. **The exposure-invariant band energy regularization is the single most important change.** darktable uses `sum(HF²)`; Ansel uses `sum((HF/LF)²)` with a guarded denominator. The article explicitly derives and motivates the latter as the correct formulation (equations 50–54). All three flag this as the top-priority bug fix.

3. **CPU and GPU must be updated together.** Nobody argues for a CPU-only port. The regularization fix must land in both `src/iop/diffuse.c` and `data/kernels/diffuse.cl` simultaneously.

4. **FMA-friendly accumulation is a real optimization.** All three mention `1565deb3` and the `a = b * c + a` rewrite. All treat it as secondary to the math fix.

5. **The SIMD infrastructure (`dt_aligned_pixel_simd_t`) is not a direct cherry-pick.** Even Gemini, which is the most aggressive about porting it, frames it as a "refactor." Claude and Codex explicitly warn it requires adaptation to darktable's existing `for_each_channel()` / `dt_aligned_pixel_t` patterns. All agree the optimization ideas have value; the disagreement is about timing and method (see Conflicts below).

6. **Pre-computing `normalized_regularization` on the host** is necessary as part of the math fix. Ansel passes one pre-computed value instead of three raw parameters to the PDE kernel. This is not a standalone cleanup — it's load-bearing because it carries the zoom-aware `sqf(real_radius)` scaling.

## Things Not All Reports Mention

### High priority — should be included in first wave

- **OpenCL blur pass ordering mismatch** (Codex only). darktable's OpenCL path runs horizontal-then-vertical (`diffuse.c:1528–1537`); the CPU path runs vertical-then-horizontal (`bspline.h:160–166`). Ansel fixed this with an explicit comment: "Keep the same separable order as the CPU path" (`c10ae197`, `f24440d5`). I verified this against the code — Codex is right. For a symmetric B-spline kernel on an infinite image the order is mathematically equivalent, but with finite precision, boundary handling, and tiling, the order matters. This is a functional correctness issue, not cosmetic.

- **`fmax(0.f)` clamp missing in the `else` (passthrough) branch** (Claude only; Codex's summary acknowledges it in the synthesis but the original Codex report doesn't call it out). When mask opacity is zero, both CPU and GPU skip the PDE but recombine `HF + LF` without clamping. Since the module operates in scene-referred linear RGB where negative values are unphysical, this is a bug. Ansel clamps in both paths (`6fbe2a41`). Straightforward fix, low risk.

- **GPU re-reads center pixel instead of using `neighbour_pixel[4]`** (Claude only). The GPU kernel does a redundant `read_imagef()` for the center pixel that was already fetched into the neighbor array. Ansel uses `neighbour_pixel_HF[4]` / `neighbour_pixel_LF[4]` consistently. This is both a correctness alignment (ensures CPU and GPU use identical data) and avoids a potential texture cache inconsistency. Should be bundled with the main fix.

### Lower priority — defer to second wave

- **`native_sqrt` vs `dtcl_sqrt`** (Claude only). Micro-optimization on GPU. The result is only used for direction normalization and exp damping, so reduced precision is fine. Low risk, low impact.

- **`__DT_CLONE_TARGETS__` on hot helpers** (Codex only). Multi-ISA compilation hint. Pure performance, not tied to the math fix. Follow-up.

### Assess during planning — relevance disputed

- **Module scaling via `dt_dev_get_module_scale()`** (Gemini only as an explicit porting target). Claude and Codex cover the *effect* (zoom-aware `real_radius = current_radius * zoom`) without suggesting the Ansel API function be ported. The zoom-awareness is already implicit in the `normalized_regularization` precomputation. The question is whether darktable's existing `piece->iscale / roi_in->scale` correctly produces the same `zoom` factor in all pipeline contexts (FULL, PREVIEW, THUMBNAIL, EXPORT). This needs investigation during planning — if darktable's existing scale calculation is equivalent, no API change is needed; if it's subtly wrong for some pipe type, it's a high-priority fix disguised as a low-priority API port.

- **B-spline decomposition optimizations** (Gemini only — decimated bspline functions, `reduce_2D_Bspline`/`expand_2D_Bspline`, local-memory OpenCL kernels). Claude explicitly excludes these as "separate optimization in the B-spline decomposition, not in the diffuse PDE itself." Codex doesn't mention them. Gemini frames them as performance-relevant. My assessment: these are real performance improvements but orthogonal to the math fix. They also have a larger blast radius (touching `src/common/bspline.h` affects other modules that use the same wavelet infrastructure). Defer unless profiling shows the bspline passes are the bottleneck.

## Things Where They Conflict

### 1. SIMD refactoring: port, adapt, or skip?

| Report | Position |
|--------|----------|
| Gemini | Port it: refactor CPU path to use `dt_aligned_pixel_simd_t`, `dt_simd_set1()`, `always_inline`, etc. |
| Codex  | Adapt ideas, don't copy code: the optimization is real but needs translation to darktable's abstractions |
| Claude | Don't port: darktable's `for_each_channel()` achieves the same via autovectorization; porting requires Ansel's SIMD headers |

**My assessment:** Claude and Codex are more convincing. darktable's autovectorization path works and is maintained. Porting Ansel's explicit SIMD layer would mean either (a) importing Ansel's SIMD headers as a dependency, or (b) rewriting the abstraction in darktable terms — at which point you're doing the same work as "adapt ideas." The right approach is: land the math fix using darktable's existing coding patterns, then profile, then optimize hot paths if needed.

### 2. Scope of first-wave port

| Report | First wave includes |
|--------|---------------------|
| Claude | Regularization math + pre-computation + `fmax` clamp + center pixel reuse + `native_sqrt` |
| Codex  | Regularization math + OpenCL pass ordering + then optionally SIMD/perf |
| Gemini | Regularization math + SIMD refactor + B-spline optimization + module scaling |

**My assessment:** The correct first wave is the union of bug fixes and correctness items from all three, without the performance/refactoring work:
1. Exposure-invariant regularization (`HF/LF` energy metric) — CPU and GPU
2. Pre-computed `normalized_regularization` with zoom-aware radius
3. OpenCL blur pass ordering fix (vertical-first to match CPU)
4. `fmax(0.f)` clamp in else branch — CPU and GPU
5. Use `neighbour_pixel[4]` instead of re-reading center pixel in GPU
6. Verify zoom/scale factor correctness across pipe types

Items 1–5 are concrete code changes. Item 6 is investigation that may or may not produce a code change. Everything else (SIMD, FMA, B-spline, clone targets, `native_sqrt`) is second wave.

### 3. Gemini's broader porting scope

Gemini is consistently the most aggressive about what to bring back, recommending B-spline optimizations, SIMD infrastructure, and the module scaling API as part of the primary port. Claude and Codex are more conservative, focusing on mathematical correctness first.

**My assessment:** Gemini's recommendations aren't wrong — those are real improvements — but they conflate "things Ansel does better" with "things that should be in the first backport." Mixing correctness fixes with infrastructure refactoring in the same change set increases risk and review burden. The math fix is well-scoped, well-motivated by the article, and testable (compare output before/after with known exposure variations). The SIMD and B-spline work is harder to validate and has wider blast radius. Keep them separate.

## Summary Table for Planning

| Change | Claude | Codex | Gemini | Priority |
|--------|--------|-------|--------|----------|
| Exposure-invariant regularization (HF/LF) | **Yes** | **Yes** | **Yes** | **P0 — must have** |
| CPU + GPU updated together | **Yes** | **Yes** | **Yes** | **P0 — must have** |
| Pre-computed `normalized_regularization` | **Yes** | Implicit | Implicit | **P0 — part of above** |
| OpenCL blur pass ordering fix | No | **Yes** | No | **P0 — correctness** |
| `fmax(0.f)` clamp in else branch | **Yes** | No | No | **P1 — bug fix** |
| Use `neighbour_pixel[4]` in GPU | **Yes** | No | No | **P1 — consistency** |
| Zoom/scale factor investigation | Partial | Partial | **Yes** | **P1 — investigate** |
| FMA accumulation rewrite | **Yes** | **Yes** | **Yes** | P2 — optimization |
| `native_sqrt` in GPU | **Yes** | No | No | P2 — micro-opt |
| SIMD refactoring | Skip | Adapt | Port | P2 — optimization |
| B-spline optimizations | Skip | Skip | Port | P2 — optimization |
| `__DT_CLONE_TARGETS__` | Skip | Mention | No | P2 — optimization |
