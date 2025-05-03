#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/custom_primaries.h"
#include "common/iop_profile.h"
#include "common/matrices.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/draw.h"
#include "iop/iop_api.h"
#include <gtk/gtk.h>
#include <math.h>             // For math functions
#include <pango/pangocairo.h> // For text rendering in graph
#include <stdlib.h>
#include "common/math.h"
#include "gui/gtk.h"
#include "gui/presets.h"

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"


// Module introspection version
DT_MODULE_INTROSPECTION(1, dt_iop_agx_user_params_t)

static const float _epsilon = 1E-6f;

typedef enum dt_iop_agx_base_primaries_t
{
  // NOTE: Keep Export Profile first to make it the default (index 0)
  DT_AGX_EXPORT_PROFILE = 0,         // $DESCRIPTION: "export profile"
  DT_AGX_WORK_PROFILE = 1,           // $DESCRIPTION: "working profile"
  DT_AGX_REC2020 = 2,                // $DESCRIPTION: "Rec2020"
  DT_AGX_DISPLAY_P3 = 3,             // $DESCRIPTION: "Display P3"
  DT_AGX_ADOBE_RGB = 4,              // $DESCRIPTION: "Adobe RGB (compatible)"
  DT_AGX_SRGB = 5,                   // $DESCRIPTION: "sRGB"
} dt_iop_agx_base_primaries_t;

// Module parameters struct
// Updated struct dt_iop_agx_user_params_t
typedef struct dt_iop_agx_user_params_t
{
  // look params
  float look_offset; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "offset"
  float look_slope; // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "slope"
  float look_power; // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "power"
  float look_saturation;             // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "saturation"
  float look_original_hue_mix_ratio; // $MIN: 0.0 $MAX: 1 $DEFAULT: 0.0 $DESCRIPTION: "preserve hue"

  // log mapping params
  float range_black_relative_exposure;  // $MIN: -20.0 $MAX: -0.1 $DEFAULT: -10 $DESCRIPTION: "black relative exposure"
  float range_white_relative_exposure;  // $MIN: 0.1 $MAX: 20 $DEFAULT: 6.5 $DESCRIPTION: "white relative exposure"

  // curve params - comments indicate the original variables from https://www.desmos.com/calculator/yrysofmx8h
  // Corresponds to p_x, but not directly -- allows shifting the default 0.18 towards black or white relative exposure
  float curve_pivot_x_shift;      // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0 $DESCRIPTION: "pivot x shift"
  // Corresponds to p_y, but not directly -- needs application of gamma
  float curve_pivot_y_linear;            // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.18 $DESCRIPTION: "pivot y (linear)"
  // P_slope
  float curve_contrast_around_pivot;      // $MIN: 0.1 $MAX: 10.0 $DEFAULT: 2.4 $DESCRIPTION: "contrast around the pivot"
  // P_tlength
  float curve_linear_percent_below_pivot;  // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "toe start %"
  // P_slength
  float curve_linear_percent_above_pivot;  // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "shoulder start %"
  // t_p
  float curve_toe_power;                  // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.5 $DESCRIPTION: "toe power"
  // s_p -> Renamed from curve_shoulder_power for clarity
  float curve_shoulder_power;             // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.5 $DESCRIPTION: "shoulder power;"
  // we don't have a parameter for pivot_x, it's set to the x value representing mid-gray, splitting [0..1] in the ratio
  // range_black_relative_exposure : range_white_relative_exposure
  // not a parameter of the original curve, they used p_x, p_y to directly set the pivot
  float curve_gamma;                // $MIN: 1.0 $MAX: 10.0 $DEFAULT: 2.2 $DESCRIPTION: "curve y gamma"
  // t_ly
  float curve_target_display_black_y;     // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "target black"
  // s_ly
  float curve_target_display_white_y;     // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "target white"

  // custom primaries
  dt_iop_agx_base_primaries_t base_primaries; // $DEFAULT: DT_AGX_EXPORT_PROFILE $DESCRIPTION: "base primaries"
  float red_inset;        // $MIN:  0.0  $MAX: 0.99 $DEFAULT: 0.0 $DESCRIPTION: "red attenuation"
  float red_rotation;     // $MIN: -0.4  $MAX: 0.4  $DEFAULT: 0.0 $DESCRIPTION: "red rotation"
  float green_inset;      // $MIN:  0.0  $MAX: 0.99 $DEFAULT: 0.0 $DESCRIPTION: "green attenuation"
  float green_rotation;   // $MIN: -0.4  $MAX: 0.4  $DEFAULT: 0.0 $DESCRIPTION: "green rotation"
  float blue_inset;       // $MIN:  0.0  $MAX: 0.99 $DEFAULT: 0.0 $DESCRIPTION: "blue attenuation"
  float blue_rotation;    // $MIN: -0.4  $MAX: 0.4  $DEFAULT: 0.0 $DESCRIPTION: "blue rotation"

  float master_outset_ratio; // $MIN:  0.0  $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "master attenuation reversal"
  float master_unrotation_ratio; // $MIN:  0.0  $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "master rotation reversal"
  float red_outset;        // $MIN:  0.0  $MAX: 2.0 $DEFAULT: 0.0 $DESCRIPTION: "red attenuation reversal"
  float red_unrotation;     // $MIN: -0.4  $MAX: 0.4  $DEFAULT: 0.0 $DESCRIPTION: "red rotation reversal"
  float green_outset;      // $MIN:  0.0  $MAX: 0.99 $DEFAULT: 0.0 $DESCRIPTION: "green attenuation reversal"
  float green_unrotation;   // $MIN: -0.4  $MAX: 0.4  $DEFAULT: 0.0 $DESCRIPTION: "green rotation reversal"
  float blue_outset;       // $MIN:  0.0  $MAX: 0.99 $DEFAULT: 0.0 $DESCRIPTION: "blue attenuation reversal"
  float blue_unrotation;    // $MIN: -0.4  $MAX: 0.4  $DEFAULT: 0.0 $DESCRIPTION: "blue rotation reversal"
} dt_iop_agx_user_params_t;

typedef struct dt_iop_agx_gui_data_t
{
  dt_gui_collapsible_section_t look_section;
  dt_gui_collapsible_section_t area_section;
  dt_gui_collapsible_section_t advanced_section;
  dt_gui_collapsible_section_t primaries_section;
  // dt_gui_collapsible_section_t gamut_compression_section;
  // GtkWidget *experimental_gamut_compression_order;
  GtkDrawingArea *area;

  // Cache Pango and Cairo stuff for the graph drawing
  float line_height;
  float sign_width;
  float zero_width;
  float graph_width;
  float graph_height;
  int inset;
  int inner_padding;

  GtkAllocation allocation;
  PangoRectangle ink;
  GtkStyleContext *context;

  // Picker widgets
  GtkWidget *range_black_picker;
  GtkWidget *range_white_picker;
  GtkWidget *auto_tune_picker;
  GtkWidget *pivot_x_picker;

  // Slider widgets for pickers
  GtkWidget *range_black_exposure;
  GtkWidget *range_white_exposure;
  GtkWidget *curve_pivot_x_shift;
  GtkNotebook *notebook; // Main tabbed interface
  GtkWidget *curve_pivot_y_linear;

} dt_iop_agx_gui_data_t;

typedef struct curve_and_look_params_t
{
  // shared
  float min_ev;
  float max_ev;
  float range_in_ev;
  float curve_gamma;

  // the toe runs from (0, target black) to (toe_transition_x, toe_transition_y)
  // t_lx = 0 for us
  float pivot_x;
  float pivot_y;
  float target_black; // t_ly
  float toe_power; // t_p
  float toe_transition_x; // t_tx
  float toe_transition_y; // t_ty
  float toe_scale; // t_s
  gboolean need_convex_toe;
  float toe_a;
  float toe_b;

  // the linear section lies on y = mx + b, running from (toe_transition_x, toe_transition_y) to (shoulder_transition_x, shoulder_transition_y)
  // it can have length 0, in which case it only contains the pivot (pivot_x, pivot_y)
  // the pivot may coincide with toe_transition or shoulder_start or both
  float slope; // m - for the linear section
  float intercept; // b parameter of the straight segment (y = mx + b, intersection with the y-axis at (0, b))

  // the shoulder runs from (shoulder_transition_x, shoulder_transition_y) to (1, target_white)
  // s_lx = 1 for us
  float target_white; // s_ly
  float shoulder_power; // s_p
  float shoulder_transition_x; // s_tx
  float shoulder_transition_y; // s_ty
  float shoulder_scale; // s_s
  gboolean need_concave_shoulder;
  float shoulder_a;
  float shoulder_b;

  // look
  float look_offset;
  float look_slope;
  float look_power;
  float look_saturation;
  float look_original_hue_mix_ratio;
} curve_and_look_params_t;

typedef struct primaries_params_t
{
  float inset[3];
  float rotation[3];

  float master_outset_ratio;
  float master_unrotation_ratio;
  float outset[3];
  float unrotation[3];
} primaries_params_t;

// Translatable name
const char *name()
{
  return _("agx");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
                                _("Applies a tone mapping curve.\n"
                                  "Inspired by Blender's AgX tone mapper"),
                                _("corrective and creative"), _("linear, RGB, scene-referred"),
                                _("non-linear, RGB"), _("linear, RGB, display-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_TONE | IOP_GROUP_TECHNICAL;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

// Legacy parameters (not needed for version 1)
int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void **new_params,
                  int32_t *new_params_size, int *new_version)
{
  return 1; // no conversion possible
}

// Commit parameters
void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, p1, self->params_size);
}

void _print_transposed_matrix(const char* name, const dt_colormatrix_t matrix)
{
  printf("%s\n", name);
  printf("%f, %f, %f\n", matrix[0][0], matrix[1][0], matrix[2][0]);
  printf("%f, %f, %f\n", matrix[0][1], matrix[1][1], matrix[2][1]);
  printf("%f, %f, %f\n", matrix[0][2], matrix[1][2], matrix[2][2]);
  printf("\n\n");
}

// Helper function to get the dt_colorspaces type from the enum
static dt_colorspaces_color_profile_type_t _get_base_profile_type_from_enum(const dt_iop_agx_base_primaries_t base_primaries_enum)
{
  switch(base_primaries_enum)
  {
    case DT_AGX_SRGB:       return DT_COLORSPACE_SRGB;
    case DT_AGX_DISPLAY_P3: return DT_COLORSPACE_DISPLAY_P3;
    case DT_AGX_ADOBE_RGB:  return DT_COLORSPACE_ADOBERGB;
    case DT_AGX_REC2020:    // Fall through
    default:                return DT_COLORSPACE_LIN_REC2020; // Default/fallback
  }
}

