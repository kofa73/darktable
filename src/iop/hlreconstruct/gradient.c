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
      gradients of the unflagged other channels scaled by the hue ratio,
      weighted by how reliable each value is (paper eq. 7, 8)
   4. one Poisson solve per channel over its clipped region, with the
      unclipped neighbors as boundary values (paper eq. 6)
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
#define GD_BORDER_SIGMA_S 5.0f
#define GD_BORDER_SIGMA_R 0.25f
// peak and floor of the confidence weight, paper eq. 8
#define GD_WEIGHT_PEAK 0.65f
#define GD_WEIGHT_EPS 1e-3f
// keeps hue ratios finite where the border is black in a channel
#define GD_RHO_MIN 1e-4f
#define GD_CG_TOL 1e-5
#define GD_CG_MAXITER 5000
#define GD_MAX_LEVELS 32
// smooth fill of paper 3.4: share of a channel's clipped region that may have
// another channel unflagged, floor of the values before the log and cap of the
// filled values, both relative to the clipping level
#define GD_FILL_TOL 0.05f
#define GD_LOG_FLOOR 1e-3f
#define GD_FILL_MAX 64.0f
// X-Trans plane fit: 6x6 pattern phases, 3 colors, 5x5 window positions
#define GD_FIT_SIZE (6 * 6 * 3 * 25)

// photosites per RGB pixel in each direction
static inline int _gd_grid_step(const uint32_t filters,
                                const gboolean full_resolution)
{
  if(!filters || full_resolution) return 1;
  return filters == 9u ? 3 : 2;
}

// confidence in a channel value normalized to its clipping level: low near
// black, where noise dominates, and near clipping, paper eq. 8
static inline float _gd_weight(const float v)
{
  const float n = CLAMPF(v, 0.0f, 1.0f);
  const float x = n <= GD_WEIGHT_PEAK
    ? n / GD_WEIGHT_PEAK
    : (1.0f - n) / (1.0f - GD_WEIGHT_PEAK);
  // the paper prints 3x^3 - 2x^2, which is negative below x = 2/3; the
  // zero slopes it asks for at 0, 1 and the peak are those of smoothstep
  return x * x * (3.0f - 2.0f * x) + GD_WEIGHT_EPS;
}

