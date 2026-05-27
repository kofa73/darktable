# Verified cache-related issues at commit 4004550db2

Scope: this report re-examines every distinct claim raised in `cache_issues_verified_1.md`, `cache_issues_verified_2.md`, `cache_issues_verified_3.md`, `verified_2.md`, and `verified_3.md`. Each claim was independently re-verified against the source tree at commit `4004550db2` by three reviewers: the primary author (inline reads of the source), a Reviewer-B subagent that performed a blind pass, and Codex. Disagreements between the prior reports were resolved by direct inspection of the code; results are reported below.

The report is split into:

1. **Issues confirmed to exist on master at `4004550db2`** — each item is grounded in a file, function, and code snippet from the tree at that commit.
2. **Claimed issues that were not present at `4004550db2`** — each rejection cites the contrary evidence in the same code.

A small set of "framing" corrections to the prior reports (cases where a real bug was present but the original phrasing overshot) is noted inline under the relevant confirmed bug.

---

## Fix status (todo)

| # | Title | Status |
|---|---|---|
| 1.1  | Full-pipe cache ignores selected export profile (AGX bug) | **fixed** — pipe-level `export_profile_info` tag added to basichash |
| 1.2  | `DT_COLORSPACE_LAB` early-return leaves `pipe->output_profile_info` stale | todo |
| 1.3  | Display ICC change invalidates only FULL pipe | todo |
| 1.4  | Non-matrix/LUT output profiles collapse to sRGB cache identity | todo (separate issue, raised by user) |
| 1.5  | Display profile-info cache keyed by symbolic type, not ICC content | todo |
| 1.6  | `_ioppr_generate_profile_info()` uses CMS profile after dropping display lock | todo |
| 1.7  | Failed profile-info creation leaks six LUT buffers | deferred (memory leak, low user impact) |
| 1.8  | Display ICC update publishes raw bytes before validating with LCMS | todo |
| 1.9  | Win32 `dt_colorspaces_set_display_profile()` leaks `xprofile_lock` on two early returns | **fixed** — `goto unlock_and_return;` on both error paths, label guarded by `#if defined G_OS_WIN32` |
| 1.10 | Colord/Win32 ICC ingestion: unchecked `g_file_get_contents`, gint narrowing | todo |
| 1.11 | `dev->allprofile_info` retained for entire develop session | deferred (memory growth, low user impact) |
| 1.12 | `dt_ioppr_get_profile_info_from_list()` ignores rendering intent | todo |
| 1.13 | Profile filename tail bytes propagate into `piece->hash` | **fixed** — `dt_strlcpy_to_fixed` (memset + g_strlcpy) applied to colorin, colorout, overlay, rasterfile, watermark (filename + font); watermark v1–v5 → v6 also initialises scale_img/scale_svg; v2 → v6 now uses o->sizeto for scale_base |
| 1.14 | `colorbalancergb` / `colorequal` cache profile pointers without signal handlers | todo |

---

## 1. Issues confirmed to exist on master (up to and including `4004550db2`)

### 1.1 (high) Full-pipe cache identity ignores the selected export profile

The cache key on the FULL (darkroom) pipe does not include `colorout`'s selected export profile.

`colorout::commit_params()` resolves the pipe output to the display profile on every non-export, non-thumb, non-preview2 pipe — i.e. on the FULL pipe:

```c
// src/iop/colorout.c:610-616
else
{
  /* we are not exporting, using display profile as output */
  out_type     = darktable.color_profiles->display_type;
  out_filename = darktable.color_profiles->display_filename;
  out_intent   = darktable.color_profiles->display_intent;
}
```

The pipeline cache key then hashes only the three interned profile-info pointers and the per-piece hashes of pieces *before* the requested position:

```c
// src/develop/pixelpipe_cache.c:125-156
hash = dt_hash(hash, &pipe->input_profile_info,  sizeof(pipe->input_profile_info));
hash = dt_hash(hash, &pipe->work_profile_info,   sizeof(pipe->work_profile_info));
hash = dt_hash(hash, &pipe->output_profile_info, sizeof(pipe->output_profile_info));

GList *pieces = pipe->nodes;
for(int k = 0; k < position && pieces; k++)
{
  dt_dev_pixelpipe_iop_t *piece = pieces->data;
  ...
  hash = dt_hash(hash, &piece->hash, sizeof(piece->hash));
  ...
}
```