// Get the profile info struct based on the user selection
static const dt_iop_order_iccprofile_info_t*
_agx_get_base_profile(dt_develop_t *dev,
                      const dt_iop_order_iccprofile_info_t *pipe_work_profile,
                      const dt_iop_agx_base_primaries_t base_primaries_selection)
{
  dt_iop_order_iccprofile_info_t *selected_profile_info = NULL;

  switch(base_primaries_selection)
  {
    case DT_AGX_EXPORT_PROFILE:
      {
        dt_colorspaces_color_profile_type_t export_type;
        char export_filename[DT_IOP_COLOR_ICC_LEN];
        dt_iop_color_intent_t export_intent;

        // Get the configured export profile settings
        gboolean settings_ok = dt_ioppr_get_configured_export_profile_settings(
            dev, &export_type, export_filename, sizeof(export_filename), &export_intent);

        if (settings_ok)
        {
          selected_profile_info = dt_ioppr_add_profile_info_to_list(dev, export_type, export_filename, export_intent);
          if (!selected_profile_info || !dt_is_valid_colormatrix(selected_profile_info->matrix_in_transposed[0][0]))
          {
             dt_print(DT_DEBUG_PIPE, "[agx] Export profile '%s' unusable or missing matrix, falling back to Rec2020.",
                      dt_colorspaces_get_name(export_type, export_filename));
             selected_profile_info = NULL; // Force fallback
          }
        }
        else
        {
           dt_print(DT_DEBUG_ALWAYS, "[agx] Failed to get configured export profile settings, falling back to Rec2020.");
           // Fallback handled below
        }
      }
      break;

    case DT_AGX_WORK_PROFILE:
      // Cast away const, as dt_ioppr_add_profile_info_to_list returns non-const
      return (dt_iop_order_iccprofile_info_t *)pipe_work_profile;

    case DT_AGX_REC2020:
    case DT_AGX_DISPLAY_P3:
    case DT_AGX_ADOBE_RGB:
    case DT_AGX_SRGB:
      {
        dt_colorspaces_color_profile_type_t profile_type = _get_base_profile_type_from_enum(base_primaries_selection);
        // Use relative intent for standard profiles when used as base
        selected_profile_info = dt_ioppr_add_profile_info_to_list(dev, profile_type, "", DT_INTENT_RELATIVE_COLORIMETRIC);
         if (!selected_profile_info || !dt_is_valid_colormatrix(selected_profile_info->matrix_in_transposed[0][0]))
         {
             dt_print(DT_DEBUG_PIPE, "[agx] Standard base profile '%s' unusable or missing matrix, falling back to Rec2020.",
                      dt_colorspaces_get_name(profile_type, ""));
             selected_profile_info = NULL; // Force fallback
         }
      }
      break;
  }

  // Fallback if selected profile wasn't found or was invalid
  if (!selected_profile_info)
  {
    selected_profile_info = dt_ioppr_add_profile_info_to_list(dev, DT_COLORSPACE_LIN_REC2020, "", DT_INTENT_RELATIVE_COLORIMETRIC);
    // If even Rec2020 fails, something is very wrong, but let the caller handle NULL if necessary.
    if (!selected_profile_info)
       dt_print(DT_DEBUG_ALWAYS, "[agx] CRITICAL: Failed to get even Rec2020 base profile info.");
  }

  return selected_profile_info;
}

DT_OMP_DECLARE_SIMD(aligned(pixel : 16))
static float _max(const dt_aligned_pixel_t pixel)
{
  return fmaxf(fmaxf(pixel[0], pixel[1]), pixel[2]);
}

DT_OMP_DECLARE_SIMD(aligned(pixel : 16))
static float _min(const dt_aligned_pixel_t pixel)
{
  return fminf(fminf(pixel[0], pixel[1]), pixel[2]);
}

// AgX implementation
static float _luminance(const dt_aligned_pixel_t pixel, const dt_iop_order_iccprofile_info_t *const profile)
{
  const float lum = dt_ioppr_get_rgb_matrix_luminance(pixel, profile->matrix_in, profile->lut_in,
                                                          profile->unbounded_coeffs_in, profile->lutsize,
                                                          profile->nonlinearlut);
  return lum;
}

static float _line(const float x, const float slope, const float intercept)
{
  return slope * x + intercept;
}

static float _scale(float limit_x, float limit_y, float transition_x, float transition_y, float slope, float power)
{
  const float dy_limit_to_transition_at_constant_slope = slope * (limit_x - transition_x);
  printf("dy_limit_to_transition = %f\n", dy_limit_to_transition_at_constant_slope);

  const float dy_to_power = powf(dy_limit_to_transition_at_constant_slope, -power);
  printf("dy_to_power = %f\n", dy_to_power);

  // in case the linear section extends too far; avoid division by 0
  const float remaining_y_span = fmaxf(_epsilon, limit_y - transition_y);
  printf("remaining_y_span = %f\n", remaining_y_span);

  const float y_delta_ratio = dy_limit_to_transition_at_constant_slope / remaining_y_span;
  printf("y_delta_ratio = %f\n", y_delta_ratio);

  float term_b = powf(y_delta_ratio, power) - 1.0f;
  term_b = fmaxf(term_b, _epsilon);
  printf("term_b = %f\n", term_b);

  const float base = dy_to_power * term_b;
  printf("base = %f\n", base);

  // this is t_s or s_s on the chart, what's next?!

  float scale_value = powf(base, -1.0f / power);

  scale_value = fminf(1e6, scale_value);
  scale_value = fmaxf(-1e6, scale_value);

  printf("scale_value = %f\n", scale_value);

  return scale_value;
}

// this is f_t(x), f_s(x) at https://www.desmos.com/calculator/yrysofmx8h
static float _exponential(float x, float power)
{
  return x / powf(1.0f + powf(x, power), 1.0f / power);
}

static float _exponential_curve(float x, float scale, float slope, float power, float transition_x,
                                float transition_y)
{
  // this is f_ss, f_ts on the original curve https://www.desmos.com/calculator/yrysofmx8h
  return scale * _exponential(slope * (x - transition_x) / scale, power) + transition_y;
}

// Fallback toe/shoulder, so we can always reach black and white.
// See https://www.desmos.com/calculator/gijzff3wlv
static float _fallback_toe(const float x, const curve_and_look_params_t *curve_params)
{
  return x <= 0 ?
    curve_params->target_black :
    curve_params->target_black + fmaxf(0, curve_params -> toe_a * powf(x, curve_params -> toe_b));
}

static float _fallback_shoulder(const float x, const curve_and_look_params_t *curve_params)
{
  return x >= 1 ?
    curve_params->target_white :
    curve_params->target_white - fmaxf(0, curve_params->shoulder_a * powf(1 - x, curve_params->shoulder_b));
}

// the commented values (t_tx, etc.) are references to https://www.desmos.com/calculator/yrysofmx8h
static float _apply_curve(const float x, const curve_and_look_params_t *curve_params)
{
  float result;

  if(x < curve_params->toe_transition_x)
  {
    result = curve_params->need_convex_toe ?
      _fallback_toe(x, curve_params) :
      _exponential_curve(x, curve_params->toe_scale, curve_params->slope, curve_params->toe_power, curve_params->toe_transition_x, curve_params->toe_transition_y);
  }
  else if(x <= curve_params->shoulder_transition_x)
  {
    result = _line(x, curve_params->slope, curve_params->intercept);
  }
  else
  {
    result = curve_params->need_concave_shoulder ?
      _fallback_shoulder(x, curve_params) :
      _exponential_curve(x, curve_params->shoulder_scale, curve_params->slope, curve_params->shoulder_power, curve_params->shoulder_transition_x, curve_params->shoulder_transition_y);
  }
  return CLAMPF(result, curve_params->target_black, curve_params->target_white);
}

static float _agx_sanitise_hue(float hue)
{
  if(hue < 0.0f) hue += 1.0f;
  if(hue >= 1.0f) hue -= 1.0f;
  return hue;
}

// 'lerp', but take care of the boundary: hue wraps around 1 -> 0
static float _lerp_hue(float original_hue, float processed_hue, float mix)
{
  // can be removed once colorspaces_inline_conversions gets merged
  original_hue = _agx_sanitise_hue(original_hue);
  processed_hue = _agx_sanitise_hue(processed_hue);

  const float hue_diff = processed_hue - original_hue;

  if(hue_diff > 0.5)
  {
    processed_hue -= 1;
  }
  else if(hue_diff < -0.5)
  {
    processed_hue += 1;
  }

  float restored_hue = processed_hue + (original_hue - processed_hue) * mix;
  return _agx_sanitise_hue(restored_hue);
}

static float _apply_slope_offset(const float x, const float slope, const float offset)
{
  // negative offset should darken the image; positive brighten it
  // without the scale: m = 1 / (1 + offset)
  // offset = 1, slope = 1, x = 0 -> m = 1 / (1+1) = 1/2, b = 1 * 1/2 = 1/2, y = 1/2*0 + 1/2 = 1/2
  const float m = slope / (1 + offset);
  const float b = offset * m;
  return m * x + b;
  // ASC CDL:
  // return x * slope + offset;
  // alternative:
  // y = mx + b, b is the offset, m = (1 - offset), so the line runs from (0, offset) to (1, 1)
  //return (1 - offset) * x + offset;
}

// https://docs.acescentral.com/specifications/acescct/#appendix-a-application-of-asc-cdl-parameters-to-acescct-image-data
DT_OMP_DECLARE_SIMD(aligned(pixel_in_out: 16))
static void _agxLook(dt_aligned_pixel_t pixel_in_out, const curve_and_look_params_t *params, const dt_iop_order_iccprofile_info_t *const rendering_profile)
{
  // Default parameters from the curve_and_look_params_t struct
  const float slope = params->look_slope;
  const float offset = params->look_offset;
  const float power = params->look_power;
  const float sat = params->look_saturation;

  // Apply ASC CDL (Slope, Offset, Power) per channel
  for_three_channels(k, aligned(pixel_in_out: 16))
  {
    // Apply slope and offset
    const float slope_and_offset_val = _apply_slope_offset(pixel_in_out[k], slope, offset);
    // Apply power
    pixel_in_out[k] = slope_and_offset_val > 0.0f ? powf(slope_and_offset_val, power) : slope_and_offset_val;
  }

  const float luma = _luminance(pixel_in_out, rendering_profile);

  // saturation
  for_three_channels(k, aligned(pixel_in_out: 16))
  {
    pixel_in_out[k] = luma + sat * (pixel_in_out[k] - luma);
  }
}

