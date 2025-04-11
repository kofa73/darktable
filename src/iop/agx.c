#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "iop/iop_api.h"
#include "gui/draw.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "control/control.h"
#include "develop/develop.h"
#include "common/iop_profile.h"
#include <gtk/gtk.h>
#include <math.h> // For math functions
#include <stdlib.h>
#include <pango/pangocairo.h> // For text rendering in graph

// Module introspection version
DT_MODULE_INTROSPECTION(1, dt_iop_agx_user_params_t)

// so we have a breakpoint target in error-handling branches, to be removed after debugging
static int errors = 0; // Use static if needed within this file only

const float _epsilon = 1E-6f;

// Module parameters struct
// Updated struct dt_iop_agx_user_params_t
typedef struct dt_iop_agx_user_params_t
{
  // look params
  float look_offset;      // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "Offset"
  float look_slope;       // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "Slope"
  float look_power;       // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "Power"
  float look_saturation;  // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "Saturation"
  float look_original_hue_mix_ratio;    // $MIN: 0.0 $MAX: 1 $DEFAULT: 0.0 $DESCRIPTION: "Restore original hue"

  // log mapping params
  float range_black_relative_exposure;  // $MIN: -20.0 $MAX: -0.1 $DEFAULT: -10 $DESCRIPTION: "Black relative exposure (below mid-gray)"
  float range_white_relative_exposure;  // $MIN: 0.1 $MAX: 20 $DEFAULT: 6.5 $DESCRIPTION: "White relative exposure (above mid-gray)"

  // curve params - comments indicate the original variables from https://www.desmos.com/calculator/yrysofmx8h
  // Corresponds to p_x, but not directly -- allows shifting the default 0.18 towards black or white relative exposure
  float curve_pivot_x_shift;      // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0 $DESCRIPTION: "Pivot x shift towards black(-) or white(+)"
  // Corresponds to p_y, but not directly -- needs application of gamma
  float curve_pivot_y_linear;            // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.18 $DESCRIPTION: "Pivot y (linear)"
  // P_slope
  float curve_contrast_around_pivot;      // $MIN: 0.1 $MAX: 10.0 $DEFAULT: 2.4 $DESCRIPTION: "Contrast around the pivot"
  // P_tlength
  float curve_linear_percent_below_pivot;  // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "Toe start %, below the pivot"
  // P_slength
  float curve_linear_percent_above_pivot;  // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "Shoulder start %, above the pivot"
  // t_p
  float curve_toe_power;                  // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.5 $DESCRIPTION: "Toe power; contrast in shadows"
  // s_p -> Renamed from curve_shoulder_power for clarity
  float curve_shoulder_power;             // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.5 $DESCRIPTION: "Shoulder power; contrast in highlights"
  // we don't have a parameter for pivot_x, it's set to the x value representing mid-gray, splitting [0..1] in the ratio
  // range_black_relative_exposure : range_white_relative_exposure
  // not a parameter of the original curve, they used p_x, p_y to directly set the pivot
  float curve_gamma;                // $MIN: 1.0 $MAX: 5.0 $DEFAULT: 2.2 $DESCRIPTION: "Curve y gamma"
  // t_ly
  float curve_target_display_black_y;     // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "Target display black"
  // s_ly
  float curve_target_display_white_y;     // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "Target display white"
} dt_iop_agx_user_params_t;


