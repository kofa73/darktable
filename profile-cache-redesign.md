# Pixelpipe profile-cache redesign — ideas mined from `agx-export-profile`

Source branch: `agx-export-profile` (13 commits after `4004550db279`, May 17 2026).
Status: branch was messy (heavy iteration, multiple back-and-forth corrections, Codex collateral finds) but solidified a coherent design. This document distills the reusable ideas for a clean re-implementation on master.

Branch commit map (chronological):

| Commit | Title |
|---|---|
| `0a84fff308` | pixelpipe: hash export+output profile identity, split helper API |
| `8f3bdb44b9` | colorout: rebuild all pipes on system profile change |
| `6cd486843e` | pixelpipe: assert and document export-profile init invariant |
| `b617cd4674` | agx: use dt_ioppr_get_pipe_export_profile_info |
| `f210851f4e` | pixelpipe_hb: add type+filename+intent for output profile; colorout populate; iop_profile consume |
| `dbb14fcba5` | iop_profile: restore sRGB fallback for non-matrix profiles |
| `a183654bd4` | pixelpipe_cache: hash resolved output identity, not display globals |
| `a8f109bf67` | iop_profile: avoid stale display profile info |
| `ac925dff2f` | pixelpipe_hb: fix output profile comment typo |
| `17cd0ca235` | pixelpipe: zero-fill profile filename buffers before strlcpy |
| `ddfa4d765d` | Fix stale display profile info cache entries |
| `928155d187` | Cache display profile info by ICC content |
| `0efec8ba65` | Synchronise pipe profile identity and dev->allprofile_info access |

---

## 1. Architectural moves (foundation)

### 1.1 Bytes-only pipe profile identity
Source: `0a84fff308`, refined `f210851f4e`.

- Remove `pipe->output_profile_info` pointer from the cache key.
- Add byte fields on `dt_dev_pixelpipe_t`:
  - `pipe->export_{type, filename, intent}`
  - `pipe->output_{type, filename, intent}`
- `colorout::commit_params` populates them **before** any early-return.
- `_dev_pixelpipe_cache_basichash` mixes those bytes, not the interned pointer.
- Resolves in one stroke:
  - **1.1** (FULL pipe cache ignores export profile)
  - **1.2** (LAB early-return leaves stale `output_profile_info`)
  - **1.4** (non-matrix profiles collapse to sRGB cache identity)
  - **A9 / pointer-recycling collisions**

### 1.2 Split helper API by semantics
Source: `0a84fff308`.

Two distinct semantic concepts behind a single field today:

| New helper | Returns | Consumers |
|---|---|---|
| `dt_ioppr_get_pipe_export_profile_info(dev, pipe)` | user's chosen export gamut (the deliverable) | `agx`, `filmicrgb` |
| `dt_ioppr_get_pipe_output_profile_info(dev, pipe)` | what `colorout` actually renders to on this pipe (export profile on EXPORT, display on FULL/PREVIEW/THUMBNAIL, display2 on PREVIEW2) | `channelmixerrgb`, `colorbalancergb`, `colorequal`, `primaries`, post-colorout branch of `dt_ioppr_get_pipe_current_profile_info` |

Clear contracts prevent the type of confusion that produced bug 1.1 (filmic naming its variable `export_profile` but reading the output-profile helper).

### 1.3 sRGB fallback location: setter → getter
Source: `dbb14fcba5` (correction commit after `a183654bd4` removed the fallback prematurely).

- Master's `dt_ioppr_set_pipe_output_profile_info` substitutes sRGB for any profile lacking a 3×3 matrix; consumers can deref `matrix_in`/`matrix_out` unconditionally.
- First branch attempt dropped the fallback — broke `primaries.c` slider painting, `channelmixerrgb.c` `gui_post_expose`, `darktable_ucs_22_helpers.h`, and `filmicrgb.c` export-gamut prep, all of which only NULL-check.
- Fix: keep the sRGB fallback, but **inside both new getters**, keyed on identity bytes. Cache identity (bytes-only) stays distinct for distinct non-matrix profiles; consumers still get a matrix-bearing profile_info object.
- **Implication for the filmic→export switch:** no new matrix-validity guards needed in `filmicrgb` if this fallback lives in the getter.

### 1.4 Display profile identity = ICC content key
Source: `928155d187`.

- Compute and store a content hash next to the display xprofile bytes; update it under `xprofile_lock` after LCMS has validated the new buffer.
- When writing identity into `pipe->{export,output}_filename` for a system display profile, write a synthetic key `display:<content-key>` / `display2:<content-key>` rather than the live display filename.
- Pipe identity stays bound to the bytes the pipe was built with, even if the live display profile is replaced concurrently.
- Resolves **1.5** (symbolic-type display cache reuse), with side benefit of making **1.8** (publish-before-validate) and **1.10** (file-ingestion checks) natural to fix in the same edit.

---

## 2. Targeted patches (smaller, layerable)

### 2.1 Rebuild ALL pipes on display profile change (bug 1.3)
Source: `8f3bdb44b9`.

`DT_SIGNAL_CONTROL_PROFILE_CHANGED` carries no payload identifying which monitor changed. Replace `dt_dev_reprocess_center` (FULL-only) with `dt_dev_pixelpipe_rebuild` which marks all three pipes' caches obsolete. Display profile changes are rare; over-invalidation is acceptable.

### 2.2 Zero-fill pipe filename buffers before strlcpy (bug 1.13 at pipe level)
Source: `17cd0ca235`.