static float _apply_log_encoding(float x, float range_in_ev, float minEv)
{
  // Assume input is linear RGB relative to 0.18 mid-gray
  // Ensure value > 0 before log
  x = fmaxf(_epsilon, x / 0.18f);
  float mapped = log2f(x);
  // normalise to [0, 1] based on minEv and range_in_ev
  mapped = (mapped - minEv) / range_in_ev;
  // Clamp result to [0, 1] - this is the input domain for the curve
  return CLAMPF(mapped, 0.0f, 1.0f);
}

// see https://www.desmos.com/calculator/gijzff3wlv
static float _calculate_B(float slope, float dx_transition_to_limit, float dy_transition_to_limit)
{
  return slope * dx_transition_to_limit / dy_transition_to_limit;
}

static float _calculate_A(const float dx_transition_to_limit, const float dy_transition_to_limit, const float B)
{
  return dy_transition_to_limit / powf(dx_transition_to_limit, B);
}

static void _avoid_negatives(dt_aligned_pixel_t pixel_in_out, const dt_iop_order_iccprofile_info_t *const profile)
{
  if(pixel_in_out[0] >= 0.0f && pixel_in_out[1] >= 0.0f && pixel_in_out[2] >= 0.0f)
  {
    // No compensation needed
    return;
  }

  const float original_luminance = _luminance(pixel_in_out, profile);
  const float most_negative_component = _min(pixel_in_out);

  // offset, so no component remains negative
  for_three_channels(k, aligned(pixel_in_out : 16))
  {
    pixel_in_out[k] -= most_negative_component;
  }

  const float offset_luminance = _luminance(pixel_in_out, profile);

  const float luminance_correction = original_luminance / offset_luminance;

  for_three_channels(k, aligned(pixel_in_out : 16))
  {
    pixel_in_out[k] *= luminance_correction;
  }
}

static void _compensate_low_side(
  dt_aligned_pixel_t pixel_in_out,
  const dt_iop_order_iccprofile_info_t *const profile)
{
  // from sigmoid; can create black pixels
  // e.g.
  // (-0.2, 0.2, 0.3) -> avg = 0.1, min = -0.2, saturation_factor = -0.1/(-0.2 - 0.1) = 1/3 -> (0, 0.13, 0.17)
  // but
  // (-0.3, 0.1, 0.1) -> avg = -0.1/3 => mapped to 0, min = -0.3, saturation_factor = 0; -> (0, 0, 0)
  // average
  const float pixel_average = fmaxf((pixel_in_out[0] + pixel_in_out[1] + pixel_in_out[2]) / 3.0f, 0.0f);
  const float min_value = _min(pixel_in_out);
  const float saturation_factor = min_value < 0.0f ? -pixel_average / (min_value - pixel_average) : 1.0f;
  for_each_channel(c, aligned(pixel_in_out))
  {
    pixel_in_out[c] = pixel_average + saturation_factor * (pixel_in_out[c] - pixel_average);
  }
  // just in case any negative remains
  // Sakari
  _avoid_negatives(pixel_in_out, profile);
}

static curve_and_look_params_t _calculate_curve_params(const dt_iop_agx_user_params_t *user_params)
{
  curve_and_look_params_t params;

  // look
  params.look_offset = user_params->look_offset;
  params.look_slope = user_params->look_slope;
  params.look_saturation = user_params->look_saturation;
  params.look_power = user_params->look_power;
  params.look_original_hue_mix_ratio = user_params->look_original_hue_mix_ratio;

  printf("===== curve params calculation =====\n");

  // log mapping
  params.max_ev = user_params->range_white_relative_exposure;
  printf("max_ev = %f\n", params.max_ev);
  params.min_ev = user_params->range_black_relative_exposure;
  printf("min_ev = %f\n", params.min_ev);
  params.range_in_ev = params.max_ev - params.min_ev;
  printf("range_in_ev = %f\n", params.range_in_ev);

  params.curve_gamma = user_params->curve_gamma;
  printf("curve_gamma = %f\n", params.curve_gamma);

  float pivot_x = fabsf(params.min_ev / params.range_in_ev);
  if (user_params->curve_pivot_x_shift < 0)
  {
    float black_ratio = - user_params->curve_pivot_x_shift;
    float gray_ratio = 1 - black_ratio;
    pivot_x = gray_ratio * pivot_x;
  } else if (user_params->curve_pivot_x_shift > 0)
  {
    float white_ratio = user_params->curve_pivot_x_shift;
    float gray_ratio = 1 - white_ratio;
    pivot_x = pivot_x * gray_ratio + white_ratio;
  }

  params.pivot_x = pivot_x;
  params.pivot_y = powf(CLAMPF(user_params->curve_pivot_y_linear, user_params->curve_target_display_black_y, user_params->curve_target_display_white_y),
    1.0f / params.curve_gamma
  );
  printf("pivot(%f, %f) at gamma = %f\n", pivot_x, params.pivot_y, params.curve_gamma);

  // avoid range altering slope - 16.5 EV is the default AgX range; keep the meaning of slope
  // new Blender defaults: 20 EV range
  params.slope = user_params->curve_contrast_around_pivot * (params.range_in_ev / 16.5f);
  // params.slope = user_params->curve_contrast_around_pivot * (params.range_in_ev / 20.0f);
  printf("scaled slope = %f from user_contrast_around_pivot = %f\n", params.slope, user_params->curve_contrast_around_pivot);

  // toe
  params.target_black = user_params->curve_target_display_black_y;
  printf("target_black = %f\n", params.target_black);
  params.toe_power = user_params->curve_toe_power;
  printf("toe_power = %f\n", params.toe_power);

  // length of (0 -> pivot_x) is just pivot_x; we take the portion specified using the percentage...
  const float dx_linear_below_pivot = pivot_x * user_params->curve_linear_percent_below_pivot / 100.0f;
  // ...and subtract it from pivot_x to get the x coordinate where the linear section joins the toe
  params.toe_transition_x = pivot_x - dx_linear_below_pivot;
  printf("toe_transition_x = %f\n", params.toe_transition_x);

  // from the 'run' pivot_x -> toe_transition_x, we calculate the 'rise'
  const float toe_y_below_pivot_y = params.slope * dx_linear_below_pivot;
  params.toe_transition_y = params.pivot_y - toe_y_below_pivot_y;
  printf("toe_transition_y = %f\n", params.toe_transition_y);

  const float toe_dx_transition_to_limit = fmaxf(_epsilon, params.toe_transition_x); // limit_x is 0; use epsilon to avoid division by 0 later
  const float toe_dy_transition_to_limit = fmaxf(_epsilon, params.toe_transition_y - params.target_black);
  const float toe_slope_transition_to_limit = toe_dy_transition_to_limit / toe_dx_transition_to_limit;

  // we use the same calculation as for the shoulder, so we flip the toe left <-> right, up <-> down
  const float inverse_toe_limit_x = 1.0f; // 1 - toeLimix_x (toeLimix_x = 0, so inverse = 1)
  const float inverse_toe_limit_y = 1.0f - params.target_black; // Inverse limit y

  const float inverse_toe_transition_x = 1.0f - params.toe_transition_x;
  const float inverse_toe_transition_y = 1.0f - params.toe_transition_y;

  // and then flip the scale
  params.toe_scale = -_scale(inverse_toe_limit_x, inverse_toe_limit_y,
                                    inverse_toe_transition_x, inverse_toe_transition_y,
                                    params.slope, params.toe_power);
  printf("toe_scale = %f\n", params.toe_scale);

  params.need_convex_toe = toe_slope_transition_to_limit > params.slope;
  printf("need_convex_toe = %d\n", params.need_convex_toe);

  // toe fallback curve params
  params.toe_b = _calculate_B(params.slope, toe_dx_transition_to_limit, toe_dy_transition_to_limit);
  printf("toe_b = %f\n", params.toe_b);
  params.toe_a = _calculate_A(toe_dx_transition_to_limit, toe_dy_transition_to_limit, params.toe_b);
  printf("toe_a = %f\n", params.toe_a);

  // if x went from toe_transition_x to 0, at the given slope, starting from toe_transition_y, where would we intersect the y-axis?
  params.intercept = params.toe_transition_y - params.slope * params.toe_transition_x;
  printf("intercept = %f\n", params.intercept);

  // shoulder
  params.target_white = user_params->curve_target_display_white_y;
  printf("target_white = %f\n", params.target_white);
  // distance between pivot_x and x = 1, times portion of linear section
  const float shoulder_x_from_pivot_x = (1 - pivot_x) * user_params->curve_linear_percent_above_pivot / 100.0f;
  params.shoulder_transition_x = pivot_x + shoulder_x_from_pivot_x;
  printf("shoulder_transition_x = %f\n", params.shoulder_transition_x);
  const float shoulder_y_above_pivot_y = params.slope * shoulder_x_from_pivot_x;
  params.shoulder_transition_y = params.pivot_y + shoulder_y_above_pivot_y;
  printf("shoulder_transition_y = %f\n", params.shoulder_transition_y);
  const float shoulder_dx_transition_to_limit = fmaxf(_epsilon, 1 - params.shoulder_transition_x); // dx to 0, avoid division by 0 later
  const float shoulder_dy_transition_to_limit = fmaxf(_epsilon, params.target_white - params.shoulder_transition_y);
  const float shoulder_slope_transition_to_limit = shoulder_dy_transition_to_limit / shoulder_dx_transition_to_limit;
  params.shoulder_power = user_params->curve_shoulder_power;
  printf("shoulder_power = %f\n", params.shoulder_power);

  const float shoulder_limit_x = 1;
  params.shoulder_scale = _scale(shoulder_limit_x, params.target_white,
                                    params.shoulder_transition_x, params.shoulder_transition_y,
                                    params.slope, params.shoulder_power);
  printf("shoulder_scale = %f\n", params.shoulder_scale);
  params.need_concave_shoulder = shoulder_slope_transition_to_limit > params.slope;
  printf("need_concave_shoulder = %d\n", params.need_concave_shoulder);

  // shoulder fallback curve params
  params.shoulder_b = _calculate_B(params.slope, shoulder_dx_transition_to_limit, shoulder_dy_transition_to_limit);
  printf("shoulder_b = %f\n", params.shoulder_b);
  params.shoulder_a = _calculate_A(shoulder_dx_transition_to_limit, shoulder_dy_transition_to_limit, params.shoulder_b);
  printf("shoulder_a = %f\n", params.shoulder_a);

  printf("================== end ==================\n");

  return params;
}

