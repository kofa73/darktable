/*
    This file is part of darktable,
    Copyright (C) 2025 darktable developers.

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

#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/chromatic_adaptation.h"
#include "common/math.h"
#include "common/matrices.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <math.h>

/* ---------------------------------------------------------------------------
 * Types and Definitions
 * ------------------------------------------------------------------------ */

typedef enum dt_iop_opendrt_whitepoint_t
{
  DT_OPENDRT_WP_D93 = 0,
  DT_OPENDRT_WP_D75 = 1,
  DT_OPENDRT_WP_D65 = 2, // Default
  DT_OPENDRT_WP_D60 = 3,
  DT_OPENDRT_WP_D55 = 4,
  DT_OPENDRT_WP_D50 = 5,
} dt_iop_opendrt_whitepoint_t;

typedef struct dt_iop_opendrt_params_t
{
  // Tonescale Settings
  float tn_Lp;  // $MIN: 100.0 $MAX: 4000.0 $DEFAULT: 100.0 $DESCRIPTION: "display peak luminance (nits)"
  float tn_Lg;  // $MIN: 3.0   $MAX: 50.0   $DEFAULT: 10.0  $DESCRIPTION: "display grey luminance (nits)"
  float tn_gb;  // $MIN: 0.0   $MAX: 1.0    $DEFAULT: 0.13  $DESCRIPTION: "hdr grey boost"
  float pt_hdr; // $MIN: 0.0   $MAX: 1.0    $DEFAULT: 0.5   $DESCRIPTION: "hdr purity"

  // Look Parameters - Exposed in GUI
  float contrast;   // $MIN: 1.0 $MAX: 2.0 $DEFAULT: 1.66 $DESCRIPTION: "contrast"
  float saturation; // $MIN: 0.0 $MAX: 0.6 $DEFAULT: 0.35 $DESCRIPTION: "saturation"
  dt_iop_opendrt_whitepoint_t cwp; // $DEFAULT: DT_OPENDRT_WP_D65 $DESCRIPTION: "creative white point"
  float cwp_lm;                    // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.25 $DESCRIPTION: "creative white range"

  // Look Parameters - Advanced / Preset driven (Hidden in GUI)
  float tn_sh;
  float tn_toe;
  float tn_off;
  int tn_hcon_enable;
  float tn_hcon;
  float tn_hcon_pv;
  float tn_hcon_st;
  int tn_lcon_enable;
  float tn_lcon;
  float tn_lcon_w;

  float rs_rw;
  float rs_bw;

  int pt_enable;
  float pt_lml;
  float pt_lml_r;
  float pt_lml_g;
  float pt_lml_b;
  float pt_lmh;
  float pt_lmh_r;
  float pt_lmh_b;

  int ptl_enable;
  float ptl_c;
  float ptl_m;
  float ptl_y;

  int ptm_enable;
  float ptm_low;
  float ptm_low_rng;
  float ptm_low_st;
  float ptm_high;
  float ptm_high_rng;
  float ptm_high_st;

  int brl_enable;
  float brl;
  float brl_r;
  float brl_g;
  float brl_b;
  float brl_rng;
  float brl_st;

  int brlp_enable;
  float brlp;
  float brlp_r;
  float brlp_g;
  float brlp_b;

  int hc_enable;
  float hc_r;
  float hc_r_rng;

  int hs_rgb_enable;
  float hs_r;
  float hs_r_rng;
  float hs_g;
  float hs_g_rng;
  float hs_b;
  float hs_b_rng;

  int hs_cmy_enable;
  float hs_c;
  float hs_c_rng;
  float hs_m;
  float hs_m_rng;
  float hs_y;
  float hs_y_rng;

} dt_iop_opendrt_params_t;

DT_MODULE_INTROSPECTION(1, dt_iop_opendrt_params_t)

typedef struct dt_iop_opendrt_data_t
{
  // Process data derived from params
  float ts_s1;
  float ts_p;
  float ts_s;
  float s_Lp100;
  float ts_m2;
  float ts_dsc;
  float tsn_min_val; // Implicit

  // Copied from params
  float tn_off;
  float tn_toe;
  float rs_sa;
  float rs_rw;
  float rs_bw;

  float cwp_lm;
  int cwp_mode;

  // Enables
  int tn_hcon_enable;
  int tn_lcon_enable;
  int brl_enable;
  int brlp_enable;
  int hc_enable;
  int hs_rgb_enable;
  int hs_cmy_enable;
  int ptm_enable;
  int ptl_enable;
  int pt_enable; // Added missing member

  // Knob values
  float tn_lcon, tn_lcon_w;
  float tn_hcon, tn_hcon_pv, tn_hcon_st;

  float pt_lml, pt_lml_r, pt_lml_g, pt_lml_b;
  float pt_lmh, pt_lmh_r, pt_lmh_b;

  float ptl_c, ptl_m, ptl_y;
  float ptm_low, ptm_low_rng, ptm_low_st;
  float ptm_high, ptm_high_rng, ptm_high_st;

  float brl, brl_r, brl_g, brl_b, brl_rng, brl_st;
  float brlp, brlp_r, brlp_g, brlp_b;

  float hc_r, hc_r_rng;
  float hs_r, hs_r_rng, hs_g, hs_g_rng, hs_b, hs_b_rng;
  float hs_c, hs_c_rng, hs_m, hs_m_rng, hs_y, hs_y_rng;

  // Matrices
  dt_colormatrix_t pipe_to_p3d65;
  dt_colormatrix_t p3d65_to_pipe;
  dt_colormatrix_t cwp_matrix;

} dt_iop_opendrt_data_t;