typedef struct dt_iop_agx_gui_data_t
{
  dt_gui_collapsible_section_t tone_mapping_section;
  dt_gui_collapsible_section_t advanced_section;
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
  GtkWidget *range_black_slider;
  GtkWidget *range_white_slider;
  GtkWidget *curve_pivot_x_shift_slider;
  GtkWidget *curve_pivot_y_linear_slider;
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

typedef struct
{
  float r, g, b;
} float3;

typedef struct
{
  float m[3][3];
} mat3f;


// Helper function: matrix multiplication
static float3 _mat3f_mul_float3(const mat3f m, const float3 v)
{
  float3 result;
  result.r = m.m[0][0] * v.r + m.m[0][1] * v.g + m.m[0][2] * v.b;
  result.g = m.m[1][0] * v.r + m.m[1][1] * v.g + m.m[1][2] * v.b;
  result.b = m.m[2][0] * v.r + m.m[2][1] * v.g + m.m[2][2] * v.b;
  return result;
}

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

// AgX implementation

const mat3f AgXInsetMatrix = { { { 0.856627153315983f, 0.0951212405381588f, 0.0482516061458583f },
                                 { 0.137318972929847f, 0.761241990602591f, 0.101439036467562f },
                                 { 0.11189821299995f, 0.0767994186031903f, 0.811302368396859f } } };


const mat3f AgXInsetMatrixInverse = { { { 1.1974410768877f, -0.14426151269800f, -0.053179564189704f },
                                        { -0.19647462632135f, 1.3540951314697f, -0.15762050514838f },
                                        { -0.14655741710660f, -0.10828405878847f, 1.2548414758951f } } };

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
  const float value = x / powf(1.0f + powf(x, power), 1.0f / power);
  if(isnan(value))
  {
    errors++; // printf("_exponential returns nan\n");
  }
  return value;
}

static float _exponential_curve(float x, float scale, float slope, float power, float transition_x,
                                float transition_y)
{
  // this is f_ss, f_ts on the original curve https://www.desmos.com/calculator/yrysofmx8h
  const float value = scale * _exponential(slope * (x - transition_x) / scale, power) + transition_y;
  if(isnan(value))
  {
    errors++; // printf("_exponential_curve returns nan\n");
  }
  return value;
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

// the commented values (t_tx, etc) are references to https://www.desmos.com/calculator/yrysofmx8h
static float _calculate_curve(const float x, const curve_and_look_params_t *curve_params)
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

  return CLAMP(result, curve_params->target_black, curve_params->target_white);
}

// 'lerp', but take care of the boundary: hue wraps around 1 -> 0
static float _lerp_hue(float hue1, float hue2, float mix)
{
  const float hue_diff = hue2 - hue1;

  if(hue_diff > 0.5)
  {
    hue2 -= 1;
  }
  else if(hue_diff < -0.5)
  {
    hue2 += 1;
  }

  float hue_out = hue2 + (hue1 - hue2) * mix;
  if(hue_out < 0)
  {
    hue_out += 1;
  }
  else if(hue_out > 1)
  {
    hue_out -= 1;
  }
  return hue_out;
}

static float apply_slope_offset(float x, float slope, float offset)
{
  // negative offset should darken the image; positive brighten it
  // without the scale: m = 1 / (1 + offset)
  // offset = 1, slope = 1, x = 0 -> m = 1 / (1+1) = 1/2, b = 1 * 1/2 = 1/2, y = 1/2*0 + 1/2 = 1/2
  const float m = slope / (1 + offset);
  const float b = offset * m;
  return fmaxf(0.0f, m * x + b);
  // ASC CDL:
  // return x * slope + offset;
  // alternative:
  // y = mx + b, b is the offset, m = (1 - offset), so the line runs from (0, offset) to (1, 1)
  //return (1 - offset) * x + offset;
}

// https://docs.acescentral.com/specifications/acescct/#appendix-a-application-of-asc-cdl-parameters-to-acescct-image-data
static float3 _agxLook(float3 val, const curve_and_look_params_t *params)
{
  // Default
  const float slope = params->look_slope;
  const float offset = params->look_offset;
  const float sat = params->look_saturation;

  // ASC CDL
  const float slope_and_offset_r = apply_slope_offset(val.r, slope, offset);
  const float slope_and_offset_g = apply_slope_offset(val.g, slope, offset);
  const float slope_and_offset_b = apply_slope_offset(val.b, slope, offset);

  const float out_r = slope_and_offset_r > 0 ? powf(slope_and_offset_r, params->look_power) : slope_and_offset_r;
  const float out_g = slope_and_offset_r > 0 ? powf(slope_and_offset_g, params->look_power) : slope_and_offset_g;
  const float out_b = slope_and_offset_r > 0 ? powf(slope_and_offset_b, params->look_power) : slope_and_offset_b;

  // Using Rec 2020 Y coefficients (we use insetting, so this is probably incorrect)
  const float luma = 0.2626983389565561f * out_r + 0.6780087657728164f * out_g + 0.05929289527062728f * out_b;

  float3 result;
  result.r = luma + sat * (out_r - luma);
  result.g = luma + sat * (out_g - luma);
  result.b = luma + sat * (out_b - luma);

  return result;
}