`basichash` hashes `sizeof(pipe->{export,output}_filename)` including tail bytes. `g_strlcpy` only NUL-terminates. Identical filenames produced different hashes; distinct filenames sharing a prefix could alias. Fix: `memset` to zero in `dt_dev_pixelpipe_init` and again in `colorout::commit_params` before each strlcpy. Mirrors the module-params fix already landed for bug 1.13.

### 2.3 Atomic display ICC publish (bug 1.8)
Source: `928155d187`.

Reorder `_update_display_profile` / `_update_display2_profile`:
1. Locate target profile slot.
2. Open and validate `cmsHPROFILE` via LCMS.
3. Only on success: publish bytes, key, profile pointer; transfer buffer ownership.
4. Caller raises `DT_SIGNAL_CONTROL_PROFILE_CHANGED` only after a successful publish.

### 2.4 Harden file-backed profile updates (bug 1.10)
Source: `928155d187`.

- Check `g_file_get_contents()` return value.
- Reject NULL, empty, and oversized buffers **before** narrowing `gsize` to `int`.
- Win32 monitor API failures route through the shared unlock cleanup path (already done as bug 1.9).

### 2.5 Intent in profile-info lookup key (bug 1.12)
Source: `0efec8ba65`.

`dt_ioppr_add_profile_info_to_list` and `dt_ioppr_get_profile_info_from_list` match on `(type, filename, intent)`. Per-intent display entries no longer collide; non-matrix profiles requested with different intents no longer silently share the first-creation intent.

### 2.6 Signal handlers in widget-painting IOPs (bug 1.14)
Source: `0efec8ba65`.

Add subscriptions in `colorbalancergb.c`, `colorequal.c`, and tighten in `primaries.c`:
- `DT_SIGNAL_CONTROL_PROFILE_CHANGED`
- `DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED`
- `DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED`

Force widget repaint after system profile change even when the cached profile_info pointer survives.

### 2.7 LUT alloc failure tolerance (partial bug 1.7)
Source: `0efec8ba65`.

`dt_ioppr_init_profile_info` and `_ioppr_alloc_profile_info` handle LUT alloc failure: clean up rather than dereference NULL. Failure branch of `dt_ioppr_add_profile_info_to_list` properly cleans the wrapper.

---

## 3. Hard-won lessons from the messy iterations

### 3.1 Commit-ordering invariant deserves an assert
Source: `6cd486843e` (Codex catch).

`process()` reads `pipe->export_*` assuming `colorout` has already committed. Enforced by `dt_dev_pixelpipe_synch_all` (commits every node in pipe order before any `process()` runs), but undocumented.

Add:
- `assert(pipe->export_type != DT_COLORSPACE_NONE)` in `dt_ioppr_get_pipe_export_profile_info`.
- Comment block on the `dt_dev_pixelpipe_t` fields documenting the invariant and pointing at the assert.

Defensive NULL return for release builds (NDEBUG).

### 3.2 GC stale display profile_info: materialise-only, never prune
Source: `0efec8ba65` (corrects `ddfa4d765d`).

- Pipe workers can hold synthetic keys captured before a display change; `basichash` needs the matching `profile_info` entry to remain reachable.
- Pruning would race with workers and double-free.
- `dt_ioppr_gc_stale_display_profile_info` only materialises entries for the current display ICC so GUI pointer-identity checks see a fresh pointer immediately after a profile change. Stale entries are freed wholesale at `dt_dev_cleanup`.
- Reframes bug **1.11**: in-session growth is the cost of safety; don't fight it.

### 3.3 Pipe identity + allprofile list need their own mutexes
Source: `0efec8ba65`.

- `pipe->profile_identity_mutex` — `colorout::commit_params` snapshots `pipe->{export,output}_*` under it; getters copy identity out under it before consulting the profile list.
- `dev->allprofile_mutex` — serialise list walks and appends; re-check before `g_list_append` so concurrent producers do not duplicate entries.
- Race surface is bigger than the original "1.6" framing suggested.

### 3.4 Bind xform creation with identity capture under one xprofile_lock critical section
Source: `0efec8ba65`.

Fixes bug **1.6** (lock released before matrix/LUT extraction) by structurally tying the two operations together: the cache key and the `cmsHPROFILE` used to populate `d->cmatrix` / `d->xform` always describe the same ICC bytes.

### 3.5 D65_adapt_iccprofile must deep-copy LUTs
Source: `0efec8ba65` (Codex-collateral find).

`D65_adapt_iccprofile` in `darktable_ucs_22_helpers.h` previously shared LUT pointers with the source profile. Free-time double-free / dangling pointer. Fix: deep-copy LUTs into the adapted copy; expose `D65_adapted_iccprofile_free` for callers. Caught because the cache redesign exercised cleanup paths more aggressively.

### 3.6 agx: treat sRGB fallback as "missing" for non-sRGB selection
Source: `0efec8ba65`.

When `dt_ioppr_get_pipe_export_profile_info` returns the sRGB fallback for a non-sRGB export selection, agx falls back to Rec2020 instead of silently using sRGB primaries. Generalisable principle for any module that cares about wide-gamut accuracy.

### 3.7 AdobeRGB mipmap intent is always colorimetric
Source: `0efec8ba65`.

Mipmap output uses AdobeRGB regardless of user display intent; force colorimetric intent for that path. Narrow but easily-overlooked fix.

---

## 4. Mapping back to the master todo list (verified_1.md)

