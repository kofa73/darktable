#include "bauhaus/bauhaus.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include "common/colorspaces_inline_conversions.h"

#include <gtk/gtk.h>
#include <stdlib.h>
#include <math.h> // For math functions

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

const float pivot_y_p_y = 0.45865644686f; //powf(mid_grey = 0.18, 1.0f / 2.2f);

typedef struct dt_iop_agx_gui_data_t {
} dt_iop_agx_gui_data_t;


// Module parameters struct
// Updated struct dt_iop_agx_params_t
typedef struct dt_iop_agx_params_t {
    float slope;   // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "Slope"
    float power;   // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "Power"
    float offset;  // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "Offset"
    float sat;     // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "Saturation"
    float mix;     // $MIN: 0.0 $MAX: 1 $DEFAULT: 0.0 $DESCRIPTION: "Restore original hue"

    gboolean sigmoid_tunable;    // $MIN: FALSE $MAX: TRUE $DEFAULT: TRUE $DESCRIPTION: "Use tunable curve vs fixed polynomial"
    float sigmoid_normalized_log2_minimum; // $MIN: -20.0 $MAX: -0.1 $DEFAULT: -10 $DESCRIPTION: "Black relative exposure (below mid-grey)"
    float sigmoid_normalized_log2_maximum; // $MIN: 0.1 $MAX: 20 $DEFAULT: 6.5 $DESCRIPTION: "White relative exposure (above mid-grey)"
    float sigmoid_linear_slope; // $MIN: 0.1 $MAX: 10.0 $DEFAULT: 2.4 $DESCRIPTION: "Slope of linear portion"
    float sigmoid_toe_length;    // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "Toe start, below mid-grey"
    float sigmoid_shoulder_length; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "Shoulder start, above mid-grey"
    float sigmoid_toe_power;     // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.5 $DESCRIPTION: "Toe Power"
    float sigmoid_shoulder_power;// $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.5 $DESCRIPTION: "Shoulder Power"
    float sigmoid_toe_intersection_y;    // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "Target display black"
    float sigmoid_shoulder_intersection_y; // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "Target display white"
} dt_iop_agx_params_t;

void gui_init(dt_iop_module_t *self) {
    dt_iop_agx_gui_data_t *g = IOP_GUI_ALLOC(agx);

    self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

    // look: sat, slope, offset, power, mix
    GtkWidget *look_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
    gtk_box_pack_start(GTK_BOX(self->widget), look_box, TRUE, TRUE, 0);
    GtkWidget *label = gtk_label_new(_("Look"));
    gtk_box_pack_start(GTK_BOX(look_box), label, FALSE, FALSE, 0);
    GtkWidget *slider;
    slider = dt_bauhaus_slider_from_params(self, "sat");
    dt_bauhaus_slider_set_soft_range(slider, 0.0f, 10.0f);
    gtk_box_pack_start(GTK_BOX(look_box), slider, TRUE, TRUE, 0);

    slider = dt_bauhaus_slider_from_params(self, "slope");
    dt_bauhaus_slider_set_soft_range(slider, 0.0f, 10.0f);
    gtk_box_pack_start(GTK_BOX(look_box), slider, TRUE, TRUE, 0);

    slider = dt_bauhaus_slider_from_params(self, "offset");
    dt_bauhaus_slider_set_soft_range(slider, -1.0f, 1.0f);
    gtk_box_pack_start(GTK_BOX(look_box), slider, TRUE, TRUE, 0);

    slider = dt_bauhaus_slider_from_params(self, "power");
    dt_bauhaus_slider_set_soft_range(slider, 0.0f, 10.0f);
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

    // Linear Section Slope
    slider = dt_bauhaus_slider_from_params(self, "sigmoid_linear_slope");
    dt_bauhaus_slider_set_soft_range(slider, 0.1f, 10.0f);
    gtk_box_pack_start(GTK_BOX(sigmoid_box), slider, TRUE, TRUE, 0);

    // Toe
    slider = dt_bauhaus_slider_from_params(self, "sigmoid_toe_length");
    dt_bauhaus_slider_set_soft_range(slider, 0.0f, 1.0f);
    gtk_box_pack_start(GTK_BOX(sigmoid_box), slider, TRUE, TRUE, 0);

    slider = dt_bauhaus_slider_from_params(self, "sigmoid_toe_power");
    dt_bauhaus_slider_set_soft_range(slider, 0.1f, 10.0f);
    gtk_box_pack_start(GTK_BOX(sigmoid_box), slider, TRUE, TRUE, 0);

    slider = dt_bauhaus_slider_from_params(self, "sigmoid_toe_intersection_y");
    dt_bauhaus_slider_set_soft_range(slider, 0.0f, 1.0f);
    gtk_box_pack_start(GTK_BOX(sigmoid_box), slider, TRUE, TRUE, 0);

    // Shoulder
    slider = dt_bauhaus_slider_from_params(self, "sigmoid_shoulder_length");
    dt_bauhaus_slider_set_soft_range(slider, 0.0f, 1.0f);
    gtk_box_pack_start(GTK_BOX(sigmoid_box), slider, TRUE, TRUE, 0);

    slider = dt_bauhaus_slider_from_params(self, "sigmoid_shoulder_power");
    dt_bauhaus_slider_set_soft_range(slider, 0.1f, 10.0f);
    gtk_box_pack_start(GTK_BOX(sigmoid_box), slider, TRUE, TRUE, 0);

    slider = dt_bauhaus_slider_from_params(self, "sigmoid_shoulder_intersection_y");
    dt_bauhaus_slider_set_soft_range(slider, 0.0f, 2.0f);
    gtk_box_pack_start(GTK_BOX(sigmoid_box), slider, TRUE, TRUE, 0);
}

