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
#include "common/math.h"
#include "common/matrices.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <math.h>

DT_MODULE_INTROSPECTION(1, dt_iop_opendrt_params_t)

/* ---------------------------------------------------------------------------
 * Parameters
 * ------------------------------------------------------------------------ */

typedef enum dt_iop_opendrt_whitepoint_t
{
  DT_OPENDRT_WP_D93 = 0,
  DT_OPENDRT_WP_D75 = 1,
  DT_OPENDRT_WP_D65 = 2,
  DT_OPENDRT_WP_D60 = 3,
  DT_OPENDRT_WP_D55 = 4,
  DT_OPENDRT_WP_D50 = 5,
} dt_iop_opendrt_whitepoint_t;

typedef struct dt_iop_opendrt_params_t
{
  // Tonescale Parameters
  float tn_Lp;  // $MIN: 100.0 $MAX: 4000.0 $DEFAULT: 100.0 $DESCRIPTION: "display peak luminance (nits)"
  float tn_Lg;  // $MIN: 3.0   $MAX: 50.0   $DEFAULT: 10.0  $DESCRIPTION: "display grey luminance (nits)"
  float tn_gb;  // $MIN: 0.0   $MAX: 1.0    $DEFAULT: 0.13  $DESCRIPTION: "hdr grey boost"
  float pt_hdr; // $MIN: 0.0   $MAX: 1.0    $DEFAULT: 0.5   $DESCRIPTION: "hdr purity"

  // Look Parameters
  float contrast;   // $MIN: 1.0 $MAX: 2.0 $DEFAULT: 1.66 $DESCRIPTION: "contrast"
  float saturation; // $MIN: 0.0 $MAX: 0.6 $DEFAULT: 0.35 $DESCRIPTION: "saturation"

  // Creative White Point
  dt_iop_opendrt_whitepoint_t cwp; // $DEFAULT: DT_OPENDRT_WP_D65 $DESCRIPTION: "creative white point"
  float cwp_lm;                    // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.25 $DESCRIPTION: "creative white range"
} dt_iop_opendrt_params_t;

/* ---------------------------------------------------------------------------
 * Internal Data and Math
 * ------------------------------------------------------------------------ */

typedef struct dt_iop_opendrt_data_t
{
  // Tonescale constraints
  float ts_s1;
  float ts_p;
  float ts_s;
  float s_Lp100;
  float tsn_norm_factor; // combined normalization factors
  float ts_m2;
  float ts_dsc;
  float tsn_min_val;

  // Look params (Standard preset values + overrides)
  float rs_sa;
  float rs_rw;
  float rs_bw;
  float tn_off;

  // Hue/Purity params
  float hc_r, hc_r_rng;
  float hs_r, hs_r_rng, hs_g, hs_g_rng, hs_b, hs_b_rng;
  float hs_c, hs_c_rng, hs_m, hs_m_rng, hs_y, hs_y_rng;
  float pt_lml, pt_lml_r, pt_lml_g, pt_lml_b;
  float pt_lmh, pt_lmh_r, pt_lmh_b;
  float ptl_c, ptl_m, ptl_y;
  float ptm_low, ptm_low_rng, ptm_low_st;
  float ptm_high, ptm_high_rng, ptm_high_st;
  float brl, brl_r, brl_g, brl_b, brl_rng, brl_st;
  int brlp_enable;
  float brlp, brlp_r, brlp_g, brlp_b;

  // Matrices
  dt_colormatrix_t pipe_to_p3d65; // Input conversion
  dt_colormatrix_t p3d65_to_pipe; // Output conversion (via XYZ)
  dt_colormatrix_t cwp_matrix;    // Creative white point adaptation matrix

  float cwp_lm;
  int cwp_mode; // 0=D93 ... 5=D50
} dt_iop_opendrt_data_t;

// Math constants
#define SQRT3 1.73205080756887729353f
#define M_PI_F 3.14159265358979323846f

// Helper functions ported from DCTL
static inline float spowf(float a, float b)
{
  return (a <= 0.0f) ? a : powf(a, b);
}

