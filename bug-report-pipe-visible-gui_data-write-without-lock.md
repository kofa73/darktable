# Several IOP modules share `gui_data` with pixelpipe threads without any locking

## Summary

A number of image operation modules let a widget callback write a field of their
`gui_data` struct, and then read that same field from `process()` or `process_cl()`.
Widget callbacks run on the GTK main thread; `process()` runs on a pixelpipe worker
thread. The access is not synchronised in any way, so this is a data race.

The affected modules never call `dt_iop_gui_enter_critical_section()` at all — the
mutex that exists for exactly this purpose is simply unused there.

A second group *does* use the mutex, but not for the field at issue — often missing it
in `commit_params()`, which also runs on a pipe thread.

A third group takes the lock but puts it in the wrong place, releasing it before the
value it protects has been used. Three of those are memory-safety bugs rather than
display glitches: `colorreconstruction.c` dereferences a freed bilateral grid,
`zonesystem.c` paints a Cairo surface larger than its backing allocation, and
`colormapping.c` can dereference NULL.

## Filing note

This is a bulk filing. The findings below come from one audit and share a root cause, so
they are recorded together rather than lost; they are **not** one unit of work and can be
split freely. Every module is independent of every other, and the four sections are
natural split boundaries.

### Relationship to #21891

#21891 is a different defect in different code: it is a *lifetime* bug in the framework
(`gui_data` freed and `gui_lock` destroyed while a pipe runs, at
`src/develop/imageop.c:572` and `src/libs/history.c:426`). This report is a
*synchronisation* bug in module code (`src/iop/*.c`). Neither fix touches the other's
files, and neither closes the other.

They interact in one direction. The remedy recommended here is "take `gui_lock` on both
sides"; #21891 says that lock may not exist. So:

| group | depends on #21891 | note |
| --- | --- | --- |
| *No critical section at all* (10 modules) | soft | fix is to start taking `gui_lock` |
| *Uses the mutex, not for this field* (6 modules) | soft | same |
| *Wrong place* — `zonesystem.c`, `colormapping.c` | none | snapshot / null-test inside a section that already exists |
| *Wrong place* — `ashift.c`, `rgblevels.c` | none | coherent scalar snapshots on the GTK side |
| *Wrong place* — `colorreconstruction.c` | partial | the live-GUI use-after-free is fixable now; the unlocked `gui_cleanup()` free at line 1258 belongs to #21891 |
| `overlay.c` | none | different fix entirely — marshal the GTK calls onto the GTK thread |

"Soft" means: fixing those groups first is safe and beneficial, but on the #21891
teardown path it adds a lock of a destroyed mutex to modules that currently never touch
`gui_lock`. That path already commits a use-after-free write, so this makes an
already-broken path more broken rather than breaking a working one. It is a reason to
land #21891 first or alongside, not a blocker.

### Suggested order, most critical first

1. **#21891** — ordinary trigger (delete instance, undo/redo), use-after-free *write*,
   silent heap corruption. Not in this report; listed for context.
2. **`overlay.c`** — not a race: GTK calls from a pipe worker every time the overlay
   cache misses, plus a leaked tooltip string.
3. **`colorreconstruction.c`** — use-after-free read of a freed bilateral grid.
4. **`zonesystem.c`** — out-of-bounds read while painting.
5. **`colormapping.c`** — null dereference; needs an allocation failure as well as the race.
6. **`ashift.c`, `rgblevels.c`** — incoherent geometry and handshake state.
7. **Everything else** — wrong pixels on screen for a frame; the bulk of the report by
   module count, and the least urgent.

Items 2-5 are the memory-safety ones and are independent of each other.

## Why it matters

Most cases found are display-mask toggles: a button that makes the module show its
internal mask instead of the image. The rest hand a computed number back from the pipe
to a GUI label. The practical consequences are mild but real:

