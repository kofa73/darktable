# Synthesis of `dos-claude.md`, `dos-codex.md`, and `dos-gemini.md`

## Things They Agree On

- All three reports agree that the live Ansel implementation is a better match than darktable for the maths article.
- All three agree that the highest-value functional delta is the regularization metric:
  - darktable still uses raw local HF variance, `sum(HF^2)`;
  - Ansel uses an exposure-relative band-energy term based on `HF / LF`, guarded against zero/negative LF;
  - this is treated as the main mathematical correction, not a cosmetic refactor.
- All three agree that this regularizer change has to be brought back on both CPU and OpenCL paths together. Nobody argues for a CPU-only import.
- All three agree that Ansel also contains useful performance work beyond the math fix, especially SIMD/FMA-oriented cleanup, but that performance work is secondary to the functional/math corrections.

## Things Not All Of Them Mention

- OpenCL wavelet-pass ordering fix:
  - Only `dos-codex.md` calls this out explicitly.
  - It looks relevant and should be treated as high priority. The current darktable OpenCL path still runs horizontal then vertical, while Ansel was changed to mirror the CPU order, vertical then horizontal. This is a functional consistency issue, not just cleanup.

- Module scaling / zoom handling (`dt_dev_get_module_scale(...)` vs `piece->iscale / roi_in->scale`):
  - Only `dos-gemini.md` calls this out explicitly.
  - It is relevant. The current darktable code still uses the older scaling expression, while Ansel uses module-scale-aware zoom. This should likely be brought back at high priority if the future plan aims for behavioral parity, because it affects radius interpretation and mipmap/downsampled behavior.

- CPU/GPU consistency fixes around the PDE update implementation:
  - `dos-claude.md` uniquely highlights two concrete items:
    - using the already-fetched center sample (`neighbour_pixel_[4]`) instead of re-reading the center pixel in OpenCL;
    - clamping the `else` branch output with `fmax(..., 0.f)` on both CPU and GPU.
  - Both are relevant. The clamp looks like a real correctness fix and should be high priority. The center-pixel reuse is also worth bringing back, but probably below the math fix, pass-order fix, and scaling fix.

- Precomputing `normalized_regularization` on the host:
  - Mostly discussed by `dos-claude.md`, partially implicit in `dos-codex.md`.
  - Relevant, but not high priority by itself. It becomes necessary once the regularizer math is imported, because it is part of how Ansel expresses the corrected denominator consistently across CPU and GPU.

- `native_sqrt` in OpenCL:
  - Only `dos-claude.md` mentions it.
  - Low priority. This is a performance tweak, not part of the mathematical bring-back.

- B-spline CPU/OpenCL optimization work:
  - Only `dos-gemini.md` mentions porting broader B-spline optimizations and decimated helpers.
  - Probably relevant as a later optimization pass, but not high priority for the first bring-back plan. The reports do not tie it to the core math mismatch as strongly as they do for the regularizer and pass-order fixes.

- Clone-target attributes / multi-target compilation hints:
  - Only `dos-codex.md` mentions `__DT_CLONE_TARGETS__`.
  - Low priority. Pure performance follow-up.

## Things Where They Conflict

- Priority and scope of the performance refactor:
  - `dos-gemini.md` presents SIMD/OpenCL optimization work as part of the specific changes to port.
  - `dos-codex.md` argues that the SIMD refactor is real but should not be treated as a direct cherry-pick and should come only after the functional fixes.
  - `dos-claude.md` also treats FMA/perf work as secondary.
  - My read: `dos-codex.md` and `dos-claude.md` are more convincing here. The future plan should separate functional parity from optimization adaptation.

- Importance of precomputing `normalized_regularization`:
  - `dos-claude.md` frames this partly as kernel cleanup / structural alignment.
  - `dos-codex.md` ties it more directly to the physical-radius normalization used by Ansel.
  - This is not a hard contradiction, but the emphasis differs.
  - My read: the important part is not the precompute itself; it is the corrected normalized denominator and radius scaling it carries. The host-side precompute is implementation detail attached to that functional change.

- Whether the future bring-back should include broad B-spline optimization work in the first wave:
  - `dos-gemini.md` leans yes.
  - The other two reports do not support that as a first-wave priority.
  - My read: this should stay out of the first-wave functional import unless later investigation shows it is required by the pass-order/scaling fixes.

## Bottom Line For Future Planning Input

- Strong consensus first-wave items:
  - import the HF/LF-based exposure-relative regularizer;
  - import it on CPU and OpenCL together;
  - treat Ansel as the article-aligned reference implementation.

- High-priority items that are not unanimous but look important:
  - OpenCL wavelet-pass ordering fix;
  - module scaling / zoom fix;
  - `else`-branch non-negativity clamp;
  - CPU/GPU consistency cleanup around reuse of the fetched center sample.

- Lower-priority follow-up items:
  - SIMD/FMA refactors;
  - `native_sqrt`;
  - clone-target attributes;
  - broader B-spline performance work unless it proves necessary for correctness parity.