static inline float compress_hyperbolic_power(float x, float s, float p)
{
  return spowf(x / (x + s), p);
}

static inline float compress_toe_quadratic(float x, float toe, int inv)
{
  if (toe == 0.0f) return x;
  if (inv == 0) {
    return spowf(x, 2.0f) / (x + toe);
  } else {
    return (x + sqrtf(x * (4.0f * toe + x))) / 2.0f;
  }
}

static inline float softplus(float x, float s)
{
  if (x > 10.0f * s || s < 1e-4f) return x;
  return s * logf(fmaxf(0.0f, 1.0f + expf(x / s)));
}

static inline float gauss_window(float x, float w)
{
  return expf(-x * x / w);
}

static inline void opponent(const float r, const float g, const float b, float *ox, float *oy)
{
  *ox = r - b;
  *oy = g - (r + b) / 2.0f;
}

static inline float hue_offset(float h, float o)
{
  return fmodf(h - o + M_PI_F, 2.0f * M_PI_F) - M_PI_F;
}

// Matrices from DCTL
// D65 to DXX CAT02
static const dt_colormatrix_t matrix_cat_d65_to_d93 = {
  { 0.95703423f, -0.02471715f, 0.06240286f, 0.f}, {-0.01792970f, 0.99001986f, 0.02481195f, 0.f}, { 0.00127589f, 0.00427919f, 1.29345715f, 0.f}
};
static const dt_colormatrix_t matrix_cat_d65_to_d75 = {
  { 0.98100108f, -0.01166193f, 0.02656141f, 0.f}, {-0.00843488f, 0.99650609f, 0.01056965f, 0.f}, { 0.00055281f, 0.00179841f, 1.12374723f, 0.f}
};
static const dt_colormatrix_t matrix_cat_d65_to_d60 = {
  { 1.01182246f, 0.00778879f, -0.01577830f, 0.f}, { 0.00561683f, 1.00150645f, -0.00628518f, 0.f}, {-0.00033574f, -0.00105095f, 0.92736667f, 0.f}
};
static const dt_colormatrix_t matrix_cat_d65_to_d55 = {
  { 1.02585089f, 0.01794398f, -0.03321378f, 0.f}, { 0.01291339f, 1.00214779f, -0.01324210f, 0.f}, {-0.00071994f, -0.00218107f, 0.84868014f, 0.f}
};
static const dt_colormatrix_t matrix_cat_d65_to_d50 = {
  { 1.04257405f, 0.03089118f, -0.05281262f, 0.f}, { 0.02219354f, 1.00185668f, -0.02107376f, 0.f}, {-0.00116488f, -0.00342053f, 0.76178908f, 0.f}
};

// XYZ D65 <-> P3 D65
static const dt_colormatrix_t matrix_p3d65_to_xyz = {
  { 0.48657095f, 0.26566769f, 0.19821729f, 0.f}, { 0.22897456f, 0.69173852f, 0.07928691f, 0.f}, { 0.00000000f, 0.04511338f, 1.04394437f, 0.f}
};
static const dt_colormatrix_t matrix_xyz_to_p3d65 = {
  { 2.49349691f, -0.93138362f, -0.40271078f, 0.f}, {-0.82948897f, 1.76266406f, 0.02362469f, 0.f}, { 0.03584583f, -0.07617239f, 0.95688452f, 0.f}
};

// Pipeline D50 to XYZ D65 (CAT16)
// Used to bridge DT pipeline to OpenDRT internal space
static const dt_colormatrix_t matrix_xyz_d50_to_d65_cat16 = {
  { 0.98946625f, -0.04003046f, 0.04405303f, 0.f}, {-0.00540519f, 1.00666069f, -0.00175552f, 0.f}, {-0.00040392f, 0.01507680f, 1.30210211f, 0.f}
};
static const dt_colormatrix_t matrix_xyz_d65_to_d50_cat16 = {
  { 1.01085433f, 0.04070861f, -0.03414458f, 0.f}, { 0.00542814f, 0.99358193f, 0.00115592f, 0.f}, { 0.00025072f, -0.01149188f, 0.76796495f, 0.f}
};

