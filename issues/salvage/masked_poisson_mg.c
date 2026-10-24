/* Extracted on 2026-10-24 from branch gradient-hl, commit 6ea0e47cd6, src/iop/hlreconstruct/gradient.c lines 1-50, 58-74, 102-108, 131-869.
   Multigrid-preconditioned CG for Poisson/Laplace problems on masked, irregular pixel domains, with the index helpers it uses.
   Not compilable by itself: it was included by src/iop/highlights.c, which provides the
   darktable headers (dt_alloc_align_*, DT_OMP_FOR, dt_aligned_pixel_t, FC/FCxtrans). */

/*
   This file is part of darktable,
   Copyright (C) 2026 darktable developers.

   darktable is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   darktable is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

/* Gradient domain color restoration of clipped highlights, after
   M. Rouf, C. Lau, W. Heidrich: "Gradient Domain Color Restoration of
   Clipped Highlights", 2012.

   The method relies on the camera channels clipping independently. It works
   on RGB pixels with a clipped flag per channel, built from the photosites:

   1. conversion into RGB pixels on a grid, see _gd_to_rgb(); pixels with
      any channel flagged form the region U, unflagged pixels 4-adjacent to
      U form its border B
   2. hue estimate rho: the colors on B, smoothed along B, interpolated
      over U by a Laplace solve (paper 3.2)
   3. guidance field for each channel inside its own clipped region: the
      gradients of the unflagged other channels times the hue ratio,
      weighted by how reliable each value is (paper eq. 7, 8, see
      _gd_edge_guidance() for where it differs from eq. 7)
   4. one Poisson solve per channel over its clipped region, with the
      unclipped neighbors as boundary values (paper eq. 6); pixels that it
      leaves below their input are held at the input, and the channel is
      solved again
   5. clipped photosites take the larger of their value and the solution,
      interpolated from the grid

   Regions with all channels clipped get no guidance, so they come out flat,
   filled from the solutions around them. Two parameters select experimental
   variants of this proof of concept: the write-back can scale the clipped
   channels of a pixel together, see _gd_joint_scale(), and the experiment
   changes one step of the method, see dt_iop_highlights_experiment_t.
*/

// border smoothing of paper 3.2: spatial sigma in full-resolution pixels,
// range sigma on colors normalized to the clipping level

#define GD_CG_TOL 1e-5
#define GD_CG_MAXITER 5000
// the multigrid solver's loops run serially up to this many unknowns: on
// smaller levels, starting the threads costs more than the loop
#define GD_MG_PARALLEL_MIN 524288
// the stencil pass that also accumulates p^T A p pays for threads earlier
#define GD_MG_DOT_MIN 8192
// guided solves of at least this many unknowns smooth by red-black
// Gauss-Seidel, which needs a third to a half fewer iterations than damped
// Jacobi; on smaller ones and on the hue it moved results more than it saved time
#define GD_MG_RBGS_MIN 1310720
// damping of the Jacobi smoother
#define GD_MG_OMEGA (2.0f / 3.0f)
// coarsening stops at this many unknowns, or at a 1x1 grid or GD_MG_MAX_LEVELS
#define GD_MG_COARSEST 128
#define GD_MG_MAX_LEVELS 32
#define GD_MAX_LEVELS 32

// photosites per RGB pixel in each direction
static inline int _gd_grid_step(const uint32_t filters,
                                const gboolean full_resolution)
{
  if(!filters || full_resolution) return 1;
  return filters == 9u ? 3 : 2;
}

// pixel indices of the 4-neighbors, -1 outside the image
static inline void _gd_neighbors(const int i,
                                 const int width,
                                 const int height,
                                 int nb[4])
{
  const int row = i / width;
  const int col = i - row * width;
  nb[0] = col > 0 ? i - 1 : -1;
  nb[1] = col < width - 1 ? i + 1 : -1;
  nb[2] = row > 0 ? i - width : -1;
  nb[3] = row < height - 1 ? i + width : -1;
}

