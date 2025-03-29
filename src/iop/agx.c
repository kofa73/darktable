#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <math.h> // For math functions
#include <stdlib.h>

/* See
 * https://github.com/EaryChow/AgX_LUT_Gen/blob/main/AgXBaseRec2020.py
 * https://iolite-engine.com/blog_posts/minimal_agx_implementation
 *
 * Note: filament and GLSL use column-major order for matrices; that is:
 * {
 *  {1, 2, 3},
 *  {4, 5, 6}
 * }
 * defines a 3x2 matrix:
 * +---+---+
 * | 1 | 4 |
 * +---+---+
 * | 2 | 5 |
 * +---+---+
 * | 3 | 6 |
 * +---+---+
 *
 * numpy uses a row-major representation:
 * np.array([[1, 2, 3],
 *           [4, 5, 6]])
 * defines a 2x3 matrix.
 */


// Silence the compiler during dev of new module
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"

// Module introspection version
DT_MODULE_INTROSPECTION(1, dt_iop_agx_params_t)

// so we have a breakpoint target in error-handling branches, to be removed after debugging
int errors = 0;

const float _epsilon = 1E-6f;


typedef struct dt_iop_agx_gui_data_t
{
  dt_gui_collapsible_section_t tone_mapping_section;
  dt_gui_collapsible_section_t advanced_section;
} dt_iop_agx_gui_data_t;

