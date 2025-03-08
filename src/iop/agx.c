#include "bauhaus/bauhaus.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>
#include <math.h> // For math functions

// Silence the compiler during dev of new module
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"

// Module introspection version
DT_MODULE_INTROSPECTION(1, dt_iop_agx_params_t)

// Module parameters struct
typedef struct dt_iop_agx_params_t {
  float slope;   // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "Slope"
  float power;   // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "Power"
  float offset;  // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "Offset"
  float sat;     // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 1.0 $DESCRIPTION: "Saturation"
} dt_iop_agx_params_t;

typedef struct dt_iop_agx_gui_data_t {
  // No combobox anymore
  // GtkWidget *look; // ComboBox for selecting AgX look
} dt_iop_agx_gui_data_t;

// Global data struct (not needed for this simple example)
typedef struct dt_iop_agx_global_data_t {} dt_iop_agx_global_data_t;

// Structs for vector and matrix math (pure C)
typedef struct {
  float r, g, b;
} float3;

typedef struct {
    float m[3][3];
} mat3f;

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

// Helper function: matrix multiplication
float3 mat3f_mul_float3(const mat3f m, const float3 v) {
    float3 result;
    result.r = m.m[0][0] * v.r + m.m[0][1] * v.g + m.m[0][2] * v.b;
    result.g = m.m[1][0] * v.r + m.m[1][1] * v.g + m.m[1][2] * v.b;
    result.b = m.m[2][0] * v.r + m.m[2][1] * v.g + m.m[2][2] * v.b;
    return result;
}

// Helper function: pow function
float3 powf3(float3 base, float3 exponent) {
  float3 result;
  result.r = powf(base.r, exponent.r);
  result.g = powf(base.g, exponent.g);
  result.b = powf(base.b, exponent.b);
  return result;
}


// Helper function: Determinant of a 3x3 matrix
float determinant(const mat3f m) {
    return m.m[0][0] * (m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1])
           - m.m[0][1] * (m.m[1][0] * m.m[2][2] - m.m[1][2] * m.m[2][0])
           + m.m[0][2] * (m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0]);
}

//AgX implementation
// These matrices taken from Blender's implementation of AgX, which works with Rec.2020 primaries.
// https://github.com/EaryChow/AgX_LUT_Gen/blob/main/AgXBaseRec2020.py

const mat3f AgXInsetMatrix = {
	{{0.856627153315983f, 0.0951212405381588f, 0.0482516061458583f},
	{0.137318972929847f, 0.761241990602591f, 0.101439036467562f},
	{0.11189821299995f, 0.0767994186031903f, 0.811302368396859f}}
};

const mat3f AgXOutsetMatrix = {
	{{1.1271005818144368f, -0.11060664309660323f, -0.016493938717834573f},
	{-0.1413297634984383f, 1.157823702216272f, -0.016493938717834257f},
	{-0.14132976349843826f, -0.11060664309660294f, 1.2519364065950405f}}
};

// LOG2_MIN      = -10.0
// LOG2_MAX      =  +6.5
// MIDDLE_GRAY   =  0.18
const float AgxMinEv = -12.47393f;      // log2(pow(2, LOG2_MIN) * MIDDLE_GRAY)
const float AgxMaxEv = 4.026069f;       // log2(pow(2, LOG2_MAX) * MIDDLE_GRAY)

// Adapted from https://iolite-engine.com/blog_posts/minimal_agx_implementation
float3 agxDefaultContrastApprox(float3 x) {
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

// Adapted from https://iolite-engine.com/blog_posts/minimal_agx_implementation
float3 agxLook(float3 val, const dt_iop_agx_params_t *p) {

    const float lw[] = {0.2126f, 0.7152f, 0.0722f};
    float luma = lw[0] * val.r + lw[1] * val.g + lw[2] * val.b;

    // Default
    float slope = p->slope;
    float3 power = {p->power, p->power, p->power};
    float offset = p->offset;
    float sat = p->sat;

    // ASC CDL
    float3 pow_val = powf3((float3){fmaxf(0.0f, val.r + offset) * slope, fmaxf(0.0f, val.g + offset) * slope, fmaxf(0.0f, val.b + offset) * slope}, power);

    float3 result;
    result.r = luma + sat * (pow_val.r - luma);
    result.g = luma + sat * (pow_val.g - luma);
    result.b = luma + sat * (pow_val.b - luma);
    return result;
}

float3 agx_tone_mapping(float3 rgb, const dt_iop_agx_params_t *p) {
    // Ensure no negative values
    float3 v = {fmaxf(0.0f, rgb.r), fmaxf(0.0f, rgb.g), fmaxf(0.0f, rgb.b)};

    // Apply Inset Matrix
    v = mat3f_mul_float3(AgXInsetMatrix, v);

    // Log2 encoding
    float small_value = 1E-10f;
    v.r = fmaxf(v.r, small_value);
    v.g = fmaxf(v.g, small_value);
    v.b = fmaxf(v.b, small_value);

    v.r = log2f(v.r);
    v.g = log2f(v.g);
    v.b = log2f(v.b);

    v.r = (v.r - AgxMinEv) / (AgxMaxEv - AgxMinEv);
    v.g = (v.g - AgxMinEv) / (AgxMaxEv - AgxMinEv);
    v.b = (v.b - AgxMinEv) / (AgxMaxEv - AgxMinEv);

    v.r = fminf(fmaxf(v.r, 0.0f), 1.0f);
    v.g = fminf(fmaxf(v.g, 0.0f), 1.0f);
    v.b = fminf(fmaxf(v.b, 0.0f), 1.0f);

    // Apply sigmoid
    v = agxDefaultContrastApprox(v);

    // Apply AgX look
    v = agxLook(v, p);

    // Apply Outset Matrix
    v = mat3f_mul_float3(AgXOutsetMatrix, v);

    // Linearize
    v.r = powf(fmaxf(0.0f, v.r), 2.2f);
    v.g = powf(fmaxf(0.0f, v.g), 2.2f);
    v.b = powf(fmaxf(0.0f, v.b), 2.2f);

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

  DT_OMP_FOR()
  for (int j = 0; j < roi_out->height; j++) {
    float *in = ((float *)ivoid) + (size_t)ch * roi_in->width * j;
    float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;

    for (int i = 0; i < roi_out->width; i++) {
      float3 rgb;
      rgb.r = in[0];
      rgb.g = in[1];
      rgb.b = in[2];

      float3 agx_rgb = agx_tone_mapping(rgb, p);

      out[0] = agx_rgb.r;
      out[1] = agx_rgb.g;
      out[2] = agx_rgb.b;

      if (ch == 4) {
        out[3] = in[3]; // Copy alpha if it exists
      }

      in += ch;
      out += ch;
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

// GUI init
void gui_init(dt_iop_module_t *self) {
  dt_iop_agx_gui_data_t *g = IOP_GUI_ALLOC(agx);

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // Create sliders for slope, power, saturation
  GtkWidget *slider;
  slider = dt_bauhaus_slider_from_params(self, "slope");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 2.0f);
  slider = dt_bauhaus_slider_from_params(self, "power");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 2.0f);
  slider = dt_bauhaus_slider_from_params(self, "offset");
  dt_bauhaus_slider_set_soft_range(slider, -1.0f, 1.0f);
  slider = dt_bauhaus_slider_from_params(self, "sat");
  dt_bauhaus_slider_set_soft_range(slider, 0.0f, 2.0f);
}

// GUI cleanup
void gui_cleanup(dt_iop_module_t *self) {}
