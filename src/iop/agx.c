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

typedef struct dt_iop_agx_gui_data_t
{
} dt_iop_agx_gui_data_t;


// Module parameters struct
// Updated struct dt_iop_agx_params_t
typedef struct dt_iop_agx_params_t
{
  float slope;  // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "Slope"
  float power;  // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "Power"
  float offset; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "Offset"
  float sat;    // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "Saturation"
  float mix;    // $MIN: 0.0 $MAX: 1 $DEFAULT: 0.0 $DESCRIPTION: "Restore original hue"

  gboolean sigmoid_tunable; // $MIN: FALSE $MAX: TRUE $DEFAULT: TRUE $DESCRIPTION: "Use tunable curve vs fixed polynomial"
  float sigmoid_normalized_log2_minimum; // $MIN: -20.0 $MAX: -0.1 $DEFAULT: -10 $DESCRIPTION: "Black relative exposure (below mid-grey)"
  float sigmoid_normalized_log2_maximum; // $MIN: 0.1 $MAX: 20 $DEFAULT: 6.5 $DESCRIPTION: "White relative exposure (above mid-grey)"
  float sigmoid_linear_slope;    // $MIN: 0.1 $MAX: 10.0 $DEFAULT: 2.4 $DESCRIPTION: "Slope of linear portion"
  float sigmoid_toe_length;      // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "Toe start, below mid-grey"
  float sigmoid_shoulder_length; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "Shoulder start, above mid-grey"
  float sigmoid_toe_power;       // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.5 $DESCRIPTION: "Toe Power"
  float sigmoid_shoulder_power;  // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.5 $DESCRIPTION: "Shoulder Power"
  float sigmoid_toe_intersection_y;      // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "Target display black"
  float sigmoid_shoulder_intersection_y; // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "Target display white"
  float sigmoid_curve_gamma; // $MIN: 1.0 $MAX: 5.0 $DEFAULT: 2.2 $DESCRIPTION: "Sigmoid curve 'gamma'"
} dt_iop_agx_params_t;