// Module parameters struct
// Updated struct dt_iop_agx_params_t
typedef struct dt_iop_agx_params_t
{
  // look params
  float look_slope;       // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "Slope (decrease or increase brightness)"
  float look_power;       // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "Power (brighten or darken midtones)"
  float look_offset;      // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "Offset (deepen or lift shadows)"
  float look_saturation;  // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "Saturation"
  float look_original_hue_mix_ratio;    // $MIN: 0.0 $MAX: 1 $DEFAULT: 0.0 $DESCRIPTION: "Restore original hue"

  // log mapping params
  float range_black_relative_exposure;  // $MIN: -20.0 $MAX: -0.1 $DEFAULT: -10 $DESCRIPTION: "Black relative exposure (below mid-grey)"
  float range_white_relative_exposure;  // $MIN: 0.1 $MAX: 20 $DEFAULT: 6.5 $DESCRIPTION: "White relative exposure (above mid-grey)"

  // curve params - comments indicate the original variables from https://www.desmos.com/calculator/yrysofmx8h
  // P_slope
  float sigmoid_contrast_around_pivot;      // $MIN: 0.1 $MAX: 10.0 $DEFAULT: 2.4 $DESCRIPTION: "Contrast around the pivot"
  // P_tlength
  float sigmoid_linear_length_below_pivot;  // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "Toe start, below the pivot"
  // P_slength
  float sigmoid_linear_length_above_pivot;  // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "Shoulder start, above the pivot"
  // t_p
  float sigmoid_toe_power;                  // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.5 $DESCRIPTION: "Toe power; contrast in shadows"
  // t_s
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

void gui_init(dt_iop_module_t *self)
{
 dt_iop_agx_gui_data_t *g = IOP_GUI_ALLOC(agx);

 self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

 // look: saturation, slope, offset, power, original hue mix ratio (hue restoration)
 GtkWidget *look_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
 gtk_box_pack_start(GTK_BOX(self->widget), look_box, TRUE, TRUE, 0);
 GtkWidget *label = gtk_label_new(_("Look"));
 gtk_box_pack_start(GTK_BOX(look_box), label, FALSE, FALSE, 0);
 GtkWidget *slider;
 slider = dt_bauhaus_slider_from_params(self, "look_saturation");
 dt_bauhaus_slider_set_soft_range(slider, 0.0f, 2.0f);
 gtk_box_pack_start(GTK_BOX(look_box), slider, TRUE, TRUE, 0);

 slider = dt_bauhaus_slider_from_params(self, "look_slope");
 dt_bauhaus_slider_set_soft_range(slider, 0.0f, 5.0f);
 gtk_box_pack_start(GTK_BOX(look_box), slider, TRUE, TRUE, 0);

 slider = dt_bauhaus_slider_from_params(self, "look_offset");
 dt_bauhaus_slider_set_soft_range(slider, -1.0f, 1.0f);
 gtk_box_pack_start(GTK_BOX(look_box), slider, TRUE, TRUE, 0);

 slider = dt_bauhaus_slider_from_params(self, "look_power");
 dt_bauhaus_slider_set_soft_range(slider, 0.0f, 5.0f);
 gtk_box_pack_start(GTK_BOX(look_box), slider, TRUE, TRUE, 0);

 slider = dt_bauhaus_slider_from_params(self, "look_original_hue_mix_ratio");
 dt_bauhaus_slider_set_soft_range(slider, 0.0f, 1.0f);
 gtk_box_pack_start(GTK_BOX(look_box), slider, TRUE, TRUE, 0);

 // Sigmoid section with collapsible container
 GtkWidget *main_box = self->widget;
 dt_gui_new_collapsible_section(&g->tone_mapping_section, "plugins/darkroom/agx/expand_tonemapping_params",
     _("Tone mapping"), GTK_BOX(main_box), DT_ACTION(self));

 self->widget = GTK_WIDGET(g->tone_mapping_section.container);

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
 dt_gui_new_collapsible_section(&g->advanced_section, "plugins/darkroom/agx/expand_sigmoid_advanced",
 _("advanced"), GTK_BOX(parent_box), DT_ACTION(self));

  self->widget = GTK_WIDGET(g->advanced_section.container);

 // Toe
 slider = dt_bauhaus_slider_from_params(self, "sigmoid_linear_length_below_pivot");
 dt_bauhaus_slider_set_soft_range(slider, 0.0f, 1.0f);
 gtk_widget_set_tooltip_text(slider, _("toe length"));

 slider = dt_bauhaus_slider_from_params(self, "sigmoid_target_display_black_y");
 dt_bauhaus_slider_set_soft_range(slider, 0.0f, 1.0f);
 gtk_widget_set_tooltip_text(slider, _("toe intersection point"));

 // Shoulder
 slider = dt_bauhaus_slider_from_params(self, "sigmoid_linear_length_above_pivot");
 dt_bauhaus_slider_set_soft_range(slider, 0.0f, 1.0f);
 gtk_widget_set_tooltip_text(slider, _("shoulder length"));

 slider = dt_bauhaus_slider_from_params(self, "sigmoid_target_display_white_y");
 dt_bauhaus_slider_set_soft_range(slider, 0.0f, 2.0f);
 gtk_widget_set_tooltip_text(slider, _("shoulder intersection point"));

 // Restore main widget
 self->widget = main_box;
}

// Global data struct (not needed for this simple example)
typedef struct dt_iop_agx_global_data_t
{
} dt_iop_agx_global_data_t;

// Structs for vector and matrix math (pure C)
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

// Helper function: pow function
static float3 _powf3(float3 base, float3 exponent)
{
  float3 result;
  result.r = powf(base.r, exponent.r);
  result.g = powf(base.g, exponent.g);
  result.b = powf(base.b, exponent.b);
  return result;
}


// Modelines (needed, but not that relevant right now)

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

static float _dx_from_hypotenuse_and_slope(float hypotenuse, float slope)
{
  // Pythagorean: if dx were 1, dy would be dx * slope = 1 * slope = slope
  // The hypotenuse would be sqrt(dx^2 + dy^2) = sqrt(1^2 + (1 * slope)^2) = sqrt(1 + slope^2)
  float hypotenuse_if_leg_were_1 = sqrtf(slope * slope + 1.0f);
  // similar triangles: the to-be-determined dx : 1 = hypotenuse : hypotenuse_if_leg_were_1
  return hypotenuse / hypotenuse_if_leg_were_1;
}

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
                                float linear_slope_P_slope,
                                // Exponential power of the toe and shoulder regions.
                                float toe_power_t_p, float shoulder_power_s_p, float transition_toe_x_t_tx,
                                float transition_toe_y_t_ty, float transition_shoulder_x_s_tx,
                                float transition_shoulder_y_s_ty, float scale_toe_s_t, float intercept,
                                float scale_shoulder,
                                gboolean need_convex_toe, float toe_a, float toe_b,
                                gboolean need_concave_shoulder, float shoulder_a, float shoulder_b)
{
  float result;

  if(x < transition_toe_x_t_tx)
  {
    result = need_convex_toe ? _fallback_toe(x, toe_a, toe_b) : _exponential_curve(x, scale_toe_s_t, linear_slope_P_slope, toe_power_t_p, transition_toe_x_t_tx,
                                transition_toe_y_t_ty);
  }
  else if(x <= transition_shoulder_x_s_tx)
  {
    result = _line(x, linear_slope_P_slope, intercept);
  }
  else
  {
    result = need_concave_shoulder ? _fallback_shoulder(x, shoulder_a, shoulder_b) : _exponential_curve(x, scale_shoulder, linear_slope_P_slope, shoulder_power_s_p,
                                transition_shoulder_x_s_tx, transition_shoulder_y_s_ty);
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

static float apply_offset(float x, float offset)
{
  // y = mx + b, b is the offset, m = (1 - offset), so the line runs from (0, offset) to (1, 1)
  return (1 - offset) * x + offset;
}

// https://iolite-engine.com/blog_posts/minimal_agx_implementation
static float3 _agxLook(float3 val, const dt_iop_agx_params_t *p)
{
  // values? {0.2126f, 0.7152f, 0.0722f} are Rec709 Y values
  // const float lw[] = {0.2126f, 0.7152f, 0.0722f};
  // Rec 2020 Y:
  const float lw[] = { 0.2626983389565561f, 0.6780087657728164f, 0.05929289527062728f };
  float luma = lw[0] * val.r + lw[1] * val.g + lw[2] * val.b;

  // Default
  float slope = p->look_slope;
  float3 power = { p->look_power, p->look_power, p->look_power };
  float offset = p->look_offset;
  float sat = p->look_saturation;

  // ASC CDL
  float offset_r = apply_offset(val.r, offset);
  float offset_g = apply_offset(val.g, offset);
  float offset_b = apply_offset(val.b, offset);
  float3 pow_val = _powf3((float3){ fmaxf(0.0f, offset_r) * slope, fmaxf(0.0f, offset_g) * slope,
                                    fmaxf(0.0f, offset_b) * slope },
                          power);

  float3 result;
  result.r = luma + sat * (pow_val.r - luma);
  result.g = luma + sat * (pow_val.g - luma);
  result.b = luma + sat * (pow_val.b - luma);
  return result;
}

static float3 _apply_log_encoding(dt_aligned_pixel_t pixel, float range_in_ev, float minEv)
{
  // Ensure no negative values
  float3 v = { fmaxf(0.0f, pixel[0] / 0.18f),
               fmaxf(0.0f, pixel[1] / 0.18f),
               fmaxf(0.0f, pixel[2] / 0.18f)
  };

  // Log2 encoding
  v.r = fmaxf(v.r, _epsilon);
  v.g = fmaxf(v.g, _epsilon);
  v.b = fmaxf(v.b, _epsilon);

  v.r = log2f(v.r);
  v.g = log2f(v.g);
  v.b = log2f(v.b);

  // normalise
  v.r = (v.r - minEv) / range_in_ev;
  v.g = (v.g - minEv) / range_in_ev;
  v.b = (v.b - minEv) / range_in_ev;

  v.r = fminf(fmaxf(v.r, 0.0f), 1.0f);
  v.g = fminf(fmaxf(v.g, 0.0f), 1.0f);
  v.b = fminf(fmaxf(v.b, 0.0f), 1.0f);

  return v;
}

static float3 _agx_tone_mapping(float3 rgb, const dt_iop_agx_params_t *p, float linear_slope_P_slope,
                                float transition_toe_x_t_tx, float transition_toe_y_t_ty,
                                float transition_shoulder_x_s_tx, float transition_shoulder_y_s_ty,
                                float range_in_ev, float minEv, float scale_toe_s_t, float intercept,
                                float scale_shoulder,
                                gboolean need_convex_toe, float toe_a, float toe_b,
                                gboolean need_concave_shoulder, float shoulder_a, float shoulder_b,
                                float sigmoid_curve_gamma)
{
  // Apply Inset Matrix
  rgb = _mat3f_mul_float3(AgXInsetMatrix, rgb);

  dt_aligned_pixel_t rgb_pixel;
  rgb_pixel[0] = rgb.r;
  rgb_pixel[1] = rgb.g;
  rgb_pixel[2] = rgb.b;

  // record current chromaticity angle
  dt_aligned_pixel_t hsv_pixel;
  dt_RGB_2_HSV(rgb_pixel, hsv_pixel);
  float h_before = hsv_pixel[0];

  float3 log_pixel = _apply_log_encoding(rgb_pixel, range_in_ev, minEv);

  // Apply sigmoid
  log_pixel.r = _calculate_sigmoid(log_pixel.r, linear_slope_P_slope, p->sigmoid_toe_power, p->sigmoid_shoulder_power,
                           transition_toe_x_t_tx, transition_toe_y_t_ty, transition_shoulder_x_s_tx,
                           transition_shoulder_y_s_ty, scale_toe_s_t, intercept, scale_shoulder,
                           need_convex_toe, toe_a, toe_b,
                           need_concave_shoulder, shoulder_a, shoulder_b);
  log_pixel.g = _calculate_sigmoid(log_pixel.g, linear_slope_P_slope, p->sigmoid_toe_power, p->sigmoid_shoulder_power,
                           transition_toe_x_t_tx, transition_toe_y_t_ty, transition_shoulder_x_s_tx,
                           transition_shoulder_y_s_ty, scale_toe_s_t, intercept, scale_shoulder,
                           need_convex_toe, toe_a, toe_b,
                           need_concave_shoulder, shoulder_a, shoulder_b);
  log_pixel.b = _calculate_sigmoid(log_pixel.b, linear_slope_P_slope, p->sigmoid_toe_power, p->sigmoid_shoulder_power,
                           transition_toe_x_t_tx, transition_toe_y_t_ty, transition_shoulder_x_s_tx,
                           transition_shoulder_y_s_ty, scale_toe_s_t, intercept, scale_shoulder,
                           need_convex_toe, toe_a, toe_b,
                           need_concave_shoulder, shoulder_a, shoulder_b);

  // Apply AgX look
  log_pixel = _agxLook(log_pixel, p);

  // Linearize
  rgb_pixel[0] = powf(fmaxf(0.0f, log_pixel.r), sigmoid_curve_gamma);
  rgb_pixel[1] = powf(fmaxf(0.0f, log_pixel.g), sigmoid_curve_gamma);
  rgb_pixel[2] = powf(fmaxf(0.0f, log_pixel.b), sigmoid_curve_gamma);

  // record post-sigmoid chroma angle
  dt_RGB_2_HSV(rgb_pixel, hsv_pixel);

  float h_after = hsv_pixel[0];

  h_after = _lerp_hue(h_before, h_after, p->look_original_hue_mix_ratio);

  hsv_pixel[0] = h_after;
  dt_HSV_2_RGB(hsv_pixel, rgb_pixel);

  float3 out;
  out.r = rgb_pixel[0];
  out.g = rgb_pixel[1];
  out.b = rgb_pixel[2];

  // Apply Outset Matrix
  out = _mat3f_mul_float3(AgXInsetMatrixInverse, out);

  return out;
}

void _print_sigmoid(float linear_slope_P_slope,
                                float toe_power_t_p, float shoulder_power_s_p, float transition_toe_x_t_tx,
                                float transition_toe_y_t_ty, float transition_shoulder_x_s_tx,
                                float transition_shoulder_y_s_ty, float scale_toe_s_t, float intercept,
                                float scale_shoulder,
                                gboolean need_convex_toe, float toe_a, float toe_b,
                                gboolean need_concave_shoulder, float shoulder_a, float shoulder_b)
{
  const int steps = 100;
  printf("\nSigmoid\n");
  for (int i = 0; i <= steps; i++)
  {
    float x = i / (float) steps;
    float y = _calculate_sigmoid(x, linear_slope_P_slope,
      toe_power_t_p, shoulder_power_s_p,
      transition_toe_x_t_tx, transition_toe_y_t_ty,
      transition_shoulder_x_s_tx, transition_shoulder_y_s_ty,
      scale_toe_s_t, intercept, scale_shoulder,
      need_convex_toe, toe_a, toe_b,
      need_concave_shoulder, shoulder_a, shoulder_b);
    printf("%f\t%f\n", x, y);
  }
  printf("\n");
}

// see https://www.desmos.com/calculator/gijzff3wlv
static float _calculate_B(float slope, float transition_x, float transition_y)
{
  return slope * transition_x / transition_y;
}

static float _calculate_A(float transition_x, float transition_y, float B)
{
  return transition_y / powf(transition_x, B);
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
  printf("sigmoid_linear_length_below_pivot = %f\n", p->sigmoid_linear_length_below_pivot);
  printf("sigmoid_linear_length_above_pivot = %f\n", p->sigmoid_linear_length_above_pivot);
  printf("sigmoid_toe_power = %f\n", p->sigmoid_toe_power);
  printf("sigmoid_shoulder_power = %f\n", p->sigmoid_shoulder_power);
  printf("sigmoid_target_display_black_y = %f\n", p->sigmoid_target_display_black_y);
  printf("sigmoid_target_display_white_y = %f\n", p->sigmoid_target_display_white_y);

  const float maxEv = p->range_white_relative_exposure;
  const float minEv = p->range_black_relative_exposure;
  const float pivot_x = fabsf(p->range_black_relative_exposure
                                  / (p->range_white_relative_exposure - p->range_black_relative_exposure));

  float range_in_ev = maxEv - minEv;

  // avoid range altering slope - 16.5 EV is the default AgX range; keep scaling pro
  float scaled_slope = p->sigmoid_contrast_around_pivot * (range_in_ev / 16.5);
  printf("scaled slope = %f\n", scaled_slope);

  const float pivot_y = powf(0.18, 1.0 / p->sigmoid_curve_gamma);

  float linear_below_pivot = p->sigmoid_linear_length_below_pivot;
  float toe_start_x_from_pivot_x = _dx_from_hypotenuse_and_slope(linear_below_pivot, scaled_slope);
  float toe_start_x = pivot_x - toe_start_x_from_pivot_x;
  float toe_y_below_pivot_y = scaled_slope * toe_start_x_from_pivot_x;
  float toe_start_y = pivot_y - toe_y_below_pivot_y;

  // toe
  float toe_b = _calculate_B(scaled_slope, toe_start_x, toe_start_y);
  float toe_a = _calculate_A(toe_start_x, toe_start_y, toe_b);

  // starting with a line of gradient scaled_slope from (0, 0 // FIXME: target_black) would take us to (pivot_x, linear_y_at_pivot_x_at_slope)
  float linear_y_at_pivot_x_at_slope = scaled_slope * toe_start_x;
  // Normally, the toe is concave: its gradient is gradually increasing, up to the slope of the linear
  // section. If the slope of the linear section is not enough to go from (0, 0) to
  // (transition_toe_x_t_tx, transition_toe_y_t_ty), we'll need a convex 'toe'
  gboolean need_convex_toe = linear_y_at_pivot_x_at_slope < toe_start_y; // FIXME: target black

  const float inverse_limit_toe_x_i_ilx = 1.0f; // 1 - t_lx
  const float inverse_limit_toe_y_t_ily = 1.0f - p->sigmoid_target_display_black_y;

  float inverse_transition_toe_x = 1.0f - toe_start_x;
  float inverse_transition_toe_y = 1.0f - toe_start_y;

  float scale_toe_s_t = -_scale(inverse_limit_toe_x_i_ilx, inverse_limit_toe_y_t_ily, inverse_transition_toe_x,
                                inverse_transition_toe_y, p->sigmoid_toe_power, scaled_slope);
  if(isnan(scale_toe_s_t))
  {
    errors++; // printf("scale_toe is NaN\n");
  }

  // shoulder
  float shoulder_length_P_slength = p->sigmoid_linear_length_above_pivot;
  float shoulder_x_from_pivot_x = _dx_from_hypotenuse_and_slope(shoulder_length_P_slength, scaled_slope);
  printf("shoulder_x_from_pivot_x = %f\n", shoulder_x_from_pivot_x);
  float transition_shoulder_x_s_tx = pivot_x + shoulder_x_from_pivot_x;
  printf("transition_shoulder_x_s_tx = %f\n", transition_shoulder_x_s_tx);
  float shoulder_y_from_pivot_y = scaled_slope * shoulder_x_from_pivot_x;
  printf("shoulder_y_from_pivot_y = %f\n", shoulder_y_from_pivot_y);
  float transition_shoulder_y_s_ty = pivot_y + shoulder_y_from_pivot_y;
  printf("transition_shoulder_y_s_ty = %f\n", transition_shoulder_y_s_ty);

  float linear_y_when_x_is_1 = transition_shoulder_y_s_ty + scaled_slope * inverse_transition_toe_x;
  // Normally, the shoulder is convex: its gradient is gradually decreasing from slope of the linear
  // section. If the slope of the linear section is not enough to go from (transition_toe_x_t_tx, transition_toe_y_t_ty) to
  // (1, 1), we'll need a concave 'shoulder'
  gboolean need_concave_shoulder = linear_y_when_x_is_1 < 1; // FIXME: target white

  const float shoulder_intersection_x_s_lx = 1;

  float shoulder_b = _calculate_B(scaled_slope, 1 - transition_shoulder_x_s_tx, 1 - transition_shoulder_y_s_ty);
  float shoulder_a = _calculate_A(1 - transition_shoulder_x_s_tx, 1 - transition_shoulder_y_s_ty, shoulder_b);

  float scale_shoulder
      = _scale(shoulder_intersection_x_s_lx, p->sigmoid_target_display_white_y,
              transition_shoulder_x_s_tx, transition_shoulder_y_s_ty,
              p->sigmoid_shoulder_power, scaled_slope);
  if(isnan(scale_shoulder))
  {
    errors++; // printf("scale_shoulder is NaN\n");
  }

  printf("scale_toe: %f, scale_shoulder: %f\n", scale_toe_s_t, scale_shoulder);

  // b
  float intercept = toe_start_y - scaled_slope * toe_start_x;
  if(isnan(intercept))
  {
    errors++; // printf("intercept is NaN\n");
  }

  printf("need_convex_toe: %d, need_concave_shoulder: %d\n", need_convex_toe, need_concave_shoulder);

  _print_sigmoid(scaled_slope,
                                p->sigmoid_toe_power, p->sigmoid_shoulder_power,
                                toe_start_x, toe_start_y,
                                transition_shoulder_x_s_tx, transition_shoulder_y_s_ty,
                                scale_toe_s_t, intercept, scale_shoulder,
                                need_convex_toe, toe_a, toe_b,
                                need_concave_shoulder, shoulder_a, shoulder_b);

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

      float3 agx_rgb = _agx_tone_mapping(
          rgb, p, scaled_slope, toe_start_x, toe_start_y, transition_shoulder_x_s_tx,
          transition_shoulder_y_s_ty, range_in_ev, minEv, scale_toe_s_t, intercept, scale_shoulder,
          need_convex_toe, toe_a, toe_b,
          need_concave_shoulder, shoulder_a, shoulder_b,
          p->sigmoid_curve_gamma);

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
}

// GUI update
void gui_update(dt_iop_module_t *self)
{
  dt_iop_agx_gui_data_t *g = self->gui_data;
  // dt_iop_agx_params_t *p = self->params;

  // No combobox anymore
  // Update the combobox
  // gtk_combo_box_set_active(GTK_COMBO_BOX(g->look), p->look);
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
  p.range_black_relative_exposure = -10;
  p.range_white_relative_exposure = 6.5;
  p.sigmoid_contrast_around_pivot = 2.4;
  p.sigmoid_linear_length_below_pivot = 0.0;
  p.sigmoid_linear_length_above_pivot = 0.0;
  p.sigmoid_toe_power = 1.5;
  p.sigmoid_shoulder_power = 1.5;
  p.sigmoid_target_display_black_y = 0.0;
  p.sigmoid_target_display_white_y = 1.0;
  p.sigmoid_curve_gamma = 2.2;
  p.look_original_hue_mix_ratio = 0.0f;

  // None preset
  p.look_slope = 1.0f;
  p.look_power = 1.0f;
  p.look_offset = 0.0f;
  p.look_saturation = 1.0f;

  dt_gui_presets_add_generic(_("None"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // Punchy preset
  p.look_slope = 1.0f;  // Slope was the same for all channels in Punchy
  p.look_power = 1.35f; // Power was the same for all channels in Punchy
  p.look_offset = 0.0f;
  p.look_saturation = 1.4f;
  dt_gui_presets_add_generic(_("Punchy"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);
}

// GUI cleanup
void gui_cleanup(dt_iop_module_t *self)
{
}