// Global data struct (not needed for this simple example)
typedef struct dt_iop_agx_global_data_t {} dt_iop_agx_global_data_t;

// Structs for vector and matrix math (pure C)
typedef struct {
  float r, g, b;
} float3;

typedef struct {
    float m[3][3];
} mat3f;

// Helper function: matrix multiplication
static float3 _mat3f_mul_float3(const mat3f m, const float3 v) {
    float3 result;
    result.r = m.m[0][0] * v.r + m.m[0][1] * v.g + m.m[0][2] * v.b;
    result.g = m.m[1][0] * v.r + m.m[1][1] * v.g + m.m[1][2] * v.b;
    result.b = m.m[2][0] * v.r + m.m[2][1] * v.g + m.m[2][2] * v.b;
    return result;
}

// Helper function: pow function
static float3 _powf3(float3 base, float3 exponent) {
  float3 result;
  result.r = powf(base.r, exponent.r);
  result.g = powf(base.g, exponent.g);
  result.b = powf(base.b, exponent.b);
  return result;
}


// Modelines (needed, but not that relevant right now)

// Translatable name
const char *name() { return _("AgX Tone Mapper"); }

// Module description
const char **description(dt_iop_module_t *self) {
  return dt_iop_set_description(self, _("Applies AgX tone mapping curve."),
                                _("Creative look and tone adjustment"),
                                _("linear, RGB, scene-referred"),
                                _("linear, RGB"), _("linear, RGB, scene-referred"));
}

// Flags
int flags() {
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

// Default group
int default_group() { return IOP_GROUP_COLOR; }

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece) {
  return IOP_CS_RGB;
}

// Legacy parameters (not needed for version 1)
int legacy_params(dt_iop_module_t *self, const void *const old_params,
                  const int old_version, void **new_params,
                  int32_t *new_params_size, int *new_version) {
  return 1; // no conversion possible
}

// Commit parameters
void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece) {
  memcpy(piece->data, p1, self->params_size);
}

//AgX implementation

/*
https://github.com/EaryChow/AgX_LUT_Gen/blob/main/AgXBaseRec2020.py
        inset_matrix = numpy.array([[0.856627153315983, 0.0951212405381588, 0.0482516061458583],
                                    [0.137318972929847, 0.761241990602591, 0.101439036467562],
                                    [0.11189821299995, 0.0767994186031903, 0.811302368396859]])
*/
const mat3f AgXInsetMatrix = {
  {
    {0.856627153315983f, 0.0951212405381588f, 0.0482516061458583f},
    {0.137318972929847f, 0.761241990602591f, 0.101439036467562f},
    {0.11189821299995f, 0.0767994186031903f, 0.811302368396859f}
  }
};


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
const mat3f AgXOutsetMatrix = {
  {
    { 1.1271005818144368f,  -0.11060664309660323f, -0.016493938717834573f},
    {-0.1413297634984383f,   1.157823702216272f,   -0.016493938717834257f},
    {-0.14132976349843826f, -0.11060664309660294f,  1.2519364065950405f}
  }
};

