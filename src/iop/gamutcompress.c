#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/iop_profile.h"
#include "common/matrices.h"
#include "control/control.h"
#include "develop/develop.h"
#include "iop/iop_api.h"
#include <gtk/gtk.h>
#include <math.h>   // For math functions
#include <stdlib.h>
#include "common/math.h"
#include "gui/draw.h"
#include "develop/imageop_gui.h"
#include "common/dttypes.h"

typedef enum dt_iop_gamut_compression_target_primaries_t
{
  // NOTE: Keep Export Profile first to make it the default (index 0)
  DT_GAMUT_COMPRESSION_EXPORT_PROFILE = 0, // $DESCRIPTION: "export profile"
  DT_GAMUT_COMPRESSION_WORK_PROFILE = 1,   // $DESCRIPTION: "working profile"
  DT_GAMUT_COMPRESSION_REC2020 = 2,        // $DESCRIPTION: "Rec2020"
  DT_GAMUT_COMPRESSION_DISPLAY_P3 = 3,     // $DESCRIPTION: "Display P3"
  DT_GAMUT_COMPRESSION_ADOBE_RGB = 4,      // $DESCRIPTION: "Adobe RGB (compatible)"
  DT_GAMUT_COMPRESSION_SRGB = 5,           // $DESCRIPTION: "sRGB"
} dt_iop_gamutcompress_target_primaries_t;

// Module parameters struct
typedef struct dt_iop_gamutcompress_params_t
{
  dt_iop_gamutcompress_target_primaries_t target_primaries; // $DEFAULT: DT_GAMUT_COMPRESSION_EXPORT_PROFILE $DESCRIPTION: "target color space"
  float gamut_compression_threshold_r;    // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.2 $DESCRIPTION: "red compression target"
  float gamut_compression_threshold_g;    // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.2 $DESCRIPTION: "green compression target"
  float gamut_compression_threshold_b;    // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.2 $DESCRIPTION: "blue compression target"
  float gamut_compression_distance_limit_c; // $MIN: 1.0 $MAX: 100.0 $DEFAULT: 1.0 $DESCRIPTION: "max cyan oversaturation"
  float gamut_compression_distance_limit_m; // $MIN: 1.0 $MAX: 100.0 $DEFAULT: 1.0 $DESCRIPTION: "max magenta oversaturation"
  float gamut_compression_distance_limit_y; // $MIN: 1.0 $MAX: 100.0 $DEFAULT: 1.0 $DESCRIPTION: "max yellow oversaturation"
  gboolean highlight_negative;  // $DEFAULT: FALSE $DESCRIPTION: "highlight negative components"

} dt_iop_gamutcompress_params_t;

typedef struct dt_iop_gamutcompress_gui_data_t
{
  float max_distances[3];
  GtkToggleButton *highlight_negative;
  GtkWidget *distance_limit_c;
  GtkWidget *distance_limit_m;
  GtkWidget *distance_limit_y;

} dt_iop_gamutcompress_gui_data_t;

typedef struct dt_iop_gamutcompress_data_t
{
  dt_iop_gamutcompress_target_primaries_t target_primaries;
  float gamut_compression_threshold_r;
  float gamut_compression_threshold_g;
  float gamut_compression_threshold_b;
  float gamut_compression_distance_limit_c;
  float gamut_compression_distance_limit_m;
  float gamut_compression_distance_limit_y;
  gboolean highlight_negative;
} dt_iop_gamutcompress_data_t;

// Module introspection version
DT_MODULE_INTROSPECTION(1, dt_iop_gamutcompress_params_t)