static primaries_params_t _get_primaries_params(const dt_iop_agx_user_params_t *user_params)
{
  primaries_params_t primaries_params;

  primaries_params.inset[0] = user_params->red_inset;
  primaries_params.inset[1] = user_params->green_inset;
  primaries_params.inset[2] = user_params->blue_inset;
  primaries_params.rotation[0] = user_params->red_rotation;
  primaries_params.rotation[1] = user_params->green_rotation;
  primaries_params.rotation[2] = user_params->blue_rotation;
  primaries_params.master_outset_ratio = user_params->master_outset_ratio;
  primaries_params.master_unrotation_ratio = user_params->master_unrotation_ratio;
  primaries_params.outset[0] = user_params->red_outset;
  primaries_params.outset[1] = user_params->green_outset;
  primaries_params.outset[2] = user_params->blue_outset;
  primaries_params.unrotation[0] = user_params->red_unrotation;
  primaries_params.unrotation[1] = user_params->green_unrotation;
  primaries_params.unrotation[2] = user_params->blue_unrotation;

  return primaries_params;
}

static void _agx_tone_mapping(dt_aligned_pixel_t rgb_in_out, const curve_and_look_params_t * params, const dt_iop_order_iccprofile_info_t *const rendering_profile)
{
  // record current chromaticity angle
  dt_aligned_pixel_t hsv_pixel = {0.0f};
  dt_RGB_2_HSV(rgb_in_out, hsv_pixel);
  const float h_before = hsv_pixel[0];

  dt_aligned_pixel_t transformed_pixel = {0.0f};

  for_three_channels(k, aligned(rgb_in_out, transformed_pixel: 16))
  {
    float log_value = _apply_log_encoding(rgb_in_out[k], params->range_in_ev, params->min_ev);
    transformed_pixel[k] = _apply_curve(log_value, params);
  }

  // Apply AgX look
  _agxLook(transformed_pixel, params, rendering_profile);

  // Linearize
  for_three_channels(k, aligned(transformed_pixel: 16))
  {
    transformed_pixel[k] = powf(fmaxf(0.0f, transformed_pixel[k]), params->curve_gamma);
  }

  // get post-curve chroma angle
  dt_RGB_2_HSV(transformed_pixel, hsv_pixel);

  float h_after = hsv_pixel[0];

  // Mix hue back if requested
  h_after = _lerp_hue(h_before, h_after, params->look_original_hue_mix_ratio);

  hsv_pixel[0] = h_after;
  dt_HSV_2_RGB(hsv_pixel, rgb_in_out);
}

// Apply logic for black point picker
static void apply_auto_black_exposure(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_agx_user_params_t *p = self->params;
  dt_iop_agx_gui_data_t *g = self->gui_data;

  const float black_norm = _min(self->picked_color_min);
  p->range_black_relative_exposure = CLAMPF(log2f(fmaxf(_epsilon, black_norm) / 0.18f), -20.0f, -0.1f);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->range_black_exposure, p->range_black_relative_exposure);
  --darktable.gui->reset;

  gtk_widget_queue_draw(GTK_WIDGET(g->area));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// Apply logic for white point picker
static void apply_auto_white_exposure(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_agx_user_params_t *p = self->params;
  dt_iop_agx_gui_data_t *g = self->gui_data;

  const float white_norm = _max(self->picked_color_max);
  p->range_white_relative_exposure = CLAMPF(log2f(fmaxf(_epsilon, white_norm) / 0.18f), 0.1f, 20.0f);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->range_white_exposure, p->range_white_relative_exposure);
  --darktable.gui->reset;

  gtk_widget_queue_draw(GTK_WIDGET(g->area));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// Apply logic for auto-tuning both black and white points
static void apply_auto_tune_exposure(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_agx_user_params_t *p = self->params;
  dt_iop_agx_gui_data_t *g = self->gui_data;

  // Black point
  const float black_norm = _min(self->picked_color_min);
  p->range_black_relative_exposure = CLAMPF(log2f(fmaxf(_epsilon, black_norm) / 0.18f), -20.0f, -0.1f);

  // White point
  const float white_norm = _max(self->picked_color_max);
  p->range_white_relative_exposure = CLAMPF(log2f(fmaxf(_epsilon, white_norm) / 0.18f), 0.1f, 20.0f);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->range_black_exposure, p->range_black_relative_exposure);
  dt_bauhaus_slider_set(g->range_white_exposure, p->range_white_relative_exposure);
  --darktable.gui->reset;

  gtk_widget_queue_draw(GTK_WIDGET(g->area));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// Apply logic for pivot x picker
static void apply_auto_pivot_x(dt_iop_module_t *self, const dt_iop_order_iccprofile_info_t *profile)
{
  if(darktable.gui->reset) return;
  dt_iop_agx_user_params_t *p = self->params;
  const dt_iop_agx_gui_data_t *g = self->gui_data;

  // Calculate norm and EV of the picked color
  const float norm = _luminance(self->picked_color, profile);
  const float picked_ev = log2f(fmaxf(_epsilon, norm) / 0.18f);

  // Calculate the target pivot_x based on the picked EV and the current EV range
  const float min_ev = p->range_black_relative_exposure;
  const float max_ev = p->range_white_relative_exposure;
  const float range_in_ev = fmaxf(_epsilon, max_ev - min_ev);
  const float target_pivot_x = CLAMPF((picked_ev - min_ev) / range_in_ev, 0.0f, 1.0f);

  // Calculate the required pivot_x_shift to achieve the target_pivot_x
  const float base_pivot_x = fabsf(min_ev / range_in_ev); // Pivot representing 0 EV (mid-gray)

  dt_iop_agx_user_params_t params_with_mid_gray = *p;
  params_with_mid_gray.curve_pivot_y_linear = 0.18;
  params_with_mid_gray.curve_pivot_x_shift = 0;

  curve_and_look_params_t curve_and_look_params = _calculate_curve_params(&params_with_mid_gray);

  // see where the target_pivot would be mapped with defaults of mid-gray to mid-gray mapped
  float target_y = _apply_curve(target_pivot_x, &curve_and_look_params);
  // try to avoid changing the brightness of the pivot
  float target_y_linearised = powf(target_y, p->curve_gamma);
  p->curve_pivot_y_linear = target_y_linearised;

  float shift;
  if(fabsf(target_pivot_x - base_pivot_x) < _epsilon)
  {
    shift = 0.0f;
  }
  else if(base_pivot_x > target_pivot_x)
  {
    // Solve target_pivot_x = (1 + s) * base_pivot_x for s
    shift = (base_pivot_x > _epsilon) ? (target_pivot_x / base_pivot_x) - 1.0f : -1.0f;
  }
  else // target_pivot_x > base_pivot_x
  {
    // Solve target_pivot_x = base_pivot_x * (1 - s) + s for s
    const float denominator = 1.0f - base_pivot_x;
    shift = (denominator > _epsilon) ? (target_pivot_x - base_pivot_x) / denominator : 1.0f;
  }

  // Clamp and set the parameter
  p->curve_pivot_x_shift = CLAMPF(shift, -1.0f, 1.0f);

  // Update the slider visually
  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->curve_pivot_x_shift, p->curve_pivot_x_shift);
  dt_bauhaus_slider_set(g->curve_pivot_y_linear, p->curve_pivot_y_linear);
  --darktable.gui->reset;

  // Redraw and add history
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void _print_curve(const curve_and_look_params_t *curve_params)
{
  const int steps = 100;
  printf("\nCurve\n");
  for (int i = 0; i <= steps; i++)
  {
    float x = i / (float)steps;
    const float y = _apply_curve(x, curve_params);
    printf("%f\t%f\n", x, y);
  }
  printf("\n");
}