static float3 _apply_log_encoding(dt_aligned_pixel_t pixel, float range_in_ev, float minEv)
{
  // Assume input is linear Rec2020 relative to 0.18 mid gray
  // Ensure all values are > 0 before log
  float3 v = { fmaxf(_epsilon, pixel[0] / 0.18f),
               fmaxf(_epsilon, pixel[1] / 0.18f),
               fmaxf(_epsilon, pixel[2] / 0.18f)
  };

  // Log2 encoding
  v.r = log2f(v.r);
  v.g = log2f(v.g);
  v.b = log2f(v.b);

  // normalise to [0, 1] based on minEv and range_in_ev
  v.r = (v.r - minEv) / range_in_ev;
  v.g = (v.g - minEv) / range_in_ev;
  v.b = (v.b - minEv) / range_in_ev;

  // Clamp result to [0, 1] - this is the input domain for the curve
  v.r = CLAMP(v.r, 0.0f, 1.0f);
  v.g = CLAMP(v.g, 0.0f, 1.0f);
  v.b = CLAMP(v.b, 0.0f, 1.0f);

  return v;
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


// Helper to calculate curve parameters based on module params
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
  params.pivot_y = powf(CLAMP(user_params->curve_pivot_y_linear, user_params->curve_target_display_black_y, user_params->curve_target_display_white_y),
    1.0f / params.curve_gamma
  );
  printf("pivot(%f, %f) at gamma = %f\n", pivot_x, params.pivot_y, params.curve_gamma);

  // avoid range altering slope - 16.5 EV is the default AgX range; keep the meaning of slope
  params.slope = user_params->curve_contrast_around_pivot * (params.range_in_ev / 16.5f);
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

  // if x went from toe_transition_x to 0, at the given slope, starting from toe_transition_y, where would we intersect the y axis?
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

  //if(isnan(shoulder_scale))
  //{
  //  errors++; // printf("shoulder_scale is NaN\n");
  //}

  return params;
}


static float3 _agx_tone_mapping(float3 rgb, const curve_and_look_params_t * params)
{
  // Apply Inset Matrix
  rgb = _mat3f_mul_float3(AgXInsetMatrix, rgb);

  dt_aligned_pixel_t rgb_pixel;
  rgb_pixel[0] = rgb.r;
  rgb_pixel[1] = rgb.g;
  rgb_pixel[2] = rgb.b;
  rgb_pixel[3] = 0.0f; // Alpha if needed

  // record current chromaticity angle
  dt_aligned_pixel_t hsv_pixel;
  dt_RGB_2_HSV(rgb_pixel, hsv_pixel);
  const float h_before = hsv_pixel[0];

  float3 log_pixel = _apply_log_encoding(rgb_pixel, params->range_in_ev, params->min_ev);

  // Apply curve using cached parameters
  log_pixel.r = _calculate_curve(log_pixel.r, params);
  log_pixel.g = _calculate_curve(log_pixel.g, params);
  log_pixel.b = _calculate_curve(log_pixel.b, params);

  // Apply AgX look
  log_pixel = _agxLook(log_pixel, params);

  // Linearize
  rgb_pixel[0] = powf(fmaxf(0.0f, log_pixel.r), params->curve_gamma);
  rgb_pixel[1] = powf(fmaxf(0.0f, log_pixel.g), params->curve_gamma);
  rgb_pixel[2] = powf(fmaxf(0.0f, log_pixel.b), params->curve_gamma);

  // record post-curve chroma angle
  dt_RGB_2_HSV(rgb_pixel, hsv_pixel);

  float h_after = hsv_pixel[0];

  // Mix hue back if requested
  h_after = _lerp_hue(h_before, h_after, params->look_original_hue_mix_ratio);

  hsv_pixel[0] = h_after;
  dt_HSV_2_RGB(hsv_pixel, rgb_pixel);

  float3 out;
  out.r = rgb_pixel[0];
  out.g = rgb_pixel[1];
  out.b = rgb_pixel[2];

  // Apply Outset Matrix
  out = _mat3f_mul_float3(AgXInsetMatrixInverse, out);

  // Clamp final output to display range [0, 1]
  out.r = CLAMP(out.r, 0.0f, 1.0f);
  out.g = CLAMP(out.g, 0.0f, 1.0f);
  out.b = CLAMP(out.b, 0.0f, 1.0f);

  return out;
}