// Translatable name
const char *name()
{
  return _("gamut compression");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("Applies gamut compression to handle out-of-gamut colors within a target color space."),
                                _("corrective"), _("linear, RGB, scene-referred"), _("linear, RGB, scene-referred"),
                                _("linear, RGB, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_TECHNICAL;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_gamutcompress_params_t *p = (dt_iop_gamutcompress_params_t *)params;
  dt_iop_gamutcompress_data_t *d = piece->data;
  
  d->target_primaries = p->target_primaries;
  d->gamut_compression_threshold_r = p->gamut_compression_threshold_r;
  d->gamut_compression_threshold_g = p->gamut_compression_threshold_g;
  d->gamut_compression_threshold_b = p->gamut_compression_threshold_b;
  d->gamut_compression_distance_limit_c = p->gamut_compression_distance_limit_c;
  d->gamut_compression_distance_limit_m = p->gamut_compression_distance_limit_m;
  d->gamut_compression_distance_limit_y = p->gamut_compression_distance_limit_y;
  d->highlight_negative = p->highlight_negative;
}

static inline dt_colorspaces_color_profile_type_t _get_base_profile_type_from_enum(const dt_iop_gamutcompress_target_primaries_t base_primaries_enum)
{
  switch(base_primaries_enum)
  {
    case DT_GAMUT_COMPRESSION_SRGB:
      return DT_COLORSPACE_SRGB;
    case DT_GAMUT_COMPRESSION_DISPLAY_P3:
      return DT_COLORSPACE_DISPLAY_P3;
    case DT_GAMUT_COMPRESSION_ADOBE_RGB:
      return DT_COLORSPACE_ADOBERGB;
    case DT_GAMUT_COMPRESSION_REC2020: // Fall through
    default:
      return DT_COLORSPACE_LIN_REC2020; // Default/fallback
  }
}

// Get the profile info struct based on the user selection
static const dt_iop_order_iccprofile_info_t *_get_target_profile(dt_develop_t *dev,
                                                                   const dt_iop_order_iccprofile_info_t *
                                                                   pipe_work_profile,
                                                                   const dt_iop_gamutcompress_target_primaries_t
                                                                   base_primaries_selection)
{
  dt_iop_order_iccprofile_info_t *selected_profile_info = NULL;

  switch(base_primaries_selection)
  {
    case DT_GAMUT_COMPRESSION_EXPORT_PROFILE:
    {
      dt_colorspaces_color_profile_type_t profile_type;
      const char *profile_filename;

      dt_ioppr_get_export_profile_type(dev, &profile_type, &profile_filename);

      if(profile_type != DT_COLORSPACE_NONE && profile_filename != NULL)
      {
        // intent does not matter, we just need the primaries
        selected_profile_info =
            dt_ioppr_add_profile_info_to_list(dev, profile_type, profile_filename, INTENT_PERCEPTUAL);
        if(!selected_profile_info || !dt_is_valid_colormatrix(selected_profile_info->matrix_in_transposed[0][0]))
        {
          dt_print(DT_DEBUG_PIPE, "[gamutcompress] Export profile '%s' unusable or missing matrix, falling back to Rec2020.",
                   dt_colorspaces_get_name(profile_type, profile_filename));
          selected_profile_info = NULL; // Force fallback
        }
      }
      else
      {
        dt_print(DT_DEBUG_ALWAYS,
                 "[gamutcompress] Failed to get configured export profile settings, falling back to Rec2020.");
        // fallback handled below
      }
    }
    break;

    case DT_GAMUT_COMPRESSION_WORK_PROFILE:
      return pipe_work_profile;

    case DT_GAMUT_COMPRESSION_REC2020:
    case DT_GAMUT_COMPRESSION_DISPLAY_P3:
    case DT_GAMUT_COMPRESSION_ADOBE_RGB:
    case DT_GAMUT_COMPRESSION_SRGB:
    {
      const dt_colorspaces_color_profile_type_t profile_type =
          _get_base_profile_type_from_enum(base_primaries_selection);
      // Use relative intent for standard profiles when used as base
      selected_profile_info =
          dt_ioppr_add_profile_info_to_list(dev, profile_type, "", DT_INTENT_RELATIVE_COLORIMETRIC);
      if(!selected_profile_info || !dt_is_valid_colormatrix(selected_profile_info->matrix_in_transposed[0][0]))
      {
        dt_print(DT_DEBUG_PIPE,
                 "[gamutcompress] Standard base profile '%s' unusable or missing matrix, falling back to Rec2020.",
                 dt_colorspaces_get_name(profile_type, ""));
        selected_profile_info = NULL; // Force fallback
      }
    }
    break;
  }

  // fallback: selected profile not found or invalid
  if(!selected_profile_info)
  {
    selected_profile_info =
        dt_ioppr_add_profile_info_to_list(dev, DT_COLORSPACE_LIN_REC2020, "", DT_INTENT_RELATIVE_COLORIMETRIC);
    // if even Rec2020 fails, something is very wrong, but let the caller handle NULL if necessary.
    if(!selected_profile_info)
      dt_print(DT_DEBUG_ALWAYS, "[comutcompress] CRITICAL: Failed to get even Rec2020 base profile info.");
  }

  return selected_profile_info;
}

static inline void _highlight_negative(float *const out,
                                       const size_t n_pixels,
                                       const gboolean pipe_target_profile_same,
                                       const dt_colormatrix_t pipe_to_target_transposed,
                                       const dt_colormatrix_t target_to_pipe_transposed)
{
  DT_OMP_FOR_SIMD()
  for(size_t k = 0; k < 4 * n_pixels; k += 4)
  {
    float *const restrict pix_out = out + k;
    dt_aligned_pixel_t target_RGB;
    if(pipe_target_profile_same)
    {
      copy_pixel(target_RGB, pix_out);
    }
    else
    {
      dt_apply_transposed_color_matrix(pix_out, pipe_to_target_transposed, target_RGB);
    }

    dt_aligned_pixel_t tmp;
    copy_pixel(tmp, target_RGB);

    target_RGB[0] = (target_RGB[0] < 0);
    target_RGB[1] = (target_RGB[1] < 0);
    target_RGB[2] = (target_RGB[2] < 0);
/*
    if (tmp[0] < 0)
    {
      target_RGB[0] = 1;
      target_RGB[1] = 0;
      target_RGB[2] = 0;
    }
    if (tmp[1] < 0)
    {
      target_RGB[1] = 1;
      if (tmp[0] > 0)
      {
        target_RGB[0] = 0;
      }
      if (tmp[2] > 0)
      {
        target_RGB[2] = 0;
      }
    }
    if (tmp[2] < 0)
    {
      target_RGB[2] = 1;
      if (tmp[0] > 0)
      {
        target_RGB[0] = 0;
      }
      if (tmp[1] > 0)
      {
        target_RGB[1] = 0;
      }
    }
    */

    if(pipe_target_profile_same)
    {
      copy_pixel(pix_out, target_RGB);
    }
    else
    {
      dt_apply_transposed_color_matrix(target_RGB, target_to_pipe_transposed, pix_out);
    }
  }
}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4, self, piece->colors, ivoid, ovoid, roi_in, roi_out))
  {
    return;
  }

  const dt_iop_gamutcompress_params_t *p = piece->data;
  dt_iop_gamutcompress_gui_data_t *g = self->gui_data;
  const float *const in = ivoid;
  float *const out = ovoid;
  const size_t n_pixels = (size_t)roi_in->width * roi_in->height;

  const dt_iop_order_iccprofile_info_t *const pipe_work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  const dt_iop_order_iccprofile_info_t *const target_profile =
      _get_target_profile(self->dev, pipe_work_profile, p->target_primaries);

  if(!target_profile)
  {
    dt_print(DT_DEBUG_ALWAYS, "[gamut compression process] Failed to obtain a valid target profile. Cannot proceed.");
    if(in != out) memcpy(out, in, n_pixels * 4 * sizeof(float));
    return;
  }

  const gboolean pipe_target_profile_same = (pipe_work_profile == target_profile);

  dt_colormatrix_t pipe_to_target_transposed;
  dt_colormatrix_t target_to_pipe_transposed;

  if(!pipe_target_profile_same)
  {
    dt_colormatrix_mul(pipe_to_target_transposed, pipe_work_profile->matrix_in_transposed, target_profile->matrix_out_transposed);
    mat3SSEinv(target_to_pipe_transposed, pipe_to_target_transposed);
  }

  dt_aligned_pixel_t thresholds;
  thresholds[0] = p->gamut_compression_threshold_r;
  thresholds[1] = p->gamut_compression_threshold_g;
  thresholds[2] = p->gamut_compression_threshold_b;

  dt_aligned_pixel_t distance_limit;
  distance_limit[0] = p->gamut_compression_distance_limit_c;
  distance_limit[1] = p->gamut_compression_distance_limit_m;
  distance_limit[2] = p->gamut_compression_distance_limit_y;

  // Local array to find the maximums in a thread-safe way. We're not interested in values less than 1.
  dt_aligned_pixel_t max_dist = {1.0f};

  DT_OMP_FOR_SIMD(reduction(max:max_dist[:3]))
  for(size_t k = 0; k < 4 * n_pixels; k += 4)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;

    dt_aligned_pixel_t target_RGB;

    if(pipe_target_profile_same)
    {
      copy_pixel(target_RGB, pix_in);
    }
    else
    {
      dt_apply_transposed_color_matrix(pix_in, pipe_to_target_transposed, target_RGB);
    }

