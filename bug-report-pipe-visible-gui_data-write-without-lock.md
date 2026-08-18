# Several IOP modules share `gui_data` with pixelpipe threads without any locking

## Summary

A number of image operation modules let a widget callback write a field of their
`gui_data` struct, and then read that same field from `process()` or `process_cl()`.
Widget callbacks run on the GTK main thread; `process()` runs on a pixelpipe worker
thread. The access is not synchronised in any way, so this is a data race.

The affected modules never call `dt_iop_gui_enter_critical_section()` at all — the
mutex that exists for exactly this purpose is simply unused there.

A second, smaller group *does* use the mutex, but misses it in `commit_params()`,
which also runs on a pipe thread.

## Why it matters

Most cases found are display-mask toggles: a button that makes the module show its
internal mask instead of the image. The rest hand a computed number back from the pipe
to a GUI label. The practical consequences are mild but real:

- **Stale value.** Nothing orders the GTK-thread write against the pipe-thread read,
  so the first frame after clicking the button can show the wrong thing. A second
  redraw corrects it, which is why this is easy to miss.
- **Mid-frame inconsistency.** Several `process()` implementations read the field more
  than once, so a change landing mid-call can make one half of the frame disagree with
  the other. `colorbalancergb.c` re-reads `g->mask_type` *once per pixel*
  (`opacities[g->mask_type]`, line 929), which is the sharpest instance.
- **Lost or mismatched values on the reverse-direction cases.** `hotpixels.c` and
  `denoiseprofile.c` hand a number from the pipe to a label; `atrous.c` hands over a
  count and the array it bounds, which can be read as a mismatched pair.

These are not memory-safety bugs: none of the fields is a pointer, and every indexed
read stays in bounds even with a racing index (`opacities` is a `float[4]` and
`MASK_NONE == 3`; `atrous`'s `num_samples` cannot exceed the `MAX_NUM_SCALES` size of
`g->sample`). Torn reads are also not the concern — the fields are naturally aligned
`int`, `gboolean`, enum and `float`, which are not split by any architecture darktable
supports. What is at stake is correctness of what the user sees, and, more importantly,
that the pattern is being copied into new modules.

### `dt_dev_reprocess_*()` does not synchronise

The mask-toggle callbacks all write the field and then call
`dt_dev_reprocess_center()`. That looks like it should order the write ahead of the
pipe's read, but it does not: `dt_dev_reprocess_center()` only sets
`pipe->changed |= DT_DEV_PIPE_SYNCH`, invalidates buffers and queues a redraw
(`src/develop/develop.c:2831`). It neither waits for a running pipe nor issues any
barrier, so there is no happens-before edge and a pipe already inside `process()` is
unaffected.

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

Found by scanning `src/iop/*.c` for modules that reference `g->` inside
`process()`/`process_cl()` — including the static helpers those call — and never call
`dt_iop_gui_enter_critical_section()`.

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

## Modules that use the mutex but miss `commit_params()`

These are not covered by the scan above, because they do call
`dt_iop_gui_enter_critical_section()` — just not on the `commit_params()` path.

| module | field | site |
| --- | --- | --- |
| `exposure.c` | `effective_exposure` | `commit_params()` writes it unlocked (line 641) |
| `channelmixerrgb.c` | `run_profile`, `run_validation` | `commit_params()` reads them unlocked (lines 3131-3132); every other access takes the lock |
| `channelmixerrgb.c` | `is_blending` | `commit_params()` writes it (line 3169), GTK-thread code reads it (lines 1186, 2027); never locked |

`exposure.c` deserves attention out of proportion to its size, for two reasons. It is
the in-tree reference for the reverse direction — `dev-doc/GUI.md` points at it — so a
gap there gets copied. And the reader is in *another module*:
`_exposure_proxy_get_effective_exposure()` is published through
`dev->proxy.exposure.get_effective_exposure`, and `agx.c` calls it from
`_adjust_relative_exposure_from_exposure_params()` on the GTK thread
(`src/iop/agx.c:1049`). A cross-module reader cannot reasonably be expected to take
`exposure`'s `gui_lock` itself, so the lock has to go inside the proxy accessor and
inside `commit_params()`.

## What correct code looks like

Three shapes exist in the tree.

**Snapshot under the lock** — `channelmixerrgb.c`, for `g->safety_margin`:

```c
// widget callback, GTK main thread (_safety_changed_callback, line 2870)
dt_iop_gui_enter_critical_section(self);
g->safety_margin = dt_bauhaus_slider_get(widget);
dt_iop_gui_leave_critical_section(self);
```

The pipe-side read is in `_extract_patches()` (line 1437), which has no lock of its
own — it is covered because `process()` holds the section across the whole
`_extract_color_checker()` call (line 2161). That works, but it holds the mutex across
a patch-extraction pass, which is the opposite of the "keep critical sections short"
advice below. Prefer taking a snapshot.

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

## Suggested fix

For each field in the two tables, wrap both the write and the read in
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

## Notes

- Found while reviewing whether a widget callback may write `gui_data` at all. It may;
  the rule is that a lock is required when a pipe thread also touches the field. That
  rule was not written down anywhere, and has now been added to `dev-doc/GUI.md`.
- Scan coverage. `process()`, `process_cl()` and their static helpers, and
  `commit_params()`, were all scanned for `gui_data` access, and every entry above was
  confirmed by reading the code. The other pipe-side callbacks — `modify_roi_in()`,
  `modify_roi_out()`, `tiling_callback()`, `init_pipe()`, `cleanup_pipe()`,
  `distort_transform()`, `distort_backtransform()` — were scanned too and none of them
  touches `gui_data` in any module.
- What is *not* covered: modules that already use the mutex were only checked for
  missing `commit_params()` locking, not audited field by field. `ashift.c`,
  `colormapping.c`, `colorreconstruction.c`, `globaltonemap.c`, `hazeremoval.c`,
  `retouch.c`, `rgblevels.c` and `zonesystem.c` all lock their pipe-side `gui_data`
  access and were spot-checked only.
- Orthogonal, and deliberately out of scope here: `gui_cleanup()` can run while a pipe
  is still inside `process()` on the instance-delete and undo/redo paths, which
  destroys `gui_lock` under a worker. Adding the locking above is still correct — it
  just does not close that window. See the TODO in
  `dev-doc/GUI.md` § *The Callback Must Not Outlive the Module*.