- **Stale value in an already-running pipe.** Toggling the button while a recompute is
  in flight gives that pipe no ordering against the write, so the frame it is producing
  can show the wrong thing. The run submitted *after* the write is ordered against it
  by the job queue — see below — so this is the in-flight case, not every frame.
- **Mid-frame inconsistency.** Several `process()` implementations read the field more
  than once, so a change landing mid-call can make one half of the frame disagree with
  the other. `colorbalancergb.c` re-reads `g->mask_type` *once per pixel*
  (`opacities[g->mask_type]`, line 929), which is the sharpest instance. Without a lock
  or an atomic the compiler is entitled to re-load the field like that, and to split or
  duplicate accesses generally.
- **Lost or mismatched values on the reverse-direction cases.** `hotpixels.c` and
  `denoiseprofile.c` hand a number from the pipe to a label; `atrous.c` hands over a
  count and the array it bounds, which can be read as a mismatched pair.

For the mask-toggle and readout fields, nothing corrupts memory in the concrete sense:
none of those fields is a pointer, and every indexed read stays in bounds even with a
racing index (`opacities` is a `float[4]` and `MASK_NONE == 3`; `atrous`'s
`num_samples` cannot exceed the `MAX_NUM_SCALES` size of `g->sample`). Nor is hardware
tearing the concern — they are naturally aligned `int`, `gboolean`, enum and `float` on
every platform darktable supports (`src/is_supported_platform.h`). They are still
formally data races, so the standard promises nothing; the bounds arguments describe
what the generated code does today, not what the language owes you.

**That mildness does not extend to the whole report.** Auditing the modules that
already use the mutex turned up three cases where the critical section is in the wrong
place and the consequence is memory-unsafe:

- `colorreconstruction.c` — **use-after-free.** The full pipe copies `g->can` out under
  the lock and dereferences it *after* releasing (lines 633-641), while the preview pipe
  frees the old grid at line 659 and `gui_update()` frees it on the GTK thread at 1183.
- `zonesystem.c` — **out-of-bounds read.** The draw callback allocates its image from
  `preview_width`/`preview_height` read under the lock (line 730), unlocks at 739, then
  re-reads both at 741 and hands *those* to Cairo (lines 743-744). If a preview run has
  grown the buffer in between, Cairo paints an allocation smaller than the surface it
  was told about (line 753).
- `colormapping.c` — **null dereference.** The preview-finished handler null-tests
  `g->buffer` at line 902, *before* taking the lock at 907, and the copy at 918 does not
  re-test. The pipe's replacement allocation at line 456 can fail and leave the field
  NULL.

`ashift.c` is a near miss worth stating precisely, because it is easy to over-report:
its GTK-side readers only truth-test `g->buf` and read the geometry scalars, never
dereferencing the buffer, and the one place that copies pixels out of it does hold the
lock (lines 1683-1698). So it is a coherence bug — mismatched geometry, a pointer test
racing a free — not a memory-safety one.

What is at stake, then, is a spectrum: a wrong first frame at the mild end, a
use-after-free at the severe end, and throughout it the fact that the pattern is being
copied into new modules.

### `dt_dev_reprocess_*()` does not synchronise the pipe that is already running

The mask-toggle callbacks all write the field and then call
`dt_dev_reprocess_center()`. `dt_dev_reprocess_center()` only sets
`pipe->changed |= DT_DEV_PIPE_SYNCH`, invalidates buffers and queues a redraw
(`src/develop/develop.c:2831`); it neither waits for a running pipe nor issues a
barrier. A pipe already inside `process()` is therefore unaffected by the write, and
that is the window this bug lives in.