static void _calculate_adjusted_primaries(const primaries_params_t *const params,
                                          const dt_iop_order_iccprofile_info_t *const pipe_work_profile,
                                          const dt_iop_order_iccprofile_info_t *const base_profile,
                                          // outputs
                                          dt_iop_order_iccprofile_info_t *rendering_profile,
                                          dt_colormatrix_t pipe_to_base_transposed,
                                          dt_colormatrix_t base_to_rendering_transposed,
                                          dt_colormatrix_t pipe_to_rendering_transposed,
                                          dt_colormatrix_t rendering_to_base_transposed,
                                          dt_colormatrix_t base_to_pipe_transposed
                                          )
{
  // Make adjusted primaries for generating the inset matrix
  //
  // References:
  // AgX by Troy Sobotka - https://github.com/sobotka/AgX-S2O3
  // Related discussions on Blender Artists forums -
  // https://blenderartists.org/t/feedback-development-filmic-baby-step-to-a-v2/1361663
  //
  // The idea is to "inset" the work RGB data toward achromatic
  // along spectral lines before per-channel curves. This makes
  // handling of bright, saturated colors much better as the
  // per-channel process desaturates them.
  // The primaries are also rotated to compensate for Abney etc.
  // and achieve a favourable shift towards yellow.

  // First, calculate the matrix from pipe the work profile to the base profile whose primaries
  // will be rotated/inset.
  dt_colormatrix_mul(pipe_to_base_transposed,
                     pipe_work_profile->matrix_in_transposed,   // pipe -> XYZ
                     base_profile->matrix_out_transposed);      // XYZ -> base
  _print_transposed_matrix("pipe_to_base_transposed", pipe_to_base_transposed);
  printf("base_profile.nonlinearlut: %d\n", base_profile->nonlinearlut);

  mat3SSEinv(base_to_pipe_transposed, pipe_to_base_transposed);
  _print_transposed_matrix("base_to_pipe_transposed", base_to_pipe_transposed);

  // inbound path, base RGB -> inset and rotated rendering space for the curve

  // Rotated, scaled primaries are calculated based on the base profile.
  float inset_and_rotated_primaries[3][2];
  for(size_t i = 0; i < 3; i++)
    dt_rotate_and_scale_primary(base_profile, 1.f - params->inset[i], params->rotation[i], i, inset_and_rotated_primaries[i]);

  // The matrix to convert from the inset/rotated to XYZ. When applying to the RGB values that are actually
  // in the 'base' space, it will convert them to XYZ coordinates that represent colors that are partly
  // desaturated (due to the inset) and skewed (do to the rotation).
  dt_make_transposed_matrices_from_primaries_and_whitepoint(inset_and_rotated_primaries, base_profile->whitepoint,
                                                            rendering_profile->matrix_in_transposed);
  _print_transposed_matrix("rendering_profile->matrix_in_transposed", rendering_profile->matrix_in_transposed);
  dt_colormatrix_transpose(rendering_profile->matrix_in, rendering_profile->matrix_in_transposed);

  mat3SSEinv(rendering_profile->matrix_out_transposed, rendering_profile->matrix_in_transposed);
  _print_transposed_matrix("rendering_profile->matrix_out_transposed", rendering_profile->matrix_out_transposed);
  // it just holds the matrix required for luminance calculation
  rendering_profile->nonlinearlut = FALSE;

  // The matrix to convert colors from the original 'base' space to their partially desaturated and skewed
  // versions, using the inset RGB -> XYZ and the original base XYZ -> RGB matrices.
  dt_colormatrix_mul(base_to_rendering_transposed,
                      rendering_profile->matrix_in_transposed,
                      base_profile->matrix_out_transposed
                      );
  _print_transposed_matrix("base_to_rendering_transposed", base_to_rendering_transposed);

  dt_colormatrix_mul(pipe_to_rendering_transposed, pipe_to_base_transposed, base_to_rendering_transposed);
  _print_transposed_matrix("pipe_to_rendering_transposed", pipe_to_rendering_transposed);

  // outbound path, inset and rotated working space for the curve -> base RGB

  // Rotated, primaries, with optional restoration of purity. This is to be applied after the sigmoid curve;
  // it can undo the skew and recover purity (saturation).
  float outset_and_unrotated_primaries[3][2];
  for(size_t i = 0; i < 3; i++)
  {
    const float scaling = 1.f - params->master_outset_ratio * params->outset[i];
    dt_rotate_and_scale_primary(base_profile, scaling, params->master_unrotation_ratio * params->unrotation[i], i, outset_and_unrotated_primaries[i]);
  }

  // The matrix to convert the curve's output to XYZ; the primaries reflect the fact that the curve's output
  // was inset and skewed at the start of the process.
  // Its inverse (see the next steps), when applied to RGB values in the curve's working space (which actually uses
  // the base primaries), will undo the rotation and, depending on purity, push colours further from achromatic,
  // resaturating them.
  dt_colormatrix_t outset_and_unrotated_to_xyz_transposed;
  dt_make_transposed_matrices_from_primaries_and_whitepoint(outset_and_unrotated_primaries, base_profile->whitepoint,
                                                            outset_and_unrotated_to_xyz_transposed);
  _print_transposed_matrix("outset_and_unrotated_to_xyz_transposed", outset_and_unrotated_to_xyz_transposed);

  dt_colormatrix_t tmp;
  dt_colormatrix_mul(tmp,
    outset_and_unrotated_to_xyz_transposed,  // custom (outset, unrotation) -> XYZ
    base_profile->matrix_out_transposed      // XYZ -> base
    );
  _print_transposed_matrix("tmp (inverse of rendering_to_base_transposed)", tmp);
  // 'tmp' is constructed the same way as inbound_inset_and_rotated_to_xyz_transposed,
  // but this matrix will be used to remap colours to the 'base' profile, so we need to invert it.
  mat3SSEinv(rendering_to_base_transposed, tmp);
  _print_transposed_matrix("rendering_to_base_transposed", rendering_to_base_transposed);
}

static void _create_matrices_and_profiles(
    const dt_iop_agx_user_params_t *p,
    const dt_iop_order_iccprofile_info_t *pipe_work_profile,
    const dt_iop_order_iccprofile_info_t *base_profile,
    // outputs
    dt_iop_order_iccprofile_info_t *rendering_profile,
    dt_colormatrix_t pipe_to_base_transposed,
    dt_colormatrix_t base_to_rendering_transposed,
    dt_colormatrix_t pipe_to_rendering_transposed,
    dt_colormatrix_t rendering_to_base_transposed,
    dt_colormatrix_t base_to_pipe_transposed)
{
  const primaries_params_t primaries_params = _get_primaries_params(p);
  _calculate_adjusted_primaries(
    &primaries_params,
    pipe_work_profile,
    base_profile,
    rendering_profile,
    // outputs
    pipe_to_base_transposed,
    base_to_rendering_transposed,
    pipe_to_rendering_transposed,
    rendering_to_base_transposed,
    base_to_pipe_transposed);
}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4, self, piece->colors, ivoid, ovoid, roi_in, roi_out))
  {
    return;
  }

  const dt_iop_agx_user_params_t *p = piece->data;
  const float *const in = ivoid;
  float *const out = ovoid;
  const size_t n_pixels = (size_t)roi_in->width * roi_in->height;

  printf("================== start ==================\n");
  printf("range_black_relative_exposure = %f\n", p->range_black_relative_exposure);
  printf("range_white_relative_exposure = %f\n", p->range_white_relative_exposure);
  printf("curve_gamma = %f\n", p->curve_gamma);
  printf("curve_contrast_around_pivot = %f\n", p->curve_contrast_around_pivot);
  printf("curve_linear_percent_below_pivot = %f\n", p->curve_linear_percent_below_pivot);
  printf("curve_linear_percent_above_pivot = %f\n", p->curve_linear_percent_above_pivot);
  printf("curve_toe_power = %f\n", p->curve_toe_power);
  printf("curve_shoulder_power = %f\n", p->curve_shoulder_power);
  printf("curve_target_display_black_y = %f\n", p->curve_target_display_black_y);
  printf("curve_target_display_white_y = %f\n", p->curve_target_display_white_y);

  // Calculate curve parameters once
  const curve_and_look_params_t curve_params = _calculate_curve_params(p);
  _print_curve(&curve_params);

  const dt_iop_order_iccprofile_info_t *const pipe_work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  // Get the base profile based on user selection
  const dt_iop_order_iccprofile_info_t *const base_profile = _agx_get_base_profile(self->dev, pipe_work_profile, p->base_primaries);

  // Check if we got a valid base profile, fallback if necessary (already handled in _agx_get_base_profile, but double-check)
  if (!base_profile) {
    dt_print(DT_DEBUG_ALWAYS, "[agx process] Failed to obtain a valid base profile. Cannot proceed.");
    // Optionally copy input to output to avoid garbage
    if (in != out) memcpy(out, in, n_pixels * 4 * sizeof(float));
    return;
  }
  dt_print(DT_DEBUG_PIPE, "[agx process] Using base profile: %s", dt_colorspaces_get_name(base_profile->type, base_profile->filename));

  dt_colormatrix_t pipe_to_base_transposed;
  dt_colormatrix_t base_to_rendering_transposed;
  dt_colormatrix_t pipe_to_rendering_transposed;
  dt_colormatrix_t rendering_to_base_transposed;
  dt_colormatrix_t base_to_pipe_transposed;
  dt_iop_order_iccprofile_info_t rendering_profile;

  _create_matrices_and_profiles(
          p,
          pipe_work_profile, base_profile,
          &rendering_profile,
          pipe_to_base_transposed,
          base_to_rendering_transposed,
          pipe_to_rendering_transposed,
          rendering_to_base_transposed,
          base_to_pipe_transposed
  );

  dt_colormatrix_t pipe_to_base_and_back_transposed;
  dt_colormatrix_mul(pipe_to_base_and_back_transposed, pipe_to_base_transposed, base_to_pipe_transposed);
  _print_transposed_matrix("pipe_to_base_and_back_transposed", pipe_to_base_and_back_transposed);

  dt_colormatrix_t base_to_rendering_and_back_transposed;
  dt_colormatrix_mul(base_to_rendering_and_back_transposed, base_to_rendering_transposed, rendering_to_base_transposed);
  _print_transposed_matrix("base_to_rendering_and_back_transposed", base_to_rendering_and_back_transposed);

  dt_colormatrix_t rendering_to_pipe_transposed;
  dt_colormatrix_mul(rendering_to_pipe_transposed, rendering_to_base_transposed, base_to_pipe_transposed);
  _print_transposed_matrix("rendering_to_pipe_transposed", rendering_to_pipe_transposed);

  dt_colormatrix_t pipe_to_rendering_and_back_transposed;
  dt_colormatrix_mul(pipe_to_rendering_and_back_transposed, pipe_to_rendering_transposed, rendering_to_pipe_transposed);
  _print_transposed_matrix("pipe_to_rendering_and_back_transposed", pipe_to_rendering_and_back_transposed);

  DT_OMP_FOR()
  for(size_t k = 0; k < 4 * n_pixels; k += 4)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;

    // Convert to "rendering space"
    dt_aligned_pixel_t rendering_RGB;

    dt_aligned_pixel_t base_RGB;

    // go from pipe to base, compress, move to rendering
    dt_apply_transposed_color_matrix(pix_in, pipe_to_base_transposed, base_RGB);

    _compensate_low_side(
      base_RGB,
      // gamut_compression_params.threshold_in,
      // gamut_compression_params.distance_limit_in,
      base_profile
    );
    dt_apply_transposed_color_matrix(base_RGB, base_to_rendering_transposed, rendering_RGB);

    // now rendering_RGB pixel holds rendering-profile values with no negative components
    _agx_tone_mapping(rendering_RGB, &curve_params, &rendering_profile);

    dt_apply_transposed_color_matrix(rendering_RGB, rendering_to_base_transposed, base_RGB);

    // back in base (output) profile, fix any negative we may have introduced
    _compensate_low_side(
      base_RGB,
      base_profile
    );

    // bring back to pipe working space
    dt_apply_transposed_color_matrix(base_RGB, base_to_pipe_transposed, pix_out);

    // Copy over the alpha channel
    pix_out[3] = pix_in[3];
  }
}

