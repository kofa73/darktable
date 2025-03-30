#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "iop/iop_api.h"
#include "gui/draw.h" // Needed for dt_draw_grid
#include "gui/accelerators.h" // For dt_action_define_iop

#include <gtk/gtk.h>
#include <math.h> // For math functions
#include <stdlib.h>
#include <pango/pangocairo.h> // For text rendering in graph

// Silence the compiler during dev of new module
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"

// Module introspection version
DT_MODULE_INTROSPECTION(1, dt_iop_agx_params_t)

// so we have a breakpoint target in error-handling branches, to be removed after debugging
static int errors = 0; // Use static if needed within this file only

const float _epsilon = 1E-6f;

// Module parameters struct
// Updated struct dt_iop_agx_params_t
typedef struct dt_iop_agx_params_t
{
  // look params
  float look_offset;      // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "Offset (deepen(-) or lift(+) shadows)"
  float look_slope;       // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "Slope (decrease(-) or increase(+) contrast and brightness)"
  float look_power;       // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "Power (increase(-) or decrease(+) brightness)"
  float look_saturation;  // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "Saturation"
  float look_original_hue_mix_ratio;    // $MIN: 0.0 $MAX: 1 $DEFAULT: 0.0 $DESCRIPTION: "Restore original hue"

  // log mapping params
  float range_black_relative_exposure;  // $MIN: -20.0 $MAX: -0.1 $DEFAULT: -10 $DESCRIPTION: "Black relative exposure (below mid-grey)"
  float range_white_relative_exposure;  // $MIN: 0.1 $MAX: 20 $DEFAULT: 6.5 $DESCRIPTION: "White relative exposure (above mid-grey)"

  // curve params - comments indicate the original variables from https://www.desmos.com/calculator/yrysofmx8h
  // P_slope
  float sigmoid_contrast_around_pivot;      // $MIN: 0.1 $MAX: 10.0 $DEFAULT: 2.4 $DESCRIPTION: "Contrast around the pivot"
  // P_tlength
  float sigmoid_linear_percent_below_pivot;  // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "Toe start %, below the pivot"
  // P_slength
  float sigmoid_linear_percent_above_pivot;  // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "Shoulder start %, above the pivot"
  // t_p
  float sigmoid_toe_power;                  // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.5 $DESCRIPTION: "Toe power; contrast in shadows"
  // s_p -> Renamed from sigmoid_shoulder_power for clarity
  float sigmoid_shoulder_power;             // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.5 $DESCRIPTION: "Shoulder power; contrast in highlights"
  // we don't have a parameter for pivot_x, it's set to the x value representing mid-grey, splitting [0..1] in the ratio
  // range_black_relative_exposure : range_white_relative_exposure
  // not a parameter of the original curve, they used p_x, p_y to directly set the pivot
  float sigmoid_curve_gamma;                // $MIN: 1.0 $MAX: 5.0 $DEFAULT: 2.2 $DESCRIPTION: "Curve y gamma"
  // t_ly
  float sigmoid_target_display_black_y;     // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "Target display black"
  // s_ly
  float sigmoid_target_display_white_y;     // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "Target display white"
} dt_iop_agx_params_t;


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
} dt_iop_agx_gui_data_t;

// Structs for vector and matrix math (pure C)
typedef struct
{
  float r, g, b;
} float3;

typedef struct
{
  float m[3][3];
} mat3f;


// --- Forward declarations ---
static float _calculate_sigmoid(float x,
                                float slope_around_pivot,
                                float toe_power, float shoulder_power, float toe_end_x,
                                float toe_end_y, float shoulder_start_x,
                                float shoulder_start_y, float toe_scale, float intercept,
                                float shoulder_scale,
                                gboolean need_convex_toe, float toe_a, float toe_b,
                                gboolean need_concave_shoulder, float shoulder_a, float shoulder_b);

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