| # | Status today | Resolved by |
|---|---|---|
| 1.1 | **fixed on master** | (already landed; this branch's `0a84fff308` is an alternative, more general approach) |
| 1.2 | todo | subsumed by §1.1 (bytes-only identity) |
| 1.3 | todo | §2.1 |
| 1.4 | todo | subsumed by §1.1 + §1.3 (getter-side fallback) |
| 1.5 | todo | §1.4 |
| 1.6 | todo | §3.4 |
| 1.7 | deferred | partially §2.7; full fix needs `dt_ioppr_cleanup_profile_info` on the failure branch |
| 1.8 | todo | §2.3 |
| 1.9 | **fixed on master** | (already landed) |
| 1.10 | todo | §2.4 |
| 1.11 | deferred | §3.2 (reframed: materialise-only is the correct policy) |
| 1.12 | todo | §2.5 |
| 1.13 | **fixed on master** (module params) | §2.2 covers the analogous pipe-level fix |
| 1.14 | todo | §2.6 |

---

## 5. Suggested re-implementation order on master

Stacked commits, each independently reviewable:

1. **§2.1** — rebuild all pipes on display change (1 line; immediate user-visible improvement).
2. **§2.6** — signal handlers in `colorbalancergb`/`colorequal`/`primaries` (3 small files, independent).
3. **§2.5** — intent in lookup key (small, but check downstream callers).
4. **§2.3 + §2.4** — atomic display publish + file-ingestion hardening (cohesive change in `colorspaces.c`).
5. **§1.4** — display profile content key + synthetic filename. Foundation for the rest.
6. **§1.1 + §1.2 + §1.3** — bytes-only pipe identity + split getter API + getter-side sRGB fallback. This is the big one; bundle with §2.2 (zero-fill filename buffers) since they touch the same struct.
7. **§3.4** — bind identity capture with xform creation under one lock (after §1.4 lands the synthetic key).
8. **§3.1** — assert + invariant docs.
9. **§3.3** — pipe identity mutex + allprofile mutex (after §1.1).
10. **§3.5** — D65_adapt_iccprofile deep-copy (independent; can land anytime).
11. **§3.6 + §3.7** — agx Rec2020 fallback + AdobeRGB intent (independent, after §1.3 getter API exists).
12. **§3.2 + §2.7** — GC materialise-only + LUT alloc tolerance.

After step 6, filmic can be migrated to `dt_ioppr_get_pipe_export_profile_info` (commit `b617cd4674` shows the agx version; filmic is analogous and equally trivial, ~6 lines).

---

## 6. Lessons about how to attack this work

- **Setter vs getter fallback location matters.** Drop the fallback from one without restoring it in the other → cascade of NULL-deref / garbage-matrix bugs across 4+ files.
- **Materialise-don't-prune** is the right policy for shared profile_info entries with concurrent pipe workers.
- **Codex flagged two non-obvious issues** (commit-ordering invariant; D65_adapt LUT sharing) that pure inline review missed.
- **Bytes-only identity** decouples cache correctness from object lifetime, eliminating an entire class of "interned pointer + fallback substitution" bugs.
- **Synthetic display keys** are the cleanest way to make pipe identity time-invariant under live profile replacement without requiring all callers to know about content hashing.

---

## Appendix A — Code anchors (master `d75585d039`)

All line numbers below are on current master HEAD. Use these as entry points for re-implementation.

### A.1 Pipe cache key
- `src/develop/pixelpipe_cache.c:125-156` — `_dev_pixelpipe_cache_basichash()`. Currently mixes 4 profile fields:
  - `&pipe->input_profile_info` (8 bytes pointer)
  - `&pipe->work_profile_info` (8 bytes pointer)
  - `&pipe->output_profile_info` (8 bytes pointer)
  - `&pipe->export_profile_info` (8 bytes pointer, added by master fix `9129daad10`)
  - Then `&piece->hash` per piece with index < position.

### A.2 Pipe struct
- `src/develop/pixelpipe_hb.h:140-150` — `dt_dev_pixelpipe_t`. Profile pointer fields here.
- `src/develop/pixelpipe_hb.h:236-260` — pipe-type classifiers (`dt_pipe_is_full/thumb/export/preview/preview2`).

### A.3 colorout
- `src/iop/colorout.c:560` — `dt_ioppr_set_pipe_export_profile_info()` call (added by `9129daad10`, unconditional, early).
- `src/iop/colorout.c:564-620` — pipe-type dispatch resolving `out_type`, `out_filename`, `out_intent`.
- `src/iop/colorout.c:622-629` — LAB early-return (currently has marginal 1.2 patch applied).
- `src/iop/colorout.c:763` — original location of `dt_ioppr_set_pipe_output_profile_info()` (removed by 1.2 patch).
- `src/iop/colorout.c:278-283` — `_signal_profile_changed` handler.
- `src/iop/colorout.c:873` — handler registration.

### A.4 Profile info management
- `src/common/iop_profile.h:38-50` — `dt_iop_order_iccprofile_info_t` definition.
- `src/common/iop_profile.h:41` — `char filename[DT_IOP_COLOR_ICC_LEN]` (= 512 bytes).
- `src/common/iop_profile.c:636-665` — `dt_ioppr_init_profile_info()` / cleanup pair; LUT allocation.
- `src/common/iop_profile.c:670-779` — `_ioppr_generate_profile_info()` (display lock bug 1.6 at lines 694-703).
- `src/common/iop_profile.c:783-801` — `dt_ioppr_get_profile_info_from_list()` (lookup key omits intent — bug 1.12).
- `src/common/iop_profile.c:803-826` — `dt_ioppr_add_profile_info_to_list()` (failure branch leaks LUTs — bug 1.7 at line 821).
- `src/common/iop_profile.c:957-986` — `dt_ioppr_set_pipe_output_profile_info()` (sRGB fallback at 969-983 — bug 1.4).
- `src/common/iop_profile.c:988-999` — `dt_ioppr_set_pipe_export_profile_info()` (no fallback — clean).
- `src/common/iop_profile.c:1020-1023` — `dt_ioppr_get_pipe_output_profile_info()` (just returns `pipe->output_profile_info`).
- `src/common/iop_profile.c:1025-1048` — `dt_ioppr_get_pipe_current_profile_info()` (post-colorout branch reads output).
- `src/common/iop_profile.c:1199-1280` — `dt_ioppr_transform_image_colorspace()` (matrix-vs-LCMS dispatch; intent consumed in `_transform_lcms2` at 294, 302).

### A.5 Display profile lifecycle
- `src/common/colorspaces.h:54` — `#define DT_IOP_COLOR_ICC_LEN 512`.
- `src/common/colorspaces.c:1245-1274` — `_update_display_profile()` (bug 1.8: publish before validate).
- `src/common/colorspaces.c:1276-1304` — `_update_display2_profile()` (mirror).
- `src/common/colorspaces.c:1810-1879` — colord callback path (bug 1.10 partial: unchecked file read, mutated state on failure).
- `src/common/colorspaces.c:1904-2094` — `dt_colorspaces_set_display_profile()` (Win32 path). Bug 1.10: `gint buffer_size = 0; ... buffer_size = size;` narrowing at 1916-1917 and 2049-2050.
- `src/common/colorspaces.c:1408` — `pthread_rwlock_init(&res->xprofile_lock, NULL)`.

### A.6 Pipe rebuild on display change
- `src/develop/develop.c:306-315` — `dt_dev_invalidate_all()` (only sets DIRTY, no `cache_obsolete`).
- `src/develop/develop.c:2762-2776` — `dt_dev_reprocess_center()` (sets `cache_obsolete` only on full pipe — bug 1.3).
- `src/develop/develop.c:2778-2788` — `dt_dev_reprocess_preview()`.
- Find `dt_dev_pixelpipe_rebuild` if it exists, else write equivalent.

### A.7 Consumer call sites
- `src/iop/filmicrgb.c:2096` (process) — `dt_ioppr_get_pipe_output_profile_info(piece->pipe)` named `export_profile`. **Migrate to `_export_profile_info`.**
- `src/iop/filmicrgb.c:2385` (process_cl) — same.
- `src/iop/filmicrgb.c:1747-1772` — `filmic_v4_prepare_matrices()` (only NULL-check; needs no extra guard once getter-side sRGB fallback lands).
- `src/iop/agx.c:417` — `dt_ioppr_get_export_profile_type(dev, &profile_type, &profile_filename)` (current introspection-based path). **Replace with `_export_profile_info()`** (see `b617cd4674`).
- `src/iop/agx.c:2745+` — `process_cl`.
- `src/iop/channelmixerrgb.c:2691` — `dt_ioppr_get_pipe_output_profile_info(self->dev->full.pipe)`. Stays on output.
- `src/iop/colorbalancergb.c:1649` — same. Stays on output. Bug 1.14: needs signal handlers.
- `src/iop/colorequal.c:2797-2807, 2944` — same. Stays on output. Bug 1.14: needs signal handlers.
- `src/iop/primaries.c:298` — same. Stays on output. Already has handlers (1.14 confirmed-not-present).

### A.8 Pre-existing handler reference
- `src/iop/primaries.c:303-306, 340-351, 407-409` — canonical signal-subscription pattern to copy into `colorbalancergb.c` / `colorequal.c`.

### A.9 Cleanup / lifetime
- `src/develop/develop.c:236` — `while(dev->allprofile_info) { ... }` cleanup loop.
- `src/develop/pixelpipe_hb.c:293` — `pipe->output_profile_info = NULL` init.
- `src/common/colorspaces.c:1724-1729` — `pthread_rwlock_destroy(&self->xprofile_lock)` + `xprofile_data` cleanup.

---

## Appendix B — Concrete branch signatures (verbatim from canonical commits)

### B.1 New pipe struct fields (`pixelpipe_hb.h`, after `0a84fff308` + `f210851f4e`)

```c
typedef struct dt_dev_pixelpipe_t
{
  // ... existing fields ...
  struct dt_iop_order_iccprofile_info_t *work_profile_info;
  struct dt_iop_order_iccprofile_info_t *input_profile_info;

  /* Resolved export profile identity for this pipe, from colorout commit_params.
     Use for gamut maths and pipe cache basichash to invalidate cache when export
     profile changes. Stored as bytes (not a profile_info pointer) so the
     hash is keyed on identity, not on a struct address that the profile
     list can recycle.
     See assert in dt_ioppr_get_pipe_export_profile_info. */
  dt_colorspaces_color_profile_type_t export_type;
  char export_filename[DT_IOP_COLOR_ICC_LEN];
  dt_iop_color_intent_t export_intent;

  /* Resolved rendering-target identity for this pipe, from colorout commit_params.
     Mirrors out_type/out_filename/out_intent the if/else dispatch in colorout
     produces: export profile on EXPORT pipe, display2 on PREVIEW2, mipmap-cache
     colorspace on THUMBNAIL, display on FULL/PREVIEW. Read via
     dt_ioppr_get_pipe_output_profile_info to obtain the resolved profile_info.
     Validity contract: populated by colorout commit_params before the Lab
     early-return; consumers must run from a process() callback or otherwise
     after pipe synch. DT_COLORSPACE_NONE is the un-populated sentinel and
     the helper returns NULL for it. */
  dt_colorspaces_color_profile_type_t output_type;
  char output_filename[DT_IOP_COLOR_ICC_LEN];
  dt_iop_color_intent_t output_intent;

  // ... existing fields ...
} dt_dev_pixelpipe_t;
```

Final form (after `0efec8ba65`) also adds:
```c
  pthread_mutex_t profile_identity_mutex;  // serialises commit_params snapshot vs getter reads
```

### B.2 New helper API (`iop_profile.h`, after `0a84fff308`)

```c
/** Resolved profile information for the user's chosen export profile (i.e. the gamut
   of the deliverable). Suitable for modules that need the target primaries even when
   rendering to a different screen profile, such as filmicrgb's gamut compression and
   AgX. Returned struct lives in dev's profile list and must not be freed by the caller. */
dt_iop_order_iccprofile_info_t *
dt_ioppr_get_pipe_export_profile_info(struct dt_develop_t *dev,
                                      const struct dt_dev_pixelpipe_t *pipe);

/** Resolved profile information for the pipe's rendering target (what colorout writes:
   export profile on EXPORT, display on FULL/PREVIEW/THUMBNAIL, display2 on PREVIEW2). */
dt_iop_order_iccprofile_info_t *
dt_ioppr_get_pipe_output_profile_info(struct dt_develop_t *dev,
                                      const struct dt_dev_pixelpipe_t *pipe);
```

The old setter `dt_ioppr_set_pipe_output_profile_info()` is **deleted** in this design; identity is populated directly into pipe fields by `colorout::commit_params`.

### B.3 colorout populate (verbatim from `0a84fff308`)

In `colorout::commit_params`, immediately after the pipe-type dispatch that sets `out_type/out_filename/out_intent`, **before** any early-return:

```c
/* Record the user's chosen export profile on the pipe so AgX/filmic and
   the pixelpipe cache basichash see a consistent identity. p->type and
   p->intent reflect the colorout combobox; for EXPORT pipes they were
   overridden above from pipe->icc_*. Populate before the Lab early-return
   so cache identity is correct on Lab transitions too. */
pipe->export_type = p->type;
g_strlcpy(pipe->export_filename, p->filename, sizeof(pipe->export_filename));
pipe->export_intent = p->intent;
```

Plus the analogous block for output (from `f210851f4e`):
```c
pipe->output_type = out_type;
g_strlcpy(pipe->output_filename, out_filename ? out_filename : "",
          sizeof(pipe->output_filename));
pipe->output_intent = out_intent;
```

With `17cd0ca235` zero-fill, wrap both filename writes:
```c
memset(pipe->export_filename, 0, sizeof(pipe->export_filename));
g_strlcpy(pipe->export_filename, p->filename, sizeof(pipe->export_filename));

memset(pipe->output_filename, 0, sizeof(pipe->output_filename));
g_strlcpy(pipe->output_filename, out_filename ? out_filename : "",
          sizeof(pipe->output_filename));
```

(Or use `dt_strlcpy_to_fixed` — already in tree.)

Delete the old `dt_ioppr_set_pipe_output_profile_info()` call near current line 763.

### B.4 New basichash mix (verbatim from `f210851f4e`)

```c
hash = dt_hash(hash, &pipe->input_profile_info, sizeof(pipe->input_profile_info));
hash = dt_hash(hash, &pipe->work_profile_info, sizeof(pipe->work_profile_info));

// export profile identity from colorout's commit_params
hash = dt_hash(hash, &pipe->export_type, sizeof(pipe->export_type));
hash = dt_hash(hash, pipe->export_filename, sizeof(pipe->export_filename));
hash = dt_hash(hash, &pipe->export_intent, sizeof(pipe->export_intent));

// output profile identity from colorout's commit_params
hash = dt_hash(hash, &pipe->output_type, sizeof(pipe->output_type));
hash = dt_hash(hash, pipe->output_filename, sizeof(pipe->output_filename));
hash = dt_hash(hash, &pipe->output_intent, sizeof(pipe->output_intent));
```

Display globals no longer mixed directly here — they flow through `pipe->output_*` which colorout already resolved per pipe type.

### B.5 Getter implementations with sRGB fallback (verbatim from `dbb14fcba5`)

```c
dt_iop_order_iccprofile_info_t *
dt_ioppr_get_pipe_export_profile_info(dt_develop_t *dev,
                                       const struct dt_dev_pixelpipe_t *pipe)
{
  if(pipe->export_type == DT_COLORSPACE_NONE) return NULL;

  dt_iop_order_iccprofile_info_t *profile_info =
    dt_ioppr_add_profile_info_to_list(dev, pipe->export_type,
                                      pipe->export_filename, pipe->export_intent);

  if(!profile_info && dt_pipe_is_preview(pipe) && (pipe->export_type == DT_COLORSPACE_FILE))
    dt_control_log(_("export icc profile '%s' missing"), pipe->export_filename);

  if(!profile_info
     || !dt_is_valid_colormatrix(profile_info->matrix_in[0][0])
     || !dt_is_valid_colormatrix(profile_info->matrix_out[0][0]))
  {
    if(pipe->export_type != DT_COLORSPACE_DISPLAY)
      dt_print(DT_DEBUG_PIPE,
               "[dt_ioppr_get_pipe_export_profile_info] profile `%s' in `%s' replaced by sRGB",
               dt_colorspaces_get_name(pipe->export_type, NULL), pipe->export_filename);
    profile_info = dt_ioppr_add_profile_info_to_list(dev, DT_COLORSPACE_SRGB, "", pipe->export_intent);
  }
  return profile_info;
}