So a colorout export-profile change does not perturb the cache key of any module that sits before colorout in pipe order, and on FULL pipe it does not perturb `pipe->output_profile_info` either (that pointer is the display profile).

A real upstream consumer of the live export profile exists. `agx` reaches into `colorout`'s params via introspection:

```c
// src/iop/agx.c:417
dt_ioppr_get_export_profile_type(dev, &profile_type, &profile_filename);
```

```c
// src/common/iop_profile.c:1088-1138
void dt_ioppr_get_export_profile_type(struct dt_develop_t *dev,
                                      dt_colorspaces_color_profile_type_t *profile_type,
                                      const char **profile_filename)
{
  ...
  dt_colorspaces_color_profile_type_t *_type = colorout_so->get_p(colorout->params, "type");
  char *_filename = colorout_so->get_p(colorout->params, "filename");
  ...
}
```

`filmicrgb` is the second affected consumer, but via a different mechanism. It explicitly asks for the export profile by name:

```c
// src/iop/filmicrgb.c:2096
const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
const dt_iop_order_iccprofile_info_t *const export_profile = dt_ioppr_get_pipe_output_profile_info(piece->pipe);
```

The variable name and the second call at `src/iop/filmicrgb.c:2385` make the intent unambiguous: `filmicrgb` wants the user's selected export profile so its preview in the darkroom matches what the export will look like. On the FULL (darkroom) pipe, `pipe->output_profile_info` is instead the *display* profile (issue 1.1 snippet from `src/iop/colorout.c:610-616`). So:

* If the monitor is calibrated to e.g. AdobeRGB or Display-P3 while the chosen export profile is sRGB, `filmicrgb`'s darkroom render uses the wider display gamut as if it were the export gamut. The darkroom preview is therefore a *false* preview of the export: the actual export run (where `pipe->output_profile_info` correctly equals the export profile) will produce different pixels.
* Changing the export profile in `colorout` does not affect `pipe->output_profile_info` on FULL pipe (it stays the display profile), so `filmicrgb`'s `piece->hash` and the basichash inputs it sees are both unchanged, and the cached buffer (which was wrong to begin with under the assumed-export-equals-display semantics) is reused.

In other words there are two distinct correctness problems wrapped up in the same root cause:

1. **`agx`:** reads the live colorout export profile via introspection; its result depends on the actual export selection, but its `piece->hash` does not change when the user picks a different export profile, so the cache reuses a stale buffer computed under the previous export gamut.
2. **`filmicrgb`:** reads `pipe->output_profile_info` and assumes it is the export profile; on FULL pipe that is the display profile, so the darkroom preview is computed under the monitor gamut rather than the chosen export gamut. The cache then locks in that wrong-gamut result until something else invalidates it.

Both consequences flow from the same defect: on the FULL pipe, `colorout::commit_params()` ignores `p->type`/`p->filename` (the export-profile combobox) and `pipe->output_profile_info` is never set to the export profile, while at the same time the cache key (which hashes that pointer and the upstream-only `piece->hash` values) cannot react to the export-profile combobox.

### 1.2 (high) `DT_COLORSPACE_LAB` early-return in `colorout::commit_params` leaves `pipe->output_profile_info` stale

```c
// src/iop/colorout.c:618-622
// when the output type is Lab then process is a nop, so we can avoid creating a transform
// and the subsequent error messages
d->type = out_type;
if(out_type == DT_COLORSPACE_LAB)
  return;
```

```c
// src/iop/colorout.c:759 — never reached for Lab
dt_ioppr_set_pipe_output_profile_info(self->dev, piece->pipe,
                                      d->type, out_filename, p->intent);
```

`pipe->output_profile_info` therefore keeps pointing at whatever was committed on the previous `colorout::commit_params()` call. That stale pointer participates in the cache key (issue 1.1), and any upstream module that consumes `pipe->output_profile_info` reads the previous profile.

### 1.3 (high) Display ICC change invalidates only the FULL pipe

The colorout signal handler only triggers FULL-pipe reprocessing:

```c
// src/iop/colorout.c:278-283
static void _signal_profile_changed(gpointer instance, dt_iop_module_t *self)
{
  dt_develop_t *dev = self->dev;
  if(!dev->gui_attached || dev->gui_leaving) return;
  dt_dev_reprocess_center(dev);
}
```