void gui_init(dt_iop_module_t *self)
{
  dt_iop_agx_gui_data_t *g = IOP_GUI_ALLOC(agx);

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // look: sat, slope, offset, power, mix
  GtkWidget *look_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(self->widget), look_box, TRUE, TRUE, 0);
  GtkWidget *label = gtk_label_new(_("Look"));
  gtk_box_pack_start(GTK_BOX(look_box), label, FALSE, FALSE, 0);
  GtkWidget *slider;
  slider = dt_bauhaus_slider_from_params(self, "sat");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 2.0f);
  gtk_box_pack_start(GTK_BOX(look_box), slider, TRUE, TRUE, 0);

  slider = dt_bauhaus_slider_from_params(self, "slope");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 5.0f);
  gtk_box_pack_start(GTK_BOX(look_box), slider, TRUE, TRUE, 0);

  slider = dt_bauhaus_slider_from_params(self, "offset");
  dt_bauhaus_slider_set_soft_range(slider, -1.0f, 1.0f);
  gtk_box_pack_start(GTK_BOX(look_box), slider, TRUE, TRUE, 0);

  slider = dt_bauhaus_slider_from_params(self, "power");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 5.0f);
  gtk_box_pack_start(GTK_BOX(look_box), slider, TRUE, TRUE, 0);

  slider = dt_bauhaus_slider_from_params(self, "mix");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 1.0f);
  gtk_box_pack_start(GTK_BOX(look_box), slider, TRUE, TRUE, 0);

  // sigmoid
  GtkWidget *sigmoid_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(self->widget), sigmoid_box, TRUE, TRUE, 0);
  label = gtk_label_new(_("Sigmoid"));
  gtk_box_pack_start(GTK_BOX(sigmoid_box), label, FALSE, FALSE, 0);

  dt_bauhaus_toggle_from_params(self, "sigmoid_tunable");

  // black/white relative exposure
  slider = dt_bauhaus_slider_from_params(self, "sigmoid_normalized_log2_minimum");
  dt_bauhaus_slider_set_soft_range(slider, -20.0f, -1.0f);
  gtk_box_pack_start(GTK_BOX(sigmoid_box), slider, TRUE, TRUE, 0);

  slider = dt_bauhaus_slider_from_params(self, "sigmoid_normalized_log2_maximum");
  dt_bauhaus_slider_set_soft_range(slider, 1.0f, 20.0f);
  gtk_box_pack_start(GTK_BOX(sigmoid_box), slider, TRUE, TRUE, 0);

  // Internal 'gamma'
  slider = dt_bauhaus_slider_from_params(self, "sigmoid_curve_gamma");
  dt_bauhaus_slider_set_soft_range(slider, 1.0f, 5.0f);
  gtk_box_pack_start(GTK_BOX(sigmoid_box), slider, TRUE, TRUE, 0);

  // Linear Section Slope
  slider = dt_bauhaus_slider_from_params(self, "sigmoid_linear_slope");
  dt_bauhaus_slider_set_soft_range(slider, 0.1f, 5.0f);
  gtk_box_pack_start(GTK_BOX(sigmoid_box), slider, TRUE, TRUE, 0);

  // Toe
  slider = dt_bauhaus_slider_from_params(self, "sigmoid_toe_length");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 1.0f);
  gtk_box_pack_start(GTK_BOX(sigmoid_box), slider, TRUE, TRUE, 0);

  slider = dt_bauhaus_slider_from_params(self, "sigmoid_toe_power");
  dt_bauhaus_slider_set_soft_range(slider, 0.1f, 5.0f);
  gtk_box_pack_start(GTK_BOX(sigmoid_box), slider, TRUE, TRUE, 0);

  slider = dt_bauhaus_slider_from_params(self, "sigmoid_toe_intersection_y");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 1.0f);
  gtk_box_pack_start(GTK_BOX(sigmoid_box), slider, TRUE, TRUE, 0);

  // Shoulder
  slider = dt_bauhaus_slider_from_params(self, "sigmoid_shoulder_length");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 1.0f);
  gtk_box_pack_start(GTK_BOX(sigmoid_box), slider, TRUE, TRUE, 0);

  slider = dt_bauhaus_slider_from_params(self, "sigmoid_shoulder_power");
  dt_bauhaus_slider_set_soft_range(slider, 0.1f, 5.0f);
  gtk_box_pack_start(GTK_BOX(sigmoid_box), slider, TRUE, TRUE, 0);

  slider = dt_bauhaus_slider_from_params(self, "sigmoid_shoulder_intersection_y");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 2.0f);
  gtk_box_pack_start(GTK_BOX(sigmoid_box), slider, TRUE, TRUE, 0);
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


/*
https://github.com/EaryChow/AgX_LUT_Gen/blob/main/AgXBaseRec2020.py
        outset_matrix = numpy.linalg.inv(numpy.array([[0.899796955911611, 0.0871996192028351, 0.013003424885555],
                                                      [0.11142098895748, 0.875575586156966, 0.0130034248855548],
                                                      [0.11142098895748, 0.0871996192028349, 0.801379391839686]]))
        inverted:
            {
                {1.1271005818144, -0.11060664309660, -0.016493938717835},
                {-0.14132976349844, 1.1578237022163, -0.016493938717834},
                {-0.14132976349844, -0.11060664309660, 1.2519364065950}
            }
*/
const mat3f AgXOutsetMatrix = { { { 1.1271005818144368f, -0.11060664309660323f, -0.016493938717834573f },
                                  { -0.1413297634984383f, 1.157823702216272f, -0.016493938717834257f },
                                  { -0.14132976349843826f, -0.11060664309660294f, 1.2519364065950405f } } };

const mat3f AgXInsetMatrixInverse = { { { 1.1974410768877f, -0.14426151269800f, -0.053179564189704f },
                                        { -0.19647462632135f, 1.3540951314697f, -0.15762050514838f },
                                        { -0.14655741710660f, -0.10828405878847f, 1.2548414758951f } } };

// LOG2_MIN       = -10.0 (EV below mid-grey)
// LOG2_MAX       =  +6.5 (EV above mid-grey)
// log2(mid-grey) = -2.47 (EV below diffuse white)
const float AgxMinEv = -12.47393f; // log2(pow(2, LOG2_MIN) * MIDDLE_GRAY) -> mid-grey + black relative exposure =
                                   // black limit, relative to (below) diffuse white
const float AgxMaxEv = 4.026069f;  // log2(pow(2, LOG2_MAX) * MIDDLE_GRAY) -> mid-grey + white relative exposure =
                                   // white limit, relative to (above) diffuse white