/*
   // Achromatic axis
    const float achromatic = fmaxf(pixel_in_out[0], fmaxf(pixel_in_out[1], pixel_in_out[2]));

    for_three_channels(k, aligned(pixel_in_out, distance_limit, threshold: 16))
    {
    // compress into the top 20% of gamut
    const float th = 1.0f - threshold[k];
    // Inverse RGB Ratios: distance from achromatic axis
    const float distance_from_achromatic = achromatic == 0.0f ? 0.0f : (achromatic - pixel_in_out[k]) / fabs(achromatic);
    // Calculate scale so compression function passes through distance limit: (x=dl, y=1)
    const float scale = (1.0f - th) / sqrtf(fmaxf(1.001f, distance_limit[k]) - 1.0f);
    // Parabolic compression function: https://www.desmos.com/calculator/nvhp63hmtj
    const float compressed_distance = distance_from_achromatic < th ? distance_from_achromatic :
    scale * sqrtf(distance_from_achromatic - th + scale * scale /4.0f) - scale * sqrtf(scale * scale /4.0f) + th;
    // Inverse RGB Ratios to RGB
    pixel_in_out[k] = achromatic - compressed_distance * fabsf(achromatic);
    }
    */

    // Jed Smith
    // Achromatic axis
    const float achromatic = max3f(target_RGB);
    const float achromatic_abs = fabsf(achromatic);

    for_three_channels(chan, aligned(target_RGB, distance_limit, thresholds : 16))
    {
      // e.g. 0.1 -> 10% at the top of the gamut
      const float threshold = thresholds[chan];

      // Amount of outer gamut to affect
      const float th = 1.0f - threshold;

      // Inverse RGB Ratio: distance from achromatic axis
      const float distance_from_achromatic = achromatic == 0.0f ? 0.0f : (achromatic - target_RGB[chan]) / achromatic_abs;

      // Update max distance for the current channel for debug output on the UI (not part of the compression algo), but ignore dark areas
      if (achromatic_abs > 0.1f)
      {
        max_dist[chan] = fmaxf(max_dist[chan], distance_from_achromatic);
      }

      // Calculate scale so compression function passes through distance limit: (x=dl, y=1)
      const float scale = (1.0f - th) / sqrtf(fmaxf(1.001f, distance_limit[chan]) - 1.0f);
      const float compressed_distance =
          distance_from_achromatic < th
              ? distance_from_achromatic
              // Parabolic compression function: https://www.desmos.com/calculator/nvhp63hmtj
              : scale * sqrtf(distance_from_achromatic - th + scale * scale / 4.0f) - scale * sqrtf(scale * scale / 4.0f) + th;
      target_RGB[chan] = achromatic - compressed_distance * achromatic_abs;
    }

    if(pipe_target_profile_same)
    {
      copy_pixel(pix_out, target_RGB);
    }
    else
    {
      dt_apply_transposed_color_matrix(target_RGB, target_to_pipe_transposed, pix_out);
    }

    pix_out[3] = pix_in[3];
  }

  // add a tiny safety margin
  for_three_channels(c, aligned(max_dist: 16))
  {
    max_dist[c] = max_dist[c] > 1.f ? max_dist[c] + 0.01 : 1;
  }

  if(g != NULL && self->dev->gui_attached && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))
  {
    memcpy(g->max_distances, max_dist, sizeof(max_dist));
    gtk_widget_queue_draw(self->widget);
  }

  if(g != NULL && self->dev->gui_attached && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL) && p->highlight_negative)
  {
    _highlight_negative(out, n_pixels, pipe_target_profile_same, pipe_to_target_transposed, target_to_pipe_transposed);
  }
}

