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
#include <stdio.h>
#include <math.h>
#include <stdbool.h>

// cross product of start_point -> end_point_a and start_point -> end_point_b, used to find the orientation of three points
float cross_product(const float* const start_point, const float* const end_point_a, const float* const end_point_b) {
  return (end_point_a[0] - start_point[0]) * (end_point_b[1] - start_point[1]) - (end_point_a[1] - start_point[1]) * (end_point_b[0] - start_point[0]);
}

float distance_sq(const float* const p1, const float* const p2) {
  const float dx = p1[0] - p2[0];
  const float dy = p1[1] - p2[1];
  return dx * dx + dy * dy;
}

bool line_segment_intersection(const float* const p1, const float* const p2, const float* const p3, const float* const p4, float* const intersection) {
  const float det = (p2[0] - p1[0]) * (p4[1] - p3[1]) - (p2[1] - p1[1]) * (p4[0] - p3[0]);
  if (det < 1e-5f) {
    return false; // Lines are parallel
  }

  const float t = ((p3[0] - p1[0]) * (p4[1] - p3[1]) - (p3[1] - p1[1]) * (p4[0] - p3[0])) / det;

  intersection[0] = p1[0] + t * (p2[0] - p1[0]);
  intersection[1] = p1[1] + t * (p2[1] - p1[1]);
  return true;
}

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

typedef enum dt_iop_gamut_compression_method_t
{
  // NOTE: Keep Export Profile first to make it the default (index 0)
  DT_GAMUT_COMPRESSION_METHOD_RGB = 0,     // $DESCRIPTION: "RGB"
  DT_GAMUT_COMPRESSION_XYY = 1,            // $DESCRIPTION: "xyY"
} dt_iop_gamut_compression_method_t;

// Module parameters struct
typedef struct dt_iop_gamutcompress_params_t
{
  dt_iop_gamutcompress_target_primaries_t target_primaries; // $DEFAULT: DT_GAMUT_COMPRESSION_EXPORT_PROFILE $DESCRIPTION: "target color space"
  dt_iop_gamut_compression_method_t method; // $DEFAULT: DT_GAMUT_COMPRESSION_XYY $DESCRIPTION: "method"
  float gamut_compression_buffer_r;         // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.2 $DESCRIPTION: "red compression buffer"
  float gamut_compression_buffer_g;         // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.2 $DESCRIPTION: "green compression buffer"
  float gamut_compression_buffer_b;         // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.2 $DESCRIPTION: "blue compression buffer"
  float gamut_compression_distance_limit_c; // $MIN: 1.0 $MAX: 100.0 $DEFAULT: 1.0 $DESCRIPTION: "cyan distance limit"
  float gamut_compression_distance_limit_m; // $MIN: 1.0 $MAX: 100.0 $DEFAULT: 1.0 $DESCRIPTION: "magenta distance limit"
  float gamut_compression_distance_limit_y; // $MIN: 1.0 $MAX: 100.0 $DEFAULT: 1.0 $DESCRIPTION: "yellow distance limit"
  float gamut_compression_start_xy;         // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.9 $DESCRIPTION: "xyY compression start"
  float gamut_compression_end_xy;           // $MIN: 1.0 $MAX: 2.0 $DEFAULT: 2.0 $DESCRIPTION: "xyY compression end"
  float preserve_hue;                       // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.2 $DESCRIPTION: "preserve JzAzBz hue"
  gboolean highlight_negative;              // $DEFAULT: FALSE $DESCRIPTION: "highlight negative components"

} dt_iop_gamutcompress_params_t;

typedef struct dt_iop_gamutcompress_gui_data_t
{
  float max_distances[3];
  float max_xy_dist_ratio;
  GtkToggleButton *highlight_negative;
  GtkWidget *distance_limit_c;
  GtkWidget *distance_limit_m;
  GtkWidget *distance_limit_y;
  GtkWidget *gamut_compression_end_xy;
  GtkWidget *gamut_compression_start_xy;

} dt_iop_gamutcompress_gui_data_t;