The *next* pipe run is a different matter, and the report should not overstate this.
The queued redraw is dispatched on the GTK thread, which calls `dt_dev_process_image()`
(and the preview equivalents), and those submit a reserved job with
`dt_control_add_job_res()` (`src/develop/develop.c:283-307`). Publication of that job
takes and releases `control->res_mutex` (`src/control/jobs.c:363-378`) and the reserved
worker acquires the same mutex before executing it (`src/control/jobs.c:222-229`). That
release/acquire pair does order the earlier GTK-thread write ahead of the newly started
`process()`. `dt_dev_add_history_item()` gives a similar edge through
`dev->history_mutex` (`src/develop/develop.c:1428-1453`, paired against
`src/develop/pixelpipe_hb.c:826-863`).

Two reasons that is still not a licence to skip the lock. Whether such a route exists
at all is internal framework detail rather than a module-facing contract — the
synchronisation effect of the mutex pair itself is real, but nothing obliges the
framework to keep routing pipe submission that way. And the edge only covers the run
submitted afterwards: it does nothing for the pipe already in flight, and it does not
stop the compiler re-reading the field mid-`process()`. Once that in-flight race has
happened the program has formally entered undefined behaviour, so "the next run sees
the write" describes the intended ordering, not a guarantee the standard still owes
you.

## Example 1 — `colorequal.c`

Seven GTK-thread sites write `g->mask_mode`: two quad-button callbacks, the notebook
tab-switch callback, `gui_changed()`, `gui_update()`, `gui_focus()` and
`reload_defaults()` (lines 2052, 2245, 2569, 2578, 2607, 2837, 2881). A fix has to
cover all of them, not just the obvious button callbacks.

```c
// quad-button callback, GTK main thread
g->mask_mode = (dt_bauhaus_widget_get_quad_active(quad)) ? g->channel + 1 : 0;
```

Both `process()` and `process_cl()` read it on a worker thread:

```c
const int mask_mode = g && fullpipe ? g->mask_mode : 0;
```

No lock on either side. `colorequal` does already snapshot into a local, so it has the
stale-value problem but not the mid-frame one.

## Example 2 — `filmicrgb.c`

A toggle-button callback writes `g->show_mask`:

```c
g->show_mask = !(g->show_mask);
```

`process()` and `process_cl()` both read it directly (lines 2124, 2457), without
snapshotting. No lock on either side.

## Modules with no critical section at all

Found by scanning `src/iop/*.c` and `src/iop/*.cc` for modules that reference `g->`
inside `process()`/`process_cl()` — including the static helpers those call — and never
call `dt_iop_gui_enter_critical_section()`.

| module | field | direction |
| --- | --- | --- |
| `colorequal.c` | `mask_mode` | callback writes, pipe reads |
| `filmicrgb.c` | `show_mask` | callback writes, pipe reads |
| `colorbalancergb.c` | `mask_display`, `mask_type` | callback writes, pipe reads |
| `colorzones.c` | `display_mask` | callback writes, pipe *and* `commit_params()` read |
| `contrastntexture.c` | `details_display` | callback writes, pipe reads |
| `demosaic.c` | `dual_mask`, `cs_mask`, `cs_boost_mask` | callback writes, pipe reads |
| `highlights.c` | `hlr_mask_mode` | callback writes, pipe *and* `commit_params()` read |
| `hotpixels.c` | `pixels_fixed` | pipe writes, GUI reads *and* clears |
| `denoiseprofile.c` | `variance_R`, `variance_G`, `variance_B` | pipe writes, GUI reads |
| `atrous.c` | `num_samples`, `sample` | pipe writes, GUI reads |

The last three run the other way round: the pipe writes a result that a GUI callback
later displays. That is the direction `dev-doc/GUI.md` already documents as needing a
critical section, so those are clearer violations of an existing written rule.

Notes on individual entries:

- **`hotpixels.c`** is not a one-way handoff. The `draw` callback reads
  `g->pixels_fixed`, formats it into the label, then resets it to `-1` (lines 422-425).
  A pipe-thread write landing between the read and the reset is lost, so a count can
  silently fail to appear.