dt_iop_order_iccprofile_info_t *
dt_ioppr_get_pipe_output_profile_info(dt_develop_t *dev,
                                      const struct dt_dev_pixelpipe_t *pipe)
{
  if(pipe->output_type == DT_COLORSPACE_NONE) return NULL;

  dt_iop_order_iccprofile_info_t *profile_info =
    dt_ioppr_add_profile_info_to_list(dev,
                                      pipe->output_type,
                                      pipe->output_filename,
                                      pipe->output_intent);

  if(!profile_info
     || !dt_is_valid_colormatrix(profile_info->matrix_in[0][0])
     || !dt_is_valid_colormatrix(profile_info->matrix_out[0][0]))
  {
    if(pipe->output_type != DT_COLORSPACE_DISPLAY)
      dt_print(DT_DEBUG_PIPE,
               "[dt_ioppr_get_pipe_output_profile_info] profile `%s' in `%s' replaced by sRGB",
               dt_colorspaces_get_name(pipe->output_type, NULL), pipe->output_filename);
    profile_info = dt_ioppr_add_profile_info_to_list(dev, DT_COLORSPACE_SRGB, "", pipe->output_intent);
  }
  return profile_info;
}
```

`dt_ioppr_get_pipe_current_profile_info()` (around `iop_profile.c:1025-1048`) gains a `dev` argument and threads it through:
```c
  else
    color_profile = dt_ioppr_get_pipe_output_profile_info(module->dev, pipe);