// Get pixel norm using max RGB method (similar to filmic's choice for black/white)
DT_OMP_DECLARE_SIMD(aligned(pixel : 16))
static inline float _agx_get_pixel_norm_max_rgb(const dt_aligned_pixel_t pixel)
{
  return fmaxf(fmaxf(pixel[0], pixel[1]), pixel[2]);
}

// Get pixel norm using min RGB method (for black)
DT_OMP_DECLARE_SIMD(aligned(pixel : 16))
static inline float _agx_get_pixel_norm_min_rgb(const dt_aligned_pixel_t pixel)
{
  return fminf(fminf(pixel[0], pixel[1]), pixel[2]);
}

// Apply logic for black point picker
static void apply_auto_black_exposure(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_agx_user_params_t *p = self->params;
  dt_iop_agx_gui_data_t *g = self->gui_data;

  const float black_norm = _agx_get_pixel_norm_min_rgb(self->picked_color_min);
  p->range_black_relative_exposure = CLAMP(log2f(fmaxf(_epsilon, black_norm) / 0.18f), -20.0f, -0.1f);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->range_black_slider, p->range_black_relative_exposure);
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

  const float white_norm = _agx_get_pixel_norm_max_rgb(self->picked_color_max);
  p->range_white_relative_exposure = CLAMP(log2f(fmaxf(_epsilon, white_norm) / 0.18f), 0.1f, 20.0f);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->range_white_slider, p->range_white_relative_exposure);
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
  const float black_norm = _agx_get_pixel_norm_min_rgb(self->picked_color_min);
  p->range_black_relative_exposure = CLAMP(log2f(fmaxf(_epsilon, black_norm) / 0.18f), -20.0f, -0.1f);

  // White point
  const float white_norm = _agx_get_pixel_norm_max_rgb(self->picked_color_max);
  p->range_white_relative_exposure = CLAMP(log2f(fmaxf(_epsilon, white_norm) / 0.18f), 0.1f, 20.0f);

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->range_black_slider, p->range_black_relative_exposure);
  dt_bauhaus_slider_set(g->range_white_slider, p->range_white_relative_exposure);
  --darktable.gui->reset;

  gtk_widget_queue_draw(GTK_WIDGET(g->area));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// Apply logic for pivot x picker