/*
https://github.com/EaryChow/AgX_LUT_Gen/blob/main/AgXBaseRec2020.py
        inset_matrix = numpy.array([[0.856627153315983, 0.0951212405381588, 0.0482516061458583],
                                    [0.137318972929847, 0.761241990602591, 0.101439036467562],
                                    [0.11189821299995, 0.0767994186031903, 0.811302368396859]])
*/
const mat3f AgXInsetMatrix = { { { 0.856627153315983f, 0.0951212405381588f, 0.0482516061458583f },
                                 { 0.137318972929847f, 0.761241990602591f, 0.101439036467562f },
                                 { 0.11189821299995f, 0.0767994186031903f, 0.811302368396859f } } };


const mat3f AgXInsetMatrixInverse = { { { 1.1974410768877f, -0.14426151269800f, -0.053179564189704f },
                                        { -0.19647462632135f, 1.3540951314697f, -0.15762050514838f },
                                        { -0.14655741710660f, -0.10828405878847f, 1.2548414758951f } } };

static float _line(float x_in, float slope, float intercept)
{
  return slope * x_in + intercept;
}

static float _scale(float limit_x_lx, float limit_y_ly, float transition_x_tx, float transition_y_ty,
                    float power_p, float slope_m)
{
  float linear_y_delta = slope_m * (limit_x_lx - transition_x_tx);
  printf("linear_y_delta = %f\n", linear_y_delta);

  float power_curved_y_delta = powf(linear_y_delta, -power_p); // dampened / steepened
  printf("power_curved_y_delta = %f\n", power_curved_y_delta);

  // in case the linear section extends too far; avoid division by 0
  float remaining_y_span = fmaxf(_epsilon, limit_y_ly - transition_y_ty);
  printf("remaining_y_span = %f\n", remaining_y_span);

  float y_delta_ratio = linear_y_delta / remaining_y_span;
  printf("y_delta_ratio = %f\n", y_delta_ratio);

  float term_b = powf(y_delta_ratio, power_p) - 1.0f;
  term_b = fmaxf(term_b, _epsilon);
  printf("term_b = %f\n", term_b);

  float base = power_curved_y_delta * term_b;
  printf("base = %f\n", base);

  float scale_value = powf(base, -1.0f / power_p);

  scale_value = fminf(1e6, scale_value);
  scale_value = fmaxf(-1e6, scale_value);

  printf("scale_value = %f\n", scale_value);

  return scale_value;
}

static float _exponential(float x_in, float power)
{
  const float value = x_in / powf(1.0f + powf(x_in, power), 1.0f / power);
  if(isnan(value))
  {
    errors++; // printf("_exponential returns nan\n");
  }
  return value;
}

static float _exponential_curve(float x_in, float scale_, float slope, float power, float transition_x,
                                float transition_y)
{
  float value = scale_ * _exponential(slope * (x_in - transition_x) / scale_, power) + transition_y;
  if(isnan(value))
  {
    errors++; // printf("_exponential_curve returns nan\n");
  }
  return value;
}

// Fallback toe/shoulder, so we can always reach black and white.
// See https://www.desmos.com/calculator/gijzff3wlv
static float _fallback_toe(float x, float toe_a, float toe_b)
{
  // FIXME target black
  return x <= 0 ? 0 : fmaxf(0, toe_a * powf(x, toe_b));
}

static float _fallback_shoulder(float x, float shoulder_a, float shoulder_b)
{
  // FIXME: target white
  return (x >= 1) ? 1 : fminf(1, 1 - shoulder_a * powf(1 - x, shoulder_b));
}