typedef struct dt_iop_gamutcompress_data_t
{
  dt_iop_gamutcompress_target_primaries_t target_primaries;
  dt_iop_gamut_compression_method_t method; // $DEFAULT: DT_GAMUT_COMPRESSION_XYZ $DESCRIPTION: "method"
  float gamut_compression_buffer_r;
  float gamut_compression_buffer_g;
  float gamut_compression_buffer_b;
  float gamut_compression_distance_limit_c;
  float gamut_compression_distance_limit_m;
  float gamut_compression_distance_limit_y;
  float gamut_compression_start_xy;
  float gamut_compression_end_xy;
  float preserve_hue;
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
  d->gamut_compression_buffer_r = p->gamut_compression_buffer_r;
  d->gamut_compression_buffer_g = p->gamut_compression_buffer_g;
  d->gamut_compression_buffer_b = p->gamut_compression_buffer_b;
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

typedef struct
{
  dt_iop_module_t *self;
  float max_distances[3];
  float max_xy_dist_ratio;
} gamutcompress_update_gui_t;

static gboolean _update_gui_from_worker(gpointer data)
{
  gamutcompress_update_gui_t *msg = data;
  dt_iop_module_t *self = msg->self;
  dt_iop_gamutcompress_gui_data_t *g = self->gui_data;

  if(g)
  {
    memcpy(g->max_distances, msg->max_distances, sizeof(g->max_distances));
    g->max_xy_dist_ratio = msg->max_xy_dist_ratio;
    gtk_widget_queue_draw(self->widget);
  }

  g_free(msg);
  return G_SOURCE_REMOVE;
}
/*
void process_jed(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
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

  dt_aligned_pixel_t buffers;
  buffers[0] = p->gamut_compression_buffer_r;
  buffers[1] = p->gamut_compression_buffer_g;
  buffers[2] = p->gamut_compression_buffer_b;

  dt_aligned_pixel_t thresholds;
  thresholds[0] = 1.0f - buffers[0];
  thresholds[1] = 1.0f - buffers[1];
  thresholds[2] = 1.0f - buffers[2];


  dt_aligned_pixel_t distance_limit;
  distance_limit[0] = p->gamut_compression_distance_limit_c;
  distance_limit[1] = p->gamut_compression_distance_limit_m;
  distance_limit[2] = p->gamut_compression_distance_limit_y;

  dt_aligned_pixel_t scales;
  dt_aligned_pixel_t params1;
  dt_aligned_pixel_t params2;
  for (int chan = 0; chan < 3; ++chan)
  {
    scales[chan] = buffers[chan] / sqrtf(fmaxf(1.001f, distance_limit[chan]) - 1.0f);
    const float scale_squared_per_4 = scales[chan] * scales[chan] / 4.0f;
    params1[chan] = - thresholds[chan] + scale_squared_per_4;
    params2[chan] = fabsf(scales[chan]) / 2.f;
  }

  gboolean highlight_negative = (g != NULL && self->dev->gui_attached && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL) && p->highlight_negative);

  // We're not interested in values less than 1.
  float max_dist[3] = {1.0f};

  // DT_OMP_FOR_SIMD(reduction(max:max_dist[:3]))
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

    // algo from Jed Smith: https://github.com/jedypod/gamut-compress

    const float achromatic = max3f(target_RGB);
    const float achromatic_abs = fabsf(achromatic);

    //for_three_channels(chan, aligned(target_RGB, thresholds, scales, params1, params2 : 16))
    for (int chan = 0; chan < 3; ++chan)
    {
      // values below this will not be compressed
      const float threshold = thresholds[chan];

      // Inverse RGB Ratio: distance from achromatic axis; a saturation-like measure
      const float distance_from_achromatic = achromatic == 0.0f ? 0.0f : (achromatic - target_RGB[chan]) / achromatic_abs;

      // The values collected here are used by the pickers to set the distance limit;
      // we ignore dark areas because compression has no visible effect there and
      // they often produce very large distances
      if (achromatic_abs > 0.01f)
      {
        max_dist[chan] = fmaxf(max_dist[chan], distance_from_achromatic);
      }

      if(distance_from_achromatic >= threshold)
      {
        // Calculate scale so compression function passes through distance limit: (x=distance_limit, y=1)
        // in the original formula, '1 - threshold' is used instead of 'buffer', but the two are equivalent
        const float scale = scales[chan];
        const float param1 = params1[chan];
        const float param2 = params2[chan];
        // Parabolic compression function: https://www.desmos.com/calculator/nvhp63hmtj
        const float compressed_distance = scale * (sqrtf(distance_from_achromatic + param1) - param2) + threshold;
        dt_aligned_pixel_t before;
        copy_pixel(before, target_RGB);
        target_RGB[chan] = achromatic - compressed_distance * achromatic_abs;
        // printf("in (pipe): (%f, %f, %f), in (target): (%f, %f, %f), out (target): (%f, %f, %f)\n",
        //        pix_in[0], pix_in[1], pix_in[2],
        //        before[0], before[1], before[2],
        //        target_RGB[0], target_RGB[1], target_RGB[2]
        //        );
      }
    }

    if (highlight_negative)
    {
      target_RGB[0] = (target_RGB[0] < 0);
      target_RGB[1] = (target_RGB[1] < 0);
      target_RGB[2] = (target_RGB[2] < 0);
    }
    else
    {
      // clip whatever negative remains
      target_RGB[0] = fmaxf(target_RGB[0], 0);
      target_RGB[1] = fmaxf(target_RGB[1], 0);
      target_RGB[2] = fmaxf(target_RGB[2], 0);
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
    gamutcompress_update_gui_t *msg = g_malloc(sizeof(gamutcompress_update_gui_t));
    msg->self = self;
    memcpy(msg->max_distances, max_dist, sizeof(msg->max_distances));
    g_idle_add(_update_gui_from_worker, msg);
  }
}
*/

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

  const float whitepoint_x = target_profile->whitepoint[0];
  const float whitepoint_y = target_profile->whitepoint[1];

  const gboolean pipe_target_profile_same = (pipe_work_profile == target_profile);

  dt_colormatrix_t pipe_to_target_transposed;
  dt_colormatrix_t target_to_pipe_transposed;

  if(!pipe_target_profile_same)
  {
    dt_colormatrix_mul(pipe_to_target_transposed, pipe_work_profile->matrix_in_transposed, target_profile->matrix_out_transposed);
    mat3SSEinv(target_to_pipe_transposed, pipe_to_target_transposed);
  }

  const gboolean highlight_negative = (g != NULL && self->dev->gui_attached && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL) && p->highlight_negative);

  // We're not interested in values less than 1.
  float max_dist[3] = {1.0f};
  float max_xy_dist_ratio = 1.f;
  const float buffer = 1 - p->gamut_compression_start_xy;
  const float threshold = p->gamut_compression_start_xy;
  const float distance_limit = p->gamut_compression_end_xy;
  const float scale = buffer / sqrtf(fmaxf(1.001f, distance_limit) - 1.0f);
  const float scale_squared_per_4 = scale * scale / 4.0f;
  const float param1 = - threshold + scale_squared_per_4;
  const float param2 = fabsf(scale) / 2.f;

  // DT_OMP_FOR_SIMD(reduction(max:max_dist[:3]))
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

    dt_aligned_pixel_t XYZ_d50;
    dt_apply_transposed_color_matrix(pix_in, pipe_work_profile->matrix_in_transposed, XYZ_d50);
    dt_aligned_pixel_t xyY;
    dt_D50_XYZ_to_xyY(XYZ_d50, xyY);

    float xy[] = {xyY[0], xyY[1]};

    float intersection[2] = {0.f};

    float distance_sq_to_gamut_boundary = 1e6;
    // float closest_intersection[] = {0.f, 0.f};

    // let's see where achromatic -> out pixel intersects the gamut triangle
    // float distances[3] = {1e6};
    bool intersects = line_segment_intersection(
      target_profile->primaries[0], target_profile->primaries[1],
      target_profile->whitepoint, xy,
      intersection
    );
    if (intersects)
    {
      const float sq_distance = distance_sq(target_profile->whitepoint, intersection);
      // use the cross product to see if the intersection and our point are on the same side of the line from the white point
      if (sq_distance < distance_sq_to_gamut_boundary && cross_product(target_profile->whitepoint, intersection, xy) > 0)
      {
        distance_sq_to_gamut_boundary = sq_distance;
        // closest_intersection[0] = intersection[0];
        // closest_intersection[1] = intersection[1];
      }
    }
    intersects = line_segment_intersection(
      target_profile->primaries[0], target_profile->primaries[2],
      target_profile->whitepoint, xy,
      intersection
    );
    if (intersects)
    {
      const float sq_distance = distance_sq(target_profile->whitepoint, intersection);
      if (sq_distance < distance_sq_to_gamut_boundary && cross_product(target_profile->whitepoint, intersection, xy) > 0)
      {
        distance_sq_to_gamut_boundary = sq_distance;
        // closest_intersection[0] = intersection[0];
        // closest_intersection[1] = intersection[1];
      }
    }
    intersects = line_segment_intersection(
      target_profile->primaries[1], target_profile->primaries[2],
      target_profile->whitepoint, xy,
      intersection
    );
    if (intersects)
    {
      const float sq_distance = distance_sq(target_profile->whitepoint, intersection);
      if (sq_distance < distance_sq_to_gamut_boundary && cross_product(target_profile->whitepoint, intersection, xy) > 0)
      {
        distance_sq_to_gamut_boundary = sq_distance;
        // closest_intersection[0] = intersection[0];
        // closest_intersection[1] = intersection[1];
      }
    }

    const float distance_ratio = distance_sq(target_profile->whitepoint, xy) / distance_sq_to_gamut_boundary;
    if (max_xy_dist_ratio < distance_ratio)
    {
      max_xy_dist_ratio = distance_ratio;
    }

    dt_aligned_pixel_t XYZ_d65;
    dt_XYZ_D50_2_XYZ_D65(XYZ_d50, XYZ_d65);
    dt_aligned_pixel_t JzAzBz;
    dt_XYZ_2_JzAzBz(XYZ_d65, JzAzBz);
    dt_aligned_pixel_t JzCzhz;
    dt_JzAzBz_2_JzCzhz(JzAzBz, JzCzhz);

    const float original_Cz = JzCzhz[2];

    const float diff_x = xyY[0] - whitepoint_x;
    const float diff_y = xyY[1] - whitepoint_y;

    const float compressed_distance = scale * (sqrtf(distance_ratio + param1) - param2) + threshold;

    xyY[0] = whitepoint_x + diff_x * compressed_distance;
    xyY[1] = whitepoint_y + diff_y * compressed_distance;

    dt_xyY_to_XYZ(xyY, XYZ_d50);
    dt_XYZ_D50_2_XYZ_D65(XYZ_d50, XYZ_d65);
    dt_XYZ_2_JzAzBz(XYZ_d65, JzAzBz);
    dt_JzAzBz_2_JzCzhz(JzAzBz, JzCzhz);

    JzCzhz[2] = (1 - p->preserve_hue) * JzCzhz[2] + p->preserve_hue * original_Cz;
    dt_JzCzhz_2_JzAzBz(JzCzhz, JzAzBz);
    dt_JzAzBz_2_XYZ(JzAzBz, XYZ_d65);
    dt_XYZ_D65_2_XYZ_D50(XYZ_d65, XYZ_d50);

    // JzCzhz[1] *= p->gamut_compression_buffer_r;
    // dt_JzCzhz_2_JzAzBz(JzCzhz, JzAzBz);
    // dt_JzAzBz_2_XYZ(JzAzBz, XYZ_d65);
    // dt_XYZ_D65_2_XYZ_D50(XYZ_d65, XYZ_d50);

    dt_apply_transposed_color_matrix(XYZ_d50, target_profile->matrix_out_transposed, target_RGB);

    // if(pipe_target_profile_same)
    // {
    //   copy_pixel(target_RGB, pix_in);
    // }
    // else
    // {
    //   dt_apply_transposed_color_matrix(pix_in, pipe_to_target_transposed, target_RGB);
    // }

    if (highlight_negative)
    {
      target_RGB[0] = (target_RGB[0] < 0);
      target_RGB[1] = (target_RGB[1] < 0);
      target_RGB[2] = (target_RGB[2] < 0);
    }
    else
    {
      // clip whatever negative remains
      target_RGB[0] = fmaxf(target_RGB[0], 0);
      target_RGB[1] = fmaxf(target_RGB[1], 0);
      target_RGB[2] = fmaxf(target_RGB[2], 0);
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
    if(g != NULL && self->dev->gui_attached && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))
    {
      gamutcompress_update_gui_t *msg = g_malloc(sizeof(gamutcompress_update_gui_t));
      msg->self = self;
      memcpy(msg->max_distances, max_dist, sizeof(msg->max_distances));
      msg->max_xy_dist_ratio = max_xy_dist_ratio;
      g_idle_add(_update_gui_from_worker, msg);
    }
  }

  // add a tiny safety margin
  for_three_channels(c, aligned(max_dist: 16))
  {
    max_dist[c] = max_dist[c] > 1.f ? max_dist[c] + 0.01 : 1;
  }

  if(g != NULL && self->dev->gui_attached && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))
  {
    gamutcompress_update_gui_t *msg = g_malloc(sizeof(gamutcompress_update_gui_t));
    msg->self = self;
    memcpy(msg->max_distances, max_dist, sizeof(msg->max_distances));
    g_idle_add(_update_gui_from_worker, msg);
  }
}