- **`denoiseprofile.c`** writes the three variance floats from `process_variance()`
  (line 1946), a static helper called from `process()`; the draw callback
  `denoiseprofile_draw_variance()` formats them with `g_strdup_printf("%.2f", ...)`
  (lines 3185-3203). The module has no critical section anywhere, and even carries a
  comment in `commit_params()` saying "we are not allowed to access gui_data here".
- **`atrous.c`** is the one worth looking at first among the reverse cases.
  `get_samples()` fills `g->sample[]` and its return value is then assigned to
  `g->num_samples` (lines 295, 382, 501), while `area_draw()` uses `g->num_samples` to
  bound a loop over `g->sample[]` (lines 1167-1182). There is no out-of-bounds risk —
  the count is clamped by the array size — but the drawing code can pair a fresh count
  with stale samples, or vice versa, and draw the wrong scale markers.

## Modules that use the mutex, but not for the field in question

These do call `dt_iop_gui_enter_critical_section()` somewhere, so a "does this module
lock at all?" scan passes over them. The field at issue is still unprotected.

| module | field | site |
| --- | --- | --- |
| `lens.cc` | `vig_masking` | `_visualize_callback()` writes it (line 4445), `gui_focus()` reads then clears it (lines 4699, 4701), `process()`/`process_cl()` read it (lines 3056, 3135); none of the six critical sections in the file covers it |
| `exposure.c` | `effective_exposure` | `commit_params()` writes it unlocked (line 641); the proxy accessor reads it unlocked (lines 824-828) |
| `channelmixerrgb.c` | `run_profile` | `commit_params()` reads it unlocked (line 3131); `process()` also *tests* it before entering the section (line 2159), so only the clear at 2164 is protected |
| `channelmixerrgb.c` | `run_validation` | `commit_params()` reads it unlocked (line 3132); the whole validation path in `process()` — the test, `_validate_color_checker()`, and the clear — is unlocked (lines 2284-2287) |
| `channelmixerrgb.c` | `is_blending` | `commit_params()` writes it (line 3169); read from *both* threads (line 1186, see below) and never locked |
| `channelmixerrgb.c` | `safety_margin` | see below — the write is locked, one of the two pipe-side reads is not |
| `retouch.c` | `display_wavelet_scale`, `mask_display`, `suppress_mask` | GTK gesture callbacks write them (lines 1682, 2060, 2082) and `change_image()` resets them (lines 2403-2405); `process()` reads them (lines 3916, 3934, 3951, 3958, 4019) and `process_cl()` reads them (lines 4735, 4757, 4777, 4854) |
| `retouch.c` | `first_scale_visible` | reverse direction: `process()`/`process_cl()` write it (lines 3975, 4806), the `rt_wdbar_draw()` GTK callback reads it (line 1396) |

`lens.cc` is a plain instance of the mask-toggle shape, identical to `colorequal` and
`filmicrgb`. It is listed separately only because the module does use the mutex
elsewhere.

`retouch.c` is the clearest illustration of why "does this module lock?" is the wrong
question. It has ten critical sections (lines 1196, 1689, 1715, 1727, 1759, 2131, 3988,
4002, 4820, 4836). They guard the `preview_auto_levels` handshake and
`displayed_wavelet_scale`; the one at 2131 just wraps `rt_shape_selection_changed()`.
The four display fields above sit right beside all of that, unprotected — note that
`displayed_wavelet_scale` is locked while the near-identically named
`display_wavelet_scale` is not — and `first_scale_visible` even runs in the opposite
direction.

`exposure.c` deserves attention out of proportion to its size, for two reasons. It is
the in-tree reference for the reverse direction — `dev-doc/GUI.md` points at it — so a
gap there gets copied. And the reader is in *another module*:
`_exposure_proxy_get_effective_exposure()` is published through
`dev->proxy.exposure.get_effective_exposure`, and `agx.c` calls it from
`_adjust_relative_exposure_from_exposure_params()` on the GTK thread
(`src/iop/agx.c:1049`, reached from a button callback at 1158-1167 and 2078-2087). A
cross-module reader cannot reasonably be expected to take `exposure`'s `gui_lock`
itself, so the lock has to go inside the proxy accessor and inside `commit_params()`.