// Plot the curve
static gboolean _agx_draw_curve(GtkWidget *widget, cairo_t *crf, dt_iop_module_t *self)
{
  dt_iop_agx_user_params_t *p = self->params;
  dt_iop_agx_gui_data_t *g = self->gui_data;

  curve_and_look_params_t curve_params = _calculate_curve_params(p);
  // Calculate current curve parameters

  // --- Boilerplate cairo/pango setup ---
  gtk_widget_get_allocation(widget, &g->allocation);
  g->allocation.height -= DT_RESIZE_HANDLE_SIZE; // Account for resize handle

  cairo_surface_t *cst =
    dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, g->allocation.width, g->allocation.height);
  PangoFontDescription *desc =
    pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  cairo_t *cr = cairo_create(cst);
  PangoLayout *layout = pango_cairo_create_layout(cr);

  pango_layout_set_font_description(layout, desc);
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);
  g->context = gtk_widget_get_style_context(widget);

  char text[256];

  // Get text metrics
  const gint font_size = pango_font_description_get_size(desc);
  pango_font_description_set_size(desc, 0.95 * font_size); // Slightly smaller font for graph
  pango_layout_set_font_description(layout, desc);

  g_strlcpy(text, "X", sizeof(text));
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &g->ink, NULL);
  g->line_height = g->ink.height;

  // Set graph dimensions and margins (simplified from filmic)
  g->inner_padding = DT_PIXEL_APPLY_DPI(4);
  g->inset = g->inner_padding;
  const float margin_left = 3. * g->line_height + 2. * g->inset; // Room for Y labels
  const float margin_bottom = 2. * g->line_height + 2. * g->inset; // Room for X labels
  const float margin_top = g->inset + 0.5 * g->line_height;
  const float margin_right = g->inset;

  g->graph_width = g->allocation.width - margin_right - margin_left;
  g->graph_height = g->allocation.height - margin_bottom - margin_top;

  // --- Drawing starts ---
  gtk_render_background(g->context, cr, 0, 0, g->allocation.width, g->allocation.height);

  // Translate origin to bottom-left of graph area for easier plotting
  cairo_translate(cr, margin_left, margin_top + g->graph_height);
  cairo_scale(cr, 1., -1.); // Flip Y axis

  // Draw graph background and border
  cairo_rectangle(cr, 0, 0, g->graph_width, g->graph_height);
  set_color(cr, darktable.bauhaus->graph_bg);
  cairo_fill_preserve(cr);
  set_color(cr, darktable.bauhaus->graph_border);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(0.5));
  cairo_stroke(cr);

  // Draw identity line (y=x)
  cairo_save(cr);
  cairo_set_source_rgba(cr, darktable.bauhaus->graph_border.red, darktable.bauhaus->graph_border.green, darktable.bauhaus->graph_border.blue, 0.5);
  cairo_move_to(cr, 0, 0);
  cairo_line_to(cr, g->graph_width, g->graph_height);
  cairo_stroke(cr);
  cairo_restore(cr);

  // --- Draw Gamma Guide Lines ---
  cairo_save(cr);
  // Use a distinct style for guides, e.g., dashed and semi-transparent
  set_color(cr, darktable.bauhaus->graph_fg); // Use foreground color for now
  cairo_set_source_rgba(cr,
                        darktable.bauhaus->graph_fg.red,
                        darktable.bauhaus->graph_fg.green,
                        darktable.bauhaus->graph_fg.blue, 0.4); // Make it semi-transparent
  double dashes[] = {4.0 / darktable.gui->ppd, 4.0 / darktable.gui->ppd}; // 4px dash, 4px gap
  cairo_set_dash(cr, dashes, 2, 0);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(0.5));

  const float linear_y_guides[] = {0.18f / 16, 0.18f / 8, 0.18f / 4, 0.18f / 2, 0.18f, 0.18f * 2, 0.18f * 4, 1.0f };
  const int num_guides = sizeof(linear_y_guides) / sizeof(linear_y_guides[0]);

  for (int i = 0; i < num_guides; ++i)
  {
      const float y_linear = linear_y_guides[i];
      const float y_pre_gamma = powf(y_linear, 1.0f / curve_params.curve_gamma);

      const float y_graph = y_pre_gamma * g->graph_height;

      cairo_move_to(cr, 0, y_graph);
      cairo_line_to(cr, g->graph_width, y_graph);
      cairo_stroke(cr);

      // Draw label for the guide line
      cairo_save(cr);
      cairo_identity_matrix(cr); // Reset transformations for text
      set_color(cr, darktable.bauhaus->graph_fg); // Use standard text color

      snprintf(text, sizeof(text), "%.2f", y_linear); // Format the linear value
      pango_layout_set_text(layout, text, -1);
      pango_layout_get_pixel_extents(layout, &g->ink, NULL);

      // Position label slightly to the left of the graph
      float label_x = margin_left - g->ink.width - g->inset / 2.0f;
      // Vertically center label on the guide line (remember Y is flipped)
      float label_y = margin_top + g->graph_height - y_graph - g->ink.height / 2.0f - g->ink.y;

      // Ensure label stays within vertical bounds of the graph area
      label_y = CLAMPF(label_y, margin_top - g->ink.height / 2.0f - g->ink.y, margin_top + g->graph_height - g->ink.height / 2.0f - g->ink.y);

      cairo_move_to(cr, label_x, label_y);
      pango_cairo_show_layout(cr, layout);
      cairo_restore(cr);
  }

  // Restore original drawing state (solid line, etc.)
  cairo_restore(cr); // Matches cairo_save(cr) at the beginning of this block
  // --- End Draw Gamma Guide Lines ---

  // --- Draw Vertical EV Guide Lines ---
  cairo_save(cr);
  // Use the same style as horizontal guides
  set_color(cr, darktable.bauhaus->graph_fg);
  cairo_set_source_rgba(cr,
                        darktable.bauhaus->graph_fg.red,
                        darktable.bauhaus->graph_fg.green,
                        darktable.bauhaus->graph_fg.blue, 0.4);
  cairo_set_dash(cr, dashes, 2, 0); // Use the same dash pattern
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(0.5));

  float min_ev = curve_params.min_ev;
  float max_ev = curve_params.max_ev;
  float range_in_ev = curve_params.range_in_ev;

  if (range_in_ev > _epsilon) // Avoid division by zero or tiny ranges
  {
    for (int ev = ceilf(min_ev); ev <= floorf(max_ev); ++ev)
    {
      float x_norm = (ev - min_ev) / range_in_ev;
      // Clamp to ensure it stays within the graph bounds if min/max_ev aren't exactly integers
      x_norm = CLAMPF(x_norm, 0.0f, 1.0f);
      float x_graph = x_norm * g->graph_width;

      cairo_move_to(cr, x_graph, 0);
      cairo_line_to(cr, x_graph, g->graph_height);
      cairo_stroke(cr);

      // Draw label for the EV guide line
      if (ev % 5 == 0 || ev == ceilf(min_ev) || ev == floorf(max_ev))
      {
        cairo_save(cr);
        cairo_identity_matrix(cr); // Reset transformations for text
        set_color(cr, darktable.bauhaus->graph_fg);
        snprintf(text, sizeof(text), "%d", ev); // Format the EV value
        pango_layout_set_text(layout, text, -1);
        pango_layout_get_pixel_extents(layout, &g->ink, NULL);
        // Position label slightly below the x-axis, centered horizontally
        float label_x = margin_left + x_graph - g->ink.width / 2.0f - g->ink.x;
        float label_y = margin_top + g->graph_height + g->inset / 2.0f;
        // Ensure label stays within horizontal bounds
        label_x = CLAMPF(label_x, margin_left - g->ink.width / 2.0f - g->ink.x, margin_left + g->graph_width - g->ink.width / 2.0f - g->ink.x);
        cairo_move_to(cr, label_x, label_y);
        pango_cairo_show_layout(cr, layout);
        cairo_restore(cr);
      }
    }
  }
  cairo_restore(cr); // Matches cairo_save(cr) at the beginning of this block
  // --- End Draw Vertical EV Guide Lines ---

  // Draw the curve
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  set_color(cr, darktable.bauhaus->graph_fg);

  const int steps = 200;
  for (int k = 0; k <= steps; k++)
  {
    float x_norm = (float)k / steps; // Input to the curve [0, 1]
    float y_norm = _apply_curve(x_norm, &curve_params);

    // Map normalized coords [0,1] to graph pixel coords
    const float x_graph = x_norm * g->graph_width;
    const float y_graph = y_norm * g->graph_height;

    if (k == 0)
      cairo_move_to(cr, x_graph, y_graph);
    else
      cairo_line_to(cr, x_graph, y_graph);
  }
  cairo_stroke(cr);

  // Draw the pivot point
  cairo_save(cr);
  cairo_rectangle(cr, -DT_PIXEL_APPLY_DPI(4.), -DT_PIXEL_APPLY_DPI(4.),
                  g->graph_width + 2. * DT_PIXEL_APPLY_DPI(4.), g->graph_height + 2. * DT_PIXEL_APPLY_DPI(4.));
  cairo_clip(cr);

  const float x_pivot_graph = curve_params.pivot_x * g->graph_width;
  const float y_pivot_graph = curve_params.pivot_y * g->graph_height;
  set_color(cr, darktable.bauhaus->graph_fg_active); // Use a distinct color, e.g., active foreground
  cairo_arc(cr, x_pivot_graph, y_pivot_graph, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI); // Adjust radius as needed
  cairo_fill(cr);
  cairo_stroke(cr);
  cairo_restore(cr);

  // --- Cleanup ---
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  g_object_unref(layout);
  pango_font_description_free(desc);

  return FALSE; // Propagate event further? Usually FALSE for draw signals
}


// Init
void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);
}

// Cleanup
void cleanup(dt_iop_module_t *self)
{
  free(self->params);
  self->params = NULL;
  free(self->default_params);
  self->default_params = NULL;
}

// GUI changed
void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  const dt_iop_agx_gui_data_t *g = self->gui_data;
  // Trigger redraw when any parameter changes
  if (g && g->area) {
    gtk_widget_queue_draw(GTK_WIDGET(g->area));
  }
}

// GUI update (called when module UI is shown/refreshed)
void gui_update(dt_iop_module_t *self)
{
  const dt_iop_agx_gui_data_t *g = self->gui_data;
  const dt_iop_agx_user_params_t *params = self->params;

  // Ensure the graph is drawn initially
  if (g && g->area) {
    gtk_widget_queue_draw(GTK_WIDGET(g->area));
  }
}