```c
// src/develop/develop.c:2762-2776
void dt_dev_reprocess_center(dt_develop_t *dev)
{
  ...
  dev->full.pipe->changed       |= DT_DEV_PIPE_SYNCH;
  dev->full.pipe->cache_obsolete = TRUE;
  dt_dev_invalidate_all(dev);
  dt_control_queue_redraw_center();
}
```

`dt_dev_invalidate_all()` only sets `status = DT_DEV_PIXELPIPE_DIRTY` on PREVIEW and PREVIEW2 — it does not set `cache_obsolete` on them:

```c
// src/develop/develop.c:306-315
void dt_dev_invalidate_all(dt_develop_t *dev)
{
  if(dev->full.pipe)     dev->full.pipe->status     = DT_DEV_PIXELPIPE_DIRTY;
  if(dev->preview_pipe)  dev->preview_pipe->status  = DT_DEV_PIXELPIPE_DIRTY;
  if(dev->preview2.pipe) dev->preview2.pipe->status = DT_DEV_PIXELPIPE_DIRTY;
  dev->timestamp++;
}
```

`DT_SIGNAL_CONTROL_PROFILE_CHANGED` is the only signal raised for both DISPLAY and DISPLAY2 changes (`src/common/colorspaces.c:1879, 2090`); it has no payload distinguishing the two. The navigator/PREVIEW and second-window PREVIEW2 caches therefore keep serving pixels rendered with the old display profile.

### 1.4 (high) Non-matrix / LUT output profiles collapse to the same sRGB cache identity

```c
// src/common/iop_profile.c:969-983
if(profile_info == NULL
   || !dt_is_valid_colormatrix(profile_info->matrix_in[0][0])
   || !dt_is_valid_colormatrix(profile_info->matrix_out[0][0]))
{
  if(type != DT_COLORSPACE_DISPLAY)
    dt_print(DT_DEBUG_PIPE,
       "[dt_ioppr_set_pipe_output_profile_info] profile `%s' in `%s' replaced by sRGB", ...);
  profile_info = dt_ioppr_add_profile_info_to_list(dev, DT_COLORSPACE_SRGB, "", intent);
}
pipe->output_profile_info = profile_info;
```

Two different LUT-based display or output profiles both end up at the single interned sRGB `profile_info` pointer. Because the cache key hashes that pointer (issue 1.1), the two profiles collide in cache identity. Switching between two non-matrix display profiles reuses the cached buffer rendered under the first.

`colorout` itself still produces visibly different pixels for those profiles by going through LCMS:

```c
// src/iop/colorout.c:702-710
if(d->mode != DT_PROFILE_NORMAL || force_lcms2
   || dt_colorspaces_get_matrix_from_output_profile(output, d->cmatrix, ...))
{
  dt_mark_colormatrix_invalid(&d->cmatrix[0][0]);
  piece->process_cl_ready = FALSE;
  d->xform = cmsCreateProofingTransform(...);
}
```

### 1.5 (high) Display profile-info cache is keyed by symbolic type, not by ICC content

`dev->allprofile_info` lookup matches `(type, filename)` only:

```c
// src/common/iop_profile.c:790-798
for(GList *profiles = dev->allprofile_info; profiles; profiles = g_list_next(profiles))
{
  dt_iop_order_iccprofile_info_t *prof = profiles->data;
  if(prof->type == profile_type && strcmp(prof->filename, profile_filename) == 0)
  {
    profile_info = prof;
    break;
  }
}
```

System display profiles always come in under the stable symbolic types `DT_COLORSPACE_DISPLAY` / `DT_COLORSPACE_DISPLAY2` and an empty filename. When `_update_display_profile()` replaces the LCMS profile under the same slot:

```c
// src/common/colorspaces.c:1257-1273
for(GList *iter = darktable.color_profiles->profiles; iter; iter = g_list_next(iter))
{
  dt_colorspaces_color_profile_t *p = iter->data;
  if(p->type == DT_COLORSPACE_DISPLAY)
  {
    if(p->profile) dt_colorspaces_cleanup_profile(p->profile);
    p->profile = profile;
    ...
  }
}
```

the lookup keeps returning the previously built `iccprofile_info` with its original (now stale) matrices and LUTs.

### 1.6 (high) `_ioppr_generate_profile_info()` uses the CMS profile after dropping the display-profile lock

The function holds `xprofile_lock` only long enough to copy the `cmsHPROFILE` pointer, then releases it before any matrix or LUT extraction:

```c
// src/common/iop_profile.c:694-710
if(type == DT_COLORSPACE_DISPLAY || type == DT_COLORSPACE_DISPLAY2)
  pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);