`channelmixerrgb.c` is the awkward one, because it looks locked and mostly is:

- `is_blending` is read in `_declare_cat_on_pipe()` (line 1186), which is called from
  `process()` (line 2132) and `process_cl()` (line 2306) as well as from
  `reload_defaults()` and `gui_changed()` on the GTK thread — so both threads read it,
  and `commit_params()` writes it. `_set_trouble_messages()` (line 2027) adds a second
  GTK-thread read, from the preview-pipe-finished signal handler.
- `safety_margin` is written under the lock in `_safety_changed_callback()` (line 2876)
  and read on the pipe side in `_extract_patches()` (line 1437). That read is covered
  on the profiling path, because `process()` holds the section across
  `_extract_color_checker()` (the section opens at line 2161, the call is at 2162). It
  is **not** covered on the validation path:
  `_validate_color_checker()` is called with no section held (line 2286) and reaches
  the same `_extract_patches()` (line 1948).

So a fix for `channelmixerrgb` cannot be confined to `commit_params()`. It has to cover
the unlocked `run_profile` test, the whole validation path, and `is_blending`.

## Modules whose critical section is in the wrong place

The three groups above are about a missing lock. This group is different and, in two
cases, worse: the lock is taken, but not around everything it needs to cover. These
were found by auditing field by field the modules that a "does this module lock?" scan
clears.

| module | field | what is wrong |
| --- | --- | --- |
| `colorreconstruction.c` | `can` | the pointer is loaded under the lock (lines 633-635, 1034-1036) and dereferenced after releasing it (lines 641, 1042), while the preview pipe (lines 659-660, 1062-1063) and `gui_update()` (lines 1182-1185) free the pointee — **use-after-free** |
| `ashift.c` | `buf_width`, `buf_height`, `buf_x_off`, `buf_y_off`, and the `buf` pointer test | the pipe replaces the buffer and its geometry under the lock (lines 3480-3509, 3619-3646); GTK-thread readers take no lock: `do_crop()` (lines 2650, 2682-2683), `crop_adjust()` (lines 2831-2832, reached from `mouse_moved()`), `gui_changed()` (lines 5280-5282) and `gui_post_expose()` (lines 4128-4135). None dereferences the buffer, so this is incoherent geometry rather than a bad access |
| `ashift.c` | `isflipped` | written under the lock by the pipe (lines 3481, 3620); `_event_draw()` reads it under the lock (lines 5805-5807), but three other GTK-side readers do not (lines 2299, 3854, 3900) |
| `zonesystem.c` | `preview_width`, `preview_height` | the draw callback allocates its image under the lock (line 730), releases at 739, then re-reads both fields at 741 to describe the Cairo surface over that same allocation (lines 743-744) — a preview run landing in between pairs the old buffer size with the new dimensions, and Cairo reads past the end at line 753 |
| `colormapping.c` | `buffer` | the preview-finished handler null-tests `g->buffer` at line 902, before taking the lock at 907, and the copy at 918 does not re-test; the pipe frees and replaces the pointer under the lock (lines 454-456, 600-602) and leaves it NULL if the allocation fails |
| `rgblevels.c` | `box_cood`, `call_auto_levels` | the pipe locks the handshake test (lines 1260-1265, 1413-1418), but `button_released()` writes both fields with no lock at all (lines 272-283), and `_get_selected_area()` reads `box_cood` (lines 1112-1113) after the pipe has released |
| `rgblevels.c` | `channel` | GTK writes it (lines 740, 755), the pipe's auto-levels path reads it (lines 1272, 1444); neither side locks |