static gboolean draw(GtkWidget *widget, cairo_t *cr, dt_iop_module_t *self)
{
  dt_iop_gamutcompress_gui_data_t *g = self->gui_data;
  if(darktable.gui->reset) return FALSE;

  if(g->max_distances[0] < 0) return FALSE;

  printf(gettext("oversaturation: %f, %f, %f\n"), g->max_distances[0], g->max_distances[1], g->max_distances[2]);

  return FALSE;
}

static void auto_adjust_distance_limit_c(GtkWidget *quad, dt_iop_module_t *self)
{
  dt_iop_gamutcompress_params_t *p = self->params;
  dt_iop_gamutcompress_gui_data_t *g = self->gui_data;

  if(g->max_distances[0] < 1.0f)
  {
    dt_control_log(_("oversaturation not yet calculated"));
    return;
  }

  p->gamut_compression_distance_limit_c = g->max_distances[0];

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->distance_limit_c, p->gamut_compression_distance_limit_c);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void auto_adjust_distance_limit_m(GtkWidget *quad, dt_iop_module_t *self)
{
  dt_iop_gamutcompress_params_t *p = self->params;
  dt_iop_gamutcompress_gui_data_t *g = self->gui_data;

  if(g->max_distances[1] < 1.0f)
  {
    dt_control_log(_("oversaturation not yet calculated"));
    return;
  }

  p->gamut_compression_distance_limit_m = g->max_distances[1];

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->distance_limit_m, p->gamut_compression_distance_limit_m);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void auto_adjust_distance_limit_y(GtkWidget *quad, dt_iop_module_t *self)
{
  dt_iop_gamutcompress_params_t *p = self->params;
  dt_iop_gamutcompress_gui_data_t *g = self->gui_data;

  if(g->max_distances[2] < 1.0f)
  {
    dt_control_log(_("oversaturation not yet calculated"));
    return;
  }

  p->gamut_compression_distance_limit_y = g->max_distances[2];

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->distance_limit_y, p->gamut_compression_distance_limit_y);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void gui_update(dt_iop_module_t *self)
{
  dt_iop_gamutcompress_gui_data_t *g = self->gui_data;
  dt_iop_gamutcompress_params_t *p = self->params;
  gtk_toggle_button_set_active(g->highlight_negative, p->highlight_negative);
}

