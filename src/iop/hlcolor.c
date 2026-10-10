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

   The method relies on the camera channels clipping independently, so it
   works on demosaiced, white-balanced camera RGB, before lens correction
   and exposure change the clipping level of a channel.

   1. per-channel clip masks; pixels with any channel clipped form the
      region U, unclipped pixels 4-adjacent to U form its border B
   2. hue estimate rho: the colors on B, smoothed along B, interpolated
      over U by a Laplace solve (paper 3.2)
   3. guidance field for each channel inside its own clipped region: the
      gradients of the other channels scaled by the hue ratio, weighted by
      how reliable each value is (paper eq. 7, 8)
   4. one Poisson solve per channel over its clipped region, with the
      unclipped neighbors as boundary values (paper eq. 6)

   Regions with all channels clipped get no usable guidance, so they come out
   flat, filled from the solutions around them. The write-back and experiment
   parameters select the same experimental variants as the gradient domain
   method of highlight reconstruction, src/iop/hlreconstruct/gradient.c.
*/

#include "bauhaus/bauhaus.h"
#include "common/imagebuf.h"
#include "common/math.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE_INTROSPECTION(1, dt_iop_hlcolor_params_t)

typedef enum dt_iop_hlcolor_writeback_t
{
  DT_HLCOLOR_WRITEBACK_BASIC = 0,      // $DESCRIPTION: "basic"
  DT_HLCOLOR_WRITEBACK_JOINT_FULL = 1, // $DESCRIPTION: "joint, fully clipped"
  DT_HLCOLOR_WRITEBACK_JOINT_ALL = 2,  // $DESCRIPTION: "joint, all clipped"
} dt_iop_hlcolor_writeback_t;

// numbered like dt_iop_highlights_experiment_t, without the X-Trans one
typedef enum dt_iop_hlcolor_experiment_t
{
  DT_HLCOLOR_EXPERIMENT_BASIC = 0,       // $DESCRIPTION: "basic"
  DT_HLCOLOR_EXPERIMENT_NEUTRAL_HUE = 1, // $DESCRIPTION: "neutral hue"
  DT_HLCOLOR_EXPERIMENT_HUE_FILL = 2,    // $DESCRIPTION: "hue estimate in fully clipped"
  DT_HLCOLOR_EXPERIMENT_SOLVED_REFS = 3, // $DESCRIPTION: "solved channels as references"
  DT_HLCOLOR_EXPERIMENT_SMOOTH_FILL = 4, // $DESCRIPTION: "smooth fill (paper 3.4)"
} dt_iop_hlcolor_experiment_t;

typedef struct dt_iop_hlcolor_params_t
{
  float clip; // $MIN: 0.5 $MAX: 1.0 $DEFAULT: 0.98 $DESCRIPTION: "clipping threshold"
  dt_iop_hlcolor_writeback_t writeback;   // $DEFAULT: DT_HLCOLOR_WRITEBACK_BASIC $DESCRIPTION: "write-back"
  dt_iop_hlcolor_experiment_t experiment; // $DEFAULT: DT_HLCOLOR_EXPERIMENT_BASIC $DESCRIPTION: "experiment"
} dt_iop_hlcolor_params_t;

// dt_iop_gui_update() syncs the widgets with the params only for modules
// that have gui data
typedef struct dt_iop_hlcolor_gui_data_t
{
  GtkWidget *clip, *writeback, *experiment;
} dt_iop_hlcolor_gui_data_t;

// border smoothing of paper 3.2: spatial sigma in full-resolution pixels,
// range sigma on colors normalized to the clipping level
#define HLC_BORDER_SIGMA_S 5.0f
#define HLC_BORDER_SIGMA_R 0.25f
// peak and floor of the confidence weight, paper eq. 8
#define HLC_WEIGHT_PEAK 0.65f
#define HLC_WEIGHT_EPS 1e-3f
// keeps hue ratios finite where the border is black in a channel
#define HLC_RHO_MIN 1e-4f
#define HLC_CG_TOL 1e-5
#define HLC_CG_MAXITER 5000
#define HLC_MAX_LEVELS 32
// smooth fill of paper 3.4: share of a channel's clipped region that may have
// another channel unclipped, floor of the values before the log and cap of the
// filled values, both relative to the clipping level
#define HLC_FILL_TOL 0.05f
#define HLC_LOG_FLOOR 1e-3f
#define HLC_FILL_MAX 64.0f