const dt_colorspaces_color_profile_t *profile =
  dt_colorspaces_get_profile(type, filename, DT_PROFILE_DIRECTION_ANY);

cmsHPROFILE *rgb_profile = profile ? profile->profile : NULL;

if(type == DT_COLORSPACE_DISPLAY || type == DT_COLORSPACE_DISPLAY2)
  pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

...
if(dt_colorspaces_get_matrix_from_input_profile(rgb_profile, profile_info->matrix_in, ...))
```

Meanwhile a writer holding the write lock can replace and free the same object:

```c
// src/common/colorspaces.c:1262-1263
if(p->profile) dt_colorspaces_cleanup_profile(p->profile);
p->profile = profile;
```

A display-profile update that lands between the rdlock release and the matrix/LUT extraction either produces profile-info populated from stale data, or accesses freed LCMS state. (The original "race in `_ioppr_profile_info_cache_filename`" claim from one reviewer is a misnaming — see section 2.4 — but this race in `_ioppr_generate_profile_info()` itself is real.)

### 1.7 (high) Failed profile-info creation leaks the six LUT buffers

`dt_ioppr_init_profile_info()` always allocates six float LUTs of `DT_IOPPR_LUT_SAMPLES = 0x10000` entries (≈ 1.5 MB total):

```c
// src/common/iop_profile.c:646-653
profile_info->lutsize = (lutsize > 0) ? lutsize: DT_IOPPR_LUT_SAMPLES;
for(int i = 0; i < 3; i++)
{
  profile_info->lut_in[i]  = dt_alloc_align_float(profile_info->lutsize);
  ...
  profile_info->lut_out[i] = dt_alloc_align_float(profile_info->lutsize);
  ...
}
```

The failure branch of `dt_ioppr_add_profile_info_to_list()` frees only the wrapper:

```c
// src/common/iop_profile.c:813-823
profile_info = dt_alloc1_align_type(dt_iop_order_iccprofile_info_t);
dt_ioppr_init_profile_info(profile_info, 0);
if(!_ioppr_generate_profile_info(profile_info, profile_type, profile_filename, intent))
{
  dev->allprofile_info = g_list_append(dev->allprofile_info, profile_info);
}
else
{
  dt_free_align(profile_info);   // LUTs LEAK — no dt_ioppr_cleanup_profile_info() call
  profile_info = NULL;
}
```

`dt_ioppr_cleanup_profile_info()` is the function that frees the LUTs (`src/common/iop_profile.c:658-665`); it is not called here. Each unresolvable `(type, filename)` request leaks ~1.5 MB.

### 1.8 (high) Display ICC update publishes raw bytes before validating with LCMS

```c
// src/common/colorspaces.c:1245-1273 (and the parallel _update_display2_profile at 1276-1304)
static void _update_display_profile(guchar *tmp_data, const gsize size, ...)
{
  g_free(darktable.color_profiles->xprofile_data);
  darktable.color_profiles->xprofile_data = tmp_data;            // published
  darktable.color_profiles->xprofile_size = size;                // before
                                                                 // validation
  cmsHPROFILE profile = cmsOpenProfileFromMem(tmp_data, size);
  if(profile)
  {
    for(GList *iter = darktable.color_profiles->profiles; ...)
      if(p->type == DT_COLORSPACE_DISPLAY) { ... p->profile = profile; ... }
  }
  // on LCMS failure: globals already mutated; profile list untouched
}
```

The caller raises `DT_SIGNAL_CONTROL_PROFILE_CHANGED` purely from a byte-level comparison, regardless of whether LCMS accepted the new ICC:

```c
// src/common/colorspaces.c:2074, 2090 (Win32 path; the colord path at :1879 is analogous)
if(profile_changed)
{
  ...
  _update_display_profile(buffer, buffer_size, name, sizeof(name));
}
...
if(profile_changed) DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_CONTROL_PROFILE_CHANGED);
```

Malformed bytes therefore replace `xprofile_data`/`xprofile_size` while the parsed `cmsHPROFILE` stays old, and downstream consumers are notified of a "profile change" that never produced a valid profile.

### 1.9 (medium) Win32 `dt_colorspaces_set_display_profile()` returns early while holding `xprofile_lock`

The function acquires the write lock and then has two early-return paths that skip the matching unlock:

```c
// src/common/colorspaces.c:1911
if(pthread_rwlock_trywrlock(&darktable.color_profiles->xprofile_lock))
  return;