// numbers the pixels with any of the given state bits, in pixel order,
// leaving out those with these bits set in skip (if not NULL)
static size_t _gd_index_pixels(int *const map,
                               int *const pos,
                               const uint8_t *const state,
                               const uint8_t bits,
                               const uint8_t *const skip,
                               const size_t npix)
{
  size_t n = 0;
  for(size_t i = 0; i < npix; i++)
  {
    if((state[i] & bits) && !(skip && (skip[i] & bits)))
    {
      map[i] = n;
      pos[n++] = i;
    }
    else
      map[i] = -1;
  }
  return n;
}

/* The Poisson solves use conjugate gradients in double precision,
   preconditioned by one multigrid V-cycle per iteration (Briggs, Henson,
   McCormick, A Multigrid Tutorial, 2nd ed.). Plain Jacobi-preconditioned CG
   needs thousands of iterations on large clipped regions, because it removes
   smooth error slowly; the coarse levels remove it in a few dozen.

   Level 0 holds the unknowns of the solve. Each coarser level has one unknown
   per 2x2 block of the grid above that holds any unknown. Corrections move
   down and up by bilinear transfers, which keep the preconditioner symmetric,
   as CG requires. The coarse operators are a cheap stand-in (see
   _gd_mg_build()): they only affect how fast CG converges, since CG works on
   the exact fine equations.
*/
typedef struct _gd_mg_level
{
  size_t n;              // unknowns
  int w, h;              // grid size, halved and rounded up from the level above
  int *pos;              // grid index of each unknown, ascending
  const int *map;        // grid index -> unknown, -1 if none; level 0 borrows the caller's
  int *nb;               // the 4 neighbor unknowns, -1 if none; level 0 points to the
                         // zero slot n instead, so its stencil needs no test
  double *weight;        // edge weights; NULL on level 0, where all are 1
  double *diag;
  int *prolong;          // the coarse unknowns each unknown interpolates from, -1 if
                         // none, in the order of _gd_mg_build(); NULL on the coarsest level
  double *prolong_w;     // their bilinear weights, normalized to sum 1
  uint8_t *parity;       // red-black color, cached on level 0
  double *x;             // the V-cycle's correction
  double *b;             // its right-hand side; on level 0 the CG residual
  double *tmp;
} _gd_mg_level;

// a connected set of unknowns without a Dirichlet neighbor
typedef struct _gd_mg_floating
{
  size_t start;          // first entry in floating_pos
  size_t count;
  double initial_mean;   // degree-weighted mean of the start values
} _gd_mg_floating;

typedef struct _gd_mg_context
{
  _gd_mg_level levels[GD_MG_MAX_LEVELS];
  int count;
  gboolean ready;        // the hierarchy is built and can serve another right-hand side
  double *x, *r, *p, *ap, *rhs;
  int *floating_pos;     // unknowns of the floating components, one range each
  double *floating_weight;
  _gd_mg_floating *floating;
  size_t nfloating;
  size_t npfloating;     // entries used in floating_pos
} _gd_mg_context;

// (A v)_i on a level
static inline double _gd_mg_row(const _gd_mg_level *const l,
                                const double *const v,
                                const size_t i)
{
  const int *const nb = l->nb + 4 * i;
  double a = l->diag[i] * v[i];
  if(!l->weight)
  {
    a -= v[nb[0]];
    a -= v[nb[1]];
    a -= v[nb[2]];
    a -= v[nb[3]];
  }
  else
  {
    for(int d = 0; d < 4; d++)
      if(nb[d] >= 0) a -= l->weight[4 * i + d] * v[nb[d]];
  }
  return a;
}

static void _gd_mg_apply(const _gd_mg_level *const l,
                         const double *const v,
                         double *const out)
{
  DT_OMP_FOR(if(l->n > GD_MG_PARALLEL_MIN))
  for(size_t i = 0; i < l->n; i++)
    out[i] = _gd_mg_row(l, v, i);
}