/* ---------------------------------------------------------------------------
 * Process
 * ------------------------------------------------------------------------ */

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
  const float tn_toe = 0.003f; // Hardcoded from Standard preset for now

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

  // Matrices
  dt_colormatrix_t in_to_p3, p3_to_out, cwp_mat, p3_to_xyz;
  // Transpose for vector math: out = M * in -> out[i] = dot(row[i], in)
  // dt_colormatrix_t is float[4][4]

  // Prepare transposed matrices for dt_apply_transposed_color_matrix
  dt_colormatrix_transpose(in_to_p3, data->pipe_to_p3d65);
  dt_colormatrix_transpose(p3_to_out, data->p3d65_to_pipe);
  dt_colormatrix_transpose(cwp_mat, data->cwp_matrix);
  dt_colormatrix_transpose(p3_to_xyz, matrix_p3d65_to_xyz); // Needed for CWP math

  #pragma omp parallel for schedule(static)
  for(int k = 0; k < width * height; k++)
  {
    const int i = k * 4;
    float r = in[i];
    float g = in[i+1];
    float b = in[i+2];

    // Input Gamut -> P3-D65
    // Manual matrix mult because we need to process pixel by pixel
    float r_p3 = in_to_p3[0][0]*r + in_to_p3[0][1]*g + in_to_p3[0][2]*b;
    float g_p3 = in_to_p3[1][0]*r + in_to_p3[1][1]*g + in_to_p3[1][2]*b;
    float b_p3 = in_to_p3[2][0]*r + in_to_p3[2][1]*g + in_to_p3[2][2]*b;

    r = r_p3; g = g_p3; b = b_p3;

    // Render Space Desaturation
    // float3 rs_w = make_float3(rs_rw, 1.0f - rs_rw - rs_bw, rs_bw);
    float rs_wg = 1.0f - rs_rw - rs_bw;
    float sat_L = r * rs_rw + g * rs_wg + b * rs_bw;
    r = sat_L * rs_sa + r * (1.0f - rs_sa);
    g = sat_L * rs_sa + g * (1.0f - rs_sa);
    b = sat_L * rs_sa + b * (1.0f - rs_sa);

    // Offset
    r += tn_off;
    g += tn_off;
    b += tn_off;

    // Tonescale Norm
    float tsn = sqrtf(fmaxf(0.0f, r*r + g*g + b*b)) / SQRT3;

    // Avoid division by zero
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
    {
      float brl_tsf = powf(tsn / (tsn + 1.0f), 1.0f - brl_rng);
      float brl_exf = (brl + brl_r * ha_rgb_x + brl_g * ha_rgb_y + brl_b * ha_rgb_z) * powf(ach_d, 1.0f / brl_st);
      float brl_ex = powf(2.0f, brl_exf * (brl_exf < 0.0f ? brl_tsf : 1.0f - brl_tsf));
      tsn *= brl_ex;
    }

    // Hyperbolic Compression (Tonescale Application)
    float tsn_pt = compress_hyperbolic_power(tsn, ts_s1, ts_p);
    float tsn_const = compress_hyperbolic_power(tsn, s_Lp100, ts_p);
    tsn = compress_hyperbolic_power(tsn, ts_s, ts_p);

    // Hue Contrast R
    {
      float hc_ts = 1.0f - tsn_const;
      float hc_c = hc_ts * (1.0f - ach_d) + ach_d * (1.0f - hc_ts);
      hc_c *= ach_d * ha_rgb_x;
      hc_ts = powf(hc_ts, 1.0f / hc_r_rng);
      float hc_f = hc_r * (hc_c - 2.0f * hc_c * hc_ts) + 1.0f;
      g *= hc_f;
      b *= hc_f;
    }

    // Hue Shift RGB
    {
      float hs_rgb_x = ha_rgb_hs_x * ach_d * spowf(tsn_pt, 1.0f / hs_r_rng);
      float hs_rgb_y = ha_rgb_hs_y * ach_d * spowf(tsn_pt, 1.0f / hs_g_rng);
      float hs_rgb_z = ha_rgb_hs_z * ach_d * spowf(tsn_pt, 1.0f / hs_b_rng);

      float hsf_x = hs_rgb_x * hs_r;
      float hsf_y = hs_rgb_y * -hs_g;
      float hsf_z = hs_rgb_z * -hs_b;

      float rot_x = hsf_z - hsf_y;
      float rot_y = hsf_x - hsf_z;
      float rot_z = hsf_y - hsf_x;

      r += rot_x; g += rot_y; b += rot_z;
    }

    // Hue Shift CMY
    {
      float tsn_pt_compl = 1.0f - tsn_pt;
      float hs_cmy_x = ha_cmy_x * ach_d * spowf(tsn_pt_compl, 1.0f / hs_c_rng);
      float hs_cmy_y = ha_cmy_y * ach_d * spowf(tsn_pt_compl, 1.0f / hs_m_rng);
      float hs_cmy_z = ha_cmy_z * ach_d * spowf(tsn_pt_compl, 1.0f / hs_y_rng);

      float hsf_x = hs_cmy_x * -hs_c;
      float hsf_y = hs_cmy_y * hs_m;
      float hsf_z = hs_cmy_z * hs_y;

      float rot_x = hsf_z - hsf_y;
      float rot_y = hsf_x - hsf_z;
      float rot_z = hsf_y - hsf_x;

      r += rot_x; g += rot_y; b += rot_z;
    }

    // Purity Compression
    // Limit Low
    float pt_lml_p = 1.0f + 4.0f * (1.0f - tsn_pt) * (pt_lml + pt_lml_r * ha_rgb_hs_x + pt_lml_g * ha_rgb_hs_y + pt_lml_b * ha_rgb_hs_z);
    float ptf = 1.0f - spowf(tsn_pt, pt_lml_p);

    // Limit High
    float pt_lmh_p = (1.0f - ach_d * (pt_lmh_r * ha_rgb_hs_x + pt_lmh_b * ha_rgb_hs_z)) * (1.0f - pt_lmh * ach_d);
    ptf = spowf(ptf, pt_lmh_p);

    // Mid-Range Purity
    float ptm_low_f = 1.0f + ptm_low * expf(-2.0f * ach_d * ach_d / ptm_low_st) * spowf(1.0f - tsn_const, 1.0f / ptm_low_rng);
    float ptm_high_f = 1.0f + ptm_high * expf(-2.0f * ach_d * ach_d / ptm_high_st) * spowf(tsn_pt, 1.0f / (4.0f * ptm_high_rng));
    ptf *= ptm_low_f * ptm_high_f;

    // Lerp to peak achromatic
    r = r * ptf + 1.0f - ptf;
    g = g * ptf + 1.0f - ptf;
    b = b * ptf + 1.0f - ptf;

    // Inverse Rendering Space
    sat_L = r * rs_rw + g * rs_wg + b * rs_bw;
    r = (sat_L * rs_sa - r) / (rs_sa - 1.0f);
    g = (sat_L * rs_sa - g) / (rs_sa - 1.0f);
    b = (sat_L * rs_sa - b) / (rs_sa - 1.0f);

    // Display Gamut Whitepoint Logic (inline)
    // 1. Convert to XYZ D65
    float x_xyz = p3_to_xyz[0][0]*r + p3_to_xyz[0][1]*g + p3_to_xyz[0][2]*b;
    float y_xyz = p3_to_xyz[1][0]*r + p3_to_xyz[1][1]*g + p3_to_xyz[1][2]*b;
    float z_xyz = p3_to_xyz[2][0]*r + p3_to_xyz[2][1]*g + p3_to_xyz[2][2]*b;

    float r_cwp = x_xyz, g_cwp = y_xyz, b_cwp = z_xyz;

    // 2. Creative White Point Adjustment
    // If CWP is not D65, we adapt.
    // NOTE: The DCTL simplifies this by assuming target gamut is P3D65 for most cases
    // We will perform the mix here.
    // cwp_matrix transforms XYZ_D65 -> XYZ_D65_Adapted (roughly)

    float r_target = cwp_mat[0][0]*x_xyz + cwp_mat[0][1]*y_xyz + cwp_mat[0][2]*z_xyz;
    float g_target = cwp_mat[1][0]*x_xyz + cwp_mat[1][1]*y_xyz + cwp_mat[1][2]*z_xyz;
    float b_target = cwp_mat[2][0]*x_xyz + cwp_mat[2][1]*y_xyz + cwp_mat[2][2]*z_xyz;

    float cwp_f = powf(tsn_const, 2.0f * cwp_lm);

    // Mix
    x_xyz = r_target * cwp_f + r_cwp * (1.0f - cwp_f);
    y_xyz = g_target * cwp_f + g_cwp * (1.0f - cwp_f);
    z_xyz = b_target * cwp_f + b_cwp * (1.0f - cwp_f);

    // 3. Convert back to P3D65 for post-processing (OpenDRT working space)
    // Inverse of p3_to_xyz (which is matrix_xyz_to_p3d65)
    // Note: In DCTL, it converts to *Final Display Gamut*.
    // Here we assume we want to return to Pipe RGB eventually.
    // But Post Brilliance and Softclip happen in "RGB". OpenDRT assumes P3D65.
    // So we convert XYZ -> P3D65.

    r = matrix_xyz_to_p3d65[0][0]*x_xyz + matrix_xyz_to_p3d65[0][1]*y_xyz + matrix_xyz_to_p3d65[0][2]*z_xyz;
    g = matrix_xyz_to_p3d65[1][0]*x_xyz + matrix_xyz_to_p3d65[1][1]*y_xyz + matrix_xyz_to_p3d65[1][2]*z_xyz;
    b = matrix_xyz_to_p3d65[2][0]*x_xyz + matrix_xyz_to_p3d65[2][1]*y_xyz + matrix_xyz_to_p3d65[2][2]*z_xyz;

    // CWP Normalization
    float cwp_norm = 1.0f;
    if (cwp != DT_OPENDRT_WP_D65) { // Simplified check, full table in DCTL
        // For P3D65 target
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

    const int brlp_enable = data->brlp_enable;
    // Post Brilliance
    if (brlp_enable) { // Assuming enabled for standard preset
      float ox_p, oy_p;
      opponent(r, g, b, &ox_p, &oy_p);
      float brlp_ach_d = hypotf(ox_p, oy_p) / 4.0f;
      brlp_ach_d = 1.1f * (brlp_ach_d * brlp_ach_d / (brlp_ach_d + 0.1f));
      float brlp_m = brlp + brlp_r * ach_d * ha_rgb_x + brlp_g * ach_d * ha_rgb_y + brlp_b * ach_d * ha_rgb_z;
      float brlp_ex = powf(2.0f, brlp_m * brlp_ach_d * tsn);
      r *= brlp_ex; g *= brlp_ex; b *= brlp_ex;
    }

    // Purity Compress Low
    r = softplus(r, ptl_c);
    g = softplus(g, ptl_m);
    b = softplus(b, ptl_y);

    // Final tonescale adjustments
    tsn *= ts_m2;
    tsn = compress_toe_quadratic(tsn, tn_toe, 0);
    tsn *= ts_dsc;

    // Return from RGB ratios
    r *= tsn; g *= tsn; b *= tsn;

    // Convert to Output (Pipeline RGB D50)
    // Current state: P3 D65 Linear
    // Target: Pipe D50 Linear
    // We reuse the p3_to_out matrix calculated in commit_params
    // Note: The DCTL output stage includes EOTF. We skip EOTF to stay linear for darktable.

    float r_out = p3_to_out[0][0]*r + p3_to_out[0][1]*g + p3_to_out[0][2]*b;
    float g_out = p3_to_out[1][0]*r + p3_to_out[1][1]*g + p3_to_out[1][2]*b;
    float b_out = p3_to_out[2][0]*r + p3_to_out[2][1]*g + p3_to_out[2][2]*b;

    out[i] = r_out;
    out[i+1] = g_out;
    out[i+2] = b_out;
    out[i+3] = in[i+3]; // Alpha
  }
}