// the commented values (t_tx, etc) are references to https://www.desmos.com/calculator/yrysofmx8h
static float _calculate_sigmoid(float x,
                                // Slope of linear portion.
                                float slope_around_pivot,
                                // Exponential power of the toe and shoulder regions.
                                float toe_power, float shoulder_power,
                                float toe_end_x, float toe_end_y,
                                float shoulder_start_x, float shoulder_start_y,
                                float toe_scale, float intercept,
                                float shoulder_scale,
                                gboolean need_convex_toe, float toe_a, float toe_b,
                                gboolean need_concave_shoulder, float shoulder_a, float shoulder_b)
{
  float result;

  if(x < toe_end_x)
  {
    result = need_convex_toe ? _fallback_toe(x, toe_a, toe_b) : _exponential_curve(x, toe_scale, slope_around_pivot, toe_power, toe_end_x,
                                toe_end_y);
  }
  else if(x <= shoulder_start_x)
  {
    result = _line(x, slope_around_pivot, intercept);
  }
  else
  {
    result = need_concave_shoulder ? _fallback_shoulder(x, shoulder_a, shoulder_b) : _exponential_curve(x, shoulder_scale, slope_around_pivot, shoulder_power,
                                shoulder_start_x, shoulder_start_y);
  }

  // TODO: white, black
  return fmaxf(0, fminf(1, result));
}

// 'lerp', but take care of the boundary: hue wraps around 1 -> 0
static float _lerp_hue(float hue1, float hue2, float mix)
{
  float hue_diff = hue2 - hue1;

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
  float m = slope / (1 + offset);
  float b = offset * m;
  return fmaxf(0.0f, m * x + b);
  // ASC CDL:
  // return x * slope + offset;
  // alternative:
  // y = mx + b, b is the offset, m = (1 - offset), so the line runs from (0, offset) to (1, 1)
  //return (1 - offset) * x + offset;
}

// https://docs.acescentral.com/specifications/acescct/#appendix-a-application-of-asc-cdl-parameters-to-acescct-image-data
static float3 _agxLook(float3 val, const dt_iop_agx_params_t *p)
{
  // Default
  float slope = p->look_slope;
  float offset = p->look_offset;
  float sat = p->look_saturation;

  // ASC CDL
  float slope_and_offset_r = apply_slope_offset(val.r, slope, offset);
  float slope_and_offset_g = apply_slope_offset(val.g, slope, offset);
  float slope_and_offset_b = apply_slope_offset(val.b, slope, offset);

  float out_r = slope_and_offset_r > 0 ? powf(slope_and_offset_r, p->look_power) : slope_and_offset_r;
  float out_g = slope_and_offset_r > 0 ? powf(slope_and_offset_g, p->look_power) : slope_and_offset_g;
  float out_b = slope_and_offset_r > 0 ? powf(slope_and_offset_b, p->look_power) : slope_and_offset_b;

  // Using Rec 2020 Y coefficients (we use insetting, so this is probably incorrect
  const float luma = 0.2626983389565561f * out_r + 0.6780087657728164f * out_g + 0.05929289527062728f * out_b;

  float3 result;
  result.r = luma + sat * (out_r - luma);
  result.g = luma + sat * (out_g - luma);
  result.b = luma + sat * (out_b - luma);

  return result;
}

static float3 _apply_log_encoding(dt_aligned_pixel_t pixel, float range_in_ev, float minEv)
{
  // Assume input is linear Rec2020 relative to 0.18 mid grey
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

  // Clamp result to [0, 1] - this is the input domain for the sigmoid
  v.r = fminf(fmaxf(v.r, 0.0f), 1.0f);
  v.g = fminf(fmaxf(v.g, 0.0f), 1.0f);
  v.b = fminf(fmaxf(v.b, 0.0f), 1.0f);

  return v;
}


// Struct to hold calculated sigmoid parameters
typedef struct dt_agx_sigmoid_params_cache_t
{
  float effective_slope;
  float toe_start_x;
  float toe_start_y;
  float shoulder_start_x;
  float shoulder_start_y;
  float scale_toe;
  float shoulder_scale;
  float intercept;
  gboolean need_convex_toe;
  float toe_a;
  float toe_b;
  gboolean need_concave_shoulder;
  float shoulder_a;
  float shoulder_b;
} dt_agx_sigmoid_params_cache_t;

// see https://www.desmos.com/calculator/gijzff3wlv
static float _calculate_B(float slope, float transition_x, float transition_y)
{
  return slope * transition_x / transition_y;
}

static float _calculate_A(float transition_x, float transition_y, float B)
{
  return transition_y / powf(transition_x, B);
}