static void _gd_mg_smooth_jacobi(_gd_mg_level *const l, const int sweeps)
{
  for(int s = 0; s < sweeps; s++)
  {
    DT_OMP_FOR(if(l->n > GD_MG_PARALLEL_MIN))
    for(size_t i = 0; i < l->n; i++)
    {
      const double a = _gd_mg_row(l, l->x, i);
      l->tmp[i] = l->x[i] + (l->diag[i] > 0 ? GD_MG_OMEGA * (l->b[i] - a) / l->diag[i] : 0);
    }
    double *const t = l->x;
    l->x = l->tmp;
    l->tmp = t;
  }
}

static void _gd_mg_smooth_rbgs(_gd_mg_level *const l,
                               const int sweeps,
                               const gboolean reverse)
{
  for(int s = 0; s < sweeps; s++)
  {
    for(int step = 0; step < 2; step++)
    {
      const int color = reverse ? 1 - step : step;
      DT_OMP_FOR(if(l->n > GD_MG_PARALLEL_MIN))
      for(size_t i = 0; i < l->n; i++)
      {
        const int parity = l->parity ? l->parity[i] : (l->pos[i] / l->w + l->pos[i] % l->w) & 1;
        if(parity != color) continue;
        double a = l->diag[i] * l->x[i];
        if(!l->weight)
        {
          a -= l->x[l->nb[4 * i]];
          a -= l->x[l->nb[4 * i + 1]];
          a -= l->x[l->nb[4 * i + 2]];
          a -= l->x[l->nb[4 * i + 3]];
        }
        else
        {
          for(int d = 0; d < 4; d++)
            if(l->nb[4 * i + d] >= 0) a -= l->weight[4 * i + d] * l->x[l->nb[4 * i + d]];
        }
        if(l->diag[i] > 0) l->x[i] += (l->b[i] - a) / l->diag[i];
      }
    }
  }
}

// rbgs: forward sweeps before the coarse correction and reverse ones after it,
// so that the V-cycle stays symmetric
static void _gd_mg_smooth(_gd_mg_level *const l,
                          const int sweeps,
                          const gboolean reverse,
                          const gboolean rbgs)
{
  if(rbgs)
    _gd_mg_smooth_rbgs(l, sweeps, reverse);
  else
    _gd_mg_smooth_jacobi(l, sweeps);
}

// x = approximate solution of A x = b on the level depth and below
static void _gd_mg_cycle(_gd_mg_level *const levels,
                         const int depth,
                         const int count,
                         const gboolean rbgs)
{
  _gd_mg_level *const l = levels + depth;
  // the correction starts at zero, where a Jacobi sweep reduces to its
  // diagonal step: do that step here and count it as the first sweep
  const int first = rbgs ? 0 : 1;
  if(rbgs)
    memset(l->x, 0, sizeof(double) * l->n);
  else
  {
    DT_OMP_FOR(if(l->n > GD_MG_PARALLEL_MIN))
    for(size_t i = 0; i < l->n; i++)
      l->x[i] = l->diag[i] > 0 ? GD_MG_OMEGA * l->b[i] / l->diag[i] : 0;
  }

  if(depth == count - 1)
  {
    // few unknowns: sweeps are cheap here, and 32 of them stand in for a direct solve
    if(rbgs)
    {
      _gd_mg_smooth(l, 16, FALSE, rbgs);
      _gd_mg_smooth(l, 16, TRUE, rbgs);
    }
    else
      _gd_mg_smooth(l, 32 - first, FALSE, rbgs);
    return;
  }

  _gd_mg_smooth(l, 2 - first, FALSE, rbgs);
  _gd_mg_apply(l, l->x, l->tmp);

  // restriction, the transpose of the prolongation: each coarse unknown sums
  // the residuals of the fine unknowns that interpolate from it. Those lie in
  // the 4x4 window of fine pixels around its 2x2 block, and the window is read
  // in ascending order of the fine unknowns, which fixes the rounding
  _gd_mg_level *const c = l + 1;
  DT_OMP_FOR(if(c->n > GD_MG_PARALLEL_MIN))
  for(size_t i = 0; i < c->n; i++)
  {
    const int cy = c->pos[i] / c->w;
    const int cx = c->pos[i] % c->w;
    double b = 0;
    for(int fy = MAX(2 * cy - 1, 0); fy <= MIN(2 * cy + 2, l->h - 1); fy++)
      for(int fx = MAX(2 * cx - 1, 0); fx <= MIN(2 * cx + 2, l->w - 1); fx++)
      {
        const int k = l->map[(size_t)fy * l->w + fx];
        if(k < 0) continue;
        // which of the fine unknown's 4 coarse sources this one is, see _gd_mg_build()
        const int d = (fy / 2 == cy ? 0 : 2) + (fx / 2 == cx ? 0 : 1);
        b += l->prolong_w[4 * k + d] * (l->b[k] - l->tmp[k]);
      }
    c->b[i] = b;
  }

  _gd_mg_cycle(levels, depth + 1, count, rbgs);

  DT_OMP_FOR(if(l->n > GD_MG_PARALLEL_MIN))
  for(size_t i = 0; i < l->n; i++)
  {
    double a = 0;
    for(int d = 0; d < 4; d++)
      if(l->prolong[4 * i + d] >= 0) a += l->prolong_w[4 * i + d] * c->x[l->prolong[4 * i + d]];
    l->x[i] += a;
  }
  _gd_mg_smooth(l, 2, TRUE, rbgs);
}