/* ---------------------------------------------------------------------------
 * Module Implementation
 * ------------------------------------------------------------------------ */

const char *name() { return _("opendrt"); }
const char *aliases() { return _("open display transform"); }
const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
                                _("apply the OpenDRT display transform\n"
                                  "creates a natural, perceptual rendering from scene-referred data"),
                                _("tone and color mapping"),
                                _("linear, RGB, scene-referred"),
                                _("linear, RGB, display-referred"),
                                _("linear, RGB, display-referred"));
}

int flags() { return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_SUPPORTS_BLENDING; }

int default_group() { return IOP_GROUP_TONE; }

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

/* ---------------------------------------------------------------------------
 * Math Helpers
 * ------------------------------------------------------------------------ */
#define SQRT3 1.73205080756887729353f
#define M_PI_F 3.14159265358979323846f

static inline float spowf(float a, float b) { return (a <= 0.0f) ? a : powf(a, b); }
static inline float compress_hyperbolic_power(float x, float s, float p) { return spowf(x / (x + s), p); }
static inline float compress_toe_quadratic(float x, float toe, int inv) {
  if (toe == 0.0f) return x;
  if (inv == 0) return spowf(x, 2.0f) / (x + toe);
  else return (x + sqrtf(x * (4.0f * toe + x))) / 2.0f;
}
static inline float compress_toe_cubic(float x, float m, float w, int inv) {
  if (m == 1.0f) return x;
  float x2 = x * x;
  if (inv == 0) return x * (x2 + m * w) / (x2 + w);
  else {
    float p0 = x2 - 3.0f * m * w;
    float p1 = 2.0f * x2 + 27.0f * w - 9.0f * m * w;
    float p2 = powf(sqrtf(x2 * p1 * p1 - 4.0f * p0 * p0 * p0) / 2.0f + x * p1 / 2.0f, 1.0f / 3.0f);
    return p0 / (3.0f * p2) + p2 / 3.0f + x / 3.0f;
  }
}
static inline float contrast_high(float x, float p, float pv, float pv_lx, int inv) {
  const float x0 = 0.18f * powf(2.0f, pv);
  if (x < x0 || p == 1.0f) return x;
  const float o = x0 - x0 / p;
  const float s0 = powf(x0, 1.0f - p) / p;
  const float x1 = x0 * powf(2.0f, pv_lx);
  const float k1 = p * s0 * powf(x1, p) / x1;
  const float y1 = s0 * powf(x1, p) + o;
  if (inv == 1) return x > y1 ? (x - y1) / k1 + x1 : powf((x - o) / s0, 1.0f / p);
  else return x > x1 ? k1 * (x - x1) + y1 : s0 * powf(x, p) + o;
}
static inline float softplus(float x, float s) {
  if (x > 10.0f * s || s < 1e-4f) return x;
  return s * logf(fmaxf(0.0f, 1.0f + expf(x / s)));
}
static inline float gauss_window(float x, float w) { return expf(-x * x / w); }
static inline void opponent(const float r, const float g, const float b, float *ox, float *oy) {
  *ox = r - b;
  *oy = g - (r + b) / 2.0f;
}
static inline float hue_offset(float h, float o) {
  return fmodf(h - o + M_PI_F, 2.0f * M_PI_F) - M_PI_F;
}

/* ---------------------------------------------------------------------------
 * Presets & Params
 * ------------------------------------------------------------------------ */