```

### B.6 Synthetic display key format (from `928155d187`)

```c
static void _compute_display_cache_key(const gboolean display2,
                                       const uint8_t *data,
                                       const int size,
                                       char out[DT_IOP_COLOR_ICC_LEN])
{
  dt_hash_t h = DT_INITHASH;
  h = dt_hash(h, &size, sizeof(size));
  if(data && size > 0) h = dt_hash(h, data, size);
  memset(out, 0, DT_IOP_COLOR_ICC_LEN);
  snprintf(out, DT_IOP_COLOR_ICC_LEN, "%s:%d:%016" PRIx64,
           display2 ? "display2" : "display", size, (uint64_t)h);
}
```

Format: `"display:<size>:<16-hex-digit-hash>"` or `"display2:<size>:<16-hex-digit-hash>"`.

New fields on `dt_colorspaces_t` (`src/common/colorspaces.h`):
```c
char xprofile_cache_key[DT_IOP_COLOR_ICC_LEN];   // for DT_COLORSPACE_DISPLAY
char xprofile_cache_key2[DT_IOP_COLOR_ICC_LEN];  // for DT_COLORSPACE_DISPLAY2
```

Initialise to `"display:0:0000000000000000"` / `"display2:0:0000000000000000"` in `dt_colorspaces_init()`.

Update both keys under `xprofile_lock` write lock **after** new ICC validates via LCMS, in `_update_display_profile()` / `_update_display2_profile()`.

When colorout writes `pipe->output_filename` and `out_type == DT_COLORSPACE_DISPLAY/DISPLAY2`, write the snapshot of `xprofile_cache_key` (captured under `xprofile_lock` read) instead of `darktable.color_profiles->display_filename`.

### B.7 Mutex layout (from `0efec8ba65`)

```c
// dt_dev_pixelpipe_t
pthread_mutex_t profile_identity_mutex;  // protects pipe->{export,output}_*