// https://iolite-engine.com/blog_posts/minimal_agx_implementation
static float3 _agxDefaultContrastApprox(float3 x)
{
  float3 x2 = { x.r * x.r, x.g * x.g, x.b * x.b };
  float3 x4 = { x2.r * x2.r, x2.g * x2.g, x2.b * x2.b };
  float3 x6 = { x4.r * x2.r, x4.g * x2.g, x4.b * x2.b };

  float3 result;
  result.r = -17.86f * x6.r * x.r + 78.01f * x6.r - 126.7f * x4.r * x.r + 92.06f * x4.r - 28.72f * x2.r * x.r
             + 4.361f * x2.r - 0.1718f * x.r + 0.002857f;

  result.g = -17.86f * x6.g * x.g + 78.01f * x6.g - 126.7f * x4.g * x.g + 92.06f * x4.g - 28.72f * x2.g * x.g
             + 4.361f * x2.g - 0.1718f * x.g + 0.002857f;

  result.b = -17.86f * x6.b * x.b + 78.01f * x6.b - 126.7f * x4.b * x.b + 92.06f * x4.b - 28.72f * x2.b * x.b
             + 4.361f * x2.b - 0.1718f * x.b + 0.002857f;
  return result;
}

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

  float remaining_y_span = limit_y_ly - transition_y_ty;
  printf("remaining_y_span = %f\n", remaining_y_span);

  float y_delta_ratio = linear_y_delta / remaining_y_span;
  printf("y_delta_ratio = %f\n", y_delta_ratio);

  float term_b = powf(y_delta_ratio, power_p) - 1.0f;
  term_b = fmaxf(term_b, 1e-3);
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
  return x <= 0 ? 0 : toe_a * powf(x, toe_b);
}

