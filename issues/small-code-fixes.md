# Small gradient domain code issues

Status: done (2026-10-23): 1 fixed (commit 79daf02591); 2 and 3 left as they are. Kind: bug (code).

1. Stale comment above `_gd_write_back()` (`src/iop/hlreconstruct/gradient.c:1801`): says the
   nearest grid point applies beyond the outermost ones; the code clamps each coordinate and
   interpolates along the edge.
2. An image clipped everywhere in one channel stays unchanged: U has no border, so `_gd_solve()`
   returns early (`gradient.c:1530`). The variants with opposed's estimate as hue could still run.
3. The smooth fill reaches its cap of 64 times the clipping level (`GD_FILL_MAX`, `gradient.c:66`).

Source: `/tmp/hl-article/HANDOFF.md`, "Open questions". Keep `dev-doc/iop/highlight/gradient_domain.md`
current with any change.

## Result (2026-10-23)

1. Fixed in `79daf02591` ("highlights: correct the gradient domain write-back comment at the
   edges"): beyond the outermost grid points each coordinate is clamped on its own and the solution is
   still interpolated along the edge; only the corners take a single grid point. The dev-doc already
   said so (`dev-doc/iop/highlight/gradient_domain.md`, "The write-back raises only clipped
   photosites"). Built before committing.
2. Not changed: the early return only matters when one channel is clipped in every grid pixel, and
   only the experiment values with opposed's estimate as hue could use such an image; none of them is
   a candidate after issues 5 and 6. The dev-doc documents the behavior ("The method leaves the image
   unchanged in five cases").
3. Not changed: the cap belongs to the experiment value "smooth fill (paper 3.4)", which is not a
   candidate; it only bounds that fill.