`colorreconstruction.c`, `zonesystem.c` and `colormapping.c` should be fixed first —
they are the memory-safety bugs in this report.

Only `colorreconstruction.c` needs design thought. Widening the section to cover the
thaw would close the race against the paths that matter here, since preview publication
(lines 658-662, 1061-1065) and `gui_update()` (lines 1182-1186) all take the same lock.
The argument against it is latency rather than correctness: `_bilateral_thaw()`
allocates and copies the whole grid, and the OpenCL variant adds device allocations and
a blocking host-to-device transfer (lines 338-362, 841-918); holding `gui_lock` across
that stalls both the preview worker and — through `gui_update()` — the GTK thread.

One path is *not* closed by any amount of widening: `gui_cleanup()` frees `g->can` with
no lock at all (line 1258). That belongs to the teardown problem noted at the end of
this report rather than to this bug, but it is the reason a refcount or an explicit
ownership handoff is the better answer here — a wider section fixes the live-GUI race
and leaves the teardown one untouched.

The rest are ordinary snapshot fixes. `zonesystem.c` need only read the dimensions once,
before its existing unlock. `colormapping.c` need only move its null test inside the
section it already takes. `ashift.c` and `rgblevels.c` need coherent scalar snapshots on
the GTK side.

`zonesystem.c` and `colormapping.c` are the instructive ones: both *look* locked, and
both leak a value across the boundary of the section that was supposed to protect it.

Two modules on the same audit came back clean: `globaltonemap.c` (`lwmax`, `hash`) and
`hazeremoval.c` (`A0`, `distance_max`, `hash`) lock every access on both sides.

## What correct code looks like

Three shapes exist in the tree.

**Snapshot under the lock** — the shape to aim for. No module in the tree is a clean
example of it end to end. `channelmixerrgb.c` gets the write side right:

```c
// widget callback, GTK main thread (_safety_changed_callback, line 2870)
dt_iop_gui_enter_critical_section(self);
g->safety_margin = dt_bauhaus_slider_get(widget);
dt_iop_gui_leave_critical_section(self);
```

but its read side is what a snapshot is meant to replace: the section is held across
the entire `_extract_color_checker()` pass (line 2161) rather than around the read, and
the validation path holds it not at all (see above). Copy the field into a local under
the lock, then drop the lock.

**Handshake flag** — `basicadj.c`. `g->call_auto_exposure` is a small state machine
(`0` idle, `1` requested, `-1` in progress, `2` done); only the transitions are inside
the critical section, and the thread that owns the `-1` state then works on `g->params`
outside it. This is the right shape when the pipe has to do real work on GUI-owned data.

**Reverse direction** — `exposure.c`: `process()` stores a computed value under the
mutex, schedules a GTK-thread callback with `g_idle_add()` (lines 509-513), and that
callback reads the value back under the same mutex.

There is also a fourth, non-standard shape: `colorharmonizer.c` carries its own
`GMutex g->histogram_lock` in `gui_data` rather than using `gui_lock`, and takes it
around the `g->hue_histogram` handoff (`_update_histogram()`, line 296). It works, but
it is inconsistent with the rest of the tree and the adjacent `g->histogram_valid` read
is still outside it.

## Adjacent, more serious: `overlay.c` calls GTK from the pipe thread

This is not a locking bug and no lock would fix it, but it was found by the same scan
and is worse than anything above, so it should not be left unrecorded.

`_setup_overlay()` (line 289) takes `self->gui_data` and calls GTK directly:
`gtk_widget_queue_draw(GTK_WIDGET(g->area))` at line 321 and
`gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), ...)` at lines 328 and 335. It also
writes `p->imgid` (line 317) and calls `dt_dev_add_history_item()` (line 319).