...

// src/common/colorspaces.c:2023-2027
HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
if(!hMonitor)
{
  dt_print(DT_DEBUG_ALWAYS, "[win32 ...] error getting monitor handle");
  return;            // lock leaked
}

// src/common/colorspaces.c:2030-2034
if(!GetMonitorInfoW(hMonitor, (LPMONITORINFO) &monitorInfo))
{
  dt_print(DT_DEBUG_ALWAYS, "[win32 ...] error getting monitor info");
  return;            // lock leaked
}
```

After either failure no reader or writer can re-acquire `xprofile_lock`. Subsequent display lookups and display-profile updates either block (readers) or silently no-op (the next `trywrlock` returns `EBUSY` at line 1911).

### 1.10 (medium / low) Colord and Win32 ICC ingestion: unchecked `g_file_get_contents()` and `gint` narrowing of `gsize`

`gint buffer_size = 0;` and the unchecked file read in the Win32 path:

```c
// src/common/colorspaces.c:1916-1917
guint8 *buffer = NULL;
gint  buffer_size = 0;
```

```c
// src/common/colorspaces.c:2047-2049
gsize size;
g_file_get_contents(path, (gchar **)&buffer, &size, NULL);
buffer_size = size;             // gsize → gint, no check
```

The colord callback ignores the return value too, after already mutating `colord_profile_file`:

```c
// src/common/colorspaces.c:1837-1843
g_free(darktable.color_profiles->colord_profile_file);
darktable.color_profiles->colord_profile_file = g_strdup(filename);
...
guchar *tmp_data = NULL;
gsize size;
g_file_get_contents(filename, (gchar **)&tmp_data, &size, NULL);
```

The `size > 0` guard later on prevents most immediate misuse, but the error context is lost and the Win32 `gsize → gint` narrowing is real: anything that would set the high bit of `size` (or, in the academic 2 GiB+ case, overflow it) feeds an incorrect size into `xprofile_size` and subsequent `memcmp`/publication. On colord the cached filename has already been advanced past the unreadable profile, which can mask a real subsequent change.

### 1.11 (medium) `dev->allprofile_info` is retained for the entire develop session

```c
// src/common/iop_profile.c:815-817
if(!_ioppr_generate_profile_info(profile_info, profile_type, profile_filename, intent))
{
  dev->allprofile_info = g_list_append(dev->allprofile_info, profile_info);  // only append
}
```

The list is only torn down at develop cleanup:

```c
// src/develop/develop.c:236
while(dev->allprofile_info)
{
  dt_ioppr_cleanup_profile_info((dt_iop_order_iccprofile_info_t *)dev->allprofile_info->data);
  dt_free_align(dev->allprofile_info->data);
  dev->allprofile_info = g_list_delete_link(dev->allprofile_info, dev->allprofile_info);
}
```

**Framing correction.** The original claim phrased this as "grows without bound across display ICC changes." That specific phrasing is wrong: because display profiles are keyed by symbolic type (issue 1.5), repeated display ICC publications reuse the existing entry rather than appending. The correct framing is **session-retention growth**: every distinct `(type, filename)` ever requested (e.g. browsing folders with many different `DT_COLORSPACE_FILE` profiles) adds a permanent ~1.5 MB entry, and the list is only freed at `dt_dev_cleanup()`.

### 1.12 (medium) `dt_ioppr_get_profile_info_from_list()` ignores rendering intent

`profile_info->intent` is stored on creation:

```c
// src/common/iop_profile.c:690-692
profile_info->type = type;
g_strlcpy(profile_info->filename, filename, sizeof(profile_info->filename));
profile_info->intent = intent;
```

but the lookup key omits it:

```c
// src/common/iop_profile.c:793
if(prof->type == profile_type && strcmp(prof->filename, profile_filename) == 0)
```

The stored intent is consumed later when building LCMS transforms (`src/common/iop_profile.c:293, 321`). The first caller wins; subsequent callers asking for the same profile with a different intent silently receive the original one. Harmless for matrix profiles, materially incorrect for non-matrix/LCMS-fallback paths.

### 1.13 (low) Profile filename tail bytes in `module->params` propagate into `piece->hash`

The full module params buffer is hashed:

```c
// src/develop/imageop.c:2218-2222
phash = dt_hash(DT_INITHASH, &module->so->op, strlen(module->so->op));
phash = dt_hash(phash, &module->instance, sizeof(int32_t));
phash = dt_hash(phash, module->params, module->params_size);   // entire buffer
```

`colorout` updates its fixed-size `filename` field with `g_strlcpy`, which only NUL-terminates and leaves any bytes past the terminator intact:

```c
// src/iop/colorout.c:264-265
p->type = pp->type;
g_strlcpy(p->filename, pp->filename, sizeof(p->filename));
```

**Framing correction.** Several original reports framed this as "aliasing across distinct profiles." That is overstated: two distinct filenames always differ in at least one byte at or before the terminator. The real defect is **hash instability for identical selections**: selecting the same short profile name after a longer profile name has previously been selected produces different `piece->hash` values (and therefore different basichash values for every downstream module), so cache lines that were valid moments earlier are missed. This is a false-miss / efficiency bug, not a false-hit / correctness bug.

### 1.14 (low) `colorbalancergb` and `colorequal` cache profile pointers without registering profile-change handlers

`colorbalancergb`:

```c
// src/iop/colorbalancergb.c:1664, 1688
const gboolean output_profile_changed = output_profile != g->sliders_output_profile;
...
g->sliders_output_profile = output_profile;
```

`colorequal`:

```c
// src/iop/colorequal.c:2797-2807
struct dt_iop_order_iccprofile_info_t *work_profile =
  dt_ioppr_get_pipe_output_profile_info(self->dev->full.pipe);