const char *name()
{
  return C_("modulename", "highlight color restoration");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("restore the color of clipped highlights from the unclipped channels"),
                                      _("corrective"),
                                      _("linear, RGB, scene-referred"),
                                      _("linear, RGB"),
                                      _("linear, RGB, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_ONE_INSTANCE;
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out,
                   dt_iop_roi_t *roi_in)
{
  // the hue estimate and the Poisson solves need every clipped region whole,
  // with its border, so always process the full image at this scale
  *roi_in = *roi_out;
  roi_in->x = 0;
  roi_in->y = 0;
  roi_in->width = (int)roundf((float)piece->buf_in.width * roi_out->scale);
  roi_in->height = (int)roundf((float)piece->buf_in.height * roi_out->scale);
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, p1, self->params_size);

  // the per-channel clipping levels are only known for raw input
  if(!dt_image_is_raw(&pipe->image))
    piece->enabled = FALSE;
}

// confidence in a channel value normalized to its clipping level: low near
// black, where noise dominates, and near clipping, paper eq. 8
static inline float _weight(const float v)
{
  const float n = CLAMPF(v, 0.0f, 1.0f);
  const float x = n <= HLC_WEIGHT_PEAK
    ? n / HLC_WEIGHT_PEAK
    : (1.0f - n) / (1.0f - HLC_WEIGHT_PEAK);
  // the paper prints 3x^3 - 2x^2, which is negative below x = 2/3; the
  // zero slopes it asks for at 0, 1 and the peak are those of smoothstep
  return x * x * (3.0f - 2.0f * x) + HLC_WEIGHT_EPS;
}

static inline float _ratio(const float *const rho[3],
                           const size_t i,
                           const int j,
                           const int k)
{
  return fmaxf(rho[j][i], 0.0f) / fmaxf(rho[k][i], HLC_RHO_MIN);
}