static inline float _gd_ratio(const float *const rho[3],
                              const size_t i,
                              const int j,
                              const int k)
{
  return fmaxf(rho[j][i], 0.0f) / fmaxf(rho[k][i], GD_RHO_MIN);
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

// out = A v, A being the Laplacian restricted to the unknowns
static void _gd_laplacian(float *const restrict out,
                          const float *const restrict v,
                          const int *const restrict map,
                          const int *const restrict pos,
                          const size_t n,
                          const int width,
                          const int height)
{
  DT_OMP_FOR()
  for(size_t k = 0; k < n; k++)
  {
    int nb[4];
    _gd_neighbors(pos[k], width, height, nb);
    float sum = 0.0f;
    int cnt = 0;
    for(int m = 0; m < 4; m++)
    {
      if(nb[m] < 0) continue;
      cnt++;
      const int j = map[nb[m]];
      if(j >= 0) sum += v[j];
    }
    out[k] = cnt * v[k] - sum;
  }
}

/* Solves  sum over the 4-neighbors q of p of (u_p - u_q) = div_p  for the n
   unknown pixels, by conjugate gradients with a Jacobi preconditioner.
   Neighbors that are not unknowns supply boundary values from bval
   (Dirichlet); neighbors outside the image are left out (Neumann).
   x holds the initial guess on entry and the solution on return.
   Returns the number of iterations, -1 if out of memory.
*/
static int _gd_solve_poisson(float *const restrict x,
                             const float *const restrict div,
                             const float *const restrict bval,
                             const size_t stride,
                             const int *const restrict map,
                             const int *const restrict pos,
                             const size_t n,
                             const int width,
                             const int height)
{
  float *const rhs = dt_alloc_align_float(n);
  float *const idiag = dt_alloc_align_float(n);
  float *const r = dt_alloc_align_float(n);
  float *const p = dt_alloc_align_float(n);
  float *const ap = dt_alloc_align_float(n);
  int iter = -1;
  if(!rhs || !idiag || !r || !p || !ap) goto cleanup;

  DT_OMP_FOR()
  for(size_t k = 0; k < n; k++)
  {
    int nb[4];
    _gd_neighbors(pos[k], width, height, nb);
    float sum = div ? div[k] : 0.0f;
    int cnt = 0;
    for(int m = 0; m < 4; m++)
    {
      if(nb[m] < 0) continue;
      cnt++;
      if(map[nb[m]] < 0) sum += bval[stride * nb[m]];
    }
    rhs[k] = sum;
    idiag[k] = cnt ? 1.0f / cnt : 0.0f;
  }

  _gd_laplacian(ap, x, map, pos, n, width, height);
  double rz = 0.0, rr = 0.0, bb = 0.0;
  DT_OMP_FOR(reduction(+ : rz, rr, bb))
  for(size_t k = 0; k < n; k++)
  {
    r[k] = rhs[k] - ap[k];
    p[k] = r[k] * idiag[k];
    rz += r[k] * p[k];
    rr += r[k] * r[k];
    bb += rhs[k] * rhs[k];
  }

  const double stop = GD_CG_TOL * GD_CG_TOL * fmax(bb, rr);
  iter = 0;
  while(rr > stop && iter < GD_CG_MAXITER)
  {
    _gd_laplacian(ap, p, map, pos, n, width, height);
    double pap = 0.0;
    DT_OMP_FOR(reduction(+ : pap))
    for(size_t k = 0; k < n; k++)
      pap += p[k] * ap[k];
    if(pap <= 0.0) break;

    const float alpha = rz / pap;
    double rz_new = 0.0;
    rr = 0.0;
    DT_OMP_FOR(reduction(+ : rz_new, rr))
    for(size_t k = 0; k < n; k++)
    {
      x[k] += alpha * p[k];
      r[k] -= alpha * ap[k];
      rz_new += r[k] * r[k] * idiag[k];
      rr += r[k] * r[k];
    }

    const float beta = rz_new / rz;
    rz = rz_new;
    DT_OMP_FOR()
    for(size_t k = 0; k < n; k++)
      p[k] = r[k] * idiag[k] + beta * p[k];
    iter++;
  }

cleanup:
  dt_free_align(rhs);
  dt_free_align(idiag);
  dt_free_align(r);
  dt_free_align(p);
  dt_free_align(ap);
  return iter;
}

/* Starting point for the hue Laplace solve, which converges slowly on large
   regions from a flat guess: a pull-push pyramid spreads the known values
   into the pixels to fill. Coarser levels sum the values and counts of 2x2
   children; on the way back, a pixel without data takes its parent's mean.
   Returns FALSE if out of memory before the first level.
*/
static gboolean _gd_pullpush_fill(float *const plane[3],
                                  const uint8_t *const known,
                                  const uint8_t *const fill,
                                  const int width,
                                  const int height)
{
  float *lev[GD_MAX_LEVELS] = { NULL };
  int lw[GD_MAX_LEVELS], lh[GD_MAX_LEVELS];
  int nlev = 0;
  int w = width;
  int h = height;

  while((w > 1 || h > 1) && nlev < GD_MAX_LEVELS)
  {
    const int cw = (w + 1) / 2;
    const int chh = (h + 1) / 2;
    float *const c = dt_calloc_align_float((size_t)4 * cw * chh);
    if(!c) break;
    const float *const f = nlev ? lev[nlev - 1] : NULL;
    DT_OMP_FOR()
    for(int row = 0; row < chh; row++)
    {
      for(int col = 0; col < cw; col++)
      {
        float *const o = c + (size_t)4 * (row * cw + col);
        for(int y = 2 * row; y < MIN(2 * row + 2, h); y++)
        {
          for(int x = 2 * col; x < MIN(2 * col + 2, w); x++)
          {
            const size_t i = (size_t)y * w + x;
            if(f)
            {
              for(int k = 0; k < 4; k++) o[k] += f[4 * i + k];
            }
            else if(known[i])
            {
              for_three_channels(k) o[k] += plane[k][i];
              o[3] += 1.0f;
            }
          }
        }
      }
    }
    lev[nlev] = c;
    lw[nlev] = cw;
    lh[nlev] = chh;
    nlev++;
    w = cw;
    h = chh;
  }

  for(int l = nlev - 1; l >= 0; l--)
  {
    float *const c = lev[l];
    const float *const parent = l + 1 < nlev ? lev[l + 1] : NULL;
    const int cw = lw[l];
    const int pw = parent ? lw[l + 1] : 0;
    DT_OMP_FOR()
    for(int row = 0; row < lh[l]; row++)
    {
      for(int col = 0; col < cw; col++)
      {
        float *const o = c + (size_t)4 * (row * cw + col);
        if(o[3] > 0.0f)
        {
          for_three_channels(k) o[k] /= o[3];
        }
        else if(parent)
        {
          const float *const pp = parent + (size_t)4 * ((row / 2) * pw + col / 2);
          for_three_channels(k) o[k] = pp[k];
        }
      }
    }
  }

  if(nlev)
  {
    const float *const c = lev[0];
    const int cw = lw[0];
    DT_OMP_FOR()
    for(int row = 0; row < height; row++)
    {
      for(int col = 0; col < width; col++)
      {
        const size_t i = (size_t)row * width + col;
        if(fill[i])
        {
          const float *const pp = c + (size_t)4 * ((row / 2) * cw + col / 2);
          for_three_channels(k) plane[k][i] = pp[k];
        }
      }
    }
  }

  for(int l = 0; l < nlev; l++)
    dt_free_align(lev[l]);
  return nlev > 0 || (width == 1 && height == 1);
}

/* Boundary cleanup of paper 3.2: noise on the border would turn into
   streaks of the interpolated hue. A bilateral filter that only collects
   border pixels smooths along the border line, like the 1D filter of the
   paper, without needing the border as an ordered curve.
*/
static void _gd_smooth_border(float *const rho[3],
                              const float *const in,
                              const uint8_t *const border,
                              const int width,
                              const int height,
                              const dt_aligned_pixel_t inv_clip,
                              const float sigma_s)
{
  const int rad = (int)ceilf(2.0f * sigma_s);
  const float ws = -0.5f / sqf(sigma_s);
  const float wr = -0.5f / sqf(GD_BORDER_SIGMA_R);

  DT_OMP_PRAGMA(parallel for default(firstprivate) schedule(dynamic, 16))
  for(int row = 0; row < height; row++)
  {
    for(int col = 0; col < width; col++)
    {
      const size_t i = (size_t)row * width + col;
      if(!border[i]) continue;
      const float *const c = in + 4 * i;
      dt_aligned_pixel_t sum = { 0.0f, 0.0f, 0.0f, 0.0f };
      float wsum = 0.0f;
      for(int y = MAX(0, row - rad); y <= MIN(height - 1, row + rad); y++)
      {
        for(int x = MAX(0, col - rad); x <= MIN(width - 1, col + rad); x++)
        {
          const size_t j = (size_t)y * width + x;
          if(!border[j]) continue;
          const float *const q = in + 4 * j;
          float d2 = 0.0f;
          for(int k = 0; k < 3; k++) d2 += sqf((q[k] - c[k]) * inv_clip[k]);
          const float wt = expf(ws * (sqf(y - row) + sqf(x - col)) + wr * d2);
          for_three_channels(k) sum[k] += wt * q[k];
          wsum += wt;
        }
      }
      // the center pixel itself guarantees wsum >= 1
      for_three_channels(k) rho[k][i] = sum[k] / wsum;
    }
  }
}

/* Channels that can serve as references at pixel i: the unflagged ones, and
   flagged ones whose values are already restored, by an earlier solve
   (solved) or by the smooth fill (filled, may be NULL).
*/
static inline uint8_t _gd_usable(const uint8_t *const state,
                                 const uint8_t *const filled,
                                 const uint8_t solved,
                                 const size_t i)
{
  return (~state[i] | solved | (filled ? filled[i] : 0)) & 7;
}

/* Target value of u_j(q) - u_j(p) on the edge p-q, from the other channels,
   paper eq. 7, with the weights min-filtered over the edge as in paper 3.5.
   A channel flagged clipped at either end is left out unless it is restored
   there, see _gd_usable(): a block mean or a window value that includes a
   clipped photosite can sit far below the clipping level, where eq. 8 would
   give it full confidence.
*/
static inline float _gd_edge_guidance(const float *const in,
                                      const uint8_t *const state,
                                      const uint8_t *const filled,
                                      const uint8_t solved,
                                      const float *const rho[3],
                                      const dt_aligned_pixel_t inv_clip,
                                      const size_t p,
                                      const size_t q,
                                      const int j)
{
  const uint8_t usable = _gd_usable(state, filled, solved, p) & _gd_usable(state, filled, solved, q);
  float num = 0.0f;
  float den = 0.0f;
  for(int k = 0; k < 3; k++)
  {
    if(k == j || !(usable & (1 << k))) continue;
    const float w = fminf(_gd_weight(in[4 * p + k] * inv_clip[k]),
                          _gd_weight(in[4 * q + k] * inv_clip[k]));
    const float r = 0.5f * (_gd_ratio(rho, p, j, k) + _gd_ratio(rho, q, j, k));
    num += w * r * (in[4 * q + k] - in[4 * p + k]);
    den += w;
  }
  // without a reference, the edge gets no guidance, as in regions with all
  // channels clipped (paper 3.2)
  return den > 0.0f ? num / den : 0.0f;
}

/* Least-squares plane fits to the photosites of each color in the 5x5 window
   centered on an X-Trans photosite, evaluated at the photosite. A 3x3 mean
   samples red and blue up to one photosite off-center, by an offset that
   changes from one photosite or block to the next and turns flat areas into
   false gradients and hues. The plane fit is exact on linear ramps at every
   phase, and its weights in a centered window are all positive, so its
   result stays within the values it uses. The weights depend only on the
   pattern phase: fit[75 * (6 * (row % 6) + col % 6) + 25 * color + 5 * (dy + 2) + dx + 2].
   Returns FALSE if a color does not span a plane in some window.
*/
static gboolean _gd_xtrans_fit(float *const fit,
                               const uint8_t (*const xtrans)[6])
{
  for(int pr = 0; pr < 6; pr++)
  {
    for(int pc = 0; pc < 6; pc++)
    {
      for(int c = 0; c < 3; c++)
      {
        // normal matrix of the plane a + b dx + c dy
        double m[3][3] = { { 0.0 } };
        for(int dy = -2; dy <= 2; dy++)
        {
          for(int dx = -2; dx <= 2; dx++)
          {
            if(FCNxtrans(pr + dy, pc + dx, xtrans) != c) continue;
            const double a[3] = { 1.0, dx, dy };
            for(int i = 0; i < 3; i++)
              for(int k = 0; k < 3; k++)
                m[i][k] += a[i] * a[k];
          }
        }
        // the plane's value at the photosite is a, so the weights come from
        // the first row of the inverse; all entries are small integers, so
        // a collinear set gives an exact 0
        const double r0 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
        const double r1 = m[0][2] * m[2][1] - m[0][1] * m[2][2];
        const double r2 = m[0][1] * m[1][2] - m[0][2] * m[1][1];
        const double det = m[0][0] * r0 + m[1][0] * r1 + m[2][0] * r2;
        if(det <= 0.0) return FALSE;

        float *const w = fit + 75 * (6 * pr + pc) + 25 * c;
        for(int dy = -2; dy <= 2; dy++)
          for(int dx = -2; dx <= 2; dx++)
            w[5 * (dy + 2) + dx + 2] = FCNxtrans(pr + dy, pc + dx, xtrans) == c
                                       ? (r0 + r1 * dx + r2 * dy) / det
                                       : 0.0f;
      }
    }
  }
  return TRUE;
}

/* One RGB pixel at the photosite (row, col). Its own color keeps the
   photosite's value and flag. Each other color comes from its photosites in
   the window of radius rad around the photosite, by the plane fit fit (X-Trans)
   or by their mean (Bayer, where a centered 3x3 mean equals bilinear
   interpolation), and is flagged if any of them is clipped.
   At the image edges the window moves inward until it lies inside the image.
   The photosite is then off the window's center, where a plane fit
   extrapolates and can leave the range of its values, so the mean replaces it.
   With block_flag, the own color is flagged if any photosite of that color in
   the 3x3 block around the photosite is clipped (X-Trans reduced resolution
   experiment), like the other colors are over their window.
*/
static inline void _gd_window_pixel(float *const restrict o,
                                    uint8_t *const restrict flags,
                                    const float *const restrict in,
                                    const uint32_t filters,
                                    const uint8_t (*const xtrans)[6],
                                    const float *const restrict fit,
                                    const dt_aligned_pixel_t clips,
                                    const int iwidth,
                                    const int iheight,
                                    const int row,
                                    const int col,
                                    const int rad,
                                    const gboolean block_flag)
{
  const int cy = CLAMP(row, rad, iheight - 1 - rad);
  const int cx = CLAMP(col, rad, iwidth - 1 - rad);
  const float *const w = fit && cy == row && cx == col ? fit + 75 * (6 * (row % 6) + col % 6) : NULL;

  dt_aligned_pixel_t sum = { 0.0f, 0.0f, 0.0f, 0.0f };
  dt_aligned_pixel_t cnt = { 0.0f, 0.0f, 0.0f, 0.0f };
  int clipped = 0;
  int block_clipped = 0;
  int k = 0;
  for(int y = cy - rad; y <= cy + rad; y++)
  {
    for(int x = cx - rad; x <= cx + rad; x++, k++)
    {
      const int c = fcol(y, x, filters, xtrans);
      const float v = in[(size_t)y * iwidth + x];
      sum[c] += w ? w[25 * c + k] * v : v;
      cnt[c] += 1.0f;
      if(v >= clips[c])
      {
        clipped |= 1 << c;
        // the moved window still holds the whole block: the photosite is at
        // most one off its center
        if(block_flag && abs(y - row) <= 1 && abs(x - col) <= 1) block_clipped |= 1 << c;
      }
    }
  }

  for_three_channels(c) o[c] = w ? sum[c] : sum[c] / cnt[c];
  o[3] = 0.0f;

  const int own = fcol(row, col, filters, xtrans);
  o[own] = in[(size_t)row * iwidth + col];
  clipped &= ~(1 << own);
  if(o[own] >= clips[own] || (block_clipped & (1 << own))) clipped |= 1 << own;
  *flags = clipped;
}

/* Builds the RGB pixels and their clipped flags (bits 0-2 of state) from the
   input, on a grid of width x height pixels:
   - linear raws: the input pixels as they are
   - Bayer at reduced resolution: 2x2 blocks, each channel the mean of the
     block's photosites of that color and flagged if any of them is clipped.
     A 2x2 block holds one period of the pattern, so all blocks sample the
     colors at the same positions; leftover photosites at the right and bottom
     edges join the last block of their row or column
   - Bayer at full resolution: a 3x3 window at every photosite
   - X-Trans: a 5x5 plane fit at every photosite, or at the center photosite
     of each 3x3 block at reduced resolution; block_flag, see
     _gd_window_pixel()
*/
static void _gd_to_rgb(float *const restrict rgb,
                       uint8_t *const restrict state,
                       const float *const restrict in,
                       const uint32_t filters,
                       const uint8_t (*const xtrans)[6],
                       const float *const restrict fit,
                       const dt_aligned_pixel_t clips,
                       const int iwidth,
                       const int iheight,
                       const int step,
                       const int width,
                       const int height,
                       const gboolean block_flag)
{
  if(!filters)
  {
    DT_OMP_FOR()
    for(size_t i = 0; i < (size_t)width * height; i++)
    {
      int clipped = 0;
      for(int c = 0; c < 3; c++)
        if(in[4 * i + c] >= clips[c]) clipped |= 1 << c;
      copy_pixel(rgb + 4 * i, in + 4 * i);
      state[i] = clipped;
    }
  }
  else if(step == 2)
  {
    DT_OMP_FOR(collapse(2))
    for(int row = 0; row < height; row++)
    {
      for(int col = 0; col < width; col++)
      {
        const int y1 = row == height - 1 ? iheight : 2 * row + 2;
        const int x1 = col == width - 1 ? iwidth : 2 * col + 2;
        dt_aligned_pixel_t sum = { 0.0f, 0.0f, 0.0f, 0.0f };
        dt_aligned_pixel_t cnt = { 0.0f, 0.0f, 0.0f, 0.0f };
        int clipped = 0;
        for(int y = 2 * row; y < y1; y++)
        {
          for(int x = 2 * col; x < x1; x++)
          {
            const int c = FC(y, x, filters);
            const float v = in[(size_t)y * iwidth + x];
            sum[c] += v;
            cnt[c] += 1.0f;
            if(v >= clips[c]) clipped |= 1 << c;
          }
        }
        const size_t i = (size_t)row * width + col;
        for_three_channels(c) rgb[4 * i + c] = sum[c] / cnt[c];
        rgb[4 * i + 3] = 0.0f;
        state[i] = clipped;
      }
    }
  }
  else
  {
    const int off = step / 2;
    const int rad = filters == 9u ? 2 : 1;
    DT_OMP_FOR(collapse(2))
    for(int row = 0; row < height; row++)
    {
      for(int col = 0; col < width; col++)
      {
        const size_t i = (size_t)row * width + col;
        _gd_window_pixel(rgb + 4 * i, state + i, in, filters, xtrans, fit, clips,
                         iwidth, iheight, step * row + off, step * col + off, rad, block_flag);
      }
    }
  }
}

/* Experiment: a fully clipped pixel takes the hue estimate, scaled just enough
   that no channel falls below its solution or its input value v.
*/
static inline void _gd_hue_fill(float *const u,
                                const float *const v,
                                const float *const rho[3],
                                const size_t i)
{
  dt_aligned_pixel_t r = { 1.0f, 1.0f, 1.0f, 1.0f };
  float scale = 0.0f;
  for(int c = 0; c < 3; c++)
  {
    r[c] = fmaxf(rho[c][i], GD_RHO_MIN);
    scale = fmaxf(scale, fmaxf(u[c], v[c]) / r[c]);
  }
  for(int c = 0; c < 3; c++) u[c] = scale * r[c];
}

/* Write-back experiment: scales the flagged channels of a pixel together, by
   the smallest factor that lifts each of them to at least its input value v.
   The basic write-back lifts each channel on its own, which changes the hue
   wherever a solution falls below its input.
*/
static inline void _gd_joint_scale(float *const u,
                                   const float *const v,
                                   const uint8_t flags)
{
  float scale = 1.0f;
  for(int c = 0; c < 3; c++)
    if((flags & (1 << c)) && u[c] > 0.0f) scale = fmaxf(scale, v[c] / u[c]);
  for(int c = 0; c < 3; c++)
    if(flags & (1 << c)) u[c] *= scale;
}

// sorts the channels by count, ascending or descending
static void _gd_channel_order(int order[3],
                              const size_t count[3],
                              const gboolean descending)
{
  for(int c = 0; c < 3; c++) order[c] = c;
  for(int a = 0; a < 2; a++)
    for(int b = a + 1; b < 3; b++)
      if(descending ? count[order[b]] > count[order[a]] : count[order[b]] < count[order[a]])
      {
        const int t = order[a];
        order[a] = order[b];
        order[b] = t;
      }
}

// log slope at pixel i between its neighbors lo and hi (-1 outside the image),
// from differences between pixels where the channel (bit) is unflagged only:
// a difference with a clipped value says nothing about the slope
static inline float _gd_log_slope(const float *const lg,
                                  const uint8_t *const state,
                                  const uint8_t bit,
                                  const int i,
                                  const int lo,
                                  const int hi)
{
  const gboolean l = lo >= 0 && !(state[lo] & bit);
  const gboolean h = hi >= 0 && !(state[hi] & bit);
  if(l && h) return 0.5f * (lg[hi] - lg[lo]);
  if(h) return lg[hi] - lg[i];
  if(l) return lg[i] - lg[lo];
  return 0.0f;
}

/* Experiment, paper 3.4: inside the fully clipped region, all channels are
   flat, so no channel guides another there. This gives one channel k a smooth
   bump instead: a Laplace solve spreads the log gradients of k on the border
   of its clipped region over the region (eq. 9), and a Poisson solve with the
   log values on the border integrates them (eq. 10). In 1D that fits a
   Gaussian to the slopes at the border. The filled values then serve as
   references for the other channels.
   The paper requires one channel to be clipped exactly where all are. Here
   each connected clipped region of a channel qualifies by itself, if at most
   GD_FILL_TOL of its pixels have another channel unflagged. Channels are tried
   from the smallest clipped area up, so the one that clips last goes first,
   and a region that overlaps one filled before is skipped. The neighbors of a
   whole connected region are unflagged in k, so the border data are measured.
   Sets bit k of filled for the filled pixels, nfill, iter and peak (largest
   filled value relative to the clipping level) per channel. map, pos, x and
   div are scratch arrays of the solver, sized for all pixels with a flagged
   channel. Returns FALSE if out of memory.
*/
static gboolean _gd_fill_fully_clipped(float *const rgb,
                                       const uint8_t *const state,
                                       uint8_t *const filled,
                                       int *const map,
                                       int *const pos,
                                       float *const x,
                                       float *const div,
                                       const dt_aligned_pixel_t inv_clip,
                                       const int width,
                                       const int height,
                                       size_t nfill[3],
                                       int iter[3],
                                       float peak[3])
{
  const size_t npix = (size_t)width * height;
  int *const queue = dt_alloc_align_int(npix);
  uint8_t *const seen = dt_alloc_align_uint8(npix);
  float *const lg = dt_alloc_align_float(npix);
  float *const gx = dt_alloc_align_float(npix);
  float *const gy = dt_alloc_align_float(npix);
  gboolean ok = FALSE;
  if(!queue || !seen || !lg || !gx || !gy) goto cleanup;

  size_t count[3] = { 0, 0, 0 };
  for(size_t i = 0; i < npix; i++)
    for(int c = 0; c < 3; c++) count[c] += (state[i] >> c) & 1;
  int order[3];
  _gd_channel_order(order, count, FALSE);

  for(int o = 0; o < 3; o++)
  {
    const int k = order[o];
    const uint8_t bit = 1 << k;

    // connected clipped regions of k, 4-neighborhood, by flood fill
    memset(seen, 0, npix);
    for(size_t s = 0; s < npix; s++)
    {
      if(!(state[s] & bit) || seen[s]) continue;
      size_t head = 0, tail = 0;
      queue[tail++] = (int)s;
      seen[s] = 1;
      size_t partial = 0;
      gboolean overlap = FALSE;
      while(head < tail)
      {
        const int p = queue[head++];
        partial += state[p] != 7;
        overlap |= filled[p] != 0;
        int nb[4];
        _gd_neighbors(p, width, height, nb);
        for(int m = 0; m < 4; m++)
        {
          if(nb[m] < 0 || !(state[nb[m]] & bit) || seen[nb[m]]) continue;
          seen[nb[m]] = 1;
          queue[tail++] = nb[m];
        }
      }
      if(overlap || partial > GD_FILL_TOL * tail) continue;
      for(size_t m = 0; m < tail; m++) filled[queue[m]] |= bit;
      nfill[k] += tail;
    }
    if(!nfill[k]) continue;

    const size_t n = _gd_index_pixels(map, pos, filled, bit, NULL, npix);
    const float floor = GD_LOG_FLOOR / inv_clip[k];
    DT_OMP_FOR()
    for(size_t i = 0; i < npix; i++)
      lg[i] = logf(fmaxf(rgb[4 * i + k], floor));

    // boundary values of eq. 9: the log gradients on the border
    DT_OMP_FOR()
    for(int i = 0; i < (int)npix; i++)
    {
      if(map[i] >= 0) continue;
      int nb[4];
      _gd_neighbors(i, width, height, nb);
      gboolean border = FALSE;
      for(int m = 0; m < 4; m++) border |= nb[m] >= 0 && map[nb[m]] >= 0;
      if(!border) continue;
      gx[i] = _gd_log_slope(lg, state, bit, i, nb[0], nb[1]);
      gy[i] = _gd_log_slope(lg, state, bit, i, nb[2], nb[3]);
    }

    float *const g[2] = { gx, gy };
    for(int d = 0; d < 2; d++)
    {
      memset(x, 0, sizeof(float) * n);
      const int it = _gd_solve_poisson(x, NULL, g[d], 1, map, pos, n, width, height);
      if(it < 0) goto cleanup;
      iter[k] += it;
      for(size_t m = 0; m < n; m++) g[d][pos[m]] = x[m];
    }

    // eq. 10: the target of u_q - u_p on each edge is the mean gradient of
    // its ends, positive towards the right and the bottom
    DT_OMP_FOR()
    for(size_t m = 0; m < n; m++)
    {
      const int p = pos[m];
      int nb[4];
      _gd_neighbors(p, width, height, nb);
      float sum = 0.0f;
      if(nb[0] >= 0) sum += 0.5f * (gx[p] + gx[nb[0]]);
      if(nb[1] >= 0) sum -= 0.5f * (gx[p] + gx[nb[1]]);
      if(nb[2] >= 0) sum += 0.5f * (gy[p] + gy[nb[2]]);
      if(nb[3] >= 0) sum -= 0.5f * (gy[p] + gy[nb[3]]);
      div[m] = sum;
      x[m] = lg[p];
    }
    const int it = _gd_solve_poisson(x, div, lg, 1, map, pos, n, width, height);
    if(it < 0) goto cleanup;
    iter[k] += it;

    // steep border slopes over a wide region would integrate to huge values
    const float lmax = logf(GD_FILL_MAX / inv_clip[k]);
    float xmax = -INFINITY;
    DT_OMP_FOR(reduction(max : xmax))
    for(size_t m = 0; m < n; m++)
    {
      rgb[4 * pos[m] + k] = expf(fminf(x[m], lmax));
      xmax = fmaxf(xmax, x[m]);
    }
    peak[k] = expf(fminf(xmax, lmax)) * inv_clip[k];
  }
  ok = TRUE;

cleanup:
  dt_free_align(queue);
  dt_free_align(seen);
  dt_free_align(lg);
  dt_free_align(gx);
  dt_free_align(gy);
  return ok;
}

/* Restores the flagged channels of the RGB pixels in place, steps 1 to 4 of
   the header. The flags stay as the conversion set them, and no solve reads
   a flagged value of another channel unless it is restored already (see
   _gd_usable()), so each solve can write its result straight into the RGB
   pixels. The write-back and experiment parameters select the variants of
   _gd_joint_scale(), _gd_hue_fill() and _gd_fill_fully_clipped(), the neutral
   hue and the solved channels as references.
   Returns the number of pixels with a flagged channel, 0 if there is none or
   no unflagged border, -1 if out of memory.
*/
static int _gd_solve(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     float *const rgb,
                     const uint8_t *const state,
                     const dt_aligned_pixel_t inv_clip,
                     const int width,
                     const int height,
                     const float sigma_s,
                     const dt_iop_highlights_writeback_t writeback,
                     const dt_iop_highlights_experiment_t experiment)
{
  const size_t npix = (size_t)width * height;

  size_t nclipped = 0;
  DT_OMP_FOR(reduction(+ : nclipped))
  for(size_t i = 0; i < npix; i++)
    nclipped += state[i] ? 1 : 0;
  if(!nclipped) return 0;

  const gboolean hue_fill = experiment == DT_HIGHLIGHTS_EXPERIMENT_HUE_FILL;
  const gboolean smooth_fill = experiment == DT_HIGHLIGHTS_EXPERIMENT_SMOOTH_FILL;
  const gboolean solved_refs = experiment == DT_HIGHLIGHTS_EXPERIMENT_SOLVED_REFS;
  const gboolean keep_input = hue_fill || writeback != DT_HIGHLIGHTS_WRITEBACK_BASIC;

  uint8_t *const border = dt_alloc_align_uint8(npix);
  float *const rhobuf = dt_alloc_align_float(3 * npix);
  int *const map = dt_alloc_align_int(npix);
  int *const pos = dt_alloc_align_int(nclipped);
  float *const x = dt_alloc_align_float(nclipped);
  float *const div = dt_alloc_align_float(nclipped);
  // the converted input of the pixels in U, which the solves overwrite
  float *const uin = keep_input ? dt_alloc_align_float(3 * nclipped) : NULL;
  uint8_t *const filled = smooth_fill ? dt_alloc_align_uint8(npix) : NULL;
  int result = -1;
  if(!border || !rhobuf || !map || !pos || !x || !div
     || (keep_input && !uin) || (smooth_fill && !filled))
    goto cleanup;

  size_t nborder = 0;
  DT_OMP_FOR(reduction(+ : nborder))
  for(int i = 0; i < (int)npix; i++)
  {
    int nb[4];
    _gd_neighbors(i, width, height, nb);
    gboolean b = FALSE;
    if(!state[i])
      for(int m = 0; m < 4; m++)
        b |= nb[m] >= 0 && state[nb[m]];
    border[i] = b;
    nborder += b ? 1 : 0;
  }
  if(!nborder)
  {
    result = 0;
    goto cleanup;
  }

  float *const rho[3] = { rhobuf, rhobuf + npix, rhobuf + 2 * npix };
  const float *const crho[3] = { rho[0], rho[1], rho[2] };

  int hue_iter[3] = { 0, 0, 0 };
  const size_t nu = _gd_index_pixels(map, pos, state, 7, NULL, npix);
  if(experiment == DT_HIGHLIGHTS_EXPERIMENT_NEUTRAL_HUE)
  {
    // the same value in all channels is white under the white balance
    DT_OMP_FOR()
    for(size_t i = 0; i < 3 * npix; i++)
      rhobuf[i] = 1.0f;
  }
  else
  {
    // hue estimate over U, paper 3.2
    _gd_smooth_border(rho, rgb, border, width, height, inv_clip, sigma_s);
    if(!_gd_pullpush_fill(rho, border, state, width, height)) goto cleanup;

    for(int c = 0; c < 3; c++)
    {
      for(size_t k = 0; k < nu; k++)
        x[k] = rho[c][pos[k]];
      hue_iter[c] = _gd_solve_poisson(x, NULL, rho[c], 1, map, pos, nu, width, height);
      if(hue_iter[c] < 0) goto cleanup;
      for(size_t k = 0; k < nu; k++)
        rho[c][pos[k]] = x[k];
    }
  }

  if(keep_input)
  {
    DT_OMP_FOR()
    for(size_t k = 0; k < nu; k++)
      for(int c = 0; c < 3; c++) uin[3 * k + c] = rgb[4 * pos[k] + c];
  }

  size_t nfill[3] = { 0, 0, 0 };
  int fill_iter[3] = { 0, 0, 0 };
  float peak[3] = { 0.0f, 0.0f, 0.0f };
  if(smooth_fill)
  {
    memset(filled, 0, npix);
    if(!_gd_fill_fully_clipped(rgb, state, filled, map, pos, x, div, inv_clip,
                               width, height, nfill, fill_iter, peak))
      goto cleanup;
  }

  // red, green, blue; with solved channels as references, the channel with the
  // largest clipped region goes first: it has the widest band of partly
  // clipped pixels, where unflagged references guide it
  int order[3] = { 0, 1, 2 };
  if(solved_refs)
  {
    size_t count[3] = { 0, 0, 0 };
    for(size_t i = 0; i < npix; i++)
      for(int c = 0; c < 3; c++) count[c] += (state[i] >> c) & 1;
    _gd_channel_order(order, count, TRUE);
  }

  // per channel: guidance field and Poisson solve over its clipped region
  uint8_t solved = 0;
  int chan_iter[3] = { 0, 0, 0 };
  for(int o = 0; o < 3; o++)
  {
    const int j = order[o];
    const size_t n = _gd_index_pixels(map, pos, state, 1 << j, filled, npix);
    if(n)
    {
      DT_OMP_FOR()
      for(size_t k = 0; k < n; k++)
      {
        const int p = pos[k];
        int nb[4];
        _gd_neighbors(p, width, height, nb);
        float sum = 0.0f;
        for(int m = 0; m < 4; m++)
          if(nb[m] >= 0) sum -= _gd_edge_guidance(rgb, state, filled, solved, crho, inv_clip, p, nb[m], j);
        div[k] = sum;

        // start from the spatial estimate of paper eq. 4, extended to
        // several reference channels like eq. 7; flagged references are
        // left out as in _gd_edge_guidance()
        const uint8_t usable = _gd_usable(state, filled, solved, p);
        float num = 0.0f;
        float den = 0.0f;
        for(int c = 0; c < 3; c++)
        {
          if(c == j || !(usable & (1 << c))) continue;
          const float w = _gd_weight(rgb[4 * p + c] * inv_clip[c]);
          num += w * _gd_ratio(crho, p, j, c) * rgb[4 * p + c];
          den += w;
        }
        x[k] = den > 0.0f ? fmaxf(rgb[4 * p + j], num / den) : rgb[4 * p + j];
      }

      chan_iter[j] = _gd_solve_poisson(x, div, rgb + j, 4, map, pos, n, width, height);
      if(chan_iter[j] < 0) goto cleanup;

      DT_OMP_FOR()
      for(size_t k = 0; k < n; k++)
        rgb[4 * pos[k] + j] = x[k];
    }
    if(solved_refs) solved |= 1 << j;
  }

  if(keep_input)
  {
    const gboolean joint_all = writeback == DT_HIGHLIGHTS_WRITEBACK_JOINT_ALL;
    const gboolean joint_full = writeback == DT_HIGHLIGHTS_WRITEBACK_JOINT_FULL;
    _gd_index_pixels(map, pos, state, 7, NULL, npix);
    DT_OMP_FOR()
    for(size_t k = 0; k < nu; k++)
    {
      const int p = pos[k];
      float *const u = rgb + 4 * p;
      const float *const v = uin + 3 * k;
      if(hue_fill && state[p] == 7) _gd_hue_fill(u, v, crho, p);
      if(joint_all || (joint_full && state[p] == 7)) _gd_joint_scale(u, v, state[p]);
    }
  }

  result = nclipped;
  dt_print_pipe(DT_DEBUG_PERF,
                "gradient domain", piece->pipe, self, DT_DEVICE_CPU, NULL, NULL,
                "%dx%d grid: %zu clipped, %zu border pixels; CG iterations hue %d/%d/%d, channels %d/%d/%d; "
                "write-back %d, experiment %d; filled %zu/%zu/%zu in %d/%d/%d iterations, peak %.1f/%.1f/%.1f x clip",
                width, height, nclipped, nborder,
                hue_iter[0], hue_iter[1], hue_iter[2], chan_iter[0], chan_iter[1], chan_iter[2],
                writeback, experiment, nfill[0], nfill[1], nfill[2],
                fill_iter[0], fill_iter[1], fill_iter[2], peak[0], peak[1], peak[2]);

cleanup:
  dt_free_align(border);
  dt_free_align(rhobuf);
  dt_free_align(map);
  dt_free_align(pos);
  dt_free_align(x);
  dt_free_align(div);
  dt_free_align(uin);
  dt_free_align(filled);
  return result;
}

/* Clipped photosites take the larger of their value and the solution of
   their color, since a clipped photosite was at least as bright as its
   clipping level; all others keep their value. On a grid coarser than the
   photosites, the solution is interpolated bilinearly between the grid
   points, which sit at the centers of their step x step blocks; beyond the
   outermost grid points, the nearest one applies.
*/
static void _gd_write_back(float *const restrict out,
                           const float *const restrict in,
                           const float *const restrict rgb,
                           const uint32_t filters,
                           const uint8_t (*const xtrans)[6],
                           const dt_aligned_pixel_t clips,
                           const int step,
                           const int width,
                           const int height,
                           const dt_iop_roi_t *const roi_in,
                           const dt_iop_roi_t *const roi_out)
{
  const int dx = roi_out->x - roi_in->x;
  const int dy = roi_out->y - roi_in->y;
  const float off = 0.5f * (step - 1);

  DT_OMP_FOR()
  for(int row = 0; row < roi_out->height; row++)
  {
    const int irow = row + dy;
    if(irow < 0 || irow >= roi_in->height) continue;
    const float gy = CLAMPF((irow - off) / step, 0.0f, height - 1);
    const int y0 = (int)gy;
    const int y1 = MIN(y0 + 1, height - 1);
    const float fy = gy - y0;

    for(int col = 0; col < roi_out->width; col++)
    {
      const int icol = col + dx;
      if(icol < 0 || icol >= roi_in->width) continue;
      const size_t ip = (size_t)irow * roi_in->width + icol;
      const size_t op = (size_t)row * roi_out->width + col;

      if(!filters)
      {
        // linear raws are solved on their own pixels
        for_three_channels(c)
          if(in[4 * ip + c] >= clips[c])
            out[4 * op + c] = fmaxf(in[4 * ip + c], rgb[4 * ip + c]);
        continue;
      }

      const int c = fcol(irow, icol, filters, xtrans);
      if(in[ip] < clips[c]) continue;
      const float gx = CLAMPF((icol - off) / step, 0.0f, width - 1);
      const int x0 = (int)gx;
      const int x1 = MIN(x0 + 1, width - 1);
      const float fx = gx - x0;
      const float top = (1.0f - fx) * rgb[4 * ((size_t)y0 * width + x0) + c]
                               + fx * rgb[4 * ((size_t)y0 * width + x1) + c];
      const float bottom = (1.0f - fx) * rgb[4 * ((size_t)y1 * width + x0) + c]
                                  + fx * rgb[4 * ((size_t)y1 * width + x1) + c];
      out[op] = fmaxf(in[ip], (1.0f - fy) * top + fy * bottom);
    }
  }
}

/* The gradient domain method. Expects the whole image in roi_in at scale 1;
   writes roi_out, which must lie inside it at the same scale. The output
   stays a copy of the input if the method cannot run.
*/
static void _process_gradient(dt_iop_module_t *self,
                              dt_dev_pixelpipe_iop_t *piece,
                              const float *const input,
                              float *const output,
                              const dt_iop_roi_t *const roi_in,
                              const dt_iop_roi_t *const roi_out,
                              const float clipval)
{
  const dt_iop_highlights_data_t *d = piece->data;
  const uint32_t filters = piece->filters;
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->xtrans;
  const int iwidth = roi_in->width;
  const int iheight = roi_in->height;

  dt_iop_copy_image_roi(output, input, filters ? 1 : 4, roi_in, roi_out);

  const float *const pmax = piece->pipe->dsc.processed_maximum;
  const dt_aligned_pixel_t clips = { clipval * pmax[0], clipval * pmax[1], clipval * pmax[2], 1.0f };
  // the clipping threshold goes down to 0, and the confidence weights
  // divide by it
  if(clips[0] <= 0.0f || clips[1] <= 0.0f || clips[2] <= 0.0f) return;
  const dt_aligned_pixel_t inv_clip = { 1.0f / clips[0], 1.0f / clips[1], 1.0f / clips[2], 1.0f };

  const int step = _gd_grid_step(filters, d->full_resolution);
  // the image must hold one block, 3x3 window (Bayer) or 5x5 window (X-Trans)
  const int minsize = !filters ? 1 : filters == 9u ? 5 : step == 2 ? 2 : 3;
  if(iwidth < minsize || iheight < minsize) return;

  if(filters && filters != 9u)
  {
    // the Bayer means need all three colors in each 2x2 block
    int colors = 0;
    for(int k = 0; k < 4; k++) colors |= 1 << FC(k >> 1, k & 1, filters);
    if(colors != 7) return;
  }

  float fit[GD_FIT_SIZE];
  if(filters == 9u && !_gd_xtrans_fit(fit, xtrans))
  {
    dt_print_pipe(DT_DEBUG_ALWAYS,
                  "gradient domain", piece->pipe, self, DT_DEVICE_CPU, roi_in, roi_out,
                  "unexpected X-Trans pattern, image left unchanged");
    return;
  }

  const int width = iwidth / step;
  const int height = iheight / step;
  const size_t npix = (size_t)width * height;
  float *const rgb = dt_alloc_align_float(4 * npix);
  uint8_t *const state = dt_alloc_align_uint8(npix);
  int solved = -1;
  if(rgb && state)
  {
    const gboolean block_flag = d->experiment == DT_HIGHLIGHTS_EXPERIMENT_XTRANS_FLAG && step == 3;
    _gd_to_rgb(rgb, state, input, filters, xtrans, filters == 9u ? fit : NULL, clips,
               iwidth, iheight, step, width, height, block_flag);

    // the preview pipe gets a downscaled image, so iscale is not 1 there
    const float sigma_s = fmaxf(0.5f, GD_BORDER_SIGMA_S * roi_in->scale / piece->iscale / step);
    solved = _gd_solve(self, piece, rgb, state, inv_clip, width, height, sigma_s,
                       d->writeback, d->experiment);
  }

  if(solved > 0)
    _gd_write_back(output, input, rgb, filters, xtrans, clips, step, width, height, roi_in, roi_out);
  else if(solved < 0)
    dt_print_pipe(DT_DEBUG_ALWAYS,
                  "gradient domain", piece->pipe, self, DT_DEVICE_CPU, roi_in, roi_out,
                  "out of memory, image left unchanged");

  dt_free_align(rgb);
  dt_free_align(state);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