static void auto_adjust_distance_limit_xy(GtkWidget *quad, dt_iop_module_t *self)
{
  dt_iop_gamutcompress_params_t *p = self->params;
  dt_iop_gamutcompress_gui_data_t *g = self->gui_data;

  if(g->max_xy_dist_ratio < 1.0f)
  {
    dt_control_log(_("max distances not yet calculated"));
    return;
  }

  p->gamut_compression_end_xy = g->max_xy_dist_ratio;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->distance_limit_c, p->gamut_compression_end_xy);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void auto_adjust_distance_limit_c(GtkWidget *quad, dt_iop_module_t *self)
{
  dt_iop_gamutcompress_params_t *p = self->params;
  dt_iop_gamutcompress_gui_data_t *g = self->gui_data;

  if(g->max_distances[0] < 1.0f)
  {
    dt_control_log(_("max distances not yet calculated"));
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
    dt_control_log(_("max distances not yet calculated"));
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
    dt_control_log(_("max distances not yet calculated"));
    return;
  }

  p->gamut_compression_distance_limit_y = g->max_distances[2];

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->distance_limit_y, p->gamut_compression_distance_limit_y);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void auto_adjust_distance_limit_all(GtkButton *button, dt_iop_module_t *self)
{
  dt_iop_gamutcompress_params_t *p = self->params;
  dt_iop_gamutcompress_gui_data_t *g = self->gui_data;

  if(g->max_distances[0] < 1.0f || g->max_distances[1] < 1.0f || g->max_distances[2] < 1.0f)
  {
    dt_control_log(_("max distances not yet calculated"));
    return;
  }

  p->gamut_compression_distance_limit_c = g->max_distances[0];
  p->gamut_compression_distance_limit_m = g->max_distances[1];
  p->gamut_compression_distance_limit_y = g->max_distances[2];

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->distance_limit_c, p->gamut_compression_distance_limit_c);
  dt_bauhaus_slider_set(g->distance_limit_m, p->gamut_compression_distance_limit_m);
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

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  GtkWidget *target_primaries_combo = dt_bauhaus_combobox_from_params(self, "target_primaries");
  gtk_widget_set_tooltip_text(target_primaries_combo, _("Color space to perform gamut compression in.\n"
                                                        "'export profile' uses the profile set in 'output color profile'."));

  // Reuse the slider variable for all sliders
  GtkWidget *slider;

  g->gamut_compression_end_xy = dt_bauhaus_slider_from_params(self, "gamut_compression_end_xy");
  dt_bauhaus_slider_set_soft_range(g->gamut_compression_end_xy, 1.0f, 2.0f);
  gtk_widget_set_tooltip_text(g->gamut_compression_end_xy, _("maximum xy oversaturation to correct"));
  dt_bauhaus_widget_set_quad(g->gamut_compression_end_xy, self, dtgtk_cairo_paint_wand, FALSE, auto_adjust_distance_limit_xy,
                             _("set to max detected xy distance"));

  slider = dt_bauhaus_slider_from_params(self, "gamut_compression_start_xy");
  dt_bauhaus_slider_set_soft_range(slider, 0.5f, 0.99f);
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_factor(slider, 100.f);
  gtk_widget_set_tooltip_text(slider, _("start compressing above saturation"));

  g->distance_limit_c = dt_bauhaus_slider_from_params(self, "gamut_compression_distance_limit_c");
  dt_bauhaus_slider_set_soft_range(g->distance_limit_c, 1.0f, 2.0f);
  gtk_widget_set_tooltip_text(g->distance_limit_c, _("maximum oversaturation to correct,\n"
                                                     "that's pushing red to negative"));
  dt_bauhaus_widget_set_quad(g->distance_limit_c, self, dtgtk_cairo_paint_wand, FALSE, auto_adjust_distance_limit_c,
                             _("set to max detected cyan distance"));

  slider = dt_bauhaus_slider_from_params(self, "gamut_compression_buffer_r");
  dt_bauhaus_slider_set_soft_range(slider, 0.1f, 0.5f);
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_factor(slider, 100.f);
  gtk_widget_set_tooltip_text(slider, _("portion of reds to receive cyan overflow"));

  g->distance_limit_m = dt_bauhaus_slider_from_params(self, "gamut_compression_distance_limit_m");
  dt_bauhaus_slider_set_soft_range(g->distance_limit_m, 1.0f, 2.0f);
  gtk_widget_set_tooltip_text(g->distance_limit_m, _("maximum oversaturation to correct,\n"
                                                     "that's pushing green to negative"));
  dt_bauhaus_widget_set_quad(g->distance_limit_m, self, dtgtk_cairo_paint_wand, FALSE, auto_adjust_distance_limit_m,
                             _("set to max detected magenta distance"));

  slider = dt_bauhaus_slider_from_params(self, "gamut_compression_buffer_g");
  dt_bauhaus_slider_set_soft_range(slider, 0.1f, 0.5f);
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_factor(slider, 100.f);
  gtk_widget_set_tooltip_text(slider, _("portion of greens to receive magenta overflow"));

  g->distance_limit_y = dt_bauhaus_slider_from_params(self, "gamut_compression_distance_limit_y");
  dt_bauhaus_slider_set_soft_range(g->distance_limit_y, 1.0f, 2.0f);
  gtk_widget_set_tooltip_text(g->distance_limit_y, _("maximum oversaturation to correct,\n"
                                                     "that's pushing blue to negative"));
  dt_bauhaus_widget_set_quad(g->distance_limit_y, self, dtgtk_cairo_paint_wand, FALSE, auto_adjust_distance_limit_y,
                             _("set to max detected yellow distance"));

  slider = dt_bauhaus_slider_from_params(self, "gamut_compression_buffer_b");
  dt_bauhaus_slider_set_soft_range(slider, 0.1f, 0.5f);
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_factor(slider, 100.f);
  gtk_widget_set_tooltip_text(slider, _("portion of blues to receive compressed yellow overflow"));

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *vbox = self->widget;

  self->widget = hbox;

  g->highlight_negative = GTK_TOGGLE_BUTTON(dt_bauhaus_toggle_from_params(self, "highlight_negative"));

  dt_iop_button_new(self, N_("set all distance limits to max detected distance"),
                    G_CALLBACK(auto_adjust_distance_limit_all), FALSE, 0, 0,
                    dtgtk_cairo_paint_wand, 0, self->widget);

  self->widget = vbox;
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, FALSE, FALSE, 0);

  gui_update(self);
}