static void _add_look_box(dt_iop_module_t *self, GtkWidget *box, dt_iop_agx_gui_data_t *gui_data)
{
  GtkWidget *main_box = self->widget;
  // Look Section
  dt_gui_new_collapsible_section(&gui_data->look_section, "plugins/darkroom/agx/expand_look_params", _("look"), GTK_BOX(box), DT_ACTION(self));

  self->widget = GTK_WIDGET(gui_data->look_section.container);
  // Reuse the slider variable for all sliders instead of creating new ones in each scope
  GtkWidget *slider;

  // look_offset
  slider = dt_bauhaus_slider_from_params(self, "look_offset");
  dt_bauhaus_slider_set_soft_range(slider, -0.5f, 0.5f);
  gtk_widget_set_tooltip_text(slider, _("deepen or lift shadows"));  // Tooltip text for look_offset

  // look_slope
  slider = dt_bauhaus_slider_from_params(self, "look_slope");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 2.0f);
  gtk_widget_set_tooltip_text(slider, _("decrease or increase contrast and brightness"));

  // look_power
  slider = dt_bauhaus_slider_from_params(self, "look_power");
  dt_bauhaus_slider_set_soft_range(slider, 0.5f, 2.0f);
  gtk_widget_set_tooltip_text(slider, _("increase or decrease brightness"));

  // look_saturation
  slider = dt_bauhaus_slider_from_params(self, "look_saturation");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 2.0f);
  gtk_widget_set_tooltip_text(slider, _("decrease or increase saturation"));

  // look_original_hue_mix_ratio
  slider = dt_bauhaus_slider_from_params(self, "look_original_hue_mix_ratio");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 1.0f);
  gtk_widget_set_tooltip_text(slider, _("Hue mix ratio adjustment"));

  self->widget = main_box; // Restore previous widget context
}

static void _add_base_box(dt_iop_module_t *self, GtkWidget *box, dt_iop_agx_gui_data_t *gui_data)
{
  GtkWidget *main_box = self->widget; // save

  GtkWidget *base_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(box), base_box, TRUE, TRUE, 0);

  // Area section – is added first so that it is at the top
  dt_gui_new_collapsible_section(&gui_data->area_section, "plugins/darkroom/agx/expand_area_params",
    _("show curve"), GTK_BOX(base_box), DT_ACTION(self));

  GtkWidget *area_container = GTK_WIDGET(gui_data->area_section.container);
  gui_data->area = GTK_DRAWING_AREA(dt_ui_resize_wrap(NULL,
                                                      0, // Initial height factor
                                                      "plugins/darkroom/agx/graphheight")); // Conf key
  g_object_set_data(G_OBJECT(gui_data->area), "iop-instance", self);
  dt_action_define_iop(self, NULL, N_("graph"), GTK_WIDGET(gui_data->area), NULL);
  gtk_widget_set_can_focus(GTK_WIDGET(gui_data->area), TRUE);
  g_signal_connect(G_OBJECT(gui_data->area), "draw", G_CALLBACK(_agx_draw_curve), self);
  gtk_widget_set_tooltip_text(GTK_WIDGET(gui_data->area), _("tone mapping curve"));

  // Pack drawing area at the top
  gtk_box_pack_start(GTK_BOX(area_container), GTK_WIDGET(gui_data->area), TRUE, TRUE, 0);

  //separated picker box for black/white  relative exposure
  GtkWidget *picker_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(box), picker_box, TRUE, TRUE, 0);
  self->widget = picker_box;

  // Create section label
  dt_gui_box_add(self->widget, dt_ui_section_label_new(C_("section", "Input exposure range")));

  // Create black point slider and associate picker
  gui_data->range_black_exposure = dt_color_picker_new(self, DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_DENOISE, dt_bauhaus_slider_from_params(self, "range_black_relative_exposure"));
  dt_bauhaus_slider_set_soft_range(gui_data->range_black_exposure, -20.0f, -1.0f);
  dt_bauhaus_slider_set_format(gui_data->range_black_exposure, _(" EV"));
  gtk_widget_set_tooltip_text(gui_data->range_black_exposure, _("relative exposure below mid-grey (black point)"));

  // Create white point slider and associate picker
  gui_data->range_white_exposure = dt_color_picker_new(self, DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_DENOISE, dt_bauhaus_slider_from_params(self, "range_white_relative_exposure"));
  dt_bauhaus_slider_set_soft_range(gui_data->range_white_exposure, 1.0f, 20.0f);
  dt_bauhaus_slider_set_format(gui_data->range_white_exposure, _(" EV"));
  gtk_widget_set_tooltip_text(gui_data->range_white_exposure, _("relative exposure above mid-grey (white point)"));

  // Auto tune slider (similar to filmic's)
  gui_data->auto_tune_picker = dt_color_picker_new(self, DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_DENOISE, dt_bauhaus_combobox_new(self));
  dt_bauhaus_widget_set_label(gui_data->auto_tune_picker, NULL, N_("auto tune levels"));
  gtk_widget_set_tooltip_text(gui_data->auto_tune_picker, _("pick image area to automatically set black and white exposure"));
  gtk_box_pack_start(GTK_BOX(self->widget), gui_data->auto_tune_picker, TRUE, TRUE, 0);

  GtkWidget *curve_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(box), curve_box, TRUE, TRUE, 0);
  self->widget = curve_box;

  dt_gui_box_add(self->widget, dt_ui_section_label_new(C_("section", "curve parameters")));

  // Reuse slider variable for all sliders that use _add_slider_with_tooltip
  GtkWidget *slider;

  // curve_gamma
  slider = dt_bauhaus_slider_from_params(self, "curve_gamma");
  dt_bauhaus_slider_set_soft_range(slider, 1.0f, 5.0f);
  gtk_widget_set_tooltip_text(slider, _("Fine-tune contrast, shifts representation of pivot along the y axis"));

  // curve_pivot_x_shift with picker
  gui_data->curve_pivot_x_shift = dt_color_picker_new(self, DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_DENOISE, dt_bauhaus_slider_from_params(self, "curve_pivot_x_shift"));
  dt_bauhaus_slider_set_soft_range(gui_data->curve_pivot_x_shift, -0.4f, 0.4f);
  gtk_widget_set_tooltip_text(gui_data->curve_pivot_x_shift, _("Pivot x shift towards black(-) or white(+)"));

  // curve_pivot_y_linear
  gui_data->curve_pivot_y_linear = dt_bauhaus_slider_from_params(self, "curve_pivot_y_linear");
  dt_bauhaus_slider_set_soft_range(gui_data->curve_pivot_y_linear, 0.0f, 0.5f);
  gtk_widget_set_tooltip_text(gui_data->curve_pivot_y_linear, _("Pivot y (linear output)"));

  // curve_contrast_around_pivot
  slider = dt_bauhaus_slider_from_params(self, "curve_contrast_around_pivot");
  dt_bauhaus_slider_set_soft_range(slider, 0.1f, 5.0f);
  gtk_widget_set_tooltip_text(slider, _("linear section slope"));

  // curve_toe_power
  slider = dt_bauhaus_slider_from_params(self, "curve_toe_power");
  dt_bauhaus_slider_set_soft_range(slider, 0.2f, 5.0f);
  gtk_widget_set_tooltip_text(slider, _("contrast in shadows"));

  // curve_shoulder_power
  slider = dt_bauhaus_slider_from_params(self, "curve_shoulder_power");
  dt_bauhaus_slider_set_soft_range(slider, 0.2f, 5.0f);
  gtk_widget_set_tooltip_text(slider, _("contrast in highlights"));

  self->widget = main_box;
}

static void _add_advanced_box(dt_iop_module_t *self, GtkWidget *box, dt_iop_agx_gui_data_t *gui_data)
{
  GtkWidget *main_box = self->widget;

  // Tone Mapping Section advanced
  dt_gui_new_collapsible_section(&gui_data->advanced_section, "plugins/darkroom/agx/expand_curve_advanced",
                                 _("advanced"), GTK_BOX(box), DT_ACTION(self));
  self->widget = GTK_WIDGET(gui_data->advanced_section.container);
   // Reuse the slider variable for all sliders
  GtkWidget *slider;

  // Toe length
  slider = dt_bauhaus_slider_from_params(self, "curve_linear_percent_below_pivot");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 100.0f);
  gtk_widget_set_tooltip_text(slider, _("toe length"));

  // Toe intersection point
  slider = dt_bauhaus_slider_from_params(self, "curve_target_display_black_y");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 1.0f);
  gtk_widget_set_tooltip_text(slider, _("toe intersection point"));

  // Shoulder length
  slider = dt_bauhaus_slider_from_params(self, "curve_linear_percent_above_pivot");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 100.0f);
  gtk_widget_set_tooltip_text(slider, _("shoulder length"));

  // Shoulder intersection point
  slider = dt_bauhaus_slider_from_params(self, "curve_target_display_white_y");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 2.0f);
  gtk_widget_set_tooltip_text(slider, _("shoulder intersection point"));

  self->widget = main_box;
}