// coarse levels get their own right-hand side and edge weights; level 0 uses
// the CG residual and unit weights
static gboolean _gd_mg_allocate(_gd_mg_level *const l,
                                const size_t n,
                                const int w,
                                const int h,
                                const gboolean coarse)
{
  l->n = n;
  l->w = w;
  l->h = h;
  l->pos = malloc(sizeof(int) * n);
  l->nb = malloc(sizeof(int) * 4 * n);
  l->diag = calloc(n, sizeof(double));
  // one more slot for level 0's absent neighbors
  l->x = calloc(n + 1, sizeof(double));
  l->tmp = calloc(n + 1, sizeof(double));
  if(coarse)
  {
    l->b = calloc(n, sizeof(double));
    l->weight = calloc(4 * n, sizeof(double));
  }
  if(!l->pos || !l->nb || !l->diag || !l->x || !l->tmp || (coarse && (!l->b || !l->weight)))
    return FALSE;

  for(size_t i = 0; i < 4 * n; i++)
    l->nb[i] = -1;
  return TRUE;
}

static void _gd_mg_free(_gd_mg_context *const ctx)
{
  for(int d = 0; d < ctx->count; d++)
  {
    _gd_mg_level *const l = ctx->levels + d;
    if(d > 0)
    {
      // level 0 borrows the caller's map and the CG residual
      free((void *)l->map);
      free(l->b);
    }
    free(l->pos);
    free(l->nb);
    free(l->weight);
    free(l->diag);
    free(l->prolong);
    free(l->prolong_w);
    free(l->parity);
    free(l->x);
    free(l->tmp);
  }
  free(ctx->x);
  free(ctx->r);
  free(ctx->p);
  free(ctx->ap);
  free(ctx->rhs);
  free(ctx->floating_pos);
  free(ctx->floating_weight);
  free(ctx->floating);
  memset(ctx, 0, sizeof(*ctx));
}

// the unknown of the coarse 2x2 block that holds unknown i of level l
static inline int _gd_mg_parent(const _gd_mg_level *const l,
                                const int *const cmap,
                                const int cw,
                                const size_t i)
{
  return cmap[(l->pos[i] / l->w / 2) * cw + (l->pos[i] % l->w) / 2];
}