if(work_profile != g->work_profile)
{
  dt_free_align(g->white_adapted_profile);
  g->white_adapted_profile = D65_adapt_iccprofile(work_profile);
  g->work_profile = work_profile;
  g->gradients_cached = FALSE;
}
```

Neither file registers any of `DT_SIGNAL_CONTROL_PROFILE_CHANGED`, `DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED`, or `DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED` (a `grep` over each file returns no matches; `primaries.c:407-409` is the only IOP module that does). Slider/gradient colors in these two modules can therefore stay based on the previous profile until some other UI interaction calls `gui_changed()`.

---

## 2. Claimed issues that were not present at `4004550db2`

### 2.1 "Thumbnail pipe cache key mixed from display globals while the thumbnail's output is resolved through the mipmap colorspace" (A2)

At master, the thumbnail branch of `colorout::commit_params()` reads the thumbnail's output type from `dt_mipmap_cache_get_colorspace()` and then calls `dt_ioppr_set_pipe_output_profile_info()` (at line 759) with that type:

```c
// src/iop/colorout.c:597-602
else if(dt_pipe_is_thumb(pipe))
{
  out_type     = dt_mipmap_cache_get_colorspace();
  out_filename = (out_type == DT_COLORSPACE_DISPLAY ? darktable.color_profiles->display_filename : "");
  out_intent   = darktable.color_profiles->display_intent;
}
```

The cache key hashes `pipe->output_profile_info` (`src/develop/pixelpipe_cache.c:127`), which on the thumbnail pipe is the profile-info that came from the mipmap colorspace lookup, not from `darktable.color_profiles->display_*` globals. Also, `pipe->output_type` / `pipe->output_filename` fields that the claim references do not exist at this commit. The claim as written does not reproduce.

### 2.2 "Repeated ICC content hashing CPU overhead — entire ICC buffer rehashed dozens of times per rebuild" (B Low #8)

`_dev_pixelpipe_cache_basichash()` hashes the **pointer values** of the three `pipe->*_profile_info` fields (8 bytes each), not the ICC payload they reference (`src/develop/pixelpipe_cache.c:125-127`). There is no ICC-content hashing on the master path. The cost described would only appear under a later content-hashing fix variant.

### 2.3 "Module reads the pipe's export profile before `colorout` has populated it" (B Low #9)

`dt_dev_pixelpipe_synch_all()` (defined at `src/develop/pixelpipe_hb.c:603`) commits all pipe nodes' params before any pipe execution begins. The two modules that actually read the live export profile (`agx` via `dt_ioppr_get_export_profile_type()`; `filmicrgb` via `dt_ioppr_get_pipe_output_profile_info()`) both observe a fully populated state when their `process()` runs. The claim is not demonstrated at this commit.

### 2.4 "Race in `_ioppr_profile_info_cache_filename()`" (A6)

No function by that name exists in the source tree at `4004550db2` (`grep -rn _ioppr_profile_info_cache_filename src/` returns nothing). The function and its split RD-lock pattern were introduced in the post-master fix attempts. The genuine display-profile race at master is the one in `_ioppr_generate_profile_info()` documented in issue 1.6.

### 2.5 "`primaries.c` GUI has a standalone pointer-equality invalidation bug" (C8)

`primaries.c` does register the relevant signal handlers and forces a full repaint on every signal:

```c
// src/iop/primaries.c:407-409
DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, _signal_profile_user_changed);
DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_CONTROL_PROFILE_CHANGED,      _signal_profile_changed);
DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,     _signal_profile_changed);