static float _fallback_shoulder(float x, float shoulder_a, float shoulder_b)
{
  // FIXME: target white
  return (x >= 1) ? 1 : 1 - shoulder_a * powf(1 - x, shoulder_b);
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

  return result;
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

// https://iolite-engine.com/blog_posts/minimal_agx_implementation
static float3 _agxLook(float3 val, const dt_iop_agx_params_t *p)
{
  // values? {0.2126f, 0.7152f, 0.0722f} are Rec709 Y values
  // const float lw[] = {0.2126f, 0.7152f, 0.0722f};
  // Rec 2020 Y:
  const float lw[] = { 0.2626983389565561f, 0.6780087657728164f, 0.05929289527062728f };
  float luma = lw[0] * val.r + lw[1] * val.g + lw[2] * val.b;

  // Default
  float slope = p->slope;
  float3 power = { p->power, p->power, p->power };
  float offset = p->offset;
  float sat = p->sat;

  // ASC CDL
  float3 pow_val = _powf3((float3){ fmaxf(0.0f, val.r + offset) * slope, fmaxf(0.0f, val.g + offset) * slope,
                                    fmaxf(0.0f, val.b + offset) * slope },
                          power);

  float3 result;
  result.r = luma + sat * (pow_val.r - luma);
  result.g = luma + sat * (pow_val.g - luma);
  result.b = luma + sat * (pow_val.b - luma);
  return result;
}

static float3 _apply_log_encoding(dt_aligned_pixel_t pixel, gboolean sigmoid_tunable, float range_in_ev, float minEv)
{
  // Ensure no negative values
  float3 v = { fmaxf(0.0f, sigmoid_tunable ? pixel[0] / 0.18f : pixel[0]),
               fmaxf(0.0f, sigmoid_tunable ? pixel[1] / 0.18f : pixel[1]),
               fmaxf(0.0f, sigmoid_tunable ? pixel[2] / 0.18f : pixel[2]) };

  // Log2 encoding
  float small_value = 1E-10f;
  v.r = fmaxf(v.r, small_value);
  v.g = fmaxf(v.g, small_value);
  v.b = fmaxf(v.b, small_value);

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

  float3 log_pixel = _apply_log_encoding(rgb_pixel, p->sigmoid_tunable, range_in_ev, minEv);

  // Apply sigmoid
  if(p->sigmoid_tunable)
  {
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
  }
  else
  {
    log_pixel = _agxDefaultContrastApprox(log_pixel);
  }

  // Apply AgX look
  log_pixel = _agxLook(log_pixel, p);

  // Linearize
  rgb_pixel[0] = powf(fmaxf(0.0f, log_pixel.r), sigmoid_curve_gamma);
  rgb_pixel[1] = powf(fmaxf(0.0f, log_pixel.g), sigmoid_curve_gamma);
  rgb_pixel[2] = powf(fmaxf(0.0f, log_pixel.b), sigmoid_curve_gamma);

  // record post-sigmoid chroma angle
  dt_RGB_2_HSV(rgb_pixel, hsv_pixel);

  float h_after = hsv_pixel[0];

  h_after = _lerp_hue(h_before, h_after, p->mix);

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
  printf("\nSigmoid\n");
  for (float x = 0 ; x <= 1.001f; x+=0.01f)
  {
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
  printf("sigmoid_tunable = %d\n", p->sigmoid_tunable);
  printf("sigmoid_normalized_log2_minimum = %f\n", p->sigmoid_normalized_log2_minimum);
  printf("sigmoid_normalized_log2_maximum = %f\n", p->sigmoid_normalized_log2_maximum);
  printf("sigmoid_linear_slope = %f\n", p->sigmoid_linear_slope);
  printf("sigmoid_toe_length = %f\n", p->sigmoid_toe_length);
  printf("sigmoid_shoulder_length = %f\n", p->sigmoid_shoulder_length);
  printf("sigmoid_toe_power = %f\n", p->sigmoid_toe_power);
  printf("sigmoid_shoulder_power = %f\n", p->sigmoid_shoulder_power);
  printf("sigmoid_toe_intersection_y = %f\n", p->sigmoid_toe_intersection_y);
  printf("sigmoid_shoulder_intersection_y = %f\n", p->sigmoid_shoulder_intersection_y);

  const float maxEv = p->sigmoid_tunable ? p->sigmoid_normalized_log2_maximum : AgxMaxEv;
  const float minEv = p->sigmoid_tunable ? p->sigmoid_normalized_log2_minimum : AgxMinEv;
  const float pivot_x_p_x = fabsf(p->sigmoid_normalized_log2_minimum
                                  / (p->sigmoid_normalized_log2_maximum - p->sigmoid_normalized_log2_minimum));

  float range_in_ev = maxEv - minEv;

  // avoid range altering slope
  float linear_slope_P_slope = p->sigmoid_linear_slope * (range_in_ev / 16.5);

  const float pivot_y_p_y = powf(0.18, 1.0 / p->sigmoid_curve_gamma);

  float toe_length_P_tlength = p->sigmoid_toe_length;
  float toe_x_from_pivot_x = _dx_from_hypotenuse_and_slope(toe_length_P_tlength, linear_slope_P_slope);
  float transition_toe_x_t_tx = pivot_x_p_x - toe_x_from_pivot_x;
  float toe_y_from_pivot_y = linear_slope_P_slope * toe_x_from_pivot_x;
  float transition_toe_y_t_ty = pivot_y_p_y - toe_y_from_pivot_y;

  // toe
  float toe_b = _calculate_B(linear_slope_P_slope, transition_toe_x_t_tx, transition_toe_y_t_ty);
  float toe_a = _calculate_A(transition_toe_x_t_tx, transition_toe_y_t_ty, toe_b);

  float linear_y_at_pivot_x_from_origin_at_slope = linear_slope_P_slope * transition_toe_x_t_tx;
  // Normally, the toe is concave: its gradient is gradually increasing, up to the slope of the linear
  // section. If the slope of the linear section is not enough to go from (0, 0) to
  // (transition_toe_x_t_tx, transition_toe_y_t_ty), we'll need a convex 'toe'
  gboolean need_convex_toe = linear_y_at_pivot_x_from_origin_at_slope < transition_toe_y_t_ty; // FIXME: target black

  const float inverse_limit_toe_x_i_ilx = 1.0f; // 1 - t_lx
  const float inverse_limit_toe_y_t_ily = 1.0f - p->sigmoid_toe_intersection_y;

  float inverse_transition_toe_x = 1.0f - transition_toe_x_t_tx;
  float inverse_transition_toe_y = 1.0f - transition_toe_y_t_ty;

  float scale_toe_s_t = -_scale(inverse_limit_toe_x_i_ilx, inverse_limit_toe_y_t_ily, inverse_transition_toe_x,
                                inverse_transition_toe_y, p->sigmoid_toe_power, linear_slope_P_slope);
  if(isnan(scale_toe_s_t))
  {
    errors++; // printf("scale_toe is NaN\n");
  }

  // shoulder
  float shoulder_length_P_slength = p->sigmoid_shoulder_length;
  float shoulder_x_from_pivot_x = _dx_from_hypotenuse_and_slope(shoulder_length_P_slength, linear_slope_P_slope);
  printf("shoulder_x_from_pivot_x = %f\n", shoulder_x_from_pivot_x);
  float transition_shoulder_x_s_tx = pivot_x_p_x + shoulder_x_from_pivot_x;
  printf("transition_shoulder_x_s_tx = %f\n", transition_shoulder_x_s_tx);
  float shoulder_y_from_pivot_y = linear_slope_P_slope * shoulder_x_from_pivot_x;
  printf("shoulder_y_from_pivot_y = %f\n", shoulder_y_from_pivot_y);
  float transition_shoulder_y_s_ty = pivot_y_p_y + shoulder_y_from_pivot_y;
  printf("transition_shoulder_y_s_ty = %f\n", transition_shoulder_y_s_ty);

  float linear_y_at_1 = transition_shoulder_y_s_ty + linear_slope_P_slope * inverse_transition_toe_x;
  // Normally, the toe is convex: its gradient is gradually decreasing from slope of the linear
  // section. If the slope of the linear section is not enough to go from (transition_toe_x_t_tx, transition_toe_y_t_ty) to
  // (1, 1), we'll need a concave 'shoulder'
  gboolean need_concave_shoulder = linear_y_at_1 < 1; // FIXME: target white

  const float shoulder_intersection_x_s_lx = 1;

  float shoulder_b = _calculate_B(linear_slope_P_slope, 1 - transition_shoulder_x_s_tx, 1 - transition_shoulder_y_s_ty);
  float shoulder_a = _calculate_A(1 - transition_shoulder_x_s_tx, 1 - transition_shoulder_y_s_ty, shoulder_b);

  float scale_shoulder
      = _scale(shoulder_intersection_x_s_lx, p->sigmoid_shoulder_intersection_y,
              transition_shoulder_x_s_tx, transition_shoulder_y_s_ty,
              p->sigmoid_shoulder_power, linear_slope_P_slope);
  if(isnan(scale_shoulder))
  {
    errors++; // printf("scale_shoulder is NaN\n");
  }

  printf("scale_toe: %f, scale_shoulder: %f\n", scale_toe_s_t, scale_shoulder);

  // b
  float intercept = transition_toe_y_t_ty - linear_slope_P_slope * transition_toe_x_t_tx;
  if(isnan(intercept))
  {
    errors++; // printf("intercept is NaN\n");
  }

  printf("need_convex_toe: %d, need_concave_shoulder: %d\n", need_convex_toe, need_concave_shoulder);

  _print_sigmoid(linear_slope_P_slope,
                                p->sigmoid_toe_power, p->sigmoid_shoulder_power,
                                transition_toe_x_t_tx, transition_toe_y_t_ty,
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
          rgb, p, linear_slope_P_slope, transition_toe_x_t_tx, transition_toe_y_t_ty, transition_shoulder_x_s_tx,
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
  p.sigmoid_tunable = TRUE;
  p.sigmoid_normalized_log2_minimum = -10;
  p.sigmoid_normalized_log2_maximum = 6.5;
  p.sigmoid_linear_slope = 2.4;
  p.sigmoid_toe_length = 0.0;
  p.sigmoid_shoulder_length = 0.0;
  p.sigmoid_toe_power = 1.5;
  p.sigmoid_shoulder_power = 1.5;
  p.sigmoid_toe_intersection_y = 0.0;
  p.sigmoid_shoulder_intersection_y = 1.0;
  p.mix = 0.0f;

  // None preset
  p.slope = 1.0f;
  p.power = 1.0f;
  p.offset = 0.0f;
  p.sat = 1.0f;

  dt_gui_presets_add_generic(_("None"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // Punchy preset
  p.slope = 1.0f;  // Slope was the same for all channels in Punchy
  p.power = 1.35f; // Power was the same for all channels in Punchy
  p.offset = 0.0f;
  p.sat = 1.4f;
  dt_gui_presets_add_generic(_("Punchy"), self->op, self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);
}

// GUI cleanup
void gui_cleanup(dt_iop_module_t *self)
{
}