// pixel indices of the 4-neighbors, -1 outside the image
static inline void _neighbors(const int i,
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
static size_t _index_pixels(int *const map,
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
static void _laplacian(float *const restrict out,
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
    _neighbors(pos[k], width, height, nb);
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
static int _solve_poisson(float *const restrict x,
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
    _neighbors(pos[k], width, height, nb);
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

  _laplacian(ap, x, map, pos, n, width, height);
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

  const double stop = HLC_CG_TOL * HLC_CG_TOL * fmax(bb, rr);
  iter = 0;
  while(rr > stop && iter < HLC_CG_MAXITER)
  {
    _laplacian(ap, p, map, pos, n, width, height);
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
static gboolean _pullpush_fill(float *const plane[3],
                           const uint8_t *const known,
                           const uint8_t *const fill,
                           const int width,
                           const int height)
{
  float *lev[HLC_MAX_LEVELS] = { NULL };
  int lw[HLC_MAX_LEVELS], lh[HLC_MAX_LEVELS];
  int nlev = 0;
  int w = width;
  int h = height;

  while((w > 1 || h > 1) && nlev < HLC_MAX_LEVELS)
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

/* Boundary cleanup of paper 3.2: noise and demosaicing artifacts on the
   border would turn into streaks of the interpolated hue. A bilateral filter
   that only collects border pixels smooths along the border line, like the
   1D filter of the paper, without needing the border as an ordered curve.
*/
static void _smooth_border(float *const rho[3],
                           const float *const in,
                           const uint8_t *const border,
                           const int width,
                           const int height,
                           const dt_aligned_pixel_t inv_clip,
                           const float sigma_s)
{
  const int rad = (int)ceilf(2.0f * sigma_s);
  const float ws = -0.5f / sqf(sigma_s);
  const float wr = -0.5f / sqf(HLC_BORDER_SIGMA_R);

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

// channels that can serve as references at pixel i under the skip rule: the
// unclipped ones, and clipped ones whose values are already restored, by an
// earlier solve (solved) or by the smooth fill (filled, may be NULL)
static inline uint8_t _usable(const uint8_t *const state,
                              const uint8_t *const filled,
                              const uint8_t solved,
                              const size_t i)
{
  return (~state[i] | solved | (filled ? filled[i] : 0)) & 7;
}

// target value of u_j(q) - u_j(p) on the edge p-q, from the other channels,
// paper eq. 7, with the weights min-filtered over the edge as in paper 3.5;
// only the channels in usable serve as references
static inline float _edge_guidance(const float *const in,
                                   const float *const rho[3],
                                   const dt_aligned_pixel_t inv_clip,
                                   const size_t p,
                                   const size_t q,
                                   const int j,
                                   const uint8_t usable)
{
  float num = 0.0f;
  float den = 0.0f;
  for(int k = 0; k < 3; k++)
  {
    if(k == j || !(usable & (1 << k))) continue;
    const float w = fminf(_weight(in[4 * p + k] * inv_clip[k]),
                          _weight(in[4 * q + k] * inv_clip[k]));
    const float r = 0.5f * (_ratio(rho, p, j, k) + _ratio(rho, q, j, k));
    num += w * r * (in[4 * q + k] - in[4 * p + k]);
    den += w;
  }
  return den > 0.0f ? num / den : 0.0f;
}

// experiment: a fully clipped pixel takes the hue estimate, scaled just
// enough that no channel falls below its solution or its input value v
static inline void _hue_fill(float *const u,
                             const float *const v,
                             const float *const rho[3],
                             const size_t i)
{
  dt_aligned_pixel_t r = { 1.0f, 1.0f, 1.0f, 1.0f };
  float scale = 0.0f;
  for(int c = 0; c < 3; c++)
  {
    r[c] = fmaxf(rho[c][i], HLC_RHO_MIN);
    scale = fmaxf(scale, fmaxf(u[c], v[c]) / r[c]);
  }
  for(int c = 0; c < 3; c++) u[c] = scale * r[c];
}

// write-back experiment: scales the clipped channels of a pixel together, by
// the smallest factor that lifts each of them to at least its input value v;
// the basic write-back lifts each channel on its own, which changes the hue
static inline void _joint_scale(float *const u,
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
static void _channel_order(int order[3],
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
// from differences between pixels where the channel (bit) is unclipped only
static inline float _log_slope(const float *const lg,
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

/* Experiment, paper 3.4: one channel k gets a smooth bump in the fully clipped
   region, from its log gradients on the region's border (eq. 9 and 10), and
   then guides the other channels there. A connected clipped region of k
   qualifies if at most HLC_FILL_TOL of its pixels have another channel
   unclipped; channels are tried from the smallest clipped area up, and a
   region that overlaps one filled before is skipped. Same as
   _gd_fill_fully_clipped() in src/iop/hlreconstruct/gradient.c, including
   the outputs nfill, iter and peak. Returns FALSE if out of memory.
*/
static gboolean _fill_fully_clipped(float *const rgb,
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
  _channel_order(order, count, FALSE);

  for(int o = 0; o < 3; o++)
  {
    const int k = order[o];
    const uint8_t bit = 1 << k;

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
        _neighbors(p, width, height, nb);
        for(int m = 0; m < 4; m++)
        {
          if(nb[m] < 0 || !(state[nb[m]] & bit) || seen[nb[m]]) continue;
          seen[nb[m]] = 1;
          queue[tail++] = nb[m];
        }
      }
      if(overlap || partial > HLC_FILL_TOL * tail) continue;
      for(size_t m = 0; m < tail; m++) filled[queue[m]] |= bit;
      nfill[k] += tail;
    }
    if(!nfill[k]) continue;

    const size_t n = _index_pixels(map, pos, filled, bit, NULL, npix);
    const float floor = HLC_LOG_FLOOR / inv_clip[k];
    DT_OMP_FOR()
    for(size_t i = 0; i < npix; i++)
      lg[i] = logf(fmaxf(rgb[4 * i + k], floor));

    DT_OMP_FOR()
    for(int i = 0; i < (int)npix; i++)
    {
      if(map[i] >= 0) continue;
      int nb[4];
      _neighbors(i, width, height, nb);
      gboolean border = FALSE;
      for(int m = 0; m < 4; m++) border |= nb[m] >= 0 && map[nb[m]] >= 0;
      if(!border) continue;
      gx[i] = _log_slope(lg, state, bit, i, nb[0], nb[1]);
      gy[i] = _log_slope(lg, state, bit, i, nb[2], nb[3]);
    }

    float *const g[2] = { gx, gy };
    for(int d = 0; d < 2; d++)
    {
      memset(x, 0, sizeof(float) * n);
      const int it = _solve_poisson(x, NULL, g[d], 1, map, pos, n, width, height);
      if(it < 0) goto cleanup;
      iter[k] += it;
      for(size_t m = 0; m < n; m++) g[d][pos[m]] = x[m];
    }

    DT_OMP_FOR()
    for(size_t m = 0; m < n; m++)
    {
      const int p = pos[m];
      int nb[4];
      _neighbors(p, width, height, nb);
      float sum = 0.0f;
      if(nb[0] >= 0) sum += 0.5f * (gx[p] + gx[nb[0]]);
      if(nb[1] >= 0) sum -= 0.5f * (gx[p] + gx[nb[1]]);
      if(nb[2] >= 0) sum += 0.5f * (gy[p] + gy[nb[2]]);
      if(nb[3] >= 0) sum -= 0.5f * (gy[p] + gy[nb[3]]);
      div[m] = sum;
      x[m] = lg[p];
    }
    const int it = _solve_poisson(x, div, lg, 1, map, pos, n, width, height);
    if(it < 0) goto cleanup;
    iter[k] += it;

    const float lmax = logf(HLC_FILL_MAX / inv_clip[k]);
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

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4, self, piece->colors, ivoid, ovoid, roi_in, roi_out))
    return;

  const dt_iop_hlcolor_params_t *d = piece->data;
  const float *const in = ivoid;
  float *const out = ovoid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const size_t npix = (size_t)width * height;

  dt_iop_copy_image_roi(out, in, 4, roi_in, roi_out);

  const float *const pmax = piece->pipe->dsc.processed_maximum;
  const dt_aligned_pixel_t clip = { d->clip * pmax[0], d->clip * pmax[1], d->clip * pmax[2], 1.0f };
  const dt_aligned_pixel_t inv_clip = { 1.0f / clip[0], 1.0f / clip[1], 1.0f / clip[2], 1.0f };

  const gboolean hue_fill = d->experiment == DT_HLCOLOR_EXPERIMENT_HUE_FILL;
  const gboolean smooth_fill = d->experiment == DT_HLCOLOR_EXPERIMENT_SMOOTH_FILL;
  const gboolean solved_refs = d->experiment == DT_HLCOLOR_EXPERIMENT_SOLVED_REFS;
  // the experiments that restore references use the skip rule of the
  // highlights method: a clipped reference is left out unless restored. The
  // basic method keeps it, with its eq. 8 weight near the floor
  const gboolean skip_rule = solved_refs || smooth_fill;

  // bits 0-2: channel clipped
  uint8_t *const state = dt_alloc_align_uint8(npix);
  uint8_t *const border = dt_alloc_align_uint8(npix);
  float *const rhobuf = dt_alloc_align_float(3 * npix);
  int *const map = dt_alloc_align_int(npix);
  // the solutions, written over a copy of the input
  float *const cur = dt_alloc_align_float(4 * npix);
  uint8_t *const filled = smooth_fill ? dt_alloc_align_uint8(npix) : NULL;
  int *pos = NULL;
  float *x = NULL;
  float *div = NULL;
  if(!state || !border || !rhobuf || !map || !cur || (smooth_fill && !filled))
  {
    dt_print(DT_DEBUG_ALWAYS, "[hlcolor] out of memory, image left unchanged");
    goto cleanup;
  }

  size_t nclipped = 0;
  DT_OMP_FOR(reduction(+ : nclipped))
  for(size_t i = 0; i < npix; i++)
  {
    uint8_t s = 0;
    for(int c = 0; c < 3; c++)
      if(in[4 * i + c] >= clip[c]) s |= 1 << c;
    state[i] = s;
    nclipped += s ? 1 : 0;
  }
  if(!nclipped) goto cleanup;

  size_t nborder = 0;
  DT_OMP_FOR(reduction(+ : nborder))
  for(int i = 0; i < (int)npix; i++)
  {
    int nb[4];
    _neighbors(i, width, height, nb);
    gboolean b = FALSE;
    if(!state[i])
      for(int m = 0; m < 4; m++)
        b |= nb[m] >= 0 && state[nb[m]];
    border[i] = b;
    nborder += b ? 1 : 0;
  }
  if(!nborder) goto cleanup;

  pos = dt_alloc_align_int(nclipped);
  x = dt_alloc_align_float(nclipped);
  div = dt_alloc_align_float(nclipped);
  if(!pos || !x || !div)
  {
    dt_print(DT_DEBUG_ALWAYS, "[hlcolor] out of memory, image left unchanged");
    goto cleanup;
  }

  float *const rho[3] = { rhobuf, rhobuf + npix, rhobuf + 2 * npix };
  const float *const crho[3] = { rho[0], rho[1], rho[2] };

  int hue_iter[3] = { 0, 0, 0 };
  const size_t nu = _index_pixels(map, pos, state, 7, NULL, npix);
  if(d->experiment == DT_HLCOLOR_EXPERIMENT_NEUTRAL_HUE)
  {
    // the same value in all channels is white under the white balance
    DT_OMP_FOR()
    for(size_t i = 0; i < 3 * npix; i++)
      rhobuf[i] = 1.0f;
  }
  else
  {
    // hue estimate over U, paper 3.2
    const float sigma_s = fmaxf(0.5f, HLC_BORDER_SIGMA_S * roi_in->scale / piece->iscale);
    _smooth_border(rho, in, border, width, height, inv_clip, sigma_s);
    if(!_pullpush_fill(rho, border, state, width, height))
    {
      dt_print(DT_DEBUG_ALWAYS, "[hlcolor] out of memory, image left unchanged");
      goto cleanup;
    }

    for(int c = 0; c < 3; c++)
    {
      for(size_t k = 0; k < nu; k++)
        x[k] = rho[c][pos[k]];
      hue_iter[c] = _solve_poisson(x, NULL, rho[c], 1, map, pos, nu, width, height);
      if(hue_iter[c] < 0) goto cleanup;
      for(size_t k = 0; k < nu; k++)
        rho[c][pos[k]] = x[k];
    }
  }

  memcpy(cur, in, sizeof(float) * 4 * npix);

  size_t nfill[3] = { 0, 0, 0 };
  int fill_iter[3] = { 0, 0, 0 };
  float peak[3] = { 0.0f, 0.0f, 0.0f };
  if(smooth_fill)
  {
    memset(filled, 0, npix);
    if(!_fill_fully_clipped(cur, state, filled, map, pos, x, div, inv_clip,
                            width, height, nfill, fill_iter, peak))
      goto cleanup;
  }

  // red, green, blue; with solved channels as references, the channel with the
  // largest clipped region goes first
  int order[3] = { 0, 1, 2 };
  if(solved_refs)
  {
    size_t count[3] = { 0, 0, 0 };
    for(size_t i = 0; i < npix; i++)
      for(int c = 0; c < 3; c++) count[c] += (state[i] >> c) & 1;
    _channel_order(order, count, TRUE);
  }

  // under the skip rule, the guidance reads restored values from the working
  // copy; otherwise it reads the input
  const float *const gin = skip_rule ? cur : in;

  // per channel: guidance field and Poisson solve over its clipped region
  uint8_t solved = 0;
  int chan_iter[3] = { 0, 0, 0 };
  for(int o = 0; o < 3; o++)
  {
    const int j = order[o];
    const size_t n = _index_pixels(map, pos, state, 1 << j, filled, npix);
    if(n)
    {
      DT_OMP_FOR()
      for(size_t k = 0; k < n; k++)
      {
        const int p = pos[k];
        const uint8_t up = skip_rule ? _usable(state, filled, solved, p) : 7;
        int nb[4];
        _neighbors(p, width, height, nb);
        float sum = 0.0f;
        for(int m = 0; m < 4; m++)
        {
          if(nb[m] < 0) continue;
          const uint8_t usable = skip_rule ? up & _usable(state, filled, solved, nb[m]) : 7;
          sum -= _edge_guidance(gin, crho, inv_clip, p, nb[m], j, usable);
        }
        div[k] = sum;

        // start from the spatial estimate of paper eq. 4, extended to
        // several reference channels like eq. 7
        float num = 0.0f;
        float den = 0.0f;
        for(int c = 0; c < 3; c++)
        {
          if(c == j || !(up & (1 << c))) continue;
          const float w = _weight(gin[4 * p + c] * inv_clip[c]);
          num += w * _ratio(crho, p, j, c) * gin[4 * p + c];
          den += w;
        }
        x[k] = den > 0.0f ? fmaxf(in[4 * p + j], num / den) : in[4 * p + j];
      }

      chan_iter[j] = _solve_poisson(x, div, in + j, 4, map, pos, n, width, height);
      if(chan_iter[j] < 0) goto cleanup;

      DT_OMP_FOR()
      for(size_t k = 0; k < n; k++)
        cur[4 * pos[k] + j] = x[k];
    }
    if(solved_refs) solved |= 1 << j;
  }

  if(hue_fill || d->writeback != DT_HLCOLOR_WRITEBACK_BASIC)
  {
    const gboolean joint_all = d->writeback == DT_HLCOLOR_WRITEBACK_JOINT_ALL;
    const gboolean joint_full = d->writeback == DT_HLCOLOR_WRITEBACK_JOINT_FULL;
    _index_pixels(map, pos, state, 7, NULL, npix);
    DT_OMP_FOR()
    for(size_t k = 0; k < nu; k++)
    {
      const int p = pos[k];
      float *const u = cur + 4 * p;
      const float *const v = in + 4 * p;
      if(hue_fill && state[p] == 7) _hue_fill(u, v, crho, p);
      if(joint_all || (joint_full && state[p] == 7)) _joint_scale(u, v, state[p]);
    }
  }

  // a clipped channel was at least as bright as its clipping level
  const int dx = roi_out->x - roi_in->x;
  const int dy = roi_out->y - roi_in->y;
  DT_OMP_FOR()
  for(int row = 0; row < roi_out->height; row++)
  {
    const int irow = row + dy;
    if(irow < 0 || irow >= height) continue;
    for(int col = 0; col < roi_out->width; col++)
    {
      const int icol = col + dx;
      if(icol < 0 || icol >= width) continue;
      const size_t p = (size_t)irow * width + icol;
      float *const o = out + 4 * ((size_t)row * roi_out->width + col);
      for(int c = 0; c < 3; c++)
        if(state[p] & (1 << c)) o[c] = fmaxf(cur[4 * p + c], in[4 * p + c]);
    }
  }

  dt_print(DT_DEBUG_PERF,
           "[hlcolor] %dx%d: %zu clipped, %zu border pixels; CG iterations hue %d/%d/%d, channels %d/%d/%d; "
           "write-back %d, experiment %d; filled %zu/%zu/%zu in %d/%d/%d iterations, peak %.1f/%.1f/%.1f x clip",
           width, height, nclipped, nborder,
           hue_iter[0], hue_iter[1], hue_iter[2], chan_iter[0], chan_iter[1], chan_iter[2],
           d->writeback, d->experiment, nfill[0], nfill[1], nfill[2],
           fill_iter[0], fill_iter[1], fill_iter[2], peak[0], peak[1], peak[2]);

cleanup:
  dt_free_align(state);
  dt_free_align(border);
  dt_free_align(rhobuf);
  dt_free_align(map);
  dt_free_align(cur);
  dt_free_align(filled);
  dt_free_align(pos);
  dt_free_align(x);
  dt_free_align(div);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_hlcolor_gui_data_t *g = IOP_GUI_ALLOC(hlcolor);

  g->clip = dt_bauhaus_slider_from_params(self, "clip");
  dt_bauhaus_slider_set_digits(g->clip, 3);
  gtk_widget_set_tooltip_text(g->clip, _("channel values above this fraction of their white-balanced\n"
                                         "clipping level are treated as clipped"));

  g->writeback = dt_bauhaus_combobox_from_params(self, "writeback");
  gtk_widget_set_tooltip_text(g->writeback, _("experimental: how the solution replaces clipped values.\n"
                                              "basic: each clipped channel takes the larger of its value and its solution.\n"
                                              "joint: the clipped channels of a pixel are scaled together until none is\n"
                                              "below its value, in fully clipped pixels or in all pixels with a clipped channel."));

  g->experiment = dt_bauhaus_combobox_from_params(self, "experiment");
  gtk_widget_set_tooltip_text(g->experiment, _("experimental variants of the method.\n"
                                               "neutral hue: white instead of the hue estimated from the border.\n"
                                               "hue estimate in fully clipped: fully clipped pixels take the estimated hue.\n"
                                               "solved channels as references: channels guide the ones solved after them,\n"
                                               "largest clipped region first.\n"
                                               "smooth fill: one channel gets a smooth bump in fully clipped regions,\n"
                                               "which then guides the others (paper section 3.4)."));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
