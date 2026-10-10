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

   The gradient fill-in for regions with all channels clipped (paper 3.4)
   is not implemented: such regions come out flat, with the hue of B.
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

typedef struct dt_iop_hlcolor_params_t
{
  float clip; // $MIN: 0.5 $MAX: 1.0 $DEFAULT: 0.98 $DESCRIPTION: "clipping threshold"
} dt_iop_hlcolor_params_t;

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

// numbers the pixels with any of the given state bits, in pixel order
static size_t _index_pixels(int *const map,
                            int *const pos,
                            const uint8_t *const state,
                            const uint8_t bits,
                            const size_t npix)
{
  size_t n = 0;
  for(size_t i = 0; i < npix; i++)
  {
    if(state[i] & bits)
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

// target value of u_j(q) - u_j(p) on the edge p-q, from the other channels,
// paper eq. 7, with the weights min-filtered over the edge as in paper 3.5
static inline float _edge_guidance(const float *const in,
                                   const float *const rho[3],
                                   const dt_aligned_pixel_t inv_clip,
                                   const size_t p,
                                   const size_t q,
                                   const int j)
{
  float num = 0.0f;
  float den = 0.0f;
  for(int k = 0; k < 3; k++)
  {
    if(k == j) continue;
    const float w = fminf(_weight(in[4 * p + k] * inv_clip[k]),
                          _weight(in[4 * q + k] * inv_clip[k]));
    const float r = 0.5f * (_ratio(rho, p, j, k) + _ratio(rho, q, j, k));
    num += w * r * (in[4 * q + k] - in[4 * p + k]);
    den += w;
  }
  return num / den;
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

  // bits 0-2: channel clipped
  uint8_t *const state = dt_alloc_align_uint8(npix);
  uint8_t *const border = dt_alloc_align_uint8(npix);
  float *const rhobuf = dt_alloc_align_float(3 * npix);
  int *const map = dt_alloc_align_int(npix);
  int *pos = NULL;
  float *x = NULL;
  float *div = NULL;
  if(!state || !border || !rhobuf || !map)
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

  // hue estimate over U, paper 3.2
  const float sigma_s = fmaxf(0.5f, HLC_BORDER_SIGMA_S * roi_in->scale / piece->iscale);
  _smooth_border(rho, in, border, width, height, inv_clip, sigma_s);
  if(!_pullpush_fill(rho, border, state, width, height))
  {
    dt_print(DT_DEBUG_ALWAYS, "[hlcolor] out of memory, image left unchanged");
    goto cleanup;
  }

  int hue_iter[3] = { 0, 0, 0 };
  const size_t nu = _index_pixels(map, pos, state, 7, npix);
  for(int c = 0; c < 3; c++)
  {
    for(size_t k = 0; k < nu; k++)
      x[k] = rho[c][pos[k]];
    hue_iter[c] = _solve_poisson(x, NULL, rho[c], 1, map, pos, nu, width, height);
    if(hue_iter[c] < 0) goto cleanup;
    for(size_t k = 0; k < nu; k++)
      rho[c][pos[k]] = x[k];
  }

  // per channel: guidance field and Poisson solve over its clipped region
  const int dx = roi_out->x - roi_in->x;
  const int dy = roi_out->y - roi_in->y;
  int chan_iter[3] = { 0, 0, 0 };
  for(int j = 0; j < 3; j++)
  {
    const size_t n = _index_pixels(map, pos, state, 1 << j, npix);
    if(!n) continue;

    DT_OMP_FOR()
    for(size_t k = 0; k < n; k++)
    {
      const int p = pos[k];
      int nb[4];
      _neighbors(p, width, height, nb);
      float sum = 0.0f;
      for(int m = 0; m < 4; m++)
        if(nb[m] >= 0) sum -= _edge_guidance(in, crho, inv_clip, p, nb[m], j);
      div[k] = sum;

      // start from the spatial estimate of paper eq. 4, extended to
      // several reference channels like eq. 7
      float num = 0.0f;
      float den = 0.0f;
      for(int c = 0; c < 3; c++)
      {
        if(c == j) continue;
        const float w = _weight(in[4 * p + c] * inv_clip[c]);
        num += w * _ratio(crho, p, j, c) * in[4 * p + c];
        den += w;
      }
      x[k] = fmaxf(in[4 * p + j], num / den);
    }

    chan_iter[j] = _solve_poisson(x, div, in + j, 4, map, pos, n, width, height);
    if(chan_iter[j] < 0) goto cleanup;

    DT_OMP_FOR()
    for(size_t k = 0; k < n; k++)
    {
      const int p = pos[k];
      const int row = p / width - dy;
      const int col = p % width - dx;
      // a clipped channel was at least as bright as its clipping level
      if(row >= 0 && row < roi_out->height && col >= 0 && col < roi_out->width)
        out[4 * ((size_t)row * roi_out->width + col) + j] = fmaxf(x[k], in[4 * p + j]);
    }
  }

  dt_print(DT_DEBUG_PERF,
           "[hlcolor] %dx%d: %zu clipped, %zu border pixels; CG iterations hue %d/%d/%d, channels %d/%d/%d",
           width, height, nclipped, nborder,
           hue_iter[0], hue_iter[1], hue_iter[2], chan_iter[0], chan_iter[1], chan_iter[2]);

cleanup:
  dt_free_align(state);
  dt_free_align(border);
  dt_free_align(rhobuf);
  dt_free_align(map);
  dt_free_align(pos);
  dt_free_align(x);
  dt_free_align(div);
}

void gui_init(dt_iop_module_t *self)
{
  GtkWidget *clip = dt_bauhaus_slider_from_params(self, "clip");
  dt_bauhaus_slider_set_digits(clip, 3);
  gtk_widget_set_tooltip_text(clip, _("channel values above this fraction of their white-balanced\n"
                                      "clipping level are treated as clipped"));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