// Helper to calculate sigmoid parameters based on module params
static void _calculate_agx_sigmoid_params(const dt_iop_agx_params_t *p, dt_agx_sigmoid_params_cache_t *cache)
{
  const float maxEv = p->range_white_relative_exposure;
  const float minEv = p->range_black_relative_exposure;
  float range_in_ev = maxEv - minEv;

  // TODO: expose the pivot; for now: 18% mid-grey
  const float pivot_x = fabsf(minEv / range_in_ev);
  const float pivot_y = powf(0.18f, 1.0f / p->sigmoid_curve_gamma);

  // avoid range altering slope - 16.5 EV is the default AgX range; keep scaling pro
  cache->effective_slope = p->sigmoid_contrast_around_pivot * (range_in_ev / 16.5f);
  //printf("scaled slope = %f\n", cache->effective_slope);

  // x distance between x = 0 and pivot_x is just pivot_x, times portion of linear section
  float toe_start_x_from_pivot_x = pivot_x * p->sigmoid_linear_percent_below_pivot / 100.0f;
  cache->toe_start_x = pivot_x - toe_start_x_from_pivot_x;
  float toe_y_below_pivot_y = cache->effective_slope * toe_start_x_from_pivot_x;
  cache->toe_start_y = pivot_y - toe_y_below_pivot_y;

  // toe fallback curve params
  cache->toe_b = _calculate_B(cache->effective_slope, cache->toe_start_x, cache->toe_start_y);
  cache->toe_a = _calculate_A(cache->toe_start_x, cache->toe_start_y, cache->toe_b);


  // starting with a line of gradient effective_slope from (0, 0 -> FIXME: target_black) would take us to (pivot_x, linear_y_at_pivot_x_at_slope)
  float linear_y_at_toe_start_x_at_slope = cache->effective_slope * cache->toe_start_x;
  // Normally, the toe is concave: its gradient is gradually increasing, up to the slope of the linear
  // section. If the slope of the linear section is not enough to go from (0, target_black) to
  // (toe_end_x, toe_end_y), we'll need a convex 'toe'
  cache->need_convex_toe = linear_y_at_toe_start_x_at_slope < cache->toe_start_y; // FIXME: use target black y param

  const float inverse_toe_limit_x = 1.0f; // 1 - t_lx (limit is 0, so inverse is 1)
  const float inverse_toe_limit_y = 1.0f - p->sigmoid_target_display_black_y; // Inverse limit y

  // we use the same calculation as for the shoulder, so we flip the toe left <-> right, up <-> down
  float inverse_toe_transition_x = 1.0f - cache->toe_start_x;
  float inverse_toe_transition_y = 1.0f - cache->toe_start_y;

  // and then flip the scale
  cache->scale_toe = -_scale(inverse_toe_limit_x, inverse_toe_limit_y, inverse_toe_transition_x,
                                inverse_toe_transition_y, p->sigmoid_toe_power, cache->effective_slope);
  //if(isnan(cache->scale_toe))
  //{
  //  errors++; // printf("scale_toe is NaN\n");
  //}

  // shoulder
  // distance between pivot_x and x = 1, times portion of linear section
  float shoulder_x_from_pivot_x = (1 - pivot_x) * p->sigmoid_linear_percent_above_pivot / 100.0f;
  //printf("shoulder_x_from_pivot_x = %f\n", shoulder_x_from_pivot_x);
  cache->shoulder_start_x = pivot_x + shoulder_x_from_pivot_x;
  //printf("shoulder_start_x = %f\n", cache->shoulder_start_x);
  float shoulder_y_from_pivot_y = cache->effective_slope * shoulder_x_from_pivot_x;
  //printf("shoulder_y_from_pivot_y = %f\n", shoulder_y_from_pivot_y);
  cache->shoulder_start_y = pivot_y + shoulder_y_from_pivot_y;
  //printf("shoulder_start_y = %f\n", cache->shoulder_start_y);

  // shoulder fallback curve params
  cache->shoulder_b = _calculate_B(cache->effective_slope, 1.0f - cache->shoulder_start_x, p->sigmoid_target_display_white_y - cache->shoulder_start_y);
  cache->shoulder_a = _calculate_A(1.0f - cache->shoulder_start_x, p->sigmoid_target_display_white_y - cache->shoulder_start_y, cache->shoulder_b);


  float linear_y_when_x_is_1 = cache->shoulder_start_y + cache->effective_slope * (1.0f - cache->shoulder_start_x);
  // Normally, the shoulder is convex: its gradient is gradually decreasing from slope of the linear
  // section. If the slope of the linear section is not enough to go from (toe_end_x, toe_end_y) to
  // (1, target_white), we'll need a concave 'shoulder'
  cache->need_concave_shoulder = linear_y_when_x_is_1 < p->sigmoid_target_display_white_y; // FIXME: use target white y param

  const float shoulder_intersection_x = 1.0f; // Limit x is 1

  cache->shoulder_scale
      = _scale(shoulder_intersection_x, p->sigmoid_target_display_white_y,
              cache->shoulder_start_x, cache->shoulder_start_y,
              p->sigmoid_shoulder_power, cache->effective_slope);
  //if(isnan(cache->shoulder_scale))
  //{
  //  errors++; // printf("shoulder_scale is NaN\n");
  //}

  //printf("scale_toe: %f, shoulder_scale: %f\n", cache->scale_toe, cache->shoulder_scale);

  // b - intercept of the linear section
  cache->intercept = cache->toe_start_y - cache->effective_slope * cache->toe_start_x;
  //if(isnan(cache->intercept))
  //{
  //  errors++; // printf("intercept is NaN\n");
  //}

  //printf("need_convex_toe: %d, need_concave_shoulder: %d\n", cache->need_convex_toe, cache->need_concave_shoulder);
}