// src/iop/primaries.c:340-351
static void _signal_profile_user_changed(gpointer instance, const uint8_t profile_type, dt_iop_module_t *self)
{ gui_changed(self, NULL, NULL); }
static void _signal_profile_changed(gpointer instance, dt_iop_module_t *self)
{ gui_changed(self, NULL, NULL); }

// src/iop/primaries.c:303-306
const gboolean repaint_all_sliders =
   !w
   || work_profile    != g->painted_work_profile
   || display_profile != g->painted_display_profile;
```

The callbacks pass `w = NULL`, so `!w` is true and `repaint_all_sliders` is forced regardless of the pointer-equality test. Any stale render in `primaries.c` after a display change is a downstream consequence of the symbolic-type display-cache bug (issue 1.5), not a separate invalidation bug in `primaries.c`. (The analogous handlers are missing in `colorbalancergb` and `colorequal` — that real defect is captured under issue 1.14.)

### 2.6 "Pointer-based cache key causes collisions across profile-list reloads" (A9 / B Low #7, allocator-reuse leg)

The cache key really does hash the pointer values (`src/develop/pixelpipe_cache.c:125-127`), but the danger described — the allocator handing back a recycled address that previously held a different profile-info — does not arise during normal develop-session lifetime. `dev->allprofile_info` entries are only ever appended and only freed at `dt_dev_cleanup()` (`src/develop/develop.c:236`). For the pointer-recycling collision to bite, the entire profile-info list would have to be released and rebuilt within the same session, and at this commit there is no code path that does so. The concrete pointer-identity failures that *do* bite on master are covered by issues 1.4 (sRGB collapse) and 1.5 (symbolic-type display reuse). The pure pointer-recycling claim is not confirmed at `4004550db2`.

---

## Appendix: review process and divergences

Three reviewers were used:

* **Primary author (inline).** Read the five claim files and the cited source files end-to-end.
* **Reviewer-B subagent.** Blind pass with no access to the other reviewers' findings; instructed to confirm or reject each merged claim against the source at `4004550db2`.
* **Codex.** Asked the same verification question via the Codex rescue path. Codex's sandbox prevented it from reading the local `.md` files but it independently re-read the cited source files and produced a per-claim verdict.

All three converged on the 14 confirmations and 6 rejections above, with two principled disagreements that were resolved by direct re-reading of the source:

1. **Codex initially flagged the pointer-based cache key as REJECTED** on the grounds that the basichash "uses image/type/detail + `piece->hash`, not profile-info pointers." Direct re-inspection of `src/develop/pixelpipe_cache.c:125-127` confirms the pointers *are* hashed. The substantive disagreement therefore narrows to whether pointer-recycling collisions are reachable, where Codex and Reviewer-B agreed: not within a normal session at this commit (issue 2.6).
2. **The `filmicrgb` framing in the earlier reports.** All three reviewers initially observed that on the FULL pipe `filmicrgb` reads what `dt_ioppr_get_pipe_output_profile_info()` returns, which is the *display* profile, not the export profile. The first draft used that observation to scope bug 1.1 to `agx` only. A subsequent reading of `filmicrgb` (variable name `export_profile`, identical second call at `src/iop/filmicrgb.c:2385`) made the intent unambiguous: the module wants the export profile so the darkroom preview matches the export. On FULL pipe it instead gets the display profile, which is a separate, in some ways more serious, manifestation of the same FULL-pipe-ignores-export-profile defect. Bug 1.1 was revised to enumerate both consequences (cache staleness in `agx`; false preview in `filmicrgb`).

The "framing corrections" inline under issues 1.1, 1.11, and 1.13 capture cases where a real bug was present at master but the original wording in one or more of the source reports overshot what the code actually does.