static void apply_auto_pivot_x(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_agx_user_params_t *p = self->params;
  dt_iop_agx_gui_data_t *g = self->gui_data;

  // Calculate norm and EV of the picked color
  const float norm = _agx_get_pixel_norm_max_rgb(self->picked_color);
  const float picked_ev = log2f(fmaxf(_epsilon, norm) / 0.18f);

  // Calculate the target pivot_x based on the picked EV and the current EV range
  const float min_ev = p->range_black_relative_exposure;
  const float max_ev = p->range_white_relative_exposure;
  const float range_in_ev = fmaxf(_epsilon, max_ev - min_ev);
  const float target_pivot_x = CLAMP((picked_ev - min_ev) / range_in_ev, 0.0f, 1.0f);

  // Calculate the required pivot_x_shift to achieve the target_pivot_x
  const float base_pivot_x = fabsf(min_ev / range_in_ev); // Pivot representing 0 EV (mid-gray)
  float s = 0.0f; // curve_pivot_x_shift

  curve_and_look_params_t curve_and_look_params = _calculate_curve_params(p);
  // see where the target_pivot is currently mapped
  float target_y = _calculate_curve(target_pivot_x, &curve_and_look_params);
  // try to avoid changing the brightness of the pivot
  float target_y_linearised = powf(target_y, p->curve_gamma);
  p->curve_pivot_y_linear = target_y_linearised;

  if(fabsf(target_pivot_x - base_pivot_x) < _epsilon)
  {
    s = 0.0f;
  }
  else if(target_pivot_x < base_pivot_x)
  {
    // Solve target_pivot_x = (1 + s) * base_pivot_x for s
    s = (base_pivot_x > _epsilon) ? (target_pivot_x / base_pivot_x) - 1.0f : -1.0f;
  }
  else // target_pivot_x > base_pivot_x
  {
    // Solve target_pivot_x = base_pivot_x * (1 - s) + s for s
    const float denominator = 1.0f - base_pivot_x;
    s = (denominator > _epsilon) ? (target_pivot_x - base_pivot_x) / denominator : 1.0f;
  }

  // Clamp and set the parameter
  p->curve_pivot_x_shift = CLAMP(s, -1.0f, 1.0f);

  // Update the slider visually
  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->curve_pivot_x_shift_slider, p->curve_pivot_x_shift);
  dt_bauhaus_slider_set(g->curve_pivot_y_linear_slider, p->curve_pivot_y_linear);
  --darktable.gui->reset;

  // Redraw and add history
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void _print_curve(curve_and_look_params_t *curve_params)
{
  const int steps = 100;
  printf("\nCurve\n");
  for (int i = 0; i <= steps; i++)
  {
    float x = i / (float)steps;
    const float y = _calculate_curve(x, curve_params);
    printf("%f\t%f\n", x, y);
  }
  printf("\n");
}

// Process
void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_agx_user_params_t *p = piece->data;
  const size_t ch = piece->colors;

  if(!dt_iop_have_required_input_format(4, self, piece->colors, ivoid, ovoid, roi_in, roi_out)) return;

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
  curve_and_look_params_t curve_params = _calculate_curve_params(p);

  DT_OMP_FOR()
  for(int j = 0; j < roi_out->height; j++)
  {
    const float *in = (float *)ivoid + (size_t)ch * roi_in->width * j;
    float *out = (float *)ovoid + (size_t)ch * roi_out->width * j;

    for(int i = 0; i < roi_out->width; i++)
    {
      float3 rgb;
      rgb.r = in[0];
      rgb.g = in[1];
      rgb.b = in[2];

      const int debug = i == 0 && j == 0;

      const float3 agx_rgb = _agx_tone_mapping(rgb, &curve_params);

      out[0] = agx_rgb.r;
      out[1] = agx_rgb.g;
      out[2] = agx_rgb.b;

      if(ch == 4)
      {
        out[3] = in[3]; // Copy alpha if it exists
      }

      in += ch;
      out += ch;
      if(debug)
      {
        printf("================== end ==================\n");
      }
    }
  }
}