static void _add_primaries_box(dt_iop_module_t *self, GtkWidget *box, dt_iop_agx_gui_data_t *gui_data)
{
  GtkWidget *main_box = self->widget;
  GtkWidget *slider;

  // primaries collapsible section
  dt_gui_new_collapsible_section(&gui_data->primaries_section, "plugins/darkroom/agx/expand_primaries",
                                 _("primaries"), GTK_BOX(box), DT_ACTION(self));
  gtk_widget_set_tooltip_text(gui_data->primaries_section.expander, _("set custom primaries"));

  self->widget = GTK_WIDGET(gui_data->primaries_section.container);
  dt_iop_module_t *sect = DT_IOP_SECTION_FOR_PARAMS(self, N_("primaries"));

  GtkWidget *base_primaries_combo = dt_bauhaus_combobox_from_params(self, "base_primaries");
  gtk_widget_set_tooltip_text(base_primaries_combo, _("Color space primaries to use as the base for below adjustments.\n"
                                                      "'export profile' uses the profile set in 'output color profile'."));

  const float desaturation = 0.2f;
#define SETUP_COLOR_COMBO(color, r, g, b, inset_tooltip, rotation_tooltip)                                        \
  slider = dt_bauhaus_slider_from_params(sect, #color "_inset");                                                  \
  dt_bauhaus_slider_set_format(slider, "%");                                                                      \
  dt_bauhaus_slider_set_digits(slider, 1);                                                                        \
  dt_bauhaus_slider_set_factor(slider, 100.f);                                                                    \
  dt_bauhaus_slider_set_soft_range(slider, 0.f, 0.5f);                                                            \
  dt_bauhaus_slider_set_stop(slider, 0.f, r, g, b);                                                               \
  gtk_widget_set_tooltip_text(slider, inset_tooltip);                                                             \
                                                                                                                  \
  slider = dt_bauhaus_slider_from_params(sect, #color "_rotation");                                               \
  dt_bauhaus_slider_set_format(slider, "°");                                                                      \
  dt_bauhaus_slider_set_digits(slider, 1);                                                                        \
  dt_bauhaus_slider_set_factor(slider, 180.f / M_PI_F);                                                           \
  dt_bauhaus_slider_set_stop(slider, 0.f, r, g, b);                                                               \
  gtk_widget_set_tooltip_text(slider, rotation_tooltip);

  SETUP_COLOR_COMBO(red, 1.f - desaturation, desaturation, desaturation, _("attenuate the purity of the red primary"),
                    _("rotate the red primary"));
  SETUP_COLOR_COMBO(green, desaturation, 1.f - desaturation, desaturation, _("attenuate the purity of the green primary"),
                    _("rotate the green primary"));
  SETUP_COLOR_COMBO(blue, desaturation, desaturation, 1.f - desaturation, _("attenuate the purity of the blue primary"),
                    _("rotate the blue primary"));
#undef SETUP_COLOR_COMBO

  slider = dt_bauhaus_slider_from_params(sect, "master_outset_ratio");
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_digits(slider, 0);
  dt_bauhaus_slider_set_factor(slider, 100.f);
  gtk_widget_set_tooltip_text(slider, _("overall purity boost"));

  slider = dt_bauhaus_slider_from_params(sect, "master_unrotation_ratio");
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_digits(slider, 0);
  dt_bauhaus_slider_set_factor(slider, 100.f);
  gtk_widget_set_tooltip_text(slider, _("overall unrotation ratio"));


#define SETUP_COLOR_COMBO(color, r, g, b, inset_tooltip, rotation_tooltip)                                        \
  slider = dt_bauhaus_slider_from_params(sect, #color "_outset");                                                  \
  dt_bauhaus_slider_set_format(slider, "%");                                                                      \
  dt_bauhaus_slider_set_digits(slider, 1);                                                                        \
  dt_bauhaus_slider_set_factor(slider, 100.f);                                                                    \
  dt_bauhaus_slider_set_soft_range(slider, 0.f, 0.5f);                                                            \
  dt_bauhaus_slider_set_stop(slider, 0.f, r, g, b);                                                               \
  gtk_widget_set_tooltip_text(slider, inset_tooltip);                                                             \
                                                                                                                  \
  slider = dt_bauhaus_slider_from_params(sect, #color "_unrotation");                                               \
  dt_bauhaus_slider_set_format(slider, "°");                                                                      \
  dt_bauhaus_slider_set_digits(slider, 1);                                                                        \
  dt_bauhaus_slider_set_factor(slider, 180.f / M_PI_F);                                                           \
  dt_bauhaus_slider_set_stop(slider, 0.f, r, g, b);                                                               \
  gtk_widget_set_tooltip_text(slider, rotation_tooltip);

  SETUP_COLOR_COMBO(red, 1.f - desaturation, desaturation, desaturation, _("boost the purity of the red primary"),
                    _("unrotate the red primary"));
  SETUP_COLOR_COMBO(green, desaturation, 1.f - desaturation, desaturation, _("boost the purity of the green primary"),
                    _("unrotate the green primary"));
  SETUP_COLOR_COMBO(blue, desaturation, desaturation, 1.f - desaturation, _("boost the purity of the blue primary"),
                    _("unrotate the blue primary"));
#undef SETUP_COLOR_COMBO

  self->widget = main_box;
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_agx_gui_data_t *gui_data = IOP_GUI_ALLOC(agx);

  // Initialize drawing cache fields
  gui_data->line_height = 0;
  gui_data->sign_width = 0;
  gui_data->zero_width = 0;
  gui_data->graph_width = 0;
  gui_data->graph_height = 0;
  gui_data->inset = 0;
  gui_data->inner_padding = 0;
  gui_data->context = NULL;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // so we can restore it later
  GtkWidget *self_widget = self->widget;

  // define the boxes
  GtkWidget *look_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(self->widget), look_box, TRUE, TRUE, 0);
  GtkWidget *tonemap_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(self->widget), tonemap_box, TRUE, TRUE, 0);
  GtkWidget *advanced_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(self->widget), advanced_box, TRUE, TRUE, 0);
  GtkWidget *primaries_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(self->widget), primaries_box, TRUE, TRUE, 0);

   _add_look_box(self, look_box, gui_data);
   _add_base_box(self, tonemap_box, gui_data);
   _add_advanced_box(self, advanced_box, gui_data);
  _add_primaries_box(self, primaries_box, gui_data);

  self->widget = self_widget;
}

static float _degrees_to_radians(float degrees)
{
  return degrees * M_PI_F / 180.f;
}

static void _set_neutral_params(dt_iop_agx_user_params_t* p)
{
  p->look_slope = 1.0f;
  p->look_power = 1.0f;
  p->look_offset = 0.0f;
  p->look_saturation = 1.0f;
  p->look_original_hue_mix_ratio = 0.0f;

  p->range_black_relative_exposure = -10;
  p->range_white_relative_exposure = 6.5;

  p->curve_contrast_around_pivot = 2.4;
  p->curve_linear_percent_below_pivot = 0.0;
  p->curve_linear_percent_below_pivot = 0.0;
  p->curve_toe_power = 1.5;
  p->curve_shoulder_power = 1.5;
  p->curve_target_display_black_y = 0.0;
  p->curve_target_display_white_y = 1.0;
  p->curve_gamma = 2.2;
  p->curve_pivot_x_shift = 0.0;
  p->curve_pivot_y_linear = 0.18;

  p->red_inset = 0.0f;
  p->red_rotation = 0.0;
  p->green_inset = 0.0;
  p->green_rotation = 0.0f;
  p->blue_inset = 0.0f;
  p->blue_rotation = 0.0f;

  p->master_outset_ratio = 1.0f;
  p->master_unrotation_ratio = 1.0f;
  p->red_outset = 0.0f;
  p->red_unrotation = 0.0f;
  p->green_outset = 0.0f;
  p->green_unrotation = 0.0f;
  p->blue_outset = 0.0f;
  p->blue_unrotation = 0.0f;

  p->base_primaries = DT_AGX_EXPORT_PROFILE;
}

void init_presets(dt_iop_module_so_t *self)
{
  const char *workflow = dt_conf_get_string_const("plugins/darkroom/workflow");
  const gboolean auto_apply_agx = strcmp(workflow, "scene-referred (agx)") == 0;

  if(auto_apply_agx)
  {
    dt_gui_presets_add_generic(_("scene-referred default"), self->op, self->version(), NULL, 0, 1,
                               DEVELOP_BLEND_CS_RGB_SCENE);

    dt_gui_presets_update_format(_("scene-referred default"), self->op, self->version(), FOR_RAW | FOR_MATRIX);

    dt_gui_presets_update_autoapply(_("scene-referred default"), self->op, self->version(), TRUE);
  }

  dt_iop_agx_user_params_t p = { 0 };

  _set_neutral_params(&p);
  // AgX primaries settings from Eary_Chow
  // https://discuss.pixls.us/t/blender-agx-in-darktable-proof-of-concept/48697/1018
  p.red_inset = 0.32965205f;
  p.green_inset = 0.28051336f;
  p.blue_inset = 0.12475368f;
  p.red_rotation = _degrees_to_radians(2.13976149);
  p.green_rotation = _degrees_to_radians(-1.22827335f);
  p.blue_rotation = _degrees_to_radians(-3.05174246f);
  p.red_outset = 0.32317438f;
  p.green_outset = 0.28325605f;
  p.blue_outset = 0.0374326f;
  p.red_unrotation = _degrees_to_radians(0.0f);
  p.green_unrotation = _degrees_to_radians(0.0f);
  p.blue_unrotation = _degrees_to_radians(0.0f);
  // Restore purity
  p.master_outset_ratio = 1.0f;
  p.master_unrotation_ratio = 1.0f;
  p.base_primaries = DT_AGX_REC2020;

  dt_gui_presets_add_generic(_("blender-like|base"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // Punchy preset
  p.look_power = 1.35f;
  p.look_offset = 0.0f;
  p.look_saturation = 1.4f;
  dt_gui_presets_add_generic(_("blender-like|punchy"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  _set_neutral_params(&p);
  // Sigmoid 'smooth' primaries settings
  p.red_inset = 0.1f;
  p.green_inset = 0.1f;
  p.blue_inset = 0.15f;
  p.red_rotation = _degrees_to_radians(2.f);
  p.green_rotation = _degrees_to_radians(-1.f);
  p.blue_rotation = _degrees_to_radians(-3.f);
  p.red_outset = 0.1f;
  p.green_outset = 0.1f;
  p.blue_outset = 0.15f;
  p.red_unrotation = _degrees_to_radians(2.f);
  p.green_unrotation = _degrees_to_radians(-1.f);
  p.blue_unrotation = _degrees_to_radians(-3.f);
  // Don't restore purity - try to avoid posterization.
  p.master_outset_ratio = 0.0f;
  p.master_unrotation_ratio = 1.0f;
  p.base_primaries = DT_AGX_WORK_PROFILE;

  dt_gui_presets_add_generic(_("smooth|base"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // 'Punchy' look
  p.look_power = 1.35f;
  p.look_offset = 0.0f;
  p.look_saturation = 1.4f;
  dt_gui_presets_add_generic(_("smooth|punchy"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);
}

// GUI cleanup
void gui_cleanup(dt_iop_module_t *self)
{
   // Nothing specific to clean up beyond default IOP gui alloc
}

// Callback for color pickers
void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker,
                        dt_dev_pixelpipe_t *pipe)
{
  dt_iop_agx_gui_data_t *g = self->gui_data;

  if(picker == g->range_black_exposure) apply_auto_black_exposure(self);
  else if(picker == g->range_white_exposure) apply_auto_white_exposure(self);
  else if(picker == g->auto_tune_picker) apply_auto_tune_exposure(self);
  else if(picker == g->curve_pivot_x_shift) apply_auto_pivot_x(self, dt_ioppr_get_pipe_work_profile_info(pipe));
}