// dt_develop_t (develop.h)
pthread_mutex_t allprofile_mutex;        // protects dev->allprofile_info list walks/appends
```

Acquisition order (must be strictly observed to avoid deadlock):
1. `xprofile_lock` (rwlock, system-display ICC)
2. `pipe->profile_identity_mutex`
3. `dev->allprofile_mutex`

In `colorout::commit_params` for system display target:
```c
pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
pthread_mutex_lock(&pipe->profile_identity_mutex);
// snapshot pipe->{export,output}_* using xprofile_cache_key as the filename
// build CMS xform from the same cmsHPROFILE
pthread_mutex_unlock(&pipe->profile_identity_mutex);
pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
```

### B.8 add_profile_info_to_list with intent key (from `0efec8ba65`)

```c
dt_iop_order_iccprofile_info_t *
dt_ioppr_add_profile_info_to_list(struct dt_develop_t *dev,
                                  const dt_colorspaces_color_profile_type_t profile_type,
                                  const char *profile_filename,
                                  const int intent)
{
  pthread_mutex_lock(&dev->allprofile_mutex);

  dt_iop_order_iccprofile_info_t *profile_info = NULL;
  for(GList *p = dev->allprofile_info; p; p = g_list_next(p))
  {
    dt_iop_order_iccprofile_info_t *prof = p->data;
    if(prof->type == profile_type
       && strcmp(prof->filename, profile_filename) == 0
       && prof->intent == intent)
    {
      profile_info = prof;
      break;
    }
  }

  if(profile_info == NULL)
  {
    pthread_mutex_unlock(&dev->allprofile_mutex);
    // generate outside the lock (may take xprofile_lock)
    dt_iop_order_iccprofile_info_t *fresh = dt_alloc1_align_type(dt_iop_order_iccprofile_info_t);
    if(!dt_ioppr_init_profile_info(fresh, 0))
    {
      // alloc failure tolerated
      dt_free_align(fresh);
      return NULL;
    }
    if(_ioppr_generate_profile_info(fresh, profile_type, profile_filename, intent))
    {
      // generation failed
      dt_ioppr_cleanup_profile_info(fresh);
      dt_free_align(fresh);
      return NULL;
    }
    pthread_mutex_lock(&dev->allprofile_mutex);
    // re-check before append: another thread may have inserted while we were generating
    for(GList *p = dev->allprofile_info; p; p = g_list_next(p))
    {
      dt_iop_order_iccprofile_info_t *prof = p->data;
      if(prof->type == profile_type
         && strcmp(prof->filename, profile_filename) == 0
         && prof->intent == intent)
      {
        profile_info = prof;
        break;
      }
    }
    if(profile_info)
    {
      // duplicate, discard fresh
      dt_ioppr_cleanup_profile_info(fresh);
      dt_free_align(fresh);
    }
    else
    {
      dev->allprofile_info = g_list_append(dev->allprofile_info, fresh);
      profile_info = fresh;
    }
  }
  pthread_mutex_unlock(&dev->allprofile_mutex);
  return profile_info;
}
```

This sketch combines bugs 1.7 (LUT alloc fail tolerance), 1.11 (re-check before append), 1.12 (intent in key).

---

## Appendix C — Consumer migration table

| File | Current call | Action | Reason |
|---|---|---|---|
| `src/iop/filmicrgb.c:2096` | `dt_ioppr_get_pipe_output_profile_info(piece->pipe)` | → `dt_ioppr_get_pipe_export_profile_info(self->dev, piece->pipe)` | Variable already named `export_profile`; semantic intent is export gamut |
| `src/iop/filmicrgb.c:2385` | same | same | process_cl path |
| `src/iop/agx.c:417` | `dt_ioppr_get_export_profile_type(dev, ...)` (introspection) | → `dt_ioppr_get_pipe_export_profile_info(dev, pipe)` (pattern from `b617cd4674`) | Replace fragile introspection with declared identity |
| `src/iop/channelmixerrgb.c:2691` | `dt_ioppr_get_pipe_output_profile_info(self->dev->full.pipe)` | gain `dev` arg | "what colorout renders to" semantics correct |
| `src/iop/colorbalancergb.c:1649` | same | gain `dev` arg + add signal handlers (bug 1.14) | same |
| `src/iop/colorequal.c:2797-2807` | same | same | same |
| `src/iop/colorequal.c:2944` | same | same | same |
| `src/iop/primaries.c:298` | same | gain `dev` arg | handlers already present |
| `src/common/iop_profile.c:1025-1048` | `dt_ioppr_get_pipe_current_profile_info(module, pipe)` post-colorout branch | gain `dev` arg threaded through | propagate API change |
| `src/common/darktable_ucs_22_helpers.h` | `D65_adapt_iccprofile()` shares LUT pointers | deep-copy LUTs + add `D65_adapted_iccprofile_free` (per §3.5) | double-free found by Codex |

OpenCL paths (filmic, agx) use `dt_ioppr_build_iccprofile_params_cl()` to upload profile_info to GPU. That function reads `matrix_in/matrix_out/lut_in/lut_out` from the returned profile_info — getter-side sRGB fallback ensures these are always valid, so no GPU path changes needed beyond the helper-call replacement.

---

## Appendix D — Test / verification approach

### D.1 Unit tests
- `src/tests/unittests/` uses `cmocka`. No existing profile-cache unit tests. Consider adding:
  - `_dev_pixelpipe_cache_basichash` collision test: build two `dt_dev_pixelpipe_t` with same params but different `export_filename` tail-byte garbage; assert distinct hashes after fix.
  - Profile-list dedup test for `dt_ioppr_add_profile_info_to_list` with concurrent producers (intent-key behaviour).

### D.2 Integration tests
- `src/tests/integration/run.sh` — image-based regression. Currently no test exercises display profile changes. Could add a test that renders the same XMP under sRGB vs AdobeRGB export profile and asserts pixel difference matches expected.
- **Important: do not regenerate references.** Per CLAUDE.md feedback — report failures, never regenerate unless explicitly told.

### D.3 Manual verification checklist (run darktable; observe)
1. **Bug 1.3 (display change → all pipes invalidate):** open darkroom + second window with different display profiles. Change display ICC via OS. Confirm both windows update.
2. **Bug 1.1 (cache invalidates on export profile change):** in darkroom, switch colorout export profile between sRGB and AdobeRGB; observe agx/filmic re-render (turn on `-d pipe` debug to confirm cache misses).
3. **Bug 1.4 (LUT profile distinct cache identity):** use two distinct LUT display profiles in sequence; confirm cached buffer not reused.
4. **Bug 1.13 / pipe-level filename hashing:** select profile with long name, then profile with short name; switch back to long name; confirm cache hit on second selection of long name.
5. **Filmic / agx with LUT display profile:** check gamut math reasonable (no NaN propagation, no all-sRGB clipping when wide-gamut display present).

### D.4 Build verification
```bash
cmake --build build --target darktable -j$(nproc)
cmake --build build --target colorout -j$(nproc)         # per-module
ctest --test-dir build                                    # unit tests
```

For OpenCL paths:
```bash
build/bin/darktable -d opencl -d pipe <test-image>
```

---

## Appendix E — Cold-start checklist for fresh session

Required reading in order (each ~5 min):

1. **This document** (you are here).
2. **`verified_1.md`** — original bug catalogue with master file:line evidence.
3. **The 13 branch commits, in order:**
   ```bash
   git log --reverse 4004550db279..agx-export-profile --oneline
   for c in 0a84fff308 8f3bdb44b9 6cd486843e b617cd4674 f210851f4e \
            dbb14fcba5 a183654bd4 a8f109bf67 ac925dff2f 17cd0ca235 \
            ddfa4d765d 928155d187 0efec8ba65; do
     git show "$c"
   done
   ```
4. **Verify master state** — read each file:line in Appendix A to confirm anchors still valid; note any drift since `d75585d039`.

Optional but useful:
- `cache-fix-discussion_*.md` (19 files) — running review/response thread during the branch's iteration.
- `plan_cache_synth_display_key-discussion-*.md` (20 files) — design discussion for synthetic display key.
- `pixelpipe_architecture.md` (in `dev-doc/`) — architectural overview.

### E.1 First actions in a fresh session
```bash
# 1. Confirm starting point
git status
git rev-parse HEAD