// Plot the curve
static gboolean agx_draw_curve(GtkWidget *widget, cairo_t *crf, dt_iop_module_t *self)
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

  // Draw grid
  dt_draw_grid(cr, 4, 0, 0, g->graph_width, g->graph_height);

  // Draw identity line (y=x)
  cairo_save(cr);
  cairo_set_source_rgba(cr, darktable.bauhaus->graph_border.red, darktable.bauhaus->graph_border.green, darktable.bauhaus->graph_border.blue, 0.5);
  cairo_move_to(cr, 0, 0);
  cairo_line_to(cr, g->graph_width, g->graph_height);
  cairo_stroke(cr);
  cairo_restore(cr);

  // Draw the curve
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  set_color(cr, darktable.bauhaus->graph_fg);

  const int steps = 200;
  for (int k = 0; k <= steps; k++)
  {
    float x_norm = (float)k / steps; // Input to the curve [0, 1]
    float y_norm = _calculate_curve(x_norm, &curve_params);

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

  // Draw Axis Labels (Simplified)
  cairo_save(cr);
  cairo_identity_matrix(cr); // Reset transformations for text
  set_color(cr, darktable.bauhaus->graph_fg);

  // Y-axis label (Output)
  snprintf(text, sizeof(text), "1.0");
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &g->ink, NULL);
  cairo_move_to(cr, margin_left - g->ink.width - g->inset, margin_top - g->ink.height / 2.0 - g->ink.y);
  pango_cairo_show_layout(cr, layout);

  snprintf(text, sizeof(text), "0.0");
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &g->ink, NULL);
  cairo_move_to(cr, margin_left - g->ink.width - g->inset, margin_top + g->graph_height - g->ink.height / 2.0 - g->ink.y);
  pango_cairo_show_layout(cr, layout);

  // X-axis label (Input - Log Encoded)
  snprintf(text, sizeof(text), "0.0");
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &g->ink, NULL);
  cairo_move_to(cr, margin_left - g->ink.width / 2.0 - g->ink.x, margin_top + g->graph_height + g->inset);
  pango_cairo_show_layout(cr, layout);

  snprintf(text, sizeof(text), "1.0");
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &g->ink, NULL);
  cairo_move_to(cr, margin_left + g->graph_width - g->ink.width / 2.0 - g->ink.x, margin_top + g->graph_height + g->inset);
  pango_cairo_show_layout(cr, layout);

  // Axis titles
  snprintf(text, sizeof(text), _("Output"));
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &g->ink, NULL);
  // Rotate and position Y axis title
  cairo_save(cr);
  cairo_translate(cr, g->inset, margin_top + g->graph_height/2.0 + g->ink.width/2.0 );
  cairo_rotate(cr, -M_PI / 2.0);
  cairo_move_to(cr, 0, 0);
  pango_cairo_show_layout(cr, layout);
  cairo_restore(cr);


  snprintf(text, sizeof(text), _("Input (Log Encoded)"));
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &g->ink, NULL);
  cairo_move_to(cr, margin_left + g->graph_width/2.0 - g->ink.width / 2.0 - g->ink.x, margin_top + g->graph_height + g->inset + g->line_height + g->inset/2.0);
  pango_cairo_show_layout(cr, layout);

  cairo_restore(cr); // Restore original matrix

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

  // Ensure the graph is drawn initially
  if (g && g->area) {
    gtk_widget_queue_draw(GTK_WIDGET(g->area));
  }
}

static void _add_look_box(dt_iop_module_t *self)
{
  // look: saturation, slope, offset, power, original hue mix ratio (hue restoration)
  GtkWidget *look_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(self->widget), look_box, TRUE, TRUE, 0);
  GtkWidget *label = gtk_label_new(_("Look"));
  gtk_box_pack_start(GTK_BOX(look_box), label, FALSE, FALSE, 0);
  GtkWidget *slider;

  slider = dt_bauhaus_slider_from_params(self, "look_offset");
  dt_bauhaus_slider_set_soft_range(slider, -1.0f, 1.0f);
  gtk_widget_set_tooltip_text(slider, _("deepen or lift shadows"));  // Tooltip text for look_offset
  gtk_box_pack_start(GTK_BOX(look_box), slider, TRUE, TRUE, 0);

  slider = dt_bauhaus_slider_from_params(self, "look_slope");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 5.0f);
  gtk_widget_set_tooltip_text(slider, _("decrease or increase contrast and brightness"));
  gtk_box_pack_start(GTK_BOX(look_box), slider, TRUE, TRUE, 0);

  slider = dt_bauhaus_slider_from_params(self, "look_power");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 5.0f);
  gtk_widget_set_tooltip_text(slider, _("increase or decrease brightness"));
  gtk_box_pack_start(GTK_BOX(look_box), slider, TRUE, TRUE, 0);

  slider = dt_bauhaus_slider_from_params(self, "look_saturation");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 2.0f);
  gtk_box_pack_start(GTK_BOX(look_box), slider, TRUE, TRUE, 0);

  slider = dt_bauhaus_slider_from_params(self, "look_original_hue_mix_ratio");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 1.0f);
  gtk_box_pack_start(GTK_BOX(look_box), slider, TRUE, TRUE, 0);
}