static float3 _agx_tone_mapping(float3 rgb, const dt_iop_agx_params_t *p, float range_in_ev, float minEv,
                                const dt_agx_sigmoid_params_cache_t* sigmoid_cache)
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
  float h_before = hsv_pixel[0];

  float3 log_pixel = _apply_log_encoding(rgb_pixel, range_in_ev, minEv);

  // Apply sigmoid using cached parameters
  log_pixel.r = _calculate_sigmoid(log_pixel.r, sigmoid_cache->effective_slope, p->sigmoid_toe_power, p->sigmoid_shoulder_power,
                           sigmoid_cache->toe_start_x, sigmoid_cache->toe_start_y, sigmoid_cache->shoulder_start_x,
                           sigmoid_cache->shoulder_start_y, sigmoid_cache->scale_toe, sigmoid_cache->intercept, sigmoid_cache->shoulder_scale,
                           sigmoid_cache->need_convex_toe, sigmoid_cache->toe_a, sigmoid_cache->toe_b,
                           sigmoid_cache->need_concave_shoulder, sigmoid_cache->shoulder_a, sigmoid_cache->shoulder_b);
  log_pixel.g = _calculate_sigmoid(log_pixel.g, sigmoid_cache->effective_slope, p->sigmoid_toe_power, p->sigmoid_shoulder_power,
                           sigmoid_cache->toe_start_x, sigmoid_cache->toe_start_y, sigmoid_cache->shoulder_start_x,
                           sigmoid_cache->shoulder_start_y, sigmoid_cache->scale_toe, sigmoid_cache->intercept, sigmoid_cache->shoulder_scale,
                           sigmoid_cache->need_convex_toe, sigmoid_cache->toe_a, sigmoid_cache->toe_b,
                           sigmoid_cache->need_concave_shoulder, sigmoid_cache->shoulder_a, sigmoid_cache->shoulder_b);
  log_pixel.b = _calculate_sigmoid(log_pixel.b, sigmoid_cache->effective_slope, p->sigmoid_toe_power, p->sigmoid_shoulder_power,
                           sigmoid_cache->toe_start_x, sigmoid_cache->toe_start_y, sigmoid_cache->shoulder_start_x,
                           sigmoid_cache->shoulder_start_y, sigmoid_cache->scale_toe, sigmoid_cache->intercept, sigmoid_cache->shoulder_scale,
                           sigmoid_cache->need_convex_toe, sigmoid_cache->toe_a, sigmoid_cache->toe_b,
                           sigmoid_cache->need_concave_shoulder, sigmoid_cache->shoulder_a, sigmoid_cache->shoulder_b);

  // Apply AgX look
  log_pixel = _agxLook(log_pixel, p);

  // Linearize
  rgb_pixel[0] = powf(fmaxf(0.0f, log_pixel.r), p->sigmoid_curve_gamma);
  rgb_pixel[1] = powf(fmaxf(0.0f, log_pixel.g), p->sigmoid_curve_gamma);
  rgb_pixel[2] = powf(fmaxf(0.0f, log_pixel.b), p->sigmoid_curve_gamma);

  // record post-sigmoid chroma angle
  dt_RGB_2_HSV(rgb_pixel, hsv_pixel);

  float h_after = hsv_pixel[0];

  // Mix hue back if requested
  h_after = _lerp_hue(h_before, h_after, p->look_original_hue_mix_ratio);

  hsv_pixel[0] = h_after;
  dt_HSV_2_RGB(hsv_pixel, rgb_pixel);

  float3 out;
  out.r = rgb_pixel[0];
  out.g = rgb_pixel[1];
  out.b = rgb_pixel[2];

  // Apply Outset Matrix
  out = _mat3f_mul_float3(AgXInsetMatrixInverse, out);

  // Clamp final output to display range [0, 1]
  out.r = fmaxf(0.0f, fminf(1.0f, out.r));
  out.g = fmaxf(0.0f, fminf(1.0f, out.g));
  out.b = fmaxf(0.0f, fminf(1.0f, out.b));

  return out;
}