# 2. Re-anchor to current master state
git log --oneline -5

# 3. Survey untracked artefacts from prior work
ls cache-fix-discussion_*.md plan_cache_synth_display_key-discussion-*.md \
   cache_issues_verified_*.md verified_*.md 2>/dev/null | wc -l

# 4. Read the bug catalogue
cat verified_1.md

# 5. Read this document
cat profile-cache-redesign.md
```

### E.2 Branch creation suggestion
```bash
# Branch per step from §5, stacked.
# Recommended starting point: §2.1 (1-line fix, low-risk).
git checkout -b cache-fix/01-rebuild-all-on-display-change master
```

---

## Appendix F — Risk inventory

| Concern | Likelihood | Mitigation |
|---|---|---|
| Breaking image-comparison integration tests | medium | Test before each commit; never regenerate references |
| Deadlock from new mutexes | medium | Strict acquisition order documented in §B.7; review with `helgrind` if doubt |
| OpenCL profile upload assumes matrix validity | low | Getter-side sRGB fallback guarantees valid matrices |
| GTK main thread vs worker race on display change | high | Materialise-only GC (§3.2) + per-pipe identity mutex (§3.3) |
| Stale `agx` cache after colorout introspection removal | low | Bytes-only basichash captures export identity directly (§1.1) |
| AdobeRGB mipmap intent change visible to users | low | Explicit colorimetric force (§3.7); document in release notes |
| Pipe filename buffer (512 B) hashed including garbage tail | resolved by §2.2 | memset before strlcpy at every write site |

---

## Appendix G — Glossary

- **basichash**: per-piece cache identity for `pipe->cache`; mixes pipe-mode, ROI, three profile pointer fields, then upstream `piece->hash` values. See `_dev_pixelpipe_cache_basichash` in `pixelpipe_cache.c`.
- **piece->hash**: per-module hash mixing `op` name, instance ID, and module params buffer. See `dt_dev_pixelpipe_piece_hash` in `imageop.c`.
- **profile_info / `dt_iop_order_iccprofile_info_t`**: cached profile struct with matrices, LUTs, primaries, whitepoint. Lives in `dev->allprofile_info`.
- **xprofile_lock**: rwlock on `darktable.color_profiles->xprofile_data{,2}`. Held WR during display ICC publication; held RD during display profile_info generation.
- **synthetic display key**: `"display:<size>:<hex-hash>"` written into `pipe->{export,output}_filename` when target is a system display profile. Decouples pipe identity from live display-filename mutation.
- **commit-ordering invariant**: `dt_dev_pixelpipe_synch_all` commits all pipe nodes in pipe order before any `process()` runs. Means colorout has populated `pipe->{export,output}_*` before any consumer's `process()`.
- **DT_COLORSPACE_NONE**: sentinel for un-populated pipe identity; getter returns NULL.
- **DT_COLORSPACE_LAB**: edge case; `colorout::process()` is a nop for it. Pipe identity now consistently `LAB` rather than "stale prior value".

---

## Appendix H — Open design questions for plan-writer

1. **`dt_ioppr_get_pipe_output_profile_info` signature change** (added `dev` arg): breaks every caller. Confirm acceptable; consider an inline shim during transition.
2. **AssertNotNull on `pipe->export_type != DT_COLORSPACE_NONE`** in release builds: NDEBUG strips it but the getter returns NULL. Some consumers may not null-check today; audit each (esp. agx, filmic, channelmixerrgb).
3. **Synthetic key for export profile too?** Currently only display has content-key indirection. If user provides a LUT ICC file that gets replaced on disk mid-session, should export profile also use content-key? Branch did not — judgement call.
4. **Live-update propagation**: when display ICC changes, currently relies on `DT_SIGNAL_CONTROL_PROFILE_CHANGED` + full pipe rebuild. With bytes-only identity + synthetic keys, pipes built before the change keep rendering against old bytes (correct) until the signal triggers rebuild. Confirm this is desired (it is — prevents flicker during rendering).
5. **Memory growth from per-intent display entries**: each (display, intent) gets its own profile_info (~1.5 MB). Most users have 1-2 intents. Acceptable per §3.2 framing.
6. **`dt_dev_pixelpipe_rebuild` vs `dt_dev_reprocess_center`**: does the former exist on master? If not, write helper that sets `cache_obsolete + status = DIRTY` on all three pipes.

---

End of document.