It is reached from the pipe: `process()` calls `_get_overlay_rgba_f()` (line 931) and
`_get_overlay_argb()` (line 901), `process_cl()` calls both as well (lines 976, 1011),
and each of those calls `_setup_overlay()` (lines 625, 783). So these are GTK calls on
a pixelpipe worker thread — the first entry in `dev-doc/GUI.md` § *Common Mistakes*.
The tooltip string built at line 325 with `g_strdup_printf()` is also never freed.

The fix is a different one from the rest of this report: marshal the redraw and the
tooltip onto the GTK thread (`dt_control_queue_redraw_widget()` for the redraw, an idle
callback for the tooltip), and move the history-item write off the pipe path.

## Suggested fix

For each field in the first two tables, wrap both the write and the read in
`dt_iop_gui_enter_critical_section()` / `dt_iop_gui_leave_critical_section()`.

Three cautions:

- The mutex is **not recursive**. Keep the critical section around the field access
  only, and do not call helpers from inside it that take the same lock.
- Copy the field into a local variable inside the critical section and use the local
  afterwards. Several `process()` implementations currently read the same field two or
  three times; taking one snapshot also removes the mid-frame inconsistency, and keeps
  the lock off the per-pixel path in `colorbalancergb.c`.
- On the `commit_params()` path, guard the critical section with
  `self->dev->gui_attached && g`. `gui_lock` is only initialised by `dt_iop_gui_init()`,
  so entering it during export locks a mutex that was never set up.

Fixing the modules one at a time is fine — they are independent, and each is a small
change. `atrous.c` needs the count and the array written under one section and read
under one section, so that the pair stays consistent.

The third table should be handled separately, because there the lock already exists and
what is wrong is its extent. Most of those are still mechanical — snapshot the value
inside the section that is already there. `colorreconstruction.c` is the exception: it
is the one case where the right fix is a decision about who owns the pointee, not a
wider section. See the notes under that table.

## Notes

- Found while reviewing whether a widget callback may write `gui_data` at all. It may;
  the rule is that a lock is required when a pipe thread also touches the field. That
  rule was not written down anywhere, and has now been added to `dev-doc/GUI.md`.
- Scan coverage. `process()`, `process_cl()` and their static helpers, and
  `commit_params()`, were scanned for `gui_data` access across `src/iop/*.c` and
  `src/iop/*.cc`, and every entry above was confirmed by reading the code. Seven other
  pipe-side callbacks — `modify_roi_in()`, `modify_roi_out()`, `tiling_callback()`,
  `init_pipe()`, `cleanup_pipe()`, `distort_transform()`, `distort_backtransform()` —
  were scanned the same way and none of them touches `gui_data` in any module.
- This is not the full callback surface. `src/iop/iop_api.h` also declares
  `input_format()`, `output_format()`, the four colorspace callbacks (lines 85-115),
  `process_tiling()` / `process_tiling_cl()` (lines 263-291) and `distort_mask()`
  (lines 307-312), all of which run pipe-side. A first pass over those found no
  `gui_data` access, but they have not been checked as carefully as the categories
  above, so treat the list as *confirmed cases*, not as an exhaustive audit.
- The modules that already use the mutex have now been audited field by field rather
  than assumed clean. That audit is what produced the third table: of the nine such
  modules, only `globaltonemap.c` and `hazeremoval.c` came back with no finding. The
  earlier drafts of this report described several of the other seven as "appearing to
  lock their pipe-side access" on the strength of a `dt_iop_gui_enter_critical_section`
  grep. That was wrong every time it mattered, `retouch.c` and `colorreconstruction.c`
  most of all, and it is worth recording as a lesson about the scan rather than
  quietly deleting.
- Orthogonal, and deliberately out of scope here: `gui_cleanup()` can run while a pipe
  is still inside `process()` on the instance-delete and undo/redo paths, which
  destroys `gui_lock` under a worker. Adding the locking above is still correct — it
  just does not close that window. See the TODO in
  `dev-doc/GUI.md` § *The Callback Must Not Outlive the Module*.