void _print_sigmoid(float slope_around_pivot,
                                float toe_power, float shoulder_power,
                                float toe_end_x, float toe_end_y,
                                float shoulder_start_x, float shoulder_start_y,
                                float toe_scale, float intercept, float shoulder_scale,
                                gboolean need_convex_toe, float toe_a, float toe_b,
                                gboolean need_concave_shoulder, float shoulder_a, float shoulder_b)
{
  const int steps = 100;
  printf("\nSigmoid\n");
  for (int i = 0; i <= steps; i++)
  {
    float x = i / (float) steps;
    float y = _calculate_sigmoid(x, slope_around_pivot,
      toe_power, shoulder_power,
      toe_end_x, toe_end_y,
      shoulder_start_x, shoulder_start_y,
      toe_scale, intercept, shoulder_scale,
      need_convex_toe, toe_a, toe_b,
      need_concave_shoulder, shoulder_a, shoulder_b);
    printf("%f\t%f\n", x, y);
  }
  printf("\n");
}

// Process
void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_agx_params_t *p = piece->data;
  const size_t ch = piece->colors;

  if(!dt_iop_have_required_input_format(4, self, piece->colors, ivoid, ovoid, roi_in, roi_out)) return;

  printf("================== start ==================\n");
  printf("range_black_relative_exposure = %f\n", p->range_black_relative_exposure);
  printf("range_white_relative_exposure = %f\n", p->range_white_relative_exposure);
  printf("sigmoid_curve_gamma = %f\n", p->sigmoid_curve_gamma);
  printf("sigmoid_contrast_around_pivot = %f\n", p->sigmoid_contrast_around_pivot);
  printf("sigmoid_linear_percent_below_pivot = %f\n", p->sigmoid_linear_percent_below_pivot);
  printf("sigmoid_linear_percent_above_pivot = %f\n", p->sigmoid_linear_percent_above_pivot);
  printf("sigmoid_toe_power = %f\n", p->sigmoid_toe_power);
  printf("sigmoid_shoulder_power = %f\n", p->sigmoid_shoulder_power);
  printf("sigmoid_target_display_black_y = %f\n", p->sigmoid_target_display_black_y);
  printf("sigmoid_target_display_white_y = %f\n", p->sigmoid_target_display_white_y);

  // Calculate sigmoid parameters once
  dt_agx_sigmoid_params_cache_t sigmoid_cache;
  _calculate_agx_sigmoid_params(p, &sigmoid_cache);

  const float maxEv = p->range_white_relative_exposure;
  const float minEv = p->range_black_relative_exposure;
  const float range_in_ev = maxEv - minEv;

  DT_OMP_FOR()
  for(int j = 0; j < roi_out->height; j++)
  {
    float *in = ((float *)ivoid) + (size_t)ch * roi_in->width * j;
    float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;

    for(int i = 0; i < roi_out->width; i++)
    {
      float3 rgb;
      rgb.r = in[0];
      rgb.g = in[1];
      rgb.b = in[2];

      int debug = (i == 0 && j == 0);

      float3 agx_rgb = _agx_tone_mapping(rgb, p, range_in_ev, minEv, &sigmoid_cache);

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

// Draw function for the sigmoid curve
static gboolean agx_draw_curve(GtkWidget *widget, cairo_t *crf, dt_iop_module_t *self)
{
  dt_iop_agx_params_t *p = self->params;
  dt_iop_agx_gui_data_t *g = self->gui_data;

  // Calculate current sigmoid parameters
  dt_agx_sigmoid_params_cache_t sigmoid_cache;
  _calculate_agx_sigmoid_params(p, &sigmoid_cache);

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

  // Draw the sigmoid curve
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  set_color(cr, darktable.bauhaus->graph_fg);

  const int steps = 200;
  for (int k = 0; k <= steps; k++)
  {
    float x_norm = (float)k / steps; // Input to sigmoid [0, 1]
    float y_norm = _calculate_sigmoid(x_norm,
        sigmoid_cache.effective_slope, p->sigmoid_toe_power, p->sigmoid_shoulder_power,
        sigmoid_cache.toe_start_x, sigmoid_cache.toe_start_y,
        sigmoid_cache.shoulder_start_x, sigmoid_cache.shoulder_start_y,
        sigmoid_cache.scale_toe, sigmoid_cache.intercept, sigmoid_cache.shoulder_scale,
        sigmoid_cache.need_convex_toe, sigmoid_cache.toe_a, sigmoid_cache.toe_b,
        sigmoid_cache.need_concave_shoulder, sigmoid_cache.shoulder_a, sigmoid_cache.shoulder_b);

    // Map normalized coords [0,1] to graph pixel coords
    float x_graph = x_norm * g->graph_width;
    float y_graph = y_norm * g->graph_height;

    if (k == 0)
      cairo_move_to(cr, x_graph, y_graph);
    else
      cairo_line_to(cr, x_graph, y_graph);
  }
  cairo_stroke(cr);

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
  dt_iop_agx_gui_data_t *g = self->gui_data;
  // Trigger redraw when any parameter changes
  if (g && g->area) {
    gtk_widget_queue_draw(GTK_WIDGET(g->area));
  }
}

// GUI update (called when module UI is shown/refreshed)
void gui_update(dt_iop_module_t *self)
{
  dt_iop_agx_gui_data_t *g = self->gui_data;

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
  gtk_box_pack_start(GTK_BOX(look_box), slider, TRUE, TRUE, 0);

  slider = dt_bauhaus_slider_from_params(self, "look_slope");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 5.0f);
  gtk_box_pack_start(GTK_BOX(look_box), slider, TRUE, TRUE, 0);

  slider = dt_bauhaus_slider_from_params(self, "look_power");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 5.0f);
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
                                               0,                                    // Initial height factor
                                               "plugins/darkroom/agx/graphheight")); // Conf key
  g_object_set_data(G_OBJECT(gui_data->area), "iop-instance", self);
  dt_action_define_iop(self, NULL, N_("graph"), GTK_WIDGET(gui_data->area), NULL);
  gtk_widget_set_can_focus(GTK_WIDGET(gui_data->area), TRUE);
  g_signal_connect(G_OBJECT(gui_data->area), "draw", G_CALLBACK(agx_draw_curve), self);
  gtk_widget_set_tooltip_text(GTK_WIDGET(gui_data->area), _("Sigmoid tone mapping curve (log input, linear output)"));

  // Pack drawing area at the top
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(gui_data->area), TRUE, TRUE, 0);


 // black/white relative exposure
 label = gtk_label_new(_("Input exposure range"));
 gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, FALSE, 0);
 slider = dt_bauhaus_slider_from_params(self, "range_black_relative_exposure");
 dt_bauhaus_slider_set_soft_range(slider, -20.0f, -1.0f);
 gtk_widget_set_tooltip_text(slider, _("minimum relative exposure (black point)"));

 slider = dt_bauhaus_slider_from_params(self, "range_white_relative_exposure");
 dt_bauhaus_slider_set_soft_range(slider, 1.0f, 20.0f);
 gtk_widget_set_tooltip_text(slider, _("maximum relative exposure (white point)"));

 label = gtk_label_new(_("Sigmoid curve parameters"));
 gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, FALSE, 0);

 // Internal 'gamma'
 slider = dt_bauhaus_slider_from_params(self, "sigmoid_curve_gamma");
 dt_bauhaus_slider_set_soft_range(slider, 1.0f, 5.0f);
 gtk_widget_set_tooltip_text(slider, _("Fine-tune contrast, shifts pivot along the y axis"));

 slider = dt_bauhaus_slider_from_params(self, "sigmoid_contrast_around_pivot");
 dt_bauhaus_slider_set_soft_range(slider, 0.1f, 5.0f);
 gtk_widget_set_tooltip_text(slider, _("linear section slope"));

 slider = dt_bauhaus_slider_from_params(self, "sigmoid_toe_power");
 dt_bauhaus_slider_set_soft_range(slider, 0.2f, 5.0f);
 gtk_widget_set_tooltip_text(slider, _("toe power"));

 slider = dt_bauhaus_slider_from_params(self, "sigmoid_shoulder_power");
 dt_bauhaus_slider_set_soft_range(slider, 0.2f, 5.0f);
 gtk_widget_set_tooltip_text(slider, _("shoulder power"));

 // Create a nested collapsible section for additional parameters
 GtkWidget *parent_box = self->widget;
 dt_gui_new_collapsible_section(&gui_data->advanced_section, "plugins/darkroom/agx/expand_sigmoid_advanced",
 _("advanced"), GTK_BOX(parent_box), DT_ACTION(self));

  self->widget = GTK_WIDGET(gui_data->advanced_section.container);

 // Toe
 slider = dt_bauhaus_slider_from_params(self, "sigmoid_linear_percent_below_pivot");
 dt_bauhaus_slider_set_soft_range(slider, 0.0f, 100.0f);
 gtk_widget_set_tooltip_text(slider, _("toe length"));

 slider = dt_bauhaus_slider_from_params(self, "sigmoid_target_display_black_y");
 dt_bauhaus_slider_set_soft_range(slider, 0.0f, 1.0f);
 gtk_widget_set_tooltip_text(slider, _("toe intersection point"));

 // Shoulder
 slider = dt_bauhaus_slider_from_params(self, "sigmoid_linear_percent_above_pivot");
 dt_bauhaus_slider_set_soft_range(slider, 0.0f, 100.0f);
 gtk_widget_set_tooltip_text(slider, _("shoulder length"));

 slider = dt_bauhaus_slider_from_params(self, "sigmoid_target_display_white_y");
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

  dt_iop_agx_params_t p = { 0 };

  // common
  p.look_slope = 1.0f;
  p.look_original_hue_mix_ratio = 0.0f;

  p.range_black_relative_exposure = -10;
  p.range_white_relative_exposure = 6.5;

  p.sigmoid_contrast_around_pivot = 2.4;
  p.sigmoid_linear_percent_below_pivot = 0.0;
  p.sigmoid_linear_percent_below_pivot = 0.0;
  p.sigmoid_toe_power = 1.5;
  p.sigmoid_shoulder_power = 1.5;
  p.sigmoid_target_display_black_y = 0.0;
  p.sigmoid_target_display_white_y = 1.0;
  p.sigmoid_curve_gamma = 2.2;

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