static void _add_tone_mapping_box(dt_iop_module_t *self, dt_iop_agx_gui_data_t *gui_data)
{
  GtkWidget *label;
  GtkWidget *slider;
  GtkWidget *main_box = self->widget;
  dt_gui_new_collapsible_section(&gui_data->tone_mapping_section, "plugins/darkroom/agx/expand_tonemapping_params",
                                 _("Tone mapping"), GTK_BOX(main_box), DT_ACTION(self));

  self->widget = GTK_WIDGET(gui_data->tone_mapping_section.container);


  gui_data->area = GTK_DRAWING_AREA(dt_ui_resize_wrap(NULL,
                                                      0, // Initial height factor
                                                      "plugins/darkroom/agx/graphheight")); // Conf key
  g_object_set_data(G_OBJECT(gui_data->area), "iop-instance", self);
  dt_action_define_iop(self, NULL, N_("graph"), GTK_WIDGET(gui_data->area), NULL);
  gtk_widget_set_can_focus(GTK_WIDGET(gui_data->area), TRUE);
  g_signal_connect(G_OBJECT(gui_data->area), "draw", G_CALLBACK(agx_draw_curve), self);
  gtk_widget_set_tooltip_text(GTK_WIDGET(gui_data->area), _("tone mapping curve"));

  // Pack drawing area at the top
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(gui_data->area), TRUE, TRUE, 0);

  // black/white relative exposure
  label = gtk_label_new(_("Input exposure range"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, FALSE, 0);

  // Create black point slider and associate picker
  gui_data->range_black_slider = dt_bauhaus_slider_from_params(self, "range_black_relative_exposure");
  slider = gui_data->range_black_slider;
  dt_bauhaus_slider_set_soft_range(slider, -20.0f, -1.0f);
  gui_data->range_black_picker = dt_color_picker_new(self, DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_DENOISE, slider);
  gtk_widget_set_tooltip_text(slider, _("minimum relative exposure (black point)"));

  // Create white point slider and associate picker
  gui_data->range_white_slider = dt_bauhaus_slider_from_params(self, "range_white_relative_exposure");
  slider = gui_data->range_white_slider;
  dt_bauhaus_slider_set_soft_range(slider, 1.0f, 20.0f);
  gui_data->range_white_picker = dt_color_picker_new(self, DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_DENOISE, slider);
  gtk_widget_set_tooltip_text(gui_data->range_white_picker, _("pick white exposure")); // Tooltip for picker icon
  gtk_widget_set_tooltip_text(slider, _("maximum relative exposure (white point)"));

  // Auto tune slider (similar to filmic's)
  gui_data->auto_tune_picker = dt_color_picker_new(self, DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_DENOISE,
                                                  dt_bauhaus_combobox_new(self));
  dt_bauhaus_widget_set_label(gui_data->auto_tune_picker, NULL, N_("auto tune levels"));
  gtk_widget_set_tooltip_text(gui_data->auto_tune_picker, _("pick image area to automatically set black and white exposure"));
  gtk_box_pack_start(GTK_BOX(self->widget), gui_data->auto_tune_picker, TRUE, TRUE, 0);

  label = gtk_label_new(_("curve parameters"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, FALSE, 0);

  // Internal 'gamma'
  slider = dt_bauhaus_slider_from_params(self, "curve_gamma");
  dt_bauhaus_slider_set_soft_range(slider, 1.0f, 5.0f);
  gtk_widget_set_tooltip_text(slider, _("Fine-tune contrast, shifts representation of pivot along the y axis"));

  slider = dt_bauhaus_slider_from_params(self, "curve_pivot_x_shift");
  dt_bauhaus_slider_set_soft_range(slider, -1.0f, 1.0f);
  gtk_widget_set_tooltip_text(slider, _("Pivot x shift"));
  gui_data->curve_pivot_x_shift_slider = slider; // Store the slider widget
  gui_data->pivot_x_picker = dt_color_picker_new(self, DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_DENOISE, slider);
  gtk_widget_set_tooltip_text(gui_data->pivot_x_picker, _("pick range to serve as contrast pivot"));


  slider = dt_bauhaus_slider_from_params(self, "curve_pivot_y_linear");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 1.0f);
  gtk_widget_set_tooltip_text(slider, _("Pivot y (linear output)"));
  gui_data->curve_pivot_y_linear_slider = slider;

  slider = dt_bauhaus_slider_from_params(self, "curve_contrast_around_pivot");
  dt_bauhaus_slider_set_soft_range(slider, 0.1f, 5.0f);
  gtk_widget_set_tooltip_text(slider, _("linear section slope"));

  slider = dt_bauhaus_slider_from_params(self, "curve_toe_power");
  dt_bauhaus_slider_set_soft_range(slider, 0.2f, 5.0f);
  gtk_widget_set_tooltip_text(slider, _("toe power"));

  slider = dt_bauhaus_slider_from_params(self, "curve_shoulder_power");
  dt_bauhaus_slider_set_soft_range(slider, 0.2f, 5.0f);
  gtk_widget_set_tooltip_text(slider, _("shoulder power"));

  // Create a nested collapsible section for additional parameters
  GtkWidget *parent_box = self->widget;
  dt_gui_new_collapsible_section(&gui_data->advanced_section, "plugins/darkroom/agx/expand_curve_advanced",
                                 _("advanced"), GTK_BOX(parent_box), DT_ACTION(self));

  self->widget = GTK_WIDGET(gui_data->advanced_section.container);

  // Toe
  slider = dt_bauhaus_slider_from_params(self, "curve_linear_percent_below_pivot");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 100.0f);
  gtk_widget_set_tooltip_text(slider, _("toe length"));

  slider = dt_bauhaus_slider_from_params(self, "curve_target_display_black_y");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 1.0f);
  gtk_widget_set_tooltip_text(slider, _("toe intersection point"));

  // Shoulder
  slider = dt_bauhaus_slider_from_params(self, "curve_linear_percent_above_pivot");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 100.0f);
  gtk_widget_set_tooltip_text(slider, _("shoulder length"));

  slider = dt_bauhaus_slider_from_params(self, "curve_target_display_white_y");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 2.0f);
  gtk_widget_set_tooltip_text(slider, _("shoulder intersection point"));
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

  _add_look_box(self);
  _add_tone_mapping_box(self, gui_data);

  // restore
  self->widget = self_widget;
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

  // common
  p.look_slope = 1.0f;
  p.look_original_hue_mix_ratio = 0.0f;

  p.range_black_relative_exposure = -10;
  p.range_white_relative_exposure = 6.5;

  p.curve_contrast_around_pivot = 2.4;
  p.curve_linear_percent_below_pivot = 0.0;
  p.curve_linear_percent_below_pivot = 0.0;
  p.curve_toe_power = 1.5;
  p.curve_shoulder_power = 1.5;
  p.curve_target_display_black_y = 0.0;
  p.curve_target_display_white_y = 1.0;
  p.curve_gamma = 2.2;
  p.curve_pivot_x_shift = 0.0;
  p.curve_pivot_y_linear = 0.18;

  // Base preset
  p.look_power = 1.0f;
  p.look_offset = 0.0f;
  p.look_saturation = 1.0f;

  dt_gui_presets_add_generic(_("AgX Base"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // Punchy preset
  p.look_power = 1.35f; // Power was the same for all channels in Punchy
  p.look_offset = 0.0f;
  p.look_saturation = 1.4f;
  dt_gui_presets_add_generic(_("Punchy"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);
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

  if(picker == g->range_black_picker) apply_auto_black_exposure(self);
  else if(picker == g->range_white_picker) apply_auto_white_exposure(self);
  else if(picker == g->auto_tune_picker) apply_auto_tune_exposure(self);
  else if(picker == g->pivot_x_picker) apply_auto_pivot_x(self);
}

/*
// Global data struct (not yet needed)
typedef struct dt_iop_agx_global_data_t
{

} dt_iop_agx_global_data_t;

// Functions for global data init/cleanup if needed
void init_global(dt_iop_module_so_t *self)
{
    // Allocate global data if needed
    // self->data = malloc(sizeof(dt_iop_agx_global_data_t));
}

void cleanup_global(dt_iop_module_so_t *self)
{
    // Free global data if allocated
    // free(self->data);
    // self->data = NULL;
}
*/