void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc1_align_type(dt_iop_gamutcompress_data_t);
}

void cleanup(dt_iop_module_t *self)
{
  dt_iop_default_cleanup(self);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}


void gui_init(dt_iop_module_t *self)
{
  dt_iop_gamutcompress_gui_data_t *g = IOP_GUI_ALLOC(gamutcompress);

  // self->gui_data = NULL; // No custom gui data needed
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  g_signal_connect(G_OBJECT(self->widget), "draw", G_CALLBACK(draw), self);

  GtkWidget *target_primaries_combo = dt_bauhaus_combobox_from_params(self, "target_primaries");
  gtk_widget_set_tooltip_text(target_primaries_combo, _("Color space to perform gamut compression in.\n"
                                                        "'export profile' uses the profile set in 'output color profile'."));

  // Reuse the slider variable for all sliders
  GtkWidget *slider;

  g->distance_limit_c = dt_bauhaus_slider_from_params(self, "gamut_compression_distance_limit_c");
  dt_bauhaus_slider_set_soft_range(g->distance_limit_c, 1.0f, 2.0f);
  gtk_widget_set_tooltip_text(g->distance_limit_c, _("maximum cyan oversaturation to correct"));
  dt_bauhaus_widget_set_quad(g->distance_limit_c, self, dtgtk_cairo_paint_wand, FALSE, auto_adjust_distance_limit_c,
                             _("set to max detected cyan oversaturation"));

  slider = dt_bauhaus_slider_from_params(self, "gamut_compression_threshold_r");
  dt_bauhaus_slider_set_soft_range(slider, 0.1f, 0.5f);
  gtk_widget_set_tooltip_text(slider, _("portion of reds to receive cyan overflow"));

  g->distance_limit_m = dt_bauhaus_slider_from_params(self, "gamut_compression_distance_limit_m");
  dt_bauhaus_slider_set_soft_range(g->distance_limit_m, 1.0f, 2.0f);
  gtk_widget_set_tooltip_text(g->distance_limit_m, _("maximum magenta oversaturation to correct"));
  dt_bauhaus_widget_set_quad(g->distance_limit_m, self, dtgtk_cairo_paint_wand, FALSE, auto_adjust_distance_limit_m,
                             _("set to max detected magenta oversaturation"));

  slider = dt_bauhaus_slider_from_params(self, "gamut_compression_threshold_g");
  dt_bauhaus_slider_set_soft_range(slider, 0.1f, 0.5f);
  gtk_widget_set_tooltip_text(slider, _("portion of greens to receive magenta overflow"));

  g->distance_limit_y = dt_bauhaus_slider_from_params(self, "gamut_compression_distance_limit_y");
  dt_bauhaus_slider_set_soft_range(g->distance_limit_y, 1.0f, 2.0f);
  gtk_widget_set_tooltip_text(g->distance_limit_y, _("maximum yellow oversaturation to correct"));
  dt_bauhaus_widget_set_quad(g->distance_limit_y, self, dtgtk_cairo_paint_wand, FALSE, auto_adjust_distance_limit_y,
                             _("set to max detected yellow oversaturation"));

  slider = dt_bauhaus_slider_from_params(self, "gamut_compression_threshold_b");
  dt_bauhaus_slider_set_soft_range(slider, 0.1f, 0.5f);
  gtk_widget_set_tooltip_text(slider, _("portion of blues to receive compressed yellow overflow"));

  g->highlight_negative = GTK_TOGGLE_BUTTON(dt_bauhaus_toggle_from_params(self, "highlight_negative"));

  gui_update(self);
}