/* Builds the levels below level 0, whose unknowns, neighbors and diagonal are
   set. The coarse graph aggregates 2x2 blocks: edges inside a block cancel
   out of the diagonal, the edges between two blocks add up, and the Dirichlet
   part of the diagonal is carried over. All of it is then halved. This is
   not the Galerkin operator P^T A P of the bilinear prolongation P (Briggs et
   al., chapter 5), which has a wider stencil, but it is symmetric and has no
   more neighbors than the fine graph.
   Returns FALSE if out of memory.
*/
static gboolean _gd_mg_build(_gd_mg_context *const ctx)
{
  _gd_mg_level *const levels = ctx->levels;
  while(ctx->count < GD_MG_MAX_LEVELS
        && levels[ctx->count - 1].n > GD_MG_COARSEST
        && (levels[ctx->count - 1].w > 1 || levels[ctx->count - 1].h > 1))
  {
    _gd_mg_level *const l = levels + ctx->count - 1;
    const int cw = (l->w + 1) / 2;
    const int ch = (l->h + 1) / 2;
    int *const cmap = malloc(sizeof(int) * (size_t)cw * ch);
    if(!cmap) return FALSE;
    for(size_t i = 0; i < (size_t)cw * ch; i++)
      cmap[i] = -1;
    for(size_t i = 0; i < l->n; i++)
      cmap[(l->pos[i] / l->w / 2) * cw + (l->pos[i] % l->w) / 2] = 0;
    size_t nc = 0;
    for(size_t i = 0; i < (size_t)cw * ch; i++)
      if(cmap[i] >= 0) cmap[i] = nc++;

    _gd_mg_level *const c = levels + ctx->count++;
    c->map = cmap;
    if(!_gd_mg_allocate(c, nc, cw, ch, TRUE)) return FALSE;
    for(size_t i = 0; i < l->n; i++)
      c->pos[_gd_mg_parent(l, cmap, cw, i)] = (l->pos[i] / l->w / 2) * cw + (l->pos[i] % l->w) / 2;

    for(size_t i = 0; i < l->n; i++)
    {
      const int k = _gd_mg_parent(l, cmap, cw, i);
      c->diag[k] += l->diag[i];
      for(int d = 0; d < 4; d++)
      {
        const int q = l->nb[4 * i + d];
        if(q < 0) continue;
        const int j = _gd_mg_parent(l, cmap, cw, q);
        const double w = l->weight ? l->weight[4 * i + d] : 1.0;
        if(j == k)
          c->diag[k] -= w;
        else
        {
          c->nb[4 * k + d] = j;
          c->weight[4 * k + d] += w;
        }
      }
    }

    /* bilinear prolongation: a fine pixel interpolates from the coarse pixel
       of its block (index d = 0) and the neighbors of that one toward the
       fine pixel's side: d = 1 horizontal, 2 vertical, 3 diagonal, with
       weights 3/4 and 1/4 in each direction. Absent sources are left out and
       the rest renormalized */
    l->prolong = malloc(sizeof(int) * 4 * l->n);
    l->prolong_w = calloc(4 * l->n, sizeof(double));
    if(!l->prolong || !l->prolong_w) return FALSE;
    for(size_t i = 0; i < l->n; i++)
    {
      const int row = l->pos[i] / l->w;
      const int col = l->pos[i] % l->w;
      const int ys[2] = { row / 2, row / 2 + (row % 2 ? 1 : -1) };
      const int xs[2] = { col / 2, col / 2 + (col % 2 ? 1 : -1) };
      double total = 0;
      for(int d = 0; d < 4; d++)
      {
        const int yy = ys[d / 2];
        const int xx = xs[d % 2];
        const int j = yy >= 0 && yy < ch && xx >= 0 && xx < cw ? cmap[yy * cw + xx] : -1;
        l->prolong[4 * i + d] = j;
        if(j >= 0)
        {
          l->prolong_w[4 * i + d] = (d / 2 ? .25f : .75f) * (d % 2 ? .25f : .75f);
          total += l->prolong_w[4 * i + d];
        }
      }
      for(int d = 0; d < 4; d++)
        if(l->prolong[4 * i + d] >= 0) l->prolong_w[4 * i + d] /= total;
    }

    for(size_t k = 0; k < c->n; k++)
    {
      c->diag[k] *= .5f;
      for(int d = 0; d < 4; d++)
        c->weight[4 * k + d] *= .5f;
    }
  }
  return TRUE;
}