/* ---------------------------------------------------------------------------
 * Module Interface
 * ------------------------------------------------------------------------ */

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_opendrt_params_t *p = (dt_iop_opendrt_params_t *)p1;
  dt_iop_opendrt_data_t *d = (dt_iop_opendrt_data_t *)piece->data;

  // Standard Look Preset values
  d->rs_sa = p->saturation; // User controlled
  d->rs_rw = 0.25f;
  d->rs_bw = 0.55f;
  d->tn_off = 0.005f;

  d->hc_r = 1.0f; d->hc_r_rng = 0.3f;
  d->hs_r = 0.6f; d->hs_r_rng = 0.7f;
  d->hs_g = 0.35f; d->hs_g_rng = 1.0f;
  d->hs_b = 0.66f; d->hs_b_rng = 1.0f;
  d->hs_c = 0.25f; d->hs_c_rng = 1.0f;
  d->hs_m = 0.0f; d->hs_m_rng = 1.0f;
  d->hs_y = 0.0f; d->hs_y_rng = 1.0f;

  d->pt_lml = 0.25f; d->pt_lml_r = 0.5f; d->pt_lml_g = 0.0f; d->pt_lml_b = 0.1f;
  d->pt_lmh = 0.25f; d->pt_lmh_r = 0.5f; d->pt_lmh_b = 0.0f;

  d->ptl_c = 0.06f; d->ptl_m = 0.08f; d->ptl_y = 0.06f;
  d->ptm_low = 0.5f; d->ptm_low_rng = 0.25f; d->ptm_low_st = 0.5f;
  d->ptm_high = -0.8f; d->ptm_high_rng = 0.3f; d->ptm_high_st = 0.4f;

  d->brl = 0.0f; d->brl_r = -2.5f; d->brl_g = -1.5f; d->brl_b = -1.5f;
  d->brl_rng = 0.5f; d->brl_st = 0.35f;
  d->brlp = -0.5f; d->brlp_r = -1.25f; d->brlp_g = -1.25f; d->brlp_b = -0.25f;

  d->brlp_enable = 1;

  // Tonescale pre-calculations
  const float tn_sh = 0.5f; // Standard
  const float tn_toe = 0.003f; // Standard
  const float tn_con = p->contrast;
  const float tn_su = 1.0f; // Dim surround (default)

  const float ts_x1 = powf(2.0f, 6.0f * tn_sh + 4.0f);
  const float ts_y1 = p->tn_Lp / 100.0f;
  const float ts_x0 = 0.18f + d->tn_off;
  const float ts_y0 = p->tn_Lg / 100.0f * (1.0f + p->tn_gb * log2f(ts_y1));
  const float ts_s0 = compress_toe_quadratic(ts_y0, tn_toe, 1);
  const float ts_p = tn_con / (1.0f + tn_su * 0.05f);
  const float ts_s10 = ts_x0 * (powf(ts_s0, -1.0f / tn_con) - 1.0f);
  const float ts_m1 = ts_y1 / powf(ts_x1 / (ts_x1 + ts_s10), tn_con);
  d->ts_m2 = compress_toe_quadratic(ts_m1, tn_toe, 1);
  d->ts_s = ts_x0 * (powf(ts_s0 / d->ts_m2, -1.0f / tn_con) - 1.0f);
  d->ts_dsc = 100.0f / p->tn_Lp; // Assuming linear output, not PQ/HLG
  d->ts_p = ts_p;

  float pt_cmp_Lf = p->pt_hdr * fminf(1.0f, (p->tn_Lp - 100.0f) / 900.0f);
  d->s_Lp100 = ts_x0 * (powf((p->tn_Lg / 100.0f), -1.0f / tn_con) - 1.0f);
  d->ts_s1 = d->ts_s * pt_cmp_Lf + d->s_Lp100 * (1.0f - pt_cmp_Lf);

  d->cwp_lm = p->cwp_lm;
  d->cwp_mode = p->cwp;

  // Color Matrices
  // Pipeline (RGB D50) -> P3 D65
  // 1. Pipe -> XYZ D50
  // 2. XYZ D50 -> XYZ D65 (CAT16)
  // 3. XYZ D65 -> P3 D65
  const dt_iop_order_iccprofile_info_t *profile = dt_ioppr_get_pipe_work_profile_info(pipe);
  dt_colormatrix_t pipe_to_xyz_d50;
  for(int i=0; i<3; i++)
    for(int j=0; j<3; j++)
      pipe_to_xyz_d50[i][j] = profile->matrix_in[i][j]; // Already D50 adapted in DT

  // dt_colormatrix_t xyz_d50_to_d65;
  // CAT16 D50->D65 matrix from common/chromatic_adaptation.h logic (precalc)
  // DCTL uses specific matrices. Let's use the ones from DCTL to match.
  // DCTL: matrix_xyz_d50_to_d65_cat16
  const dt_colormatrix_t mat_cat16 = {
    { 0.98946625f, -0.04003046f, 0.04405303f, 0.f}, {-0.00540519f, 1.00666069f, -0.00175552f, 0.f}, {-0.00040392f, 0.01507680f, 1.30210211f, 0.f}
  };

  dt_colormatrix_t xyz_d65_to_p3d65 = {
    { 2.49349691f, -0.93138362f, -0.40271078f, 0.f}, {-0.82948897f, 1.76266406f, 0.02362469f, 0.f}, { 0.03584583f, -0.07617239f, 0.95688452f, 0.f}
  };

  dt_colormatrix_t tmp;
  dt_colormatrix_mul(tmp, mat_cat16, pipe_to_xyz_d50);
  dt_colormatrix_mul(d->pipe_to_p3d65, xyz_d65_to_p3d65, tmp);

  // P3 D65 -> Pipeline (RGB D50)
  // Inverse of above
  dt_colormatrix_t p3d65_to_xyz_d65;
  mat3SSEinv(p3d65_to_xyz_d65, xyz_d65_to_p3d65);
  dt_colormatrix_t xyz_d65_to_d50;
  mat3SSEinv(xyz_d65_to_d50, mat_cat16);
  dt_colormatrix_t xyz_d50_to_pipe;
  mat3SSEinv(xyz_d50_to_pipe, pipe_to_xyz_d50);

  dt_colormatrix_mul(tmp, xyz_d65_to_d50, p3d65_to_xyz_d65);
  dt_colormatrix_mul(d->p3d65_to_pipe, xyz_d50_to_pipe, tmp);

  // Creative White Point Matrix (XYZ D65 -> XYZ D65 Adapted)
  // Used in display_gamut_whitepoint. DCTL selects from list.
  if (p->cwp == DT_OPENDRT_WP_D65) {
     for(int i=0; i<4; i++) for(int j=0; j<4; j++) d->cwp_matrix[i][j] = (i==j) ? 1.f : 0.f;
  } else {
      // Load appropriate matrix based on DCTL definitions
      // Example for D60 (common choice)
      if (p->cwp == DT_OPENDRT_WP_D60) {
         const dt_colormatrix_t d65_to_d60 = {
            { 1.01182246f, 0.00778879f, -0.01577830f, 0.f}, { 0.00561683f, 1.00150645f, -0.00628518f, 0.f}, {-0.00033574f, -0.00105095f, 0.92736667f, 0.f}
         };
         memcpy(d->cwp_matrix, d65_to_d60, sizeof(dt_colormatrix_t));
      } else if (p->cwp == DT_OPENDRT_WP_D50) {
         const dt_colormatrix_t d65_to_d50 = {
          { 1.04257405f, 0.03089118f, -0.05281262f, 0.f}, { 0.02219354f, 1.00185668f, -0.02107376f, 0.f}, {-0.00116488f, -0.00342053f, 0.76178908f, 0.f}
         };
         memcpy(d->cwp_matrix, d65_to_d50, sizeof(dt_colormatrix_t));
      }
      // Fallback/TODO: Add other matrices if needed
      else {
         for(int i=0; i<4; i++) for(int j=0; j<4; j++) d->cwp_matrix[i][j] = (i==j) ? 1.f : 0.f;
      }
  }
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_opendrt_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
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

const char *name() { return _("opendrt"); }
const char *aliases() { return _("open display transform"); }
int flags() { return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_SUPPORTS_BLENDING; }