const mat3f AgXInsetMatrixInverse = {
  {
    {1.1974410768877f, -0.14426151269800f, -0.053179564189704f},
    {-0.19647462632135f, 1.3540951314697f, -0.15762050514838f},
    {-0.14655741710660f, -0.10828405878847f, 1.2548414758951f}
  }
};

// LOG2_MIN       = -10.0 (EV below mid-grey)
// LOG2_MAX       =  +6.5 (EV above mid-grey)
// log2(mid-grey) = -2.47 (EV below diffuse white)
const float AgxMinEv = -12.47393f;      // log2(pow(2, LOG2_MIN) * MIDDLE_GRAY) -> mid-grey + black relative exposure = black limit, relative to (below) diffuse white
const float AgxMaxEv = 4.026069f;       // log2(pow(2, LOG2_MAX) * MIDDLE_GRAY) -> mid-grey + white relative exposure = white limit, relative to (above) diffuse white

// https://iolite-engine.com/blog_posts/minimal_agx_implementation
static float3 _agxDefaultContrastApprox(float3 x) {
    float3 x2 = {x.r * x.r, x.g * x.g, x.b * x.b};
    float3 x4 = {x2.r * x2.r, x2.g * x2.g, x2.b * x2.b};
    float3 x6 = {x4.r * x2.r, x4.g * x2.g, x4.b * x2.b};

    float3 result;
    result.r =  - 17.86f    * x6.r * x.r
                + 78.01f    * x6.r
                - 126.7f    * x4.r * x.r
                + 92.06f    * x4.r
                - 28.72f    * x2.r * x.r
                + 4.361f    * x2.r
                - 0.1718f   * x.r
                + 0.002857f;

    result.g =  - 17.86f    * x6.g * x.g
                + 78.01f    * x6.g
                - 126.7f    * x4.g * x.g
                + 92.06f    * x4.g
                - 28.72f    * x2.g * x.g
                + 4.361f    * x2.g
                - 0.1718f   * x.g
                + 0.002857f;

    result.b =  - 17.86f    * x6.b * x.b
                + 78.01f    * x6.b
                - 126.7f    * x4.b * x.b
                + 92.06f    * x4.b
                - 28.72f    * x2.b * x.b
                + 4.361f    * x2.b
                - 0.1718f   * x.b
                + 0.002857f;
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

static float _linear_breakpoint(float numerator, float slope, float coordinate) {
    // Pythagorean: if dx were 1, dy would be 1 * slope = slope
    // The hypotenuse would be sqrt(dx^2 + dy^2) = sqrt(1^2 + (1 * slope)^2) = sqrt(1 + slope^2)
    float denominator = sqrtf(slope * slope + 1.0f);
    // we are given the length of a hypotenuse as 'numerator'; the length of that hypotenuse and the one
    // we calculated is the same as the length of the corresponding leg and dx = 1, so calculate proportionally;
    // the line segments start coordinate is 'coordinate', so 'start the segment' at that point
    return numerator / denominator + coordinate;
}

static float _line(float x_in, float slope, float intercept) {
    return slope * x_in + intercept;
}

static float _scale(float limit_x_lx, float limit_y_ly, float transition_x_tx, float transition_y_ty, float power_p, float slope_m) {
  float linear_y_delta = slope_m * (limit_x_lx - transition_x_tx);
  float power_curved_y_delta = powf(linear_y_delta, -power_p); // dampened / steepened
    if(isnan(power_curved_y_delta) || power_curved_y_delta < 0.0f)
  {
    power_curved_y_delta = 0;
  }
  float remaining_y_span = limit_y_ly - transition_y_ty;
  float y_delta_ratio = linear_y_delta / remaining_y_span;
  float term_b = powf(y_delta_ratio, power_p) - 1.0f;
    // if(isnan(term_b) || term_b < 0.0f)
    // {
    //   term_b = 0;
    // }

    float scale_value = powf(power_curved_y_delta * term_b, -1.0f / power_p);
    if(isnan(scale_value) || isinf(scale_value))
    {
      // with extreme settings, the scale value explodes, let's limit that
      scale_value = 1000;
    }

  return scale_value;
}

static float _exponential(float x_in, float power) {
  float value = x_in / powf(1.0f + powf(x_in, power), 1.0f / power);
  if (isnan(value))
  {
    errors++; //printf("_exponential returns nan\n");
  }
  return value;
}

static float _exponential_curve(float x_in, float scale_, float slope, float power, float transition_x, float transition_y) {
  float value = scale_ * _exponential(slope * (x_in - transition_x) / scale_, power) + transition_y;
  if (isnan(value))
  {
    errors++; //printf("_exponential_curve returns nan\n");
  }
  return value;
}

// the commented values (t_tx, etc) are references to https://www.desmos.com/calculator/yrysofmx8h
static float _calculate_sigmoid(
    float x,
    // Slope of linear portion.
    float linear_slope_P_slope,
    // Exponential power of the toe and shoulder regions.
    float toe_power_t_p, float shoulder_power_s_p,
    // Intersection limit values for the toe and shoulder.
    float toe_intersection_y_ly, float shoulder_intersection_y_s_ly,
    float transition_toe_x_t_tx, float transition_toe_y_t_ty,
    float transition_shoulder_x_s_tx, float transition_shoulder_y_s_ty,
  int debug
) {
    // shoulder transition
    float inverse_transition_toe_x = 1.0f - transition_toe_x_t_tx;
    float inverse_transition_toe_y = 1.0f - transition_toe_y_t_ty;

    const float inverse_limit_toe_x_i_ilx = 1.0f; // 1 - t_lx
    const float inverse_limit_toe_y_t_ily = 1.0f - toe_intersection_y_ly;

    const float shoulder_intersection_x_s_lx = 1;

    float scale_toe_s_t = -_scale(
        inverse_limit_toe_x_i_ilx,
        inverse_limit_toe_y_t_ily,
        inverse_transition_toe_x,
        inverse_transition_toe_y,
        toe_power_t_p,
        linear_slope_P_slope
    );
    if (isnan(scale_toe_s_t))
    {
      errors++; // printf("scale_toe is NaN\n");
    }

    float scale_shoulder = _scale(
        shoulder_intersection_x_s_lx,
        shoulder_intersection_y_s_ly,
        transition_shoulder_x_s_tx,
        transition_shoulder_y_s_ty,
        shoulder_power_s_p,
        linear_slope_P_slope
    );
    if (isnan(scale_shoulder))
    {
      errors++; // printf("scale_shoulder is NaN\n");
    }

    if (debug)
    {
      printf("scale_toe: %f, scale_shoulder: %f\n", scale_toe_s_t, scale_shoulder);
    }

    // b
    float intercept = transition_toe_y_t_ty - linear_slope_P_slope * transition_toe_x_t_tx;
    if (isnan(intercept))
    {
      errors++; // printf("intercept is NaN\n");
    }

  float result;

  if (x < transition_toe_x_t_tx) {
    result = _exponential_curve(
        x,
        scale_toe_s_t,
        linear_slope_P_slope,
        toe_power_t_p,
        transition_toe_x_t_tx,
        transition_toe_y_t_ty
    );
  } else if (x <= transition_shoulder_x_s_tx) {
    result = _line(x, linear_slope_P_slope, intercept);
  } else {
    result = _exponential_curve(
        x,
        scale_shoulder,
        linear_slope_P_slope,
        shoulder_power_s_p,
        transition_shoulder_x_s_tx,
        transition_shoulder_y_s_ty
    );
  }

  return result;
}

// https://iolite-engine.com/blog_posts/minimal_agx_implementation
static float3 _agxLook(float3 val, const dt_iop_agx_params_t *p) {
    // values? {0.2126f, 0.7152f, 0.0722f} are Rec709 Y values
    //const float lw[] = {0.2126f, 0.7152f, 0.0722f};
    // Rec 2020 Y:
    const float lw[] = {0.2626983389565561f, 0.6780087657728164f, 0.05929289527062728f};
    float luma = lw[0] * val.r + lw[1] * val.g + lw[2] * val.b;

    // Default
    float slope = p->slope;
    float3 power = {p->power, p->power, p->power};
    float offset = p->offset;
    float sat = p->sat;

    // ASC CDL
    float3 pow_val = _powf3((float3){fmaxf(0.0f, val.r + offset) * slope, fmaxf(0.0f, val.g + offset) * slope, fmaxf(0.0f, val.b + offset) * slope}, power);

    float3 result;
    result.r = luma + sat * (pow_val.r - luma);
    result.g = luma + sat * (pow_val.g - luma);
    result.b = luma + sat * (pow_val.b - luma);
    return result;
}

static float3 _agx_tone_mapping(
  float3 rgb,
  const dt_iop_agx_params_t *p,
  float linear_slope_P_slope,
  float transition_toe_x_t_tx, float transition_toe_y_t_ty,
  float transition_shoulder_x_s_tx, float transition_shoulder_y_s_ty,
  float range_in_ev,
  float minEv,
  int debug)
{
  dt_aligned_pixel_t rgb_pixel;
  rgb_pixel[0] = rgb.r;
  rgb_pixel[1] = rgb.g;
  rgb_pixel[2] = rgb.b;
  dt_aligned_pixel_t hsv_before;
  dt_RGB_2_HSV(rgb_pixel, hsv_before);

  // Ensure no negative values
  float3 v = {fmaxf(0.0f, p->sigmoid_tunable ? rgb.r / 0.18f : rgb.r), fmaxf(0.0f, p->sigmoid_tunable ?rgb.g / 0.18f : rgb.g), fmaxf(0.0f, p->sigmoid_tunable ? rgb.b / 0.18f : rgb.b)};

  // Apply Inset Matrix
  v = _mat3f_mul_float3(AgXInsetMatrix, v);

  // Log2 encoding
  float small_value = 1E-10f;
  v.r = fmaxf(v.r, small_value);
  v.g = fmaxf(v.g, small_value);
  v.b = fmaxf(v.b, small_value);

  v.r = log2f(v.r);
  v.g = log2f(v.g);
  v.b = log2f(v.b);

  v.r = (v.r - minEv) / range_in_ev;
  v.g = (v.g - minEv) / range_in_ev;
  v.b = (v.b - minEv) / range_in_ev;

  v.r = fminf(fmaxf(v.r, 0.0f), 1.0f);
  v.g = fminf(fmaxf(v.g, 0.0f), 1.0f);
  v.b = fminf(fmaxf(v.b, 0.0f), 1.0f);

  // Apply sigmoid
  if (p->sigmoid_tunable)
  {
    v.r = _calculate_sigmoid(
      v.r,
      linear_slope_P_slope,
      p->sigmoid_toe_power, p->sigmoid_shoulder_power,
      p->sigmoid_toe_intersection_y,
      p->sigmoid_shoulder_intersection_y,
      transition_toe_x_t_tx, transition_toe_y_t_ty,
      transition_shoulder_x_s_tx, transition_shoulder_y_s_ty,
      debug
    );
    v.g = _calculate_sigmoid(
      v.g,
      linear_slope_P_slope,
      p->sigmoid_toe_power, p->sigmoid_shoulder_power,
      p->sigmoid_toe_intersection_y,
      p->sigmoid_shoulder_intersection_y,
      transition_toe_x_t_tx, transition_toe_y_t_ty,
      transition_shoulder_x_s_tx, transition_shoulder_y_s_ty,
      debug
    );
    v.b = _calculate_sigmoid(
      v.b,
      linear_slope_P_slope,
      p->sigmoid_toe_power, p->sigmoid_shoulder_power,
      p->sigmoid_toe_intersection_y,
      p->sigmoid_shoulder_intersection_y,
      transition_toe_x_t_tx, transition_toe_y_t_ty,
      transition_shoulder_x_s_tx, transition_shoulder_y_s_ty,
      debug
    );
  } else
  {
    v = _agxDefaultContrastApprox(v);
  }

  // Apply AgX look
  v = _agxLook(v, p);


  // Linearize
  rgb_pixel[0] = powf(fmaxf(0.0f, v.r), 2.2f);
  rgb_pixel[1] = powf(fmaxf(0.0f, v.g), 2.2f);
  rgb_pixel[2] = powf(fmaxf(0.0f, v.b), 2.2f);

  // Apply Outset Matrix
  v = _mat3f_mul_float3(AgXInsetMatrixInverse, v);

    dt_aligned_pixel_t hsv_after;
    dt_RGB_2_HSV(rgb_pixel, hsv_after);

    float mix = p->mix;

    float h_before = hsv_before[0];
    float h_after = hsv_after[0];

    float hue_diff = h_after - h_before;

    if (hue_diff > 0.5) {
      h_after -= 1;
    } else if (hue_diff < -0.5) {
      h_after += 1;
    }

    h_after = h_after + (h_before - h_after) * mix;
    if (h_after < 0) {
      h_after += 1;
    } else if (h_after > 1) {
      h_after -= 1;
    }

    hsv_after[0] = h_after;
    dt_HSV_2_RGB(hsv_after, rgb_pixel);

    v.r = rgb_pixel[0];
    v.g = rgb_pixel[1];
    v.b = rgb_pixel[2];

    return v;
}

// Process
void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out) {

  dt_iop_agx_params_t *p = piece->data;
  const size_t ch = piece->colors;

  if (!dt_iop_have_required_input_format(4, self, piece->colors, ivoid, ovoid,
                                          roi_in, roi_out))
    return;

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
  const float pivot_x_p_x = fabsf(p->sigmoid_normalized_log2_minimum / (p->sigmoid_normalized_log2_maximum - p->sigmoid_normalized_log2_minimum));

  float range_in_ev = maxEv - minEv;

      float linear_slope_P_slope = p->sigmoid_linear_slope * (range_in_ev / 16.5);

    // toe
    float toe_length_P_tlength = p->sigmoid_toe_length;
    float toe_x_from_pivot_x = _dx_from_hypotenuse_and_slope(toe_length_P_tlength, linear_slope_P_slope);
    float transition_toe_x_t_tx = pivot_x_p_x - toe_x_from_pivot_x;
    float toe_y_from_pivot_y = linear_slope_P_slope * toe_x_from_pivot_x;
    float transition_toe_y_t_ty = pivot_y_p_y - toe_y_from_pivot_y;

    float original_transition_toe_x_t_tx = _linear_breakpoint(-toe_length_P_tlength, linear_slope_P_slope, pivot_x_p_x);
    float original_transition_toe_y_t_ty = _linear_breakpoint(-linear_slope_P_slope * toe_length_P_tlength, linear_slope_P_slope, pivot_y_p_y);

    if (fabs(original_transition_toe_x_t_tx - transition_toe_x_t_tx) > 1.0e-5)
    {
      printf("Warning, original_transition_toe_x_t_tx=%f, transition_toe_x_t_tx=%f, toe_length_P_tlength=%f, linear_slope_P_slope=%f, pivot_x_p_x=%f\n",
                              original_transition_toe_x_t_tx, transition_toe_x_t_tx, toe_length_P_tlength, linear_slope_P_slope, pivot_x_p_x);
    }
    if (fabs(original_transition_toe_y_t_ty - transition_toe_y_t_ty) > 1.0e-5)
    {
      printf("Warning, original_transition_toe_y_t_ty=%f, transition_toe_y_t_ty=%f, toe_length_P_tlength=%f, linear_slope_P_slope=%f, pivot_y_p_y=%f\n",
                              original_transition_toe_y_t_ty, transition_toe_y_t_ty, toe_length_P_tlength, linear_slope_P_slope, pivot_y_p_y);
    }

    // shoulder
    float shoulder_length_P_slength = p->sigmoid_shoulder_length;
    float shoulder_x_from_pivot_x = _dx_from_hypotenuse_and_slope(shoulder_length_P_slength, linear_slope_P_slope);
    float transition_shoulder_x_s_tx = pivot_x_p_x - shoulder_x_from_pivot_x;
    float shoulder_y_from_pivot_y = linear_slope_P_slope * shoulder_x_from_pivot_x;
    float transition_shoulder_y_s_ty = pivot_y_p_y - shoulder_y_from_pivot_y;

    float original_transition_shoulder_x_s_tx = _linear_breakpoint(-shoulder_length_P_slength, linear_slope_P_slope, pivot_x_p_x);
    float original_transition_shoulder_y_s_ty = _linear_breakpoint(-linear_slope_P_slope * shoulder_length_P_slength, linear_slope_P_slope, pivot_y_p_y);

    if (fabs(original_transition_shoulder_x_s_tx - transition_shoulder_x_s_tx) > 1.0e-5)
    {
      printf("Warning, original_transition_shoulder_x_s_tx=%f, transition_shoulder_x_s_tx=%f, shoulder_length_P_tlength=%f, linear_slope_P_slope=%f, pivot_x_p_x=%f\n",
                              original_transition_shoulder_x_s_tx, transition_shoulder_x_s_tx, shoulder_length_P_slength, linear_slope_P_slope, pivot_x_p_x);
    }
    if (fabs(original_transition_shoulder_y_s_ty - transition_shoulder_y_s_ty) > 1.0e-5)
    {
      printf("Warning, original_transition_shoulder_y_s_ty=%f, transition_shoulder_y_s_ty=%f, shoulder_length_P_tlength=%f, linear_slope_P_slope=%f, pivot_y_p_y=%f\n",
                              original_transition_shoulder_y_s_ty, transition_shoulder_y_s_ty, shoulder_length_P_slength, linear_slope_P_slope, pivot_y_p_y);
    }


  DT_OMP_FOR()
  for (int j = 0; j < roi_out->height; j++) {
    float *in = ((float *)ivoid) + (size_t)ch * roi_in->width * j;
    float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;

    for (int i = 0; i < roi_out->width; i++) {
      float3 rgb;
      rgb.r = in[0];
      rgb.g = in[1];
      rgb.b = in[2];

      int debug = (i == 0 && j == 0);

      float3 agx_rgb = _agx_tone_mapping(
        rgb,
        p,
        linear_slope_P_slope,
        transition_toe_x_t_tx, transition_toe_y_t_ty,
        transition_shoulder_x_s_tx, transition_shoulder_y_s_ty,
        range_in_ev,
        minEv,
        debug);

      out[0] = agx_rgb.r;
      out[1] = agx_rgb.g;
      out[2] = agx_rgb.b;

      if (ch == 4) {
        out[3] = in[3]; // Copy alpha if it exists
      }

      in += ch;
      out += ch;
      if (debug)
      {
        printf("================== end ==================\n");
      }
    }
  }
}

// Init
void init(dt_iop_module_t *self) {
  dt_iop_default_init(self);
}

// Cleanup
void cleanup(dt_iop_module_t *self) {
  free(self->params);
  self->params = NULL;
  free(self->default_params);
  self->default_params = NULL;
}

// GUI changed
void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous) {}

// GUI update
void gui_update(dt_iop_module_t *self) {
  dt_iop_agx_gui_data_t *g = self->gui_data;
  // dt_iop_agx_params_t *p = self->params;

  // No combobox anymore
  // Update the combobox
  // gtk_combo_box_set_active(GTK_COMBO_BOX(g->look), p->look);
}

void init_presets(dt_iop_module_so_t *self) {
  dt_iop_agx_params_t p = {0};

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

  dt_gui_presets_add_generic(_("None"), self->op, self->version(), &p,
                             sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // Punchy preset
  p.slope = 1.0f; // Slope was the same for all channels in Punchy
  p.power = 1.35f; // Power was the same for all channels in Punchy
  p.offset = 0.0f;
  p.sat = 1.4f;
  dt_gui_presets_add_generic(_("Punchy"), self->op, self->version(), &p,
                             sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);
}

// GUI cleanup
void gui_cleanup(dt_iop_module_t *self) {}