static void _set_standard_params(dt_iop_opendrt_params_t *p)
{
  p->contrast = 1.66f; p->tn_sh = 0.5f; p->tn_toe = 0.003f; p->tn_off = 0.005f;
  p->tn_hcon_enable = 0; p->tn_hcon = 0.0f; p->tn_hcon_pv = 1.0f; p->tn_hcon_st = 4.0f;
  p->tn_lcon_enable = 0; p->tn_lcon = 0.0f; p->tn_lcon_w = 0.5f;

  p->cwp = DT_OPENDRT_WP_D65; p->cwp_lm = 0.25f;

  p->saturation = 0.35f; p->rs_rw = 0.25f; p->rs_bw = 0.55f;

  p->pt_enable = 1;
  p->pt_lml = 0.25f; p->pt_lml_r = 0.5f; p->pt_lml_g = 0.0f; p->pt_lml_b = 0.1f;
  p->pt_lmh = 0.25f; p->pt_lmh_r = 0.5f; p->pt_lmh_b = 0.0f;

  p->ptl_enable = 1;
  p->ptl_c = 0.06f; p->ptl_m = 0.08f; p->ptl_y = 0.06f;

  p->ptm_enable = 1;
  p->ptm_low = 0.5f; p->ptm_low_rng = 0.25f; p->ptm_low_st = 0.5f;
  p->ptm_high = -0.8f; p->ptm_high_rng = 0.3f; p->ptm_high_st = 0.4f;

  p->brl_enable = 1;
  p->brl = 0.0f; p->brl_r = -2.5f; p->brl_g = -1.5f; p->brl_b = -1.5f;
  p->brl_rng = 0.5f; p->brl_st = 0.35f;

  p->brlp_enable = 1;
  p->brlp = -0.5f; p->brlp_r = -1.25f; p->brlp_g = -1.25f; p->brlp_b = -0.25f;

  p->hc_enable = 1;
  p->hc_r = 1.0f; p->hc_r_rng = 0.3f;

  p->hs_rgb_enable = 1;
  p->hs_r = 0.6f; p->hs_r_rng = 0.7f; p->hs_g = 0.35f; p->hs_g_rng = 1.0f; p->hs_b = 0.66f; p->hs_b_rng = 1.0f;

  p->hs_cmy_enable = 1;
  p->hs_c = 0.25f; p->hs_c_rng = 1.0f; p->hs_m = 0.0f; p->hs_m_rng = 1.0f; p->hs_y = 0.0f; p->hs_y_rng = 1.0f;

  p->tn_Lp = 100.0f; p->tn_Lg = 10.0f; p->tn_gb = 0.13f; p->pt_hdr = 0.5f;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_opendrt_params_t p = { 0 };

  _set_standard_params(&p);
  dt_gui_presets_add_generic(_("Standard"), self->op, self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  // Arriba
  _set_standard_params(&p);
  p.contrast = 1.05f; p.tn_toe = 0.1f; p.tn_off = 0.01f;
  p.tn_lcon_enable = 1; p.tn_lcon = 1.5f; p.tn_lcon_w = 0.2f;
  p.pt_lml_r = 0.45f; p.pt_lmh_r = 0.25f;
  p.ptm_low = 1.0f; p.ptm_low_rng = 0.4f; p.ptm_high_rng = 0.66f; p.ptm_high_st = 0.6f;
  p.brlp = 0.0f; p.brlp_r = -1.7f; p.brlp_g = -2.0f; p.brlp_b = -0.5f;
  p.hs_r_rng = 0.8f; p.hs_c = 0.15f;
  dt_gui_presets_add_generic(_("Arriba"), self->op, self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  // Sylvan
  _set_standard_params(&p);
  p.contrast = 1.6f; p.tn_toe = 0.01f; p.tn_off = 0.01f;
  p.tn_lcon_enable = 1; p.tn_lcon = 0.25f; p.tn_lcon_w = 0.75f;
  p.saturation = 0.25f;
  p.pt_lml = 0.15f; p.pt_lml_g = 0.15f;
  p.pt_lmh_r = 0.15f; p.pt_lmh_b = 0.15f;
  p.ptl_c = 0.05f; p.ptl_y = 0.05f;
  p.ptm_low_rng = 0.5f; p.ptm_high_rng = 0.5f; p.ptm_high_st = 0.5f;
  p.brl = -1.0f; p.brl_r = -2.0f; p.brl_g = -2.0f; p.brl_b = 0.0f; p.brl_rng = 0.25f; p.brl_st = 0.25f;
  p.brlp = -1.0f; p.brlp_r = -0.5f; p.brlp_g = -0.25f; p.brlp_b = -0.25f;
  p.hc_r_rng = 0.4f;
  p.hs_r_rng = 1.15f; p.hs_g = 0.8f; p.hs_g_rng = 1.25f; p.hs_b = 0.6f;
  p.hs_m = 0.25f; p.hs_m_rng = 0.5f; p.hs_y = 0.35f; p.hs_y_rng = 0.5f;
  dt_gui_presets_add_generic(_("Sylvan"), self->op, self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  // Colorful
  _set_standard_params(&p);
  p.contrast = 1.5f; p.tn_off = 0.003f;
  p.tn_lcon_enable = 1; p.tn_lcon = 0.4f;
  p.pt_lml = 0.5f; p.pt_lml_r = 1.0f; p.pt_lml_b = 0.5f;
  p.pt_lmh_b = 0.15f;
  p.ptl_c = 0.05f; p.ptl_m = 0.06f; p.ptl_y = 0.05f;
  p.ptm_low = 0.8f; p.ptm_low_rng = 0.5f; p.ptm_low_st = 0.4f;
  p.brl_r = -1.25f; p.brl_g = -1.25f; p.brl_b = -0.25f; p.brl_rng = 0.3f; p.brl_st = 0.5f;
  p.brlp_b = 0.0f;
  p.hc_r_rng = 0.4f;
  p.hs_r = 0.5f; p.hs_r_rng = 0.8f; p.hs_b = 0.5f; p.hs_y = 0.25f;
  dt_gui_presets_add_generic(_("Colorful"), self->op, self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  // Aery
  _set_standard_params(&p);
  p.contrast = 1.15f; p.tn_toe = 0.04f; p.tn_off = 0.006f;
  p.tn_hcon_pv = 0.0f; p.tn_hcon_st = 0.5f;
  p.tn_lcon_enable = 1; p.tn_lcon = 0.5f; p.tn_lcon_w = 2.0f;
  p.cwp = DT_OPENDRT_WP_D75;
  p.saturation = 0.25f; p.rs_rw = 0.2f; p.rs_bw = 0.5f;
  p.pt_lml = 0.0f; p.pt_lml_r = 0.35f; p.pt_lml_g = 0.15f;
  p.pt_lmh = 0.0f;
  p.ptl_c = 0.05f; p.ptl_y = 0.05f;
  p.ptm_low = 0.8f; p.ptm_low_rng = 0.35f; p.ptm_high = -0.9f; p.ptm_high_rng = 0.5f; p.ptm_high_st = 0.3f;
  p.brl = -3.0f; p.brl_r = 0.0f; p.brl_g = 0.0f; p.brl_b = 1.0f; p.brl_rng = 0.8f; p.brl_st = 0.15f;
  p.brlp = -1.0f; p.brlp_r = -1.0f; p.brlp_g = -1.0f; p.brlp_b = 0.0f;
  p.hc_r = 0.5f; p.hc_r_rng = 0.25f;
  p.hs_r_rng = 1.0f; p.hs_g_rng = 2.0f; p.hs_b = 0.5f; p.hs_b_rng = 1.5f;
  p.hs_c = 0.35f; p.hs_m = 0.25f; p.hs_y = 0.35f; p.hs_y_rng = 0.5f;
  dt_gui_presets_add_generic(_("Aery"), self->op, self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_opendrt_params_t *p = (dt_iop_opendrt_params_t *)p1;
  dt_iop_opendrt_data_t *d = (dt_iop_opendrt_data_t *)piece->data;

  // Copy parameters to data
  d->tn_off = p->tn_off;
  d->tn_toe = p->tn_toe;
  d->rs_sa = p->saturation;
  d->rs_rw = p->rs_rw;
  d->rs_bw = p->rs_bw;
  d->cwp_lm = p->cwp_lm;
  d->cwp_mode = p->cwp;

  d->tn_hcon_enable = p->tn_hcon_enable;
  d->tn_hcon = p->tn_hcon; d->tn_hcon_pv = p->tn_hcon_pv; d->tn_hcon_st = p->tn_hcon_st;
  d->tn_lcon_enable = p->tn_lcon_enable;
  d->tn_lcon = p->tn_lcon; d->tn_lcon_w = p->tn_lcon_w;

  d->pt_enable = p->pt_enable;
  d->pt_lml = p->pt_lml; d->pt_lml_r = p->pt_lml_r; d->pt_lml_g = p->pt_lml_g; d->pt_lml_b = p->pt_lml_b;
  d->pt_lmh = p->pt_lmh; d->pt_lmh_r = p->pt_lmh_r; d->pt_lmh_b = p->pt_lmh_b;

  d->ptl_enable = p->ptl_enable;
  d->ptl_c = p->ptl_c; d->ptl_m = p->ptl_m; d->ptl_y = p->ptl_y;

  d->ptm_enable = p->ptm_enable;
  d->ptm_low = p->ptm_low; d->ptm_low_rng = p->ptm_low_rng; d->ptm_low_st = p->ptm_low_st;
  d->ptm_high = p->ptm_high; d->ptm_high_rng = p->ptm_high_rng; d->ptm_high_st = p->ptm_high_st;

  d->brl_enable = p->brl_enable;
  d->brl = p->brl; d->brl_r = p->brl_r; d->brl_g = p->brl_g; d->brl_b = p->brl_b;
  d->brl_rng = p->brl_rng; d->brl_st = p->brl_st;

  d->brlp_enable = p->brlp_enable;
  d->brlp = p->brlp; d->brlp_r = p->brlp_r; d->brlp_g = p->brlp_g; d->brlp_b = p->brlp_b;

  d->hc_enable = p->hc_enable;
  d->hc_r = p->hc_r; d->hc_r_rng = p->hc_r_rng;

  d->hs_rgb_enable = p->hs_rgb_enable;
  d->hs_r = p->hs_r; d->hs_r_rng = p->hs_r_rng; d->hs_g = p->hs_g; d->hs_g_rng = p->hs_g_rng;
  d->hs_b = p->hs_b; d->hs_b_rng = p->hs_b_rng;

  d->hs_cmy_enable = p->hs_cmy_enable;
  d->hs_c = p->hs_c; d->hs_c_rng = p->hs_c_rng; d->hs_m = p->hs_m; d->hs_m_rng = p->hs_m_rng;
  d->hs_y = p->hs_y; d->hs_y_rng = p->hs_y_rng;

  // Tonescale Calculations
  const float tn_sh = p->tn_sh;
  const float tn_con = p->contrast;
  const float tn_su = 1.0f; // Dim surround

  const float ts_x1 = powf(2.0f, 6.0f * tn_sh + 4.0f);
  const float ts_y1 = p->tn_Lp / 100.0f;
  const float ts_x0 = 0.18f + d->tn_off;
  const float ts_y0 = p->tn_Lg / 100.0f * (1.0f + p->tn_gb * log2f(ts_y1));
  const float ts_s0 = compress_toe_quadratic(ts_y0, d->tn_toe, 1);
  const float ts_p = tn_con / (1.0f + tn_su * 0.05f);
  const float ts_s10 = ts_x0 * (powf(ts_s0, -1.0f / tn_con) - 1.0f);
  const float ts_m1 = ts_y1 / powf(ts_x1 / (ts_x1 + ts_s10), tn_con);
  d->ts_m2 = compress_toe_quadratic(ts_m1, d->tn_toe, 1);
  d->ts_s = ts_x0 * (powf(ts_s0 / d->ts_m2, -1.0f / tn_con) - 1.0f);
  d->ts_dsc = 100.0f / p->tn_Lp;
  d->ts_p = ts_p;

  float pt_cmp_Lf = p->pt_hdr * fminf(1.0f, (p->tn_Lp - 100.0f) / 900.0f);
  d->s_Lp100 = ts_x0 * (powf((p->tn_Lg / 100.0f), -1.0f / tn_con) - 1.0f);
  d->ts_s1 = d->ts_s * pt_cmp_Lf + d->s_Lp100 * (1.0f - pt_cmp_Lf);

  // Matrices
  const dt_iop_order_iccprofile_info_t *profile = dt_ioppr_get_pipe_work_profile_info(pipe);
  dt_colormatrix_t pipe_to_xyz_d50;
  for(int i=0; i<3; i++)
    for(int j=0; j<3; j++)
      pipe_to_xyz_d50[i][j] = profile->matrix_in[i][j];

  // Use standard CAT16 matrix from common
  const dt_colormatrix_t *mat_cat16 = &XYZ_D50_to_D65_CAT16;

  dt_colormatrix_t xyz_d65_to_p3d65;
  memcpy(xyz_d65_to_p3d65, matrix_xyz_to_p3d65, sizeof(dt_colormatrix_t));

  dt_colormatrix_t tmp;
  dt_colormatrix_mul(tmp, *mat_cat16, pipe_to_xyz_d50);
  dt_colormatrix_mul(d->pipe_to_p3d65, xyz_d65_to_p3d65, tmp);

  // Inverse
  dt_colormatrix_t p3d65_to_xyz_d65;
  memcpy(p3d65_to_xyz_d65, matrix_p3d65_to_xyz, sizeof(dt_colormatrix_t));

  const dt_colormatrix_t *xyz_d65_to_d50 = &XYZ_D65_to_D50_CAT16;
  dt_colormatrix_t xyz_d50_to_pipe;
  mat3SSEinv(xyz_d50_to_pipe, pipe_to_xyz_d50);

  dt_colormatrix_mul(tmp, *xyz_d65_to_d50, p3d65_to_xyz_d65);
  dt_colormatrix_mul(d->p3d65_to_pipe, xyz_d50_to_pipe, tmp);

  // Creative Whitepoint
  if (p->cwp == DT_OPENDRT_WP_D65) {
     for(int i=0; i<4; i++) for(int j=0; j<4; j++) d->cwp_matrix[i][j] = (i==j) ? 1.f : 0.f;
  } else {
     // Use matrices from DCTL definitions
     if (p->cwp == DT_OPENDRT_WP_D60) memcpy(d->cwp_matrix, matrix_cat_d65_to_d60, sizeof(dt_colormatrix_t));
     else if (p->cwp == DT_OPENDRT_WP_D50) memcpy(d->cwp_matrix, matrix_cat_d65_to_d50, sizeof(dt_colormatrix_t));
     else if (p->cwp == DT_OPENDRT_WP_D55) memcpy(d->cwp_matrix, matrix_cat_d65_to_d55, sizeof(dt_colormatrix_t));
     else if (p->cwp == DT_OPENDRT_WP_D75) memcpy(d->cwp_matrix, matrix_cat_d65_to_d75, sizeof(dt_colormatrix_t));
     else if (p->cwp == DT_OPENDRT_WP_D93) memcpy(d->cwp_matrix, matrix_cat_d65_to_d93, sizeof(dt_colormatrix_t));
     else for(int i=0; i<4; i++) for(int j=0; j<4; j++) d->cwp_matrix[i][j] = (i==j) ? 1.f : 0.f;
  }
}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_opendrt_data_t *data = (dt_iop_opendrt_data_t *)piece->data;
  const float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  // Unpack data for performance
  const float ts_s1 = data->ts_s1;
  const float ts_p = data->ts_p;
  const float ts_s = data->ts_s;
  const float s_Lp100 = data->s_Lp100;
  const float ts_m2 = data->ts_m2;
  const float ts_dsc = data->ts_dsc;
  const float tn_off = data->tn_off;
  const float rs_sa = data->rs_sa;
  const float rs_rw = data->rs_rw;
  const float rs_bw = data->rs_bw;
  const float tn_toe = data->tn_toe;

  const float cwp_lm = data->cwp_lm;
  const int cwp = data->cwp_mode;

  // Purity and Hue constants
  const float brl = data->brl, brl_r = data->brl_r, brl_g = data->brl_g, brl_b = data->brl_b;
  const float brl_rng = data->brl_rng, brl_st = data->brl_st;
  const float brlp = data->brlp, brlp_r = data->brlp_r, brlp_g = data->brlp_g, brlp_b = data->brlp_b;
  const float hc_r = data->hc_r, hc_r_rng = data->hc_r_rng;
  const float hs_r = data->hs_r, hs_r_rng = data->hs_r_rng, hs_g = data->hs_g, hs_g_rng = data->hs_g_rng;
  const float hs_b = data->hs_b, hs_b_rng = data->hs_b_rng;
  const float hs_c = data->hs_c, hs_c_rng = data->hs_c_rng, hs_m = data->hs_m, hs_m_rng = data->hs_m_rng;
  const float hs_y = data->hs_y, hs_y_rng = data->hs_y_rng;
  const float pt_lml = data->pt_lml, pt_lml_r = data->pt_lml_r, pt_lml_g = data->pt_lml_g, pt_lml_b = data->pt_lml_b;
  const float pt_lmh = data->pt_lmh, pt_lmh_r = data->pt_lmh_r, pt_lmh_b = data->pt_lmh_b;
  const float ptm_low = data->ptm_low, ptm_low_rng = data->ptm_low_rng, ptm_low_st = data->ptm_low_st;
  const float ptm_high = data->ptm_high, ptm_high_rng = data->ptm_high_rng, ptm_high_st = data->ptm_high_st;
  const float ptl_c = data->ptl_c, ptl_m = data->ptl_m, ptl_y = data->ptl_y;

  const int brl_enable = data->brl_enable;
  const int brlp_enable = data->brlp_enable;
  const int hc_enable = data->hc_enable;
  const int hs_rgb_enable = data->hs_rgb_enable;
  const int hs_cmy_enable = data->hs_cmy_enable;
  const int ptm_enable = data->ptm_enable;
  const int ptl_enable = data->ptl_enable;
  const int pt_enable = data->pt_enable;
  const int tn_lcon_enable = data->tn_lcon_enable;
  const int tn_hcon_enable = data->tn_hcon_enable;

  // Matrices
  dt_colormatrix_t in_to_p3, p3_to_out, cwp_mat, p3_to_xyz;
  dt_colormatrix_transpose(in_to_p3, data->pipe_to_p3d65);
  dt_colormatrix_transpose(p3_to_out, data->p3d65_to_pipe);
  dt_colormatrix_transpose(cwp_mat, data->cwp_matrix);
  dt_colormatrix_transpose(p3_to_xyz, matrix_p3d65_to_xyz);

  #pragma omp parallel for schedule(static)
  for(int k = 0; k < width * height; k++)
  {
    const int i = k * 4;
    float r = in[i];
    float g = in[i+1];
    float b = in[i+2];

    // Input Gamut -> P3-D65
    float r_p3 = in_to_p3[0][0]*r + in_to_p3[0][1]*g + in_to_p3[0][2]*b;
    float g_p3 = in_to_p3[1][0]*r + in_to_p3[1][1]*g + in_to_p3[1][2]*b;
    float b_p3 = in_to_p3[2][0]*r + in_to_p3[2][1]*g + in_to_p3[2][2]*b;

    r = r_p3; g = g_p3; b = b_p3;

    // Render Space Desaturation
    float sat_L = r * rs_rw + g * (1.0f - rs_rw - rs_bw) + b * rs_bw;
    r = sat_L * rs_sa + r * (1.0f - rs_sa);
    g = sat_L * rs_sa + g * (1.0f - rs_sa);
    b = sat_L * rs_sa + b * (1.0f - rs_sa);

    // Offset
    r += tn_off;
    g += tn_off;
    b += tn_off;

    // Tonescale Norm
    float tsn = sqrtf(fmaxf(0.0f, r*r + g*g + b*b)) / SQRT3;
    if (tsn < 1e-9f) tsn = 1e-9f;

    // RGB Ratios
    r /= tsn; g /= tsn; b /= tsn;

    // Opponent space
    float ox, oy;
    opponent(r, g, b, &ox, &oy);
    float ach_d = hypotf(ox, oy) / 2.0f;

    // Smooth ach_d
    ach_d = 1.25f * compress_toe_quadratic(ach_d, 0.25f, 0);

    // Hue angle
    float hue = fmodf(atan2f(ox, oy) + M_PI_F + 1.10714931f, 2.0f * M_PI_F);

    // RGB Hue Angles
    float ha_rgb_x = gauss_window(hue_offset(hue, 0.1f), 0.66f);
    float ha_rgb_y = gauss_window(hue_offset(hue, 4.3f), 0.66f);
    float ha_rgb_z = gauss_window(hue_offset(hue, 2.3f), 0.66f);

    // RGB Hue Angles for hue shift
    float ha_rgb_hs_x = gauss_window(hue_offset(hue, -0.4f), 0.66f);
    float ha_rgb_hs_y = ha_rgb_y;
    float ha_rgb_hs_z = gauss_window(hue_offset(hue, 2.5f), 0.66f);

    // CMY Hue Angles
    float ha_cmy_x = gauss_window(hue_offset(hue, 3.3f), 0.5f);
    float ha_cmy_y = gauss_window(hue_offset(hue, 1.3f), 0.5f);
    float ha_cmy_z = gauss_window(hue_offset(hue, -1.15f), 0.5f);

    // Brilliance
    if(brl_enable) {
      float brl_tsf = powf(tsn / (tsn + 1.0f), 1.0f - brl_rng);
      float brl_exf = (brl + brl_r * ha_rgb_x + brl_g * ha_rgb_y + brl_b * ha_rgb_z) * powf(ach_d, 1.0f / brl_st);
      float brl_ex = powf(2.0f, brl_exf * (brl_exf < 0.0f ? brl_tsf : 1.0f - brl_tsf));
      tsn *= brl_ex;
    }

    // Contrast Low
    if (tn_lcon_enable) {
        float lcon_m = powf(2.0f, -data->tn_lcon);
        float lcon_w = data->tn_lcon_w/4.0f;
        lcon_w *= lcon_w;
        const float lcon_cnst_sc = compress_toe_cubic(ts_x0, lcon_m, lcon_w, 1)/ts_x0;
        tsn *= lcon_cnst_sc;
        tsn = compress_toe_cubic(tsn, lcon_m, lcon_w, 0);
    }

    // Contrast High
    if (tn_hcon_enable) {
        float hcon_p = powf(2.0f, data->tn_hcon);
        tsn = contrast_high(tsn, hcon_p, data->tn_hcon_pv, data->tn_hcon_st, 0);
    }

    // Hyperbolic Compression
    float tsn_pt = compress_hyperbolic_power(tsn, ts_s1, ts_p);
    float tsn_const = compress_hyperbolic_power(tsn, s_Lp100, ts_p);
    tsn = compress_hyperbolic_power(tsn, ts_s, ts_p);

    // Hue Contrast R
    if(hc_enable) {
      float hc_ts = 1.0f - tsn_const;
      float hc_c = hc_ts * (1.0f - ach_d) + ach_d * (1.0f - hc_ts);
      hc_c *= ach_d * ha_rgb_x;
      hc_ts = powf(hc_ts, 1.0f / hc_r_rng);
      float hc_f = hc_r * (hc_c - 2.0f * hc_c * hc_ts) + 1.0f;
      g *= hc_f;
      b *= hc_f;
    }

    // Hue Shift RGB
    if(hs_rgb_enable) {
      float hs_rgb_x = ha_rgb_hs_x * ach_d * spowf(tsn_pt, 1.0f / hs_r_rng);
      float hs_rgb_y = ha_rgb_hs_y * ach_d * spowf(tsn_pt, 1.0f / hs_g_rng);
      float hs_rgb_z = ha_rgb_hs_z * ach_d * spowf(tsn_pt, 1.0f / hs_b_rng);

      float hsf_x = hs_rgb_x * hs_r;
      float hsf_y = hs_rgb_y * -hs_g;
      float hsf_z = hs_rgb_z * -hs_b;

      r += hsf_z - hsf_y;
      g += hsf_x - hsf_z;
      b += hsf_y - hsf_x;
    }

    // Hue Shift CMY
    if(hs_cmy_enable) {
      float tsn_pt_compl = 1.0f - tsn_pt;
      float hs_cmy_x = ha_cmy_x * ach_d * spowf(tsn_pt_compl, 1.0f / hs_c_rng);
      float hs_cmy_y = ha_cmy_y * ach_d * spowf(tsn_pt_compl, 1.0f / hs_m_rng);
      float hs_cmy_z = ha_cmy_z * ach_d * spowf(tsn_pt_compl, 1.0f / hs_y_rng);

      float hsf_x = hs_cmy_x * -hs_c;
      float hsf_y = hs_cmy_y * hs_m;
      float hsf_z = hs_cmy_z * hs_y;

      r += hsf_z - hsf_y;
      g += hsf_x - hsf_z;
      b += hsf_y - hsf_x;
    }

    // Purity Compression
    float pt_lml_p = 1.0f + 4.0f * (1.0f - tsn_pt) * (pt_lml + pt_lml_r * ha_rgb_hs_x + pt_lml_g * ha_rgb_hs_y + pt_lml_b * ha_rgb_hs_z);
    float ptf = 1.0f - spowf(tsn_pt, pt_lml_p);

    if (pt_enable) {
        float pt_lmh_p = (1.0f - ach_d * (pt_lmh_r * ha_rgb_hs_x + pt_lmh_b * ha_rgb_hs_z)) * (1.0f - pt_lmh * ach_d);
        ptf = spowf(ptf, pt_lmh_p);
    }

    if(ptm_enable) {
        float ptm_low_f = 1.0f + ptm_low * expf(-2.0f * ach_d * ach_d / ptm_low_st) * spowf(1.0f - tsn_const, 1.0f / ptm_low_rng);
        float ptm_high_f = 1.0f + ptm_high * expf(-2.0f * ach_d * ach_d / ptm_high_st) * spowf(tsn_pt, 1.0f / (4.0f * ptm_high_rng));
        ptf *= ptm_low_f * ptm_high_f;
    }

    // Lerp to peak achromatic
    r = r * ptf + 1.0f - ptf;
    g = g * ptf + 1.0f - ptf;
    b = b * ptf + 1.0f - ptf;

    // Inverse Rendering Space
    sat_L = r * rs_rw + g * (1.0f - rs_rw - rs_bw) + b * rs_bw;
    r = (sat_L * rs_sa - r) / (rs_sa - 1.0f);
    g = (sat_L * rs_sa - g) / (rs_sa - 1.0f);
    b = (sat_L * rs_sa - b) / (rs_sa - 1.0f);

    // Display Gamut Whitepoint Logic
    float x_xyz = p3_to_xyz[0][0]*r + p3_to_xyz[0][1]*g + p3_to_xyz[0][2]*b;
    float y_xyz = p3_to_xyz[1][0]*r + p3_to_xyz[1][1]*g + p3_to_xyz[1][2]*b;
    float z_xyz = p3_to_xyz[2][0]*r + p3_to_xyz[2][1]*g + p3_to_xyz[2][2]*b;

    float r_cwp = x_xyz, g_cwp = y_xyz, b_cwp = z_xyz;

    float r_target = cwp_mat[0][0]*x_xyz + cwp_mat[0][1]*y_xyz + cwp_mat[0][2]*z_xyz;
    float g_target = cwp_mat[1][0]*x_xyz + cwp_mat[1][1]*y_xyz + cwp_mat[1][2]*z_xyz;
    float b_target = cwp_mat[2][0]*x_xyz + cwp_mat[2][1]*y_xyz + cwp_mat[2][2]*z_xyz;

    float cwp_f = powf(tsn_const, 2.0f * cwp_lm);

    x_xyz = r_target * cwp_f + r_cwp * (1.0f - cwp_f);
    y_xyz = g_target * cwp_f + g_cwp * (1.0f - cwp_f);
    z_xyz = b_target * cwp_f + b_cwp * (1.0f - cwp_f);

    r = matrix_xyz_to_p3d65[0][0]*x_xyz + matrix_xyz_to_p3d65[0][1]*y_xyz + matrix_xyz_to_p3d65[0][2]*z_xyz;
    g = matrix_xyz_to_p3d65[1][0]*x_xyz + matrix_xyz_to_p3d65[1][1]*y_xyz + matrix_xyz_to_p3d65[1][2]*z_xyz;
    b = matrix_xyz_to_p3d65[2][0]*x_xyz + matrix_xyz_to_p3d65[2][1]*y_xyz + matrix_xyz_to_p3d65[2][2]*z_xyz;

    float cwp_norm = 1.0f;
    if (cwp != DT_OPENDRT_WP_D65) {
        if (cwp == DT_OPENDRT_WP_D93) cwp_norm = 0.762687057298f;
        else if (cwp == DT_OPENDRT_WP_D75) cwp_norm = 0.884054083328f;
        else if (cwp == DT_OPENDRT_WP_D60) cwp_norm = 0.964320186739f;
        else if (cwp == DT_OPENDRT_WP_D55) cwp_norm = 0.923076518860f;
        else if (cwp == DT_OPENDRT_WP_D50) cwp_norm = 0.876572837784f;
    }

    float norm_factor = cwp_norm * cwp_f + 1.0f - cwp_f;
    r *= norm_factor;
    g *= norm_factor;
    b *= norm_factor;

    // Post Brilliance
    if (brlp_enable) {
      float ox_p, oy_p;
      opponent(r, g, b, &ox_p, &oy_p);
      float brlp_ach_d = hypotf(ox_p, oy_p) / 4.0f;
      brlp_ach_d = 1.1f * (brlp_ach_d * brlp_ach_d / (brlp_ach_d + 0.1f));
      float brlp_m = brlp + brlp_r * ach_d * ha_rgb_x + brlp_g * ach_d * ha_rgb_y + brlp_b * ach_d * ha_rgb_z;
      float brlp_ex = spowf(2.0f, brlp_m * brlp_ach_d * tsn);
      r *= brlp_ex; g *= brlp_ex; b *= brlp_ex;
    }

    // Purity Compress Low
    if (ptl_enable) {
        r = softplus(r, ptl_c);
        g = softplus(g, ptl_m);
        b = softplus(b, ptl_y);
    }

    // Final tonescale adjustments
    tsn *= ts_m2;
    tsn = compress_toe_quadratic(tsn, tn_toe, 0);
    tsn *= ts_dsc;

    // Return from RGB ratios
    r *= tsn; g *= tsn; b *= tsn;

    // Convert to Output
    float r_out = p3_to_out[0][0]*r + p3_to_out[0][1]*g + p3_to_out[0][2]*b;
    float g_out = p3_to_out[1][0]*r + p3_to_out[1][1]*g + p3_to_out[1][2]*b;
    float b_out = p3_to_out[2][0]*r + p3_to_out[2][1]*g + p3_to_out[2][2]*b;

    out[i] = r_out;
    out[i+1] = g_out;
    out[i+2] = b_out;
    out[i+3] = in[i+3];
  }
}

void gui_init(dt_iop_module_t *self)
{
  GtkWidget *vbox = dt_gui_vbox();

  GtkWidget *g = dt_ui_section_label_new(_("tonescale"));
  gtk_box_pack_start(GTK_BOX(vbox), g, FALSE, FALSE, 0);

  GtkWidget *w = dt_bauhaus_slider_from_params(self, "tn_Lp");
  gtk_box_pack_start(GTK_BOX(vbox), w, FALSE, FALSE, 0);

  w = dt_bauhaus_slider_from_params(self, "tn_Lg");
  gtk_box_pack_start(GTK_BOX(vbox), w, FALSE, FALSE, 0);

  w = dt_bauhaus_slider_from_params(self, "tn_gb");
  gtk_box_pack_start(GTK_BOX(vbox), w, FALSE, FALSE, 0);

  w = dt_bauhaus_slider_from_params(self, "pt_hdr");
  gtk_box_pack_start(GTK_BOX(vbox), w, FALSE, FALSE, 0);

  g = dt_ui_section_label_new(_("look"));
  gtk_box_pack_start(GTK_BOX(vbox), g, FALSE, FALSE, 0);

  w = dt_bauhaus_slider_from_params(self, "contrast");
  gtk_box_pack_start(GTK_BOX(vbox), w, FALSE, FALSE, 0);

  w = dt_bauhaus_slider_from_params(self, "saturation");
  gtk_box_pack_start(GTK_BOX(vbox), w, FALSE, FALSE, 0);

  w = dt_bauhaus_combobox_from_params(self, "cwp");
  gtk_box_pack_start(GTK_BOX(vbox), w, FALSE, FALSE, 0);

  w = dt_bauhaus_slider_from_params(self, "cwp_lm");
  gtk_box_pack_start(GTK_BOX(vbox), w, FALSE, FALSE, 0);

  self->widget = vbox;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_opendrt_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}