/* Finds the floating components of level 0 and records their unknowns and
   degrees. The Laplacian is singular on them: adding a constant to one
   changes no equation. Jacobi-preconditioned CG keeps their degree-weighted
   mean at that of the start values, and the solve keeps it too, since the
   start values are the method's estimate there. The hue solves (no div) have
   a constant solution on such a component, so its equations are replaced by
   x = mean. Returns FALSE if out of memory.
*/
static gboolean _gd_mg_find_floating(_gd_mg_context *const ctx, const gboolean guided)
{
  _gd_mg_level *const f = ctx->levels;
  const size_t n = f->n;
  int *const seen = calloc(n, sizeof(int));
  int *const queue = malloc(n * sizeof(int));
  gboolean ok = seen && queue;
  for(size_t first = 0; ok && first < n; first++)
  {
    if(seen[first]) continue;

    // breadth-first over the component; anchored if any unknown has a Dirichlet neighbor
    size_t head = 0;
    size_t tail = 1;
    queue[0] = first;
    seen[first] = 1;
    gboolean anchored = FALSE;
    while(head < tail)
    {
      const int k = queue[head++];
      int edges = 0;
      for(int d = 0; d < 4; d++)
      {
        const int q = f->nb[4 * k + d];
        if(q < 0) continue;
        edges++;
        if(!seen[q])
        {
          seen[q] = 1;
          queue[tail++] = q;
        }
      }
      if(f->diag[k] > edges) anchored = TRUE;
    }
    if(anchored) continue;

    if(!ctx->floating_pos) ctx->floating_pos = malloc(n * sizeof(int));
    if(!ctx->floating_weight) ctx->floating_weight = malloc(n * sizeof(double));
    _gd_mg_floating *const ranges = realloc(ctx->floating, (ctx->nfloating + 1) * sizeof(*ranges));
    if(ranges) ctx->floating = ranges;
    if(!ctx->floating_pos || !ctx->floating_weight || !ranges)
    {
      ok = FALSE;
      break;
    }

    ctx->floating[ctx->nfloating++] = (_gd_mg_floating){ ctx->npfloating, tail, 0 };
    memcpy(ctx->floating_pos + ctx->npfloating, queue, tail * sizeof(int));
    for(size_t t = 0; t < tail; t++)
    {
      const int k = queue[t];
      // an isolated unknown has no equation; it keeps its start value
      ctx->floating_weight[ctx->npfloating + t] = f->diag[k] > 0 ? f->diag[k] : 1;
      if(!guided)
      {
        f->diag[k] = 1;
        for(int d = 0; d < 4; d++)
          f->nb[4 * k + d] = -1;
      }
    }
    ctx->npfloating += tail;
  }
  free(seen);
  free(queue);
  return ok;
}

// subtracts the mean over each floating component
static void _gd_mg_project(const _gd_mg_context *const ctx, double *const v)
{
  for(size_t i = 0; i < ctx->nfloating; i++)
  {
    const _gd_mg_floating range = ctx->floating[i];
    double mean = 0;
    for(size_t t = 0; t < range.count; t++)
      mean += v[ctx->floating_pos[range.start + t]];
    mean /= range.count;
    for(size_t t = 0; t < range.count; t++)
      v[ctx->floating_pos[range.start + t]] -= mean;
  }
}

static void _gd_mg_restore_gauge(const _gd_mg_context *const ctx, double *const x)
{
  for(size_t i = 0; i < ctx->nfloating; i++)
  {
    const _gd_mg_floating range = ctx->floating[i];
    double sum = 0;
    double weight = 0;
    for(size_t t = 0; t < range.count; t++)
    {
      const size_t j = range.start + t;
      sum += ctx->floating_weight[j] * x[ctx->floating_pos[j]];
      weight += ctx->floating_weight[j];
    }
    const double shift = range.initial_mean - sum / weight;
    for(size_t t = 0; t < range.count; t++)
      x[ctx->floating_pos[range.start + t]] += shift;
  }
}

/* Solves  sum over the 4-neighbors q of p of (u_p - u_q) = div_p  for the n
   unknown pixels, by the multigrid-preconditioned CG described above.
   Neighbors that are not unknowns supply boundary values from bval
   (Dirichlet); neighbors outside the image are left out (Neumann), and so
   are those not marked in known, if given. A component of unknowns without a
   Dirichlet neighbor keeps the degree-weighted mean of its start values.
   out_x holds the initial guess on entry and the solution on return. ctx starts
   zeroed; it keeps the hierarchy for further solves on the same unknowns,
   map and known with another bval and the same kind of div (given or not),
   and must be released with _gd_mg_free().
   Returns the number of iterations, -1 if out of memory.
*/
static int _gd_mg_solve(_gd_mg_context *const ctx,
                        float *const restrict out_x,
                        const float *const restrict div,
                        const float *const restrict bval,
                        const size_t stride,
                        const int *const restrict map,
                        const int *const restrict pos,
                        const uint8_t *const restrict known,
                        const size_t n,
                        const int width,
                        const int height)
{
  _gd_mg_level *const levels = ctx->levels;
  _gd_mg_level *const f = levels;
  const gboolean reused = ctx->ready;
  if(!reused)
  {
    ctx->count = 1;
    ctx->x = calloc(n + 1, sizeof(double));
    ctx->r = malloc(sizeof(double) * n);
    ctx->p = calloc(n + 1, sizeof(double));
    ctx->ap = malloc(sizeof(double) * n);
    ctx->rhs = malloc(sizeof(double) * n);
    if(!_gd_mg_allocate(f, n, width, height, FALSE)
       || !ctx->x || !ctx->r || !ctx->p || !ctx->ap || !ctx->rhs)
      return -1;
    f->b = ctx->r;
  }
  f->map = map;

  double *const x = ctx->x;
  double *const r = ctx->r;
  double *const p = ctx->p;
  double *const ap = ctx->ap;
  double *const rhs = ctx->rhs;
  for(size_t k = 0; k < n; k++)
    x[k] = out_x[k];

  DT_OMP_FOR(if(n > GD_MG_PARALLEL_MIN))
  for(size_t k = 0; k < n; k++)
  {
    int nb[4];
    _gd_neighbors(pos[k], width, height, nb);
    if(!reused) f->pos[k] = pos[k];
    float b = div ? div[k] : 0;
    for(int d = 0; d < 4; d++)
    {
      const int q = nb[d];
      if(q < 0 || (known && map[q] < 0 && !known[q])) continue;
      if(!reused)
      {
        f->diag[k]++;
        if(map[q] >= 0) f->nb[4 * k + d] = map[q];
      }
      if(map[q] < 0) b += bval[stride * q];
    }
    rhs[k] = b;
  }

  if(!reused)
  {
    if(!_gd_mg_find_floating(ctx, div != NULL) || !_gd_mg_build(ctx)) return -1;
    f->parity = malloc(n);
    if(!f->parity) return -1;
    for(size_t i = 0; i < n; i++)
    {
      f->parity[i] = (f->pos[i] / f->w + f->pos[i] % f->w) & 1;
      for(int d = 0; d < 4; d++)
        if(f->nb[4 * i + d] < 0) f->nb[4 * i + d] = n;
    }
    ctx->ready = TRUE;
  }
  const gboolean rbgs = div && n >= GD_MG_RBGS_MIN;

  for(size_t i = 0; i < ctx->nfloating; i++)
  {
    _gd_mg_floating *const range = ctx->floating + i;
    double sum = 0;
    double weight = 0;
    for(size_t t = 0; t < range->count; t++)
    {
      const size_t j = range->start + t;
      sum += ctx->floating_weight[j] * x[ctx->floating_pos[j]];
      weight += ctx->floating_weight[j];
    }
    range->initial_mean = sum / weight;
    if(!div)
    {
      for(size_t t = 0; t < range->count; t++)
      {
        const int k = ctx->floating_pos[range->start + t];
        rhs[k] = range->initial_mean;
        x[k] = range->initial_mean;
      }
    }
  }
  // the guidance is summed in float, which can leave a component of rounding
  // size that no solution satisfies
  if(div) _gd_mg_project(ctx, rhs);

  _gd_mg_apply(f, x, ap);
  double rr = 0.0, bb = 0.0;
  DT_OMP_FOR(if(n > GD_MG_PARALLEL_MIN) reduction(+ : bb, rr))
  for(size_t k = 0; k < n; k++)
  {
    r[k] = rhs[k] - ap[k];
    rr += r[k] * r[k];
    bb += rhs[k] * rhs[k];
  }

  const double stop = GD_CG_TOL * GD_CG_TOL * fmax(bb, rr);
  double rz = 0.0;
  int iter = 0;
  while(rr > stop && iter < GD_CG_MAXITER)
  {
    // z = V-cycle applied to r, in f->x
    _gd_mg_cycle(levels, 0, ctx->count, rbgs);
    if(div)
      _gd_mg_project(ctx, f->x);
    else
    {
      // the coarse levels mix components; keep the fixed hue values fixed
      for(size_t t = 0; t < ctx->npfloating; t++)
        f->x[ctx->floating_pos[t]] = 0;
    }

    double next_rz = 0.0;
    DT_OMP_FOR(if(n > GD_MG_PARALLEL_MIN) reduction(+ : next_rz))
    for(size_t k = 0; k < n; k++)
      next_rz += r[k] * f->x[k];

    const double beta = iter && rz ? next_rz / rz : 0;
    DT_OMP_FOR(if(n > GD_MG_PARALLEL_MIN))
    for(size_t k = 0; k < n; k++)
      p[k] = beta ? f->x[k] + beta * p[k] : f->x[k];
    rz = next_rz;

    double pap = 0.0;
    DT_OMP_FOR(if(n > GD_MG_DOT_MIN) reduction(+ : pap))
    for(size_t k = 0; k < n; k++)
    {
      const double a = _gd_mg_row(f, p, k);
      ap[k] = a;
      pap += p[k] * a;
    }
    if(pap <= 0.0) break;

    const double alpha = rz / pap;
    rr = 0.0;
    DT_OMP_FOR(if(n > GD_MG_PARALLEL_MIN) reduction(+ : rr))
    for(size_t k = 0; k < n; k++)
    {
      x[k] += alpha * p[k];
      r[k] -= alpha * ap[k];
      rr += r[k] * r[k];
    }
    iter++;

    // the updated residual drifts from the true one: stop only on the true
    // one, and restart CG from it if it misses the target
    if(rr <= stop)
    {
      _gd_mg_apply(f, x, ap);
      rr = 0.0;
      DT_OMP_FOR(if(n > GD_MG_PARALLEL_MIN) reduction(+ : rr))
      for(size_t k = 0; k < n; k++)
      {
        r[k] = rhs[k] - ap[k];
        rr += r[k] * r[k];
      }
      if(rr > stop) rz = 0.0;
    }
  }

  _gd_mg_restore_gauge(ctx, x);
  for(size_t k = 0; k < n; k++)
    out_x[k] = x[k];
  return iter;
}

// _gd_mg_solve() without a hierarchy kept for later solves
static int _gd_solve_poisson(float *const restrict x,
                             const float *const restrict div,
                             const float *const restrict bval,
                             const size_t stride,
                             const int *const restrict map,
                             const int *const restrict pos,
                             const uint8_t *const restrict known,
                             const size_t n,
                             const int width,
                             const int height)
{
  _gd_mg_context ctx = { 0 };
  const int iter = _gd_mg_solve(&ctx, x, div, bval, stride, map, pos, known, n, width, height);
  _gd_mg_free(&ctx);
  return iter;
}
