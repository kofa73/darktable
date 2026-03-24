/*
   This file is part of darktable,
   Copyright (C) 2021-2026 darktable developers.

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

#include "common/extra_optimizations.h"

#include "bauhaus/bauhaus.h"
#include "common/bspline.h"
#include "common/darktable.h"
#include "common/dwt.h"
#include "common/gaussian.h"
#include "common/image.h"
#include "common/imagebuf.h"
#include "common/iop_profile.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "develop/noise_generator.h"
#include "develop/openmp_maths.h"
#include "develop/tiling.h"
#include "dtgtk/button.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/expander.h"
#include "dtgtk/paint.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

DT_MODULE_INTROSPECTION(2, dt_iop_diffuse_params_t)

#define MAX_NUM_SCALES 10
typedef struct dt_iop_diffuse_params_t
{
  // global parameters
  int iterations;           // $MIN: 0    $MAX: 500  $DEFAULT: 1  $DESCRIPTION: "iterations"
  float sharpness;          // $MIN: -1.  $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "sharpness"
  int radius;               // $MIN: 0    $MAX: 2048 $DEFAULT: 8  $DESCRIPTION: "radius span"
  float regularization;     // $MIN: 0.   $MAX: 4.   $DEFAULT: 0. $DESCRIPTION: "edge sensitivity"
  float variance_threshold; // $MIN: -2.  $MAX: 2.   $DEFAULT: 0. $DESCRIPTION: "edge threshold"

  float anisotropy_first;   // $MIN: -10. $MAX: 10.  $DEFAULT: 0. $DESCRIPTION: "1st order anisotropy"
  float anisotropy_second;  // $MIN: -10. $MAX: 10.  $DEFAULT: 0. $DESCRIPTION: "2nd order anisotropy"
  float anisotropy_third;   // $MIN: -10. $MAX: 10.  $DEFAULT: 0. $DESCRIPTION: "3rd order anisotropy"
  float anisotropy_fourth;  // $MIN: -10. $MAX: 10.  $DEFAULT: 0. $DESCRIPTION: "4th order anisotropy"

  float threshold;          // $MIN: 0.   $MAX: 8.   $DEFAULT: 0. $DESCRIPTION: "luminance masking threshold"

  float first;              // $MIN: -1.  $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "1st order speed"
  float second;             // $MIN: -1.  $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "2nd order speed"
  float third;              // $MIN: -1.  $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "3rd order speed"
  float fourth;             // $MIN: -1.  $MAX: 1.   $DEFAULT: 0. $DESCRIPTION: "4th order speed"

  // v2
  int radius_center;        // $MIN: 0    $MAX: 1024 $DEFAULT: 0  $DESCRIPTION: "central radius"

  // new versions add params mandatorily at the end, so we can memcpy old parameters at the beginning

} dt_iop_diffuse_params_t;


typedef struct dt_iop_diffuse_gui_data_t
{
  GtkWidget *iterations, *fourth, *third, *second, *radius, *radius_center, *sharpness, *threshold, *regularization, *first,
      *anisotropy_first, *anisotropy_second, *anisotropy_third, *anisotropy_fourth, *regularization_first, *variance_threshold;
} dt_iop_diffuse_gui_data_t;

typedef struct dt_iop_diffuse_global_data_t
{
  int kernel_filmic_bspline_vertical;
  int kernel_filmic_bspline_horizontal;
  int kernel_filmic_wavelets_detail;

  int kernel_diffuse_build_mask;
  int kernel_diffuse_inpaint_mask;
  int kernel_diffuse_pde;
} dt_iop_diffuse_global_data_t;


// only copy params struct to avoid a commit_params()
typedef struct dt_iop_diffuse_params_t dt_iop_diffuse_data_t;


typedef enum dt_isotropy_t
{
  DT_ISOTROPY_ISOTROPE = 0, // diffuse in all directions with same intensity
  DT_ISOTROPY_ISOPHOTE = 1, // diffuse more in the isophote direction (orthogonal to gradient)
  DT_ISOTROPY_GRADIENT = 2  // diffuse more in the gradient direction
} dt_isotropy_t;


DT_OMP_DECLARE_SIMD()
static inline dt_isotropy_t check_isotropy_mode(const float anisotropy)
{
  // user param is negative, positive or zero. The sign encodes the direction of diffusion, the magnitude encodes the ratio of anisotropy
  // ultimately, the anisotropy factor needs to be positive before going into the exponential
  if(anisotropy == 0.f)
    return DT_ISOTROPY_ISOTROPE;
  else if(anisotropy > 0.f)
    return DT_ISOTROPY_ISOPHOTE;
  else
    return DT_ISOTROPY_GRADIENT; // if(anisotropy > 0.f)
}


const char *name()
{
  return _("diffuse or sharpen");
}

const char *aliases()
{
  return _("diffusion|deconvolution|blur|sharpening|bloom|clarity|dehaze|denoise|inpaint|watercolor");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description
    (self,
     _("simulate directional diffusion of light with heat transfer model\n"
       "to apply an iterative edge-oriented blur, inpaint damaged parts\n"
       "of the image, or to remove blur with blind deconvolution"),
     _("corrective and creative"),
     _("linear, RGB, scene-referred"),
     _("linear, RGB"),
     _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_EFFECTS;
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_diffuse_params_v2_t
  {
    // global parameters
    int iterations;
    float sharpness;
    int radius;
    float regularization;
    float variance_threshold;

    float anisotropy_first;
    float anisotropy_second;
    float anisotropy_third;
    float anisotropy_fourth;

    float threshold;

    float first;
    float second;
    float third;
    float fourth;

    // v2
    int radius_center;
  } dt_iop_diffuse_params_v2_t;

  if(old_version == 1)
  {
    typedef struct dt_iop_diffuse_params_v1_t
    {
      // global parameters
      int iterations;
      float sharpness;
      int radius;
      float regularization;
      float variance_threshold;

      float anisotropy_first;
      float anisotropy_second;
      float anisotropy_third;
      float anisotropy_fourth;

      float threshold;

      float first;
      float second;
      float third;
      float fourth;
    } dt_iop_diffuse_params_v1_t;

    const dt_iop_diffuse_params_v1_t *o = (dt_iop_diffuse_params_v1_t *)old_params;
    dt_iop_diffuse_params_v2_t *n = malloc(sizeof(dt_iop_diffuse_params_v2_t));

    // copy common parameters
    memcpy(n, o, sizeof(dt_iop_diffuse_params_v1_t));

    // init only new parameters
    n->radius_center = 0;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_diffuse_params_v2_t);
    *new_version = 2;
    return 0;
  }
  return 1;
}

void init_presets(dt_iop_module_so_t *self)
{
  // deblurring presets

  dt_gui_presets_add_generic(_("lens deblur | soft"), self->op, self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 8,
                                 .radius_center = 0,
                                 .radius = 8,

                                 .first = -0.25f,
                                 .second = +0.125f,
                                 .third = -0.50f,
                                 .fourth = +0.25f,

                                 .anisotropy_first = +1.f,
                                 .anisotropy_second = 0.f,
                                 .anisotropy_third = +1.f,
                                 .anisotropy_fourth = 0.f,

                                 .sharpness = 0.0f,
                                 .regularization = 3.f,
                                 .variance_threshold = +1.f,

                                 .threshold = 0.0f
                               },
                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("lens deblur | medium"), self->op, self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 16,
                                 .radius_center = 0,
                                 .radius = 10,

                                 .first = -0.25f,
                                 .second = +0.125f,
                                 .third = -0.50f,
                                 .fourth = +0.25f,

                                 .anisotropy_first = +1.f,
                                 .anisotropy_second = 0.f,
                                 .anisotropy_third = +1.f,
                                 .anisotropy_fourth = 0.f,

                                 .sharpness = 0.0f,
                                 .regularization = 3.f,
                                 .variance_threshold = +1.f,

                                 .threshold = 0.0f
                               },
                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("lens deblur | hard"), self->op, self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 24,
                                 .radius_center = 0,
                                 .radius = 12,

                                 .first = -0.25f,
                                 .second = +0.125f,
                                 .third = -0.50f,
                                 .fourth = +0.25f,

                                 .anisotropy_first = +1.f,
                                 .anisotropy_second = 0.f,
                                 .anisotropy_third = +1.f,
                                 .anisotropy_fourth = 0.f,

                                 .sharpness = 0.0f,
                                 .regularization = 3.f,
                                 .variance_threshold = +1.f,

                                 .threshold = 0.0f
                               },

                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("dehaze | default"), self->op, self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 10,
                                 .radius_center = 0,
                                 .radius = 512,

                                 .first = -0.20f,
                                 .second = +0.10f,
                                 .third = -0.20f,
                                 .fourth = +0.10f,

                                 .anisotropy_first = 2.f,
                                 .anisotropy_second = 0.f,
                                 .anisotropy_third = 2.f,
                                 .anisotropy_fourth = 0.f,

                                 .sharpness = 0.0f,
                                 .regularization = 2.5f,
                                 .variance_threshold = 0.25f,

                                 .threshold = 0.0f
                               },

                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("dehaze | extra contrast"), self->op, self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 10,
                                 .radius_center = 0,
                                 .radius = 512,

                                 .first = -0.20f,
                                 .second = +0.10f,
                                 .third = -0.20f,
                                 .fourth = +0.10f,

                                 .anisotropy_first = 2.f,
                                 .anisotropy_second = 0.f,
                                 .anisotropy_third = 2.f,
                                 .anisotropy_fourth = 0.f,

                                 .sharpness = 0.007f,
                                 .regularization = 1.0f,
                                 .variance_threshold = 0.25f,

                                 .threshold = 0.0f
                               },

                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("denoise | fine"), self->op, self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 32,
                                 .radius_center = 2,
                                 .radius = 1,

                                 .first = 0.06f,
                                 .second = 0.0f,
                                 .third = 0.06f,
                                 .fourth = 0.0f,

                                 .anisotropy_first = +2.f,
                                 .anisotropy_second = 0.f,
                                 .anisotropy_third = +2.f,
                                 .anisotropy_fourth = 0.f,

                                 .sharpness = 0.0f,
                                 .regularization = 4.f,
                                 .variance_threshold = -0.25f,

                                 .threshold = 0.0f
                               },
                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("denoise | medium"), self->op, self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 32,
                                 .radius_center = 4,
                                 .radius = 3,

                                 .first = 0.05f,
                                 .second = 0.0f,
                                 .third = 0.05f,
                                 .fourth = 0.0f,

                                 .anisotropy_first = +2.f,
                                 .anisotropy_second = 0.f,
                                 .anisotropy_third = +2.f,
                                 .anisotropy_fourth = 0.f,

                                 .sharpness = 0.0f,
                                 .regularization = 4.f,
                                 .variance_threshold = -0.25f,

                                 .threshold = 0.0f
                               },
                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("denoise | coarse"), self->op, self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 32,
                                 .radius_center = 8,
                                 .radius = 6,

                                 .first = 0.04f,
                                 .second = 0.0f,
                                 .third = 0.04f,
                                 .fourth = 0.0f,

                                 .anisotropy_first = +2.f,
                                 .anisotropy_second = 0.f,
                                 .anisotropy_third = +2.f,
                                 .anisotropy_fourth = 0.f,

                                 .sharpness = 0.0f,
                                 .regularization = 4.f,
                                 .variance_threshold = -0.25f,

                                 .threshold = 0.0f
                               },
                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("surface blur"), self->op, self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 2,
                                 .radius_center = 0,
                                 .radius = 32,

                                 .first = 1.0f,
                                 .second = 1.0f,
                                 .third = 1.0f,
                                 .fourth = 1.0f,

                                 .anisotropy_first = +4.f,
                                 .anisotropy_second = +4.f,
                                 .anisotropy_third = +4.f,
                                 .anisotropy_fourth = +4.f,

                                 .sharpness = 0.0f,
                                 .regularization = 4.f,
                                 .variance_threshold = 0.f,

                                 .threshold = 0.0f
                               },
                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("artistic effects | bloom"), self->op, self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 1,
                                 .radius_center = 0,
                                 .radius = 32,

                                 .first = 0.5f,
                                 .second = 0.5f,
                                 .third = 0.5f,
                                 .fourth = 0.5f,

                                 .anisotropy_first = 0.f,
                                 .anisotropy_second = 0.f,
                                 .anisotropy_third =  0.f,
                                 .anisotropy_fourth = 0.f,

                                 .sharpness = 0.0f,
                                 .regularization = 0.f,
                                 .variance_threshold = 0.f,

                                 .threshold = 0.0f
                               },
                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("sharpen demosaicing | no AA filter"), self->op,
                             self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 1,
                                 .radius_center = 0,
                                 .radius = 4,

                                 .first = -0.25f,
                                 .second = -0.25f,
                                 .third = -0.25f,
                                 .fourth = -0.25f,

                                 .anisotropy_first = 1.f,
                                 .anisotropy_second = 1.f,
                                 .anisotropy_third =  1.f,
                                 .anisotropy_fourth = 1.f,

                                 .sharpness = 0.0f,
                                 .regularization = 1.f,
                                 .variance_threshold = 0.f,

                                 .threshold = 0.0f
                               },
                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("sharpen demosaicing | AA filter"), self->op,
                             self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 1,
                                 .radius_center = 0,
                                 .radius = 8,

                                 .first = -0.25f,
                                 .second = -0.25f,
                                 .third = -0.25f,
                                 .fourth = -0.25f,

                                 .anisotropy_first = 1.f,
                                 .anisotropy_second = 1.f,
                                 .anisotropy_third =  1.f,
                                 .anisotropy_fourth = 1.f,

                                 .sharpness = 0.0f,
                                 .regularization = 1.f,
                                 .variance_threshold = 0.f,

                                 .threshold = 0.0f
                               },

                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("artistic effects | simulate watercolor"), self->op, self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 4,
                                 .radius_center = 0,
                                 .radius = 64,

                                 .first = 0.f,
                                 .second = 0.f,
                                 .third = 0.5f,
                                 .fourth = 0.5f,

                                 .anisotropy_first = 0.f,
                                 .anisotropy_second = 0.f,
                                 .anisotropy_third =  4.f,
                                 .anisotropy_fourth = 4.f,

                                 .sharpness = 0.0f,
                                 .regularization = 2.f,
                                 .variance_threshold = 0.f,

                                 .threshold = 0.0f
                               },
                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("artistic effects | simulate line drawing"), self->op, self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 50,
                                 .radius_center = 0,
                                 .radius = 64,

                                 .first = -1.f,
                                 .second = -1.f,
                                 .third = -1.f,
                                 .fourth = -1.f,

                                 .anisotropy_first = -5.f,
                                 .anisotropy_second = -5.f,
                                 .anisotropy_third =  -5.f,
                                 .anisotropy_fourth = -5.f,

                                 .sharpness = 0.0f,
                                 .regularization = 4.f,
                                 .variance_threshold = 0.f,

                                 .threshold = 0.0f
                               },
                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  // local contrast

  dt_gui_presets_add_generic(_("local contrast | normal"), self->op, self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 10,
                                 .radius_center = 512,
                                 .radius = 384,

                                 .first = -0.50f,
                                 .second = 0.f,
                                 .third = 0.f,
                                 .fourth = -0.50f,

                                 .anisotropy_first = -2.5f,
                                 .anisotropy_second = 0.f,
                                 .anisotropy_third = 0.f,
                                 .anisotropy_fourth = -2.5f,

                                 .sharpness = 0.0f,
                                 .regularization = 1.f,
                                 .variance_threshold = 1.f,

                                 .threshold = 0.0f
                               },
                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("local contrast | fine"), self->op, self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 5,
                                 .radius_center = 0,
                                 .radius = 170,

                                 .first = -0.15f,
                                 .second = 0.05f,
                                 .third = 0.05f,
                                 .fourth = -0.15f,

                                 .anisotropy_first = 10.f,
                                 .anisotropy_second = 0.f,
                                 .anisotropy_third = 0.f,
                                 .anisotropy_fourth = 10.f,

                                 .sharpness = 0.0f,
                                 .regularization = 2.f,
                                 .variance_threshold = 0.f,

                                 .threshold = 0.0f
                               },
                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("inpaint highlights"), self->op, self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 32,
                                 .radius_center = 0,
                                 .radius = 4,

                                 .first = +0.0f,
                                 .second = +0.0f,
                                 .third = +0.0f,
                                 .fourth = +0.5f,

                                 .anisotropy_first = +0.f,
                                 .anisotropy_second = +0.f,
                                 .anisotropy_third = +0.f,
                                 .anisotropy_fourth = +2.f,

                                 .sharpness = 0.0f,
                                 .regularization = 0.f,
                                 .variance_threshold = 0.f,

                                 .threshold = 1.41f,
                               },
                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  // fast presets for slow hardware

  dt_gui_presets_add_generic(_("sharpness | fast"), self->op, self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 1,
                                 .radius_center = 0,
                                 .radius = 128,

                                 .first = 0.f,
                                 .second = 0.f,
                                 .third = -0.50f,
                                 .fourth = 0.f,

                                 .anisotropy_first = 0.f,
                                 .anisotropy_second = 0.f,
                                 .anisotropy_third = 5.f,
                                 .anisotropy_fourth = 0.f,

                                 .sharpness = 0.0f,
                                 .regularization = 0.25f,
                                 .variance_threshold = 0.25f,

                                 .threshold = 0.0f,
                               },
                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  // two more sharpness (standard & strong)

  dt_gui_presets_add_generic(_("sharpness | normal"), self->op, self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 3,
                                 .radius_center = 0,
                                 .radius = 3,

                                 .first = 0.0065f,
                                 .second = -0.25f,
                                 .third = -0.25f,
                                 .fourth = -0.2774f,

                                 .anisotropy_first = 1.f,
                                 .anisotropy_second = 1.f,
                                 .anisotropy_third = 1.f,
                                 .anisotropy_fourth = 1.f,

                                 .sharpness = 0.0f,
                                 .regularization = 2.94f,
                                 .threshold = 0.0f,

                                 .variance_threshold = 0.0f,
                               },
                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("sharpness | strong"), self->op, self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 6,
                                 .radius_center = 0,
                                 .radius = 3,

                                 .first = 0.0065f,
                                 .second = -0.25f,
                                 .third = -0.25f,
                                 .fourth = -0.2774f,

                                 .anisotropy_first = 1.f,
                                 .anisotropy_second = 1.f,
                                 .anisotropy_third = 1.f,
                                 .anisotropy_fourth = 1.f,

                                 .sharpness = 0.0f,
                                 .regularization = 2.15f,
                                 .variance_threshold = 0.22f,

                                 .threshold = 0.0f,
                               },
                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  dt_gui_presets_add_generic(_("local contrast | fast"), self->op, self->version(),
                             &(dt_iop_diffuse_params_t)
                               { .iterations = 1,
                                 .radius_center = 512,
                                 .radius = 512,

                                 .first = 0.f,
                                 .second = 0.f,
                                 .third = -0.50f,
                                 .fourth = 0.f,

                                 .anisotropy_first = 0.f,
                                 .anisotropy_second = 0.f,
                                 .anisotropy_third = 5.f,
                                 .anisotropy_fourth = 0.f,

                                 .sharpness = 0.0f,
                                 .regularization = 0.01f,
                                 .variance_threshold = 0.05f,

                                 .threshold = 0.0f,
                               },
                             sizeof(dt_iop_diffuse_params_t), TRUE,
                             DEVELOP_BLEND_CS_RGB_SCENE);
}

void tiling_callback(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
{
  dt_iop_diffuse_data_t *data = piece->data;

  const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);
  const float final_radius = (data->radius + data->radius_center) * 2.f / scale;
  const int diffusion_scales =
    num_steps_to_reach_equivalent_sigma(B_SPLINE_SIGMA, final_radius);
  const int scales = CLAMP(diffusion_scales, 1, MAX_NUM_SCALES);
  const int max_filter_radius = (1 << scales);

  // in + out + 2 * tmp + 2 * LF + s details + grey mask
  tiling->factor = 6.25f + scales;
  tiling->factor_cl = 6.25f + scales;

  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = max_filter_radius;
  tiling->align = 1;
  return;
}

// Discretization parameters for the Partial Derivative Equation solver
#define H 1         // spatial step
#define KAPPA 0.25f // 0.25 if h = 1, 1 if h = 2




DT_OMP_DECLARE_SIMD(aligned(a, c2, cos_theta_sin_theta, cos_theta2, sin_theta2:16))
static inline void rotation_matrix_isophote(const dt_aligned_pixel_t c2,
                                            const dt_aligned_pixel_t cos_theta_sin_theta,
                                            const dt_aligned_pixel_t cos_theta2,
                                            const dt_aligned_pixel_t sin_theta2,
                                            dt_aligned_pixel_t a[2][2])
{
  // Write the coefficients of a square symmetrical matrice of rotation of the gradient :
  // [[ a11, a12 ],
  //  [ a12, a22 ]]
  // taken from https://www.researchgate.net/publication/220663968
  // c dampens the gradient direction
  for_each_channel(c)
  {
    a[0][0][c] = cos_theta2[c] + c2[c] * sin_theta2[c];
    a[1][1][c] = c2[c] * cos_theta2[c] + sin_theta2[c];
    a[0][1][c] = a[1][0][c] = (c2[c] - 1.0f) * cos_theta_sin_theta[c];
  }
}

DT_OMP_DECLARE_SIMD(aligned(a, c2, cos_theta_sin_theta, cos_theta2, sin_theta2:16))
static inline void rotation_matrix_gradient(const dt_aligned_pixel_t c2,
                                            const dt_aligned_pixel_t cos_theta_sin_theta,
                                            const dt_aligned_pixel_t cos_theta2,
                                            const dt_aligned_pixel_t sin_theta2,
                                            dt_aligned_pixel_t a[2][2])
{
  // Write the coefficients of a square symmetrical matrice of rotation of the gradient :
  // [[ a11, a12 ],
  //  [ a12, a22 ]]
  // based on https://www.researchgate.net/publication/220663968 and inverted
  // c dampens the isophote direction
  for_each_channel(c)
  {
    a[0][0][c] = c2[c] * cos_theta2[c] + sin_theta2[c];
    a[1][1][c] = cos_theta2[c] + c2[c] * sin_theta2[c];
    a[0][1][c] = a[1][0][c] = (1.0f - c2[c]) * cos_theta_sin_theta[c];
  }
}


DT_OMP_DECLARE_SIMD(aligned(a, kernel: 64))
static inline void build_matrix(const dt_aligned_pixel_t a[2][2],
                                dt_aligned_pixel_t kernel[9])
{
  for_each_channel(c)
  {
    const float b11 = a[0][1][c] / 2.0f;
    const float b13 = -b11;
    const float b22 = -2.0f * (a[0][0][c] + a[1][1][c]);

    // build the kernel of rotated anisotropic laplacian
    // from https://www.researchgate.net/publication/220663968 :
    // [ [ a12 / 2,  a22,            -a12 / 2 ],
    //   [ a11,      -2 (a11 + a22), a11      ],
    //   [ -a12 / 2,   a22,          a12 / 2  ] ]
    // N.B. we have flipped the signs of the a12 terms
    // compared to the paper. There's probably a mismatch
    // of coordinate convention between the paper and the
    // original derivation of this convolution mask
    // (Witkin 1991, https://doi.org/10.1145/127719.122750).
    kernel[0][c] = b11;
    kernel[1][c] = a[1][1][c];
    kernel[2][c] = b13;
    kernel[3][c] = a[0][0][c];
    kernel[4][c] = b22;
    kernel[5][c] = a[0][0][c];
    kernel[6][c] = b13;
    kernel[7][c] = a[1][1][c];
    kernel[8][c] = b11;
  }
}

DT_OMP_DECLARE_SIMD(aligned(kernel: 64))
static inline void isotrope_laplacian(dt_aligned_pixel_t kernel[9])
{
  // see in https://eng.aurelienpierre.com/2021/03/rotation-invariant-laplacian-for-2d-grids/#Second-order-isotropic-finite-differences
  // for references (Oono & Puri)
  for_each_channel(c)
  {
    kernel[0][c] = 0.25f;
    kernel[1][c] = 0.5f;
    kernel[2][c] = 0.25f;
    kernel[3][c] = 0.5f;
    kernel[4][c] = -3.f;
    kernel[5][c] = 0.5f;
    kernel[6][c] = 0.25f;
    kernel[7][c] = 0.5f;
    kernel[8][c] = 0.25f;
  }
}

DT_OMP_DECLARE_SIMD(aligned(kernel, c2: 64) uniform(isotropy_type))
static inline void compute_kernel(const dt_aligned_pixel_t c2,
                                  const dt_aligned_pixel_t cos_theta_sin_theta,
                                  const dt_aligned_pixel_t cos_theta2,
                                  const dt_aligned_pixel_t sin_theta2,
                                  const dt_isotropy_t isotropy_type,
                                  dt_aligned_pixel_t kernel[9])
{
  // Build the matrix of rotation with anisotropy

  switch(isotropy_type)
  {
    case(DT_ISOTROPY_ISOTROPE):
    default:
    {
      isotrope_laplacian(kernel);
      break;
    }
    case(DT_ISOTROPY_ISOPHOTE):
    {
      dt_aligned_pixel_t a[2][2] = { { { 0.f } } };
      rotation_matrix_isophote(c2, cos_theta_sin_theta, cos_theta2, sin_theta2, a);
      build_matrix(a, kernel);
      break;
    }
    case(DT_ISOTROPY_GRADIENT):
    {
      dt_aligned_pixel_t a[2][2] = { { { 0.f } } };
      rotation_matrix_gradient(c2, cos_theta_sin_theta, cos_theta2, sin_theta2, a);
      build_matrix(a, kernel);
      break;
    }
  }
}

// Compute convolution directly using kernel symmetries, avoiding
// intermediate 9-element kernel arrays.
// Anisotropic kernel: b11*(N0-N2-N6+N8) + a22*(N1+N7) + a11*(N3+N5) - 2*(a11+a22)*N4
// Isotropic kernel:   0.25*(N0+N2+N6+N8) + 0.5*(N1+N7+N3+N5) - 3*N4
DT_OMP_DECLARE_SIMD(aligned(c2, cos_theta_sin_theta, cos_theta2, sin_theta2: 16) \
                     aligned(cross_corners, sum_corners, sum_tb, sum_lr, center, acc: 16) \
                     uniform(isotropy_type, abcd))
static inline void accumulate_convolution_direct(
    const dt_aligned_pixel_t c2,
    const dt_aligned_pixel_t cos_theta_sin_theta,
    const dt_aligned_pixel_t cos_theta2,
    const dt_aligned_pixel_t sin_theta2,
    const dt_isotropy_t isotropy_type,
    const dt_aligned_pixel_t cross_corners,
    const dt_aligned_pixel_t sum_corners,
    const dt_aligned_pixel_t sum_tb,
    const dt_aligned_pixel_t sum_lr,
    const dt_aligned_pixel_t center,
    const float abcd,
    dt_aligned_pixel_t acc)
{
  switch(isotropy_type)
  {
    case(DT_ISOTROPY_ISOTROPE):
    default:
    {
      for_each_channel(c)
      {
        acc[c] += abcd * (0.25f * sum_corners[c] + 0.5f * (sum_tb[c] + sum_lr[c]) - 3.f * center[c]);
      }
      break;
    }
    case(DT_ISOTROPY_ISOPHOTE):
    {
      for_each_channel(c)
      {
        const float a11 = cos_theta2[c] + c2[c] * sin_theta2[c];
        const float a22 = c2[c] * cos_theta2[c] + sin_theta2[c];
        const float b11 = (c2[c] - 1.0f) * cos_theta_sin_theta[c] * 0.5f;
        acc[c] += abcd * (b11 * cross_corners[c] + a22 * sum_tb[c] + a11 * sum_lr[c]
                          - 2.0f * (a11 + a22) * center[c]);
      }
      break;
    }
    case(DT_ISOTROPY_GRADIENT):
    {
      for_each_channel(c)
      {
        const float a11 = c2[c] * cos_theta2[c] + sin_theta2[c];
        const float a22 = cos_theta2[c] + c2[c] * sin_theta2[c];
        const float b11 = (1.0f - c2[c]) * cos_theta_sin_theta[c] * 0.5f;
        acc[c] += abcd * (b11 * cross_corners[c] + a22 * sum_tb[c] + a11 * sum_lr[c]
                          - 2.0f * (a11 + a22) * center[c]);
      }
      break;
    }
  }
}

static inline void heat_PDE_diffusion(const float *const restrict high_freq,
                                      const float *const restrict low_freq,
                                      const uint8_t *const restrict mask,
                                      const gboolean has_mask,
                                      float *const restrict output,
                                      const size_t width,
                                      const size_t height,
                                      const dt_aligned_pixel_t anisotropy,
                                      const dt_isotropy_t isotropy_type[4],
                                      const float regularization,
                                      const float variance_threshold,
                                      const float current_radius_square,
                                      const int mult,
                                      const dt_aligned_pixel_t ABCD,
                                      const float strength)
{
  // Simultaneous inpainting for image structure and texture using
  // anisotropic heat transfer model
  // https://www.researchgate.net/publication/220663968
  // modified as follow :
  //
  //  * apply it in a multi-scale wavelet setup : we basically solve
  //    it twice, on the wavelets LF and HF layers.
  //  * replace the manual texture direction/distance selection by an
  //    automatic detection similar to the structure one,
  //  * generalize the framework for isotropic diffusion and
  //    anisotropic weighted on the isophote direction
  //  * add a variance regularization to better avoid edges.
  // The sharpness setting mimics the contrast equalizer effect by
  // simply multiplying the HF by some gain.

  float *const restrict out = DT_IS_ALIGNED(output);
  const float *const restrict LF = DT_IS_ALIGNED(low_freq);
  const float *const restrict HF = DT_IS_ALIGNED(high_freq);

  const float regularization_factor = regularization * current_radius_square / 9.f;

  const gboolean grad_is_isotropic = (isotropy_type[0] == DT_ISOTROPY_ISOTROPE && isotropy_type[2] == DT_ISOTROPY_ISOTROPE);
  const gboolean lapl_is_isotropic = (isotropy_type[1] == DT_ISOTROPY_ISOTROPE && isotropy_type[3] == DT_ISOTROPY_ISOTROPE);

  // Pre-compute half-anisotropy to absorb the 0.5 gradient/laplacian scaling factor.
  // Removing the per-pixel 0.5f multiply from grad/lapl computation makes magnitude 2x larger,
  // so we halve the anisotropy factor once here to compensate.
  const dt_aligned_pixel_t half_anisotropy
      = { 0.5f * anisotropy[0], 0.5f * anisotropy[1],
          0.5f * anisotropy[2], 0.5f * anisotropy[3] };

/* Pixel processing body extracted for column loop peeling.
   J_LEFT and J_RIGHT are the left and right column neighbor indices. */
#define DIFFUSE_PIXEL_BODY(J_LEFT, J_RIGHT, GRAD_ISOTROPIC, LAPL_ISOTROPIC, GRAD_ZERO, LAPL_ZERO) \
    { \
      const size_t _jl = (J_LEFT); \
      const size_t _jr = (J_RIGHT); \
      const size_t idx = (i * width + j); \
      const size_t index = idx * 4; \
      const uint8_t opacity = (has_mask) ? mask[idx] : 1; \
\
      if(opacity) \
      { \
        /* Pre-compute symmetric pixel combinations to exploit kernel symmetries. */ \
        /* Instead of building 9-element kernel arrays and doing 9 multiplies per order, */ \
        /* we use 4 unique weights with pre-combined neighbor pixels. */ \
        dt_aligned_pixel_t LF_cross, LF_sum_corners, LF_sum_tb, LF_sum_lr, LF_center; \
        dt_aligned_pixel_t HF_cross, HF_sum_corners, HF_sum_tb, HF_sum_lr, HF_center; \
        dt_aligned_pixel_t variance = { 0.f }; \
\
        /* c² in https://www.researchgate.net/publication/220663968 */ \
        dt_aligned_pixel_t c2[4] = { { 0.f } }; \
        /* build the local anisotropic convolution filters for gradients and laplacians */ \
        dt_aligned_pixel_t cos_theta_grad_sq; \
        dt_aligned_pixel_t sin_theta_grad_sq; \
        dt_aligned_pixel_t cos_theta_sin_theta_grad; \
\
        dt_aligned_pixel_t cos_theta_lapl_sq; \
        dt_aligned_pixel_t sin_theta_lapl_sq; \
        dt_aligned_pixel_t cos_theta_sin_theta_lapl; \
\
        const size_t n0 = 4 * (i_neighbours[0] + _jl); \
        const size_t n1 = 4 * (i_neighbours[0] + j); \
        const size_t n2 = 4 * (i_neighbours[0] + _jr); \
        const size_t n3 = 4 * (i_neighbours[1] + _jl); \
        const size_t n4 = 4 * (i_neighbours[1] + j); \
        const size_t n5 = 4 * (i_neighbours[1] + _jr); \
        const size_t n6 = 4 * (i_neighbours[2] + _jl); \
        const size_t n7 = 4 * (i_neighbours[2] + j); \
        const size_t n8 = 4 * (i_neighbours[2] + _jr); \
\
        for_each_channel(c) \
        { \
          const float lf0 = LF[n0 + c]; \
          const float lf1 = LF[n1 + c]; \
          const float lf2 = LF[n2 + c]; \
          const float lf3 = LF[n3 + c]; \
          const float lf4 = LF[n4 + c]; \
          const float lf5 = LF[n5 + c]; \
          const float lf6 = LF[n6 + c]; \
          const float lf7 = LF[n7 + c]; \
          const float lf8 = LF[n8 + c]; \
\
          const float lf08 = lf0 + lf8; \
          const float lf26 = lf2 + lf6; \
          LF_cross[c] = lf08 - lf26; \
          LF_sum_corners[c] = lf08 + lf26; \
          LF_sum_tb[c] = lf1 + lf7; \
          LF_sum_lr[c] = lf3 + lf5; \
          LF_center[c] = lf4; \
\
          if(!(GRAD_ZERO) && !(GRAD_ISOTROPIC)) \
          { \
            float grad_x = lf7 - lf1; \
            float grad_y = lf5 - lf3; \
            float gx_sq = sqf(grad_x); \
            float gy_sq = sqf(grad_y); \
            float mag_sq_grad = gx_sq + gy_sq; \
            float magnitude_grad = sqrtf(mag_sq_grad); \
            c2[0][c] = -magnitude_grad * half_anisotropy[0]; \
            c2[2][c] = -magnitude_grad * half_anisotropy[2]; \
            /* Compute cos²θ, sin²θ, cosθ·sinθ directly from squared components */ \
            /* cos²θ = gx²/m², sin²θ = gy²/m², cosθ·sinθ = gx·gy/m² */ \
            float inv_mag_sq_grad = (mag_sq_grad != 0.f) ? 1.0f / mag_sq_grad : 0.f; \
            cos_theta_grad_sq[c] = (mag_sq_grad != 0.f) ? gx_sq * inv_mag_sq_grad : 1.f; \
            sin_theta_grad_sq[c] = gy_sq * inv_mag_sq_grad; \
            cos_theta_sin_theta_grad[c] = grad_x * grad_y * inv_mag_sq_grad; \
          } \
\
          const float hf0 = HF[n0 + c]; \
          const float hf1 = HF[n1 + c]; \
          const float hf2 = HF[n2 + c]; \
          const float hf3 = HF[n3 + c]; \
          const float hf4 = HF[n4 + c]; \
          const float hf5 = HF[n5 + c]; \
          const float hf6 = HF[n6 + c]; \
          const float hf7 = HF[n7 + c]; \
          const float hf8 = HF[n8 + c]; \
\
          const float hf08 = hf0 + hf8; \
          const float hf26 = hf2 + hf6; \
          HF_cross[c] = hf08 - hf26; \
          HF_sum_corners[c] = hf08 + hf26; \
          HF_sum_tb[c] = hf1 + hf7; \
          HF_sum_lr[c] = hf3 + hf5; \
          HF_center[c] = hf4; \
          \
          variance[c] = sqf(hf0) + sqf(hf1) + sqf(hf2) + sqf(hf3) + sqf(hf4) + \
                        sqf(hf5) + sqf(hf6) + sqf(hf7) + sqf(hf8); \
\
          if(!(LAPL_ZERO) && !(LAPL_ISOTROPIC)) \
          { \
            float lapl_x = hf7 - hf1; \
            float lapl_y = hf5 - hf3; \
            float lx_sq = sqf(lapl_x); \
            float ly_sq = sqf(lapl_y); \
            float mag_sq_lapl = lx_sq + ly_sq; \
            float magnitude_lapl = sqrtf(mag_sq_lapl); \
            c2[1][c] = -magnitude_lapl * half_anisotropy[1]; \
            c2[3][c] = -magnitude_lapl * half_anisotropy[3]; \
            /* Compute cos²θ, sin²θ, cosθ·sinθ directly from squared components */ \
            /* cos²θ = lx²/m², sin²θ = ly²/m², cosθ·sinθ = lx·ly/m² */ \
            float inv_mag_sq_lapl = (mag_sq_lapl != 0.f) ? 1.0f / mag_sq_lapl : 0.f; \
            cos_theta_lapl_sq[c] = (mag_sq_lapl != 0.f) ? lx_sq * inv_mag_sq_lapl : 1.f; \
            sin_theta_lapl_sq[c] = ly_sq * inv_mag_sq_lapl; \
            cos_theta_sin_theta_lapl[c] = lapl_x * lapl_y * inv_mag_sq_lapl; \
          } \
        } \
\
        /* elements of c2 need to be expf(mag*anistropy), but we */ \
        /* haven't applied the expf() yet.  Do that now. */ \
        if (!(GRAD_ZERO)) \
        { \
          if(isotropy_type[0] != DT_ISOTROPY_ISOTROPE) \
            dt_vector_exp(c2[0], c2[0]); \
          if(isotropy_type[2] != DT_ISOTROPY_ISOTROPE) \
            dt_vector_exp(c2[2], c2[2]); \
        } \
        if (!(LAPL_ZERO)) \
        { \
          if(isotropy_type[1] != DT_ISOTROPY_ISOTROPE) \
            dt_vector_exp(c2[1], c2[1]); \
          if(isotropy_type[3] != DT_ISOTROPY_ISOTROPE) \
            dt_vector_exp(c2[3], c2[3]); \
        } \
\
        /* Regularize the variance taking into account the blurring scale. */ \
        /* This allows to keep the scene-referred variance roughly constant */ \
        /* regardless of the wavelet scale where we compute it. */ \
        /* Prevents large scale halos when deblurring. */ \
        for_each_channel(c, aligned(variance)) \
        { \
          variance[c] = variance_threshold + variance[c] * regularization_factor; \
        } \
\
        /* Compute convolutions directly using kernel symmetries and accumulate directly */ \
        dt_aligned_pixel_t acc = { 0.f }; \
        if (!(GRAD_ZERO)) \
        { \
          if(GRAD_ISOTROPIC) \
          { \
            for_each_channel(c) \
            { \
              acc[c] += ABCD[0] * (0.25f * LF_sum_corners[c] + 0.5f * (LF_sum_tb[c] + LF_sum_lr[c]) - 3.f * LF_center[c]); \
              acc[c] += ABCD[2] * (0.25f * HF_sum_corners[c] + 0.5f * (HF_sum_tb[c] + HF_sum_lr[c]) - 3.f * HF_center[c]); \
            } \
          } \
          else \
          { \
            accumulate_convolution_direct(c2[0], cos_theta_sin_theta_grad, cos_theta_grad_sq, \
                                       sin_theta_grad_sq, isotropy_type[0], \
                                       LF_cross, LF_sum_corners, LF_sum_tb, LF_sum_lr, \
                                       LF_center, ABCD[0], acc); \
            accumulate_convolution_direct(c2[2], cos_theta_sin_theta_grad, cos_theta_grad_sq, \
                                       sin_theta_grad_sq, isotropy_type[2], \
                                       HF_cross, HF_sum_corners, HF_sum_tb, HF_sum_lr, \
                                       HF_center, ABCD[2], acc); \
          } \
        } \
        if (!(LAPL_ZERO)) \
        { \
          if(LAPL_ISOTROPIC) \
          { \
            for_each_channel(c) \
            { \
              acc[c] += ABCD[1] * (0.25f * LF_sum_corners[c] + 0.5f * (LF_sum_tb[c] + LF_sum_lr[c]) - 3.f * LF_center[c]); \
              acc[c] += ABCD[3] * (0.25f * HF_sum_corners[c] + 0.5f * (HF_sum_tb[c] + HF_sum_lr[c]) - 3.f * HF_center[c]); \
            } \
          } \
          else \
          { \
            accumulate_convolution_direct(c2[1], cos_theta_sin_theta_lapl, cos_theta_lapl_sq, \
                                       sin_theta_lapl_sq, isotropy_type[1], \
                                       LF_cross, LF_sum_corners, LF_sum_tb, LF_sum_lr, \
                                       LF_center, ABCD[1], acc); \
            accumulate_convolution_direct(c2[3], cos_theta_sin_theta_lapl, cos_theta_lapl_sq, \
                                       sin_theta_lapl_sq, isotropy_type[3], \
                                       HF_cross, HF_sum_corners, HF_sum_tb, HF_sum_lr, \
                                       HF_center, ABCD[3], acc); \
          } \
        } \
        dt_aligned_pixel_t result; \
        for_each_channel(c, aligned(acc,HF,LF,variance,result)) \
        { \
          acc[c] = (HF[index + c] * strength + acc[c] / variance[c]); \
          /* update the solution */ \
          result[c] = fmaxf(acc[c] + LF[index + c], 0.f); \
        } \
        copy_pixel_nontemporal(out + index, result); \
      } \
      else \
      { \
        /* only copy input to output, do nothing */ \
        dt_aligned_pixel_t passthrough; \
        for_each_channel(c, aligned(passthrough, HF, LF : 64)) \
          passthrough[c] = HF[index + c] + LF[index + c]; \
        copy_pixel_nontemporal(out + index, passthrough); \
      } \
    }

#define DIFFUSE_ROW_LOOP(GRAD_ISOTROPIC, LAPL_ISOTROPIC, GRAD_ZERO, LAPL_ZERO) \
  DT_OMP_FOR() \
  for(size_t row = 0; row < height; ++row) \
  { \
    /* interleave the order in which we process the rows so that we minimize cache misses */ \
    const size_t i = dwt_interleave_rows(row, height, mult); \
    /* compute the 'above' and 'below' coordinates, clamping them to the image, once for the entire row */ \
    const size_t i_neighbours[3] \
      = { MAX((int)(i - mult * H), (int)0) * width,            /* x - mult */ \
          i * width,                                           /* x */ \
          MIN((int)(i + mult * H), (int)height - 1) * width }; /* x + mult */ \
    /* Peel column loop into left edge, center, and right edge to eliminate */ \
    /* MAX/MIN clamping from the performance-critical center region. */ \
    const size_t col_step = (size_t)(mult * H); \
    const size_t center_start = (col_step < width) ? col_step : width; \
    const size_t center_end = (width > col_step) ? (width - col_step) : 0; \
    const size_t right_start = (center_end > center_start) ? center_end : center_start; \
    /* Left edge: column neighbors need boundary clamping */ \
    for(size_t j = 0; j < center_start; ++j) \
    { \
      DIFFUSE_PIXEL_BODY((size_t)MAX((int)(j - mult * H), (int)0), \
                         MIN(j + col_step, width - 1), \
                         GRAD_ISOTROPIC, LAPL_ISOTROPIC, GRAD_ZERO, LAPL_ZERO) \
    } \
    /* Center: no boundary clamping needed, compiler can use fixed offsets */ \
    for(size_t j = center_start; j < center_end; ++j) \
    { \
      DIFFUSE_PIXEL_BODY(j - col_step, j + col_step, \
                         GRAD_ISOTROPIC, LAPL_ISOTROPIC, GRAD_ZERO, LAPL_ZERO) \
    } \
    /* Right edge: column neighbors need boundary clamping */ \
    for(size_t j = right_start; j < width; ++j) \
    { \
      DIFFUSE_PIXEL_BODY((size_t)MAX((int)(j - mult * H), (int)0), \
                         MIN(j + col_step, width - 1), \
                         GRAD_ISOTROPIC, LAPL_ISOTROPIC, GRAD_ZERO, LAPL_ZERO) \
    } \
  }

  const gboolean grad_orders_zero = (ABCD[0] == 0.f && ABCD[2] == 0.f);
  const gboolean lapl_orders_zero = (ABCD[1] == 0.f && ABCD[3] == 0.f);

#define DISPATCH_ROW_LOOP(GRAD_ISO, LAPL_ISO) \
  if (grad_orders_zero && lapl_orders_zero) { \
    DIFFUSE_ROW_LOOP(GRAD_ISO, LAPL_ISO, 1, 1) \
  } else if (grad_orders_zero && !lapl_orders_zero) { \
    DIFFUSE_ROW_LOOP(GRAD_ISO, LAPL_ISO, 1, 0) \
  } else if (!grad_orders_zero && lapl_orders_zero) { \
    DIFFUSE_ROW_LOOP(GRAD_ISO, LAPL_ISO, 0, 1) \
  } else { \
    DIFFUSE_ROW_LOOP(GRAD_ISO, LAPL_ISO, 0, 0) \
  }

  if(grad_is_isotropic && lapl_is_isotropic)
  {
    DISPATCH_ROW_LOOP(1, 1)
  }
  else if(grad_is_isotropic && !lapl_is_isotropic)
  {
    DISPATCH_ROW_LOOP(1, 0)
  }
  else if(!grad_is_isotropic && lapl_is_isotropic)
  {
    DISPATCH_ROW_LOOP(0, 1)
  }
  else
  {
    DISPATCH_ROW_LOOP(0, 0)
  }
  dt_omploop_sfence();
#undef DISPATCH_ROW_LOOP
#undef DIFFUSE_ROW_LOOP
#undef DIFFUSE_PIXEL_BODY
}

static inline float compute_anisotropy_factor(const float user_param)
{
  // compute the inverse of the K param in c evaluation from
  // https://www.researchgate.net/publication/220663968
  // but in a perceptually-even way, for better GUI interaction
  return sqf(user_param);
}

// Local version of decompose_2D_Bspline that uses nontemporal (streaming)
// stores for HF detail writes.  HF buffers are written once during
// decomposition and not read until the reconstruction pass after ALL
// scales have been decomposed, so the data is guaranteed to be evicted
// from cache before first read.  Nontemporal stores avoid the
// read-for-ownership cache line fetch that normal stores require.
DT_OMP_DECLARE_SIMD(aligned(in, HF, LF:64) aligned(tempbuf:16))
static inline void decompose_2D_Bspline_diffuse(const float *const restrict in,
                                                 float *const restrict HF,
                                                 float *const restrict LF,
                                                 const size_t width,
                                                 const size_t height,
                                                 const int mult,
                                                 float *const restrict tempbuf,
                                                 const size_t padded_size)
{
  // Blur and compute the decimated wavelet at once
  DT_OMP_FOR()
  for(size_t row = 0; row < height; row++)
  {
    // get a thread-private one-row temporary buffer
    float *restrict const temp = dt_get_perthread(tempbuf, padded_size);
    // interleave the order in which we process the rows so that we minimize cache misses
    const size_t i = dwt_interleave_rows(row, height, mult);
    // Convolve B-spline filter over columns: for each pixel in the current row, compute vertical blur
    _bspline_vertical_pass(in, temp, i, width, height, mult, TRUE); // always clip negatives
    // Convolve B-spline filter horizontally over current row
    for(size_t j = 0; j < width; j++)
    {
      const size_t index = 4U * (i * width + j);
      dt_aligned_pixel_t blur;
      _bspline_horizontal(temp, blur, j, width, mult, TRUE); // always clip negatives
      // Write LF (blur) normally — it is reused as input for the next scale
      for_four_channels(c)
        LF[index + c] = blur[c];
      // Write HF (detail) with nontemporal stores — not read until reconstruction
      dt_aligned_pixel_t detail;
      for_each_channel(c, aligned(detail, in : 64))
        detail[c] = in[index + c] - blur[c];
      copy_pixel_nontemporal(HF + index, detail);
    }
  }
  dt_omploop_sfence();
}

static inline gboolean wavelets_process(const float *const restrict in,
                                    float *const restrict reconstructed,
                                    const uint8_t *const restrict mask,
                                    const size_t width,
                                    const size_t height,
                                    const dt_iop_diffuse_data_t *const data,
                                    const float final_radius,
                                    const float zoom,
                                    const int scales,
                                    const gboolean has_mask,
                                    float *const restrict HF[MAX_NUM_SCALES],
                                    float *const restrict LF_odd,
                                    float *const restrict LF_even)
{
  gboolean success = TRUE;

  const dt_aligned_pixel_t anisotropy
      = { compute_anisotropy_factor(data->anisotropy_first),
          compute_anisotropy_factor(data->anisotropy_second),
          compute_anisotropy_factor(data->anisotropy_third),
          compute_anisotropy_factor(data->anisotropy_fourth) };

  const dt_isotropy_t DT_ALIGNED_PIXEL isotropy_type[4]
      = { check_isotropy_mode(data->anisotropy_first),
          check_isotropy_mode(data->anisotropy_second),
          check_isotropy_mode(data->anisotropy_third),
          check_isotropy_mode(data->anisotropy_fourth) };

  const float regularization = powf(10.f, data->regularization) - 1.f;
  const float variance_threshold = powf(10.f, data->variance_threshold);

  // À trous decimated wavelet decompose there is a paper from a guy
  // we know that explains it :
  // https://jo.dreggn.org/home/2010_atrous.pdf the wavelets
  // decomposition here is the same as the equalizer/atrous module,
  float *restrict residual; // will store the temp buffer containing the last step of blur
  // allocate a one-row temporary buffer for the decomposition
  size_t padded_size;
  float *const restrict tempbuf = dt_alloc_perthread_float(4 * width, &padded_size); //TODO: alloc in caller
  for(int s = 0; s < scales; ++s)
  {
    /* fprintf(stdout, "Wavelet decompose : scale %i\n", s); */
    const int mult = 1 << s;

    const float *restrict buffer_in;
    float *restrict buffer_out;

    if(s == 0)
    {
      buffer_in = in;
      buffer_out = LF_odd;
    }
    else if(s % 2 != 0)
    {
      buffer_in = LF_odd;
      buffer_out = LF_even;
    }
    else
    {
      buffer_in = LF_even;
      buffer_out = LF_odd;
    }

    decompose_2D_Bspline_diffuse(buffer_in, HF[s], buffer_out, width, height,
                                 mult, tempbuf, padded_size);

    residual = buffer_out;

    if(darktable.dump_pfm_module)
    {
      char name[64];
      sprintf(name, "scale-input-%i", s);
      dt_dump_pfm(name, buffer_in, width, height, 4 * sizeof(float), "diffuse");

      sprintf(name, "scale-blur-%i", s);
      dt_dump_pfm(name, buffer_out, width, height, 4 * sizeof(float), "diffuse");
    }
  }
  dt_free_align(tempbuf);

  // will store the temp buffer NOT containing the last step of blur
  float *restrict temp = (residual == LF_even) ? LF_odd : LF_even;

  int count = 0;
  for(int s = scales - 1; s > -1; --s)
  {
    const int mult = 1 << s;
    const float current_radius = equivalent_sigma_at_step(B_SPLINE_SIGMA, s);
    const float real_radius = current_radius * zoom;

    const float norm =
      expf(-sqf(real_radius - (float)data->radius_center) / sqf(data->radius));
    const dt_aligned_pixel_t ABCD = { data->first * KAPPA * norm,
                                      data->second * KAPPA * norm,
                                      data->third * KAPPA * norm,
                                      data->fourth * KAPPA * norm };
    const float strength = data->sharpness * norm + 1.f;

    /* debug
    fprintf(stdout, "PDE solve : scale %i : mult = %i ; current rad = %.0f ; real rad = %.0f ; norm = %f ; strength = %f\n", s,
            1 << s, current_radius, real_radius, norm, strength);
    */

    const float *restrict buffer_in;
    float *restrict buffer_out;

    if(count == 0)
    {
      buffer_in = residual;
      buffer_out = temp;
    }
    else if(count % 2 != 0)
    {
      buffer_in = temp;
      buffer_out = residual;
    }
    else
    {
      buffer_in = residual;
      buffer_out = temp;
    }

    if(s == 0) buffer_out = reconstructed;

    // Check whether the Gaussian norm weight makes the PDE correction negligible
    // for this scale. When the scale is far from the user's center radius, norm
    // decays exponentially toward zero, making all ABCD speeds ~0 and strength ~1.
    // In that case we can skip the expensive PDE solver entirely and just add
    // HF[s] + buffer_in (the identity reconstruction).
    const gboolean negligible_scale =
        (fabsf(ABCD[0]) < FLT_EPSILON
      && fabsf(ABCD[1]) < FLT_EPSILON
      && fabsf(ABCD[2]) < FLT_EPSILON
      && fabsf(ABCD[3]) < FLT_EPSILON
      && fabsf(strength - 1.0f) < FLT_EPSILON);

    if(negligible_scale)
    {
      // Fast path: just reconstruct output = HF[s] + buffer_in with nontemporal stores
      const float *const restrict hf = HF[s];
      const float *const restrict lf = buffer_in;
      float *const restrict out = buffer_out;
      DT_OMP_FOR()
      for(size_t k = 0; k < (size_t)height * width * 4; k += 4)
      {
        dt_aligned_pixel_t pixel;
        for_each_channel(c, aligned(pixel, hf, lf : 64))
          pixel[c] = hf[k + c] + lf[k + c];
        copy_pixel_nontemporal(out + k, pixel);
      }
      dt_omploop_sfence();
    }
    else
    {
      // Compute wavelets low-frequency scales
      heat_PDE_diffusion(HF[s], buffer_in, mask, has_mask, buffer_out, width, height,
                         anisotropy, isotropy_type, regularization,
                         variance_threshold, sqf(current_radius), mult, ABCD, strength);
    }

    if(darktable.dump_pfm_module)
    {
      char name[64];
      sprintf(name, "scale-up-unblur-%i", s);
      dt_dump_pfm(name, buffer_out, width, height,  4 * sizeof(float), "diffuse");
    }
    count++;
  }

  return success;
}


static inline void build_mask(const float *const restrict input,
                              uint8_t *const restrict mask,
                              const float threshold,
                              const size_t width,
                              const size_t height)
{
  DT_OMP_FOR_SIMD(aligned(mask, input : 64))
  for(size_t k = 0; k < height * width * 4; k += 4)
  {
    // TRUE if any channel is above threshold
    mask[k / 4] = (input[k] > threshold || input[k + 1] > threshold || input[k + 2] > threshold);
  }
}

static inline void inpaint_mask(float *const restrict inpainted,
                                const float *const restrict original,
                                const uint8_t *const restrict mask,
                                const size_t width,
                                const size_t height)
{
  // init the reconstruction with noise inside the masked areas
  DT_OMP_FOR()
  for(size_t k = 0; k < height * width * 4; k += 4)
  {
    if(mask[k / 4])
    {
      const uint32_t i = k / width;
      const uint32_t j = k - i;
      uint32_t DT_ALIGNED_ARRAY state[4]
          = { splitmix32(j + 1), splitmix32((uint64_t)(j + 1) * (i + 3)),
              splitmix32(1337), splitmix32(666) };
      xoshiro128plus(state);
      xoshiro128plus(state);
      xoshiro128plus(state);
      xoshiro128plus(state);

      for_four_channels(c, aligned(inpainted, original, state:64))
        inpainted[k + c] = fabsf(gaussian_noise(original[k + c],
                                                original[k + c], i % 2 || j % 2, state));
    }
    else
    {
      for_four_channels(c, aligned(original, inpainted:64))
        inpainted[k + c] = original[k + c];
    }
  }
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const restrict ivoid,
             void *const restrict ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const gboolean fastmode = piece->pipe->type & DT_DEV_PIXELPIPE_FAST;

  const dt_iop_diffuse_data_t *const data = piece->data;

  const size_t width = roi_out->width;
  const size_t height = roi_out->height;

  // allow fast mode, just copy input to output
  if(fastmode)
  {
    const size_t ch = piece->colors;
    dt_iop_copy_image_roi(ovoid, ivoid, ch, roi_in, roi_out);
    return;
  }

  uint8_t *const restrict mask = dt_alloc_align_uint8(width * height);

  float *restrict in = DT_IS_ALIGNED((float *const restrict)ivoid);
  float *const restrict out = DT_IS_ALIGNED((float *const restrict)ovoid);

  float *restrict temp1, *restrict temp2;
  // temp buffer for blurs. We will need to cycle between them for memory efficiency
  float *restrict LF_odd, *restrict LF_even;

  float *restrict temp_in = NULL;
  float *restrict temp_out = NULL;

  gboolean out_of_memory = !mask
    || !dt_iop_alloc_image_buffers(self, roi_in, roi_out,
                                 4 | DT_IMGSZ_OUTPUT, &temp1,
                                 4 | DT_IMGSZ_OUTPUT, &temp2,
                                 4 | DT_IMGSZ_OUTPUT, &LF_odd,
                                 4 | DT_IMGSZ_OUTPUT, &LF_even,
                                 0, NULL); // if failing all pointers are NULL

  const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);
  const float final_radius = (data->radius + data->radius_center) * 2.f / scale;

  const int iterations = MAX(ceilf((float)data->iterations), 1);
  const int diffusion_scales = num_steps_to_reach_equivalent_sigma(B_SPLINE_SIGMA, final_radius);
  const int scales = CLAMP(diffusion_scales, 1, MAX_NUM_SCALES);

  // Pre-compute effective number of scales by trimming contiguous negligible
  // scales at the deep end.  For those scales the Gaussian norm weight decays
  // to zero, making all PDE speeds ~0 and sharpness strength ~1, so
  // decompose + reconstruct is the identity.  Skipping them saves HF
  // allocation, HF-write bandwidth, blur computation, and reconstruction I/O.
  int effective_scales = scales;
  for(int s = scales - 1; s >= 1; s--)
  {
    const float current_radius = equivalent_sigma_at_step(B_SPLINE_SIGMA, s);
    const float real_radius = current_radius * scale;
    const float norm =
      expf(-sqf(real_radius - (float)data->radius_center) / sqf(data->radius));
    if(fabsf(data->first * KAPPA * norm) < FLT_EPSILON
    && fabsf(data->second * KAPPA * norm) < FLT_EPSILON
    && fabsf(data->third * KAPPA * norm) < FLT_EPSILON
    && fabsf(data->fourth * KAPPA * norm) < FLT_EPSILON
    && fabsf(data->sharpness * norm) < FLT_EPSILON)
      effective_scales = s;
    else
      break;
  }

  // wavelets scales buffers — only allocate for effective (non-negligible) scales
  float *restrict HF[MAX_NUM_SCALES];
  for(int s = 0; s < effective_scales; s++)
  {
    HF[s] = out_of_memory ? NULL : dt_alloc_align_float(width * height * 4);
    if(!HF[s]) out_of_memory = TRUE;
  }
  for(int s = effective_scales; s < scales; s++)
    HF[s] = NULL;

  // check that all buffers exist before processing because we use a lot of memory here.
  if(out_of_memory)
  {
    dt_iop_copy_image_roi(ovoid, ivoid, piece->colors, roi_in, roi_out);
    dt_control_log(_("diffuse/sharpen failed to allocate memory, check your RAM settings"));
    goto finish;
  }

  const gboolean has_mask = (data->threshold > 0.f);
  if(has_mask)
  {
    // build a boolean mask, TRUE where image is above threshold, FALSE otherwise
    build_mask(in, mask, data->threshold, roi_out->width, roi_out->height);

    // init the inpainting area with noise
    inpaint_mask(temp1, in, mask, roi_out->width, roi_out->height);

    in = temp1;
  }

  for(int it = 0; it < iterations; it++)
  {
    if(it == 0)
    {
      temp_in = in;
      temp_out = temp2;
    }
    else if(it % 2 == 0)
    {
      temp_in = temp1;
      temp_out = temp2;
    }
    else
    {
      temp_in = temp2;
      temp_out = temp1;
    }

    if(it == iterations - 1)
      temp_out = out;

    wavelets_process(temp_in, temp_out, mask,
                     roi_out->width, roi_out->height,
                     data, final_radius, scale, effective_scales, has_mask, HF, LF_odd, LF_even);
  }

finish:
  dt_free_align(mask);
  dt_free_align(temp1);
  dt_free_align(temp2);
  dt_free_align(LF_even);
  dt_free_align(LF_odd);
  for(int s = 0; s < effective_scales; s++)
    if(HF[s]) dt_free_align(HF[s]);
}

#if HAVE_OPENCL
static inline cl_int wavelets_process_cl(const int devid,
                                         cl_mem in,
                                         cl_mem reconstructed,
                                         cl_mem mask,
                                         const int width,
                                         const int height,
                                         const dt_iop_diffuse_data_t *const data,
                                         dt_iop_diffuse_global_data_t *const gd,
                                         const float final_radius,
                                         const float zoom, const int scales,
                                         const int has_mask,
                                         cl_mem HF[MAX_NUM_SCALES],
                                         cl_mem LF_odd,
                                         cl_mem LF_even)
{
  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  const dt_aligned_pixel_t anisotropy
      = { compute_anisotropy_factor(data->anisotropy_first),
          compute_anisotropy_factor(data->anisotropy_second),
          compute_anisotropy_factor(data->anisotropy_third),
          compute_anisotropy_factor(data->anisotropy_fourth) };

  /*
  fprintf(stdout, "anisotropy : %f ; %f ; %f ; %f \n",
                  anisotropy[0], anisotropy[1], anisotropy[2], anisotropy[3]);
  */

  const dt_isotropy_t DT_ALIGNED_PIXEL isotropy_type[4]
      = { check_isotropy_mode(data->anisotropy_first),
          check_isotropy_mode(data->anisotropy_second),
          check_isotropy_mode(data->anisotropy_third),
          check_isotropy_mode(data->anisotropy_fourth) };

  /*
  fprintf(stdout, "type : %d ; %d ; %d ; %d \n",
                  isotropy_type[0], isotropy_type[1], isotropy_type[2], isotropy_type[3]);
  */

  const float regularization = powf(10.f, data->regularization) - 1.f;
  const float variance_threshold = powf(10.f, data->variance_threshold);


  // À trous wavelet decompose
  // there is a paper from a guy we know that explains it :
  // https://jo.dreggn.org/home/2010_atrous.pdf
  // the wavelets decomposition here is the same as the equalizer/atrous module,
  cl_mem residual;
  for(int s = 0; s < scales; ++s)
  {
    const int mult = 1 << s;

    cl_mem buffer_in;
    cl_mem buffer_out;

    if(s == 0)
    {
      buffer_in = in;
      buffer_out = LF_odd;
    }
    else if(s % 2 != 0)
    {
      buffer_in = LF_odd;
      buffer_out = LF_even;
    }
    else
    {
      buffer_in = LF_even;
      buffer_out = LF_odd;
    }

    // Compute wavelets low-frequency scales
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_filmic_bspline_horizontal, width, height,
                              CLARG(buffer_in), CLARG(HF[s]),
                              CLARG(width), CLARG(height), CLARG(mult));
    if(err != CL_SUCCESS) return err;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_filmic_bspline_vertical, width, height,
                              CLARG(HF[s]), CLARG(buffer_out),
                              CLARG(width), CLARG(height), CLARG(mult));
    if(err != CL_SUCCESS) return err;

    // Compute wavelets high-frequency scales and backup the maximum
    // of texture over the RGB channels Note : HF = detail - LF
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_filmic_wavelets_detail, width, height,
                              CLARG(buffer_in), CLARG(buffer_out),
                              CLARG(HF[s]), CLARG(width), CLARG(height));
    if(err != CL_SUCCESS) return err;

    residual = buffer_out;
  }

  // will store the temp buffer NOT containing the last step of blur
  cl_mem temp = (residual == LF_even) ? LF_odd : LF_even;

  int count = 0;
  for(int s = scales - 1; s > -1; --s)
  {
    const int mult = 1 << s;
    const float current_radius = equivalent_sigma_at_step(B_SPLINE_SIGMA, s);
    const float real_radius = current_radius * zoom;
    const float current_radius_square = sqf(current_radius);

    const float norm =
      expf(-sqf(real_radius - (float)data->radius_center) / sqf(data->radius));
    const dt_aligned_pixel_t ABCD = { data->first * KAPPA * norm,
                                      data->second * KAPPA * norm,
                                      data->third * KAPPA * norm,
                                      data->fourth * KAPPA * norm };
    const float strength = data->sharpness * norm + 1.f;

    cl_mem buffer_in;
    cl_mem buffer_out;

    if(count == 0)
    {
      buffer_in = residual;
      buffer_out = temp;
    }
    else if(count % 2 != 0)
    {
      buffer_in = temp;
      buffer_out = residual;
    }
    else
    {
      buffer_in = residual;
      buffer_out = temp;
    }

    if(s == 0) buffer_out = reconstructed;

    // Compute wavelets low-frequency scales
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_diffuse_pde, width, height,
                              CLARG(HF[s]), CLARG(buffer_in), CLARG(mask),
                              CLARG(has_mask), CLARG(buffer_out),
                              CLARG(width), CLARG(height),
                              CLARG(anisotropy), CLARG(isotropy_type),
                              CLARG(regularization), CLARG(variance_threshold),
                              CLARG(current_radius_square), CLARG(mult), CLARG(ABCD),
                              CLARG(strength));
    if(err != CL_SUCCESS) return err;

    count++;
  }

  return err;
}

int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  const gboolean fastmode = piece->pipe->type & DT_DEV_PIXELPIPE_FAST;

  const dt_iop_diffuse_data_t *const data = piece->data;
  dt_iop_diffuse_global_data_t *const gd = self->global_data;

  gboolean out_of_memory = FALSE;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t origin[] = { 0, 0, 0 };
  size_t region[] = { width, height, 1 };

  // allow fast mode, just copy input to output
  if(fastmode)
    return dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);

  size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  cl_mem in = dev_in;
  cl_mem temp_in = NULL;
  cl_mem temp_out = NULL;

  cl_mem temp1 = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
  cl_mem temp2 = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
  cl_mem mask = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(uint8_t));
  // temp buffer for blurs. We will need to cycle between them for memory efficiency
  cl_mem LF_even = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
  cl_mem LF_odd = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);

  const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);
  const float final_radius = (data->radius + data->radius_center) * 2.f / scale;

  const int iterations = MAX(ceilf((float)data->iterations), 1);
  const int diffusion_scales = num_steps_to_reach_equivalent_sigma(B_SPLINE_SIGMA, final_radius);
  const int scales = CLAMP(diffusion_scales, 1, MAX_NUM_SCALES);

  // wavelets scales buffers
  cl_mem HF[MAX_NUM_SCALES];
  for(int s = 0; s < scales; s++)
  {
    HF[s] = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
    if(!HF[s]) out_of_memory = TRUE;
  }

  // check that all buffers exist before processing,
  // because we use a lot of memory here.
  if(!temp1 || !temp2 || !LF_odd || !LF_even || !mask || out_of_memory)
  {
    dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto error;
  }

  const gboolean has_mask = (data->threshold > 0.f);
  if(has_mask)
  {
    // build a boolean mask, TRUE where image is above threshold, FALSE otherwise
    // FIXME OPENCL these kernels don't check for width & height
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_diffuse_build_mask, width, height,
                              CLARG(in), CLARG(mask), CLARG(data->threshold),
                              CLARG(roi_out->width), CLARG(roi_out->height));
    if(err != CL_SUCCESS) goto error;

    // init the inpainting area with noise
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_diffuse_inpaint_mask, width, height,
                              CLARG(temp1), CLARG(in), CLARG(mask),
                              CLARG(roi_out->width), CLARG(roi_out->height));

    if(err != CL_SUCCESS) goto error;

    in = temp1;
  }

  for(int it = 0; it < iterations; it++)
  {
    if(it == 0)
    {
      temp_in = in;
      temp_out = temp2;
    }
    else if(it % 2 == 0)
    {
      temp_in = temp1;
      temp_out = temp2;
    }
    else
    {
      temp_in = temp2;
      temp_out = temp1;
    }

    if(it == iterations - 1)
      temp_out = dev_out;
    err = wavelets_process_cl(devid, temp_in, temp_out, mask,
                              width, height, data, gd, final_radius,
                              scale, scales, has_mask, HF, LF_odd, LF_even);
  }

error:
  dt_opencl_release_mem_object(temp1);
  dt_opencl_release_mem_object(temp2);
  dt_opencl_release_mem_object(mask);
  dt_opencl_release_mem_object(LF_even);
  dt_opencl_release_mem_object(LF_odd);
  for(int s = 0; s < scales; s++)
    dt_opencl_release_mem_object(HF[s]);
  return err;
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 33; // extended.cl in programs.conf
  dt_iop_diffuse_global_data_t *gd = malloc(sizeof(dt_iop_diffuse_global_data_t));

  self->data = gd;
  gd->kernel_diffuse_build_mask = dt_opencl_create_kernel(program, "build_mask");
  gd->kernel_diffuse_inpaint_mask = dt_opencl_create_kernel(program, "inpaint_mask");
  gd->kernel_diffuse_pde = dt_opencl_create_kernel(program, "diffuse_pde");

  const int wavelets = 35; // bspline.cl, from programs.conf
  gd->kernel_filmic_bspline_horizontal =
    dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_horizontal");
  gd->kernel_filmic_bspline_vertical =
    dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_vertical");
  gd->kernel_filmic_wavelets_detail =
    dt_opencl_create_kernel(wavelets, "wavelets_detail_level");
}


void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_diffuse_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_diffuse_build_mask);
  dt_opencl_free_kernel(gd->kernel_diffuse_inpaint_mask);
  dt_opencl_free_kernel(gd->kernel_diffuse_pde);

  dt_opencl_free_kernel(gd->kernel_filmic_bspline_vertical);
  dt_opencl_free_kernel(gd->kernel_filmic_bspline_horizontal);
  dt_opencl_free_kernel(gd->kernel_filmic_wavelets_detail);
  free(self->data);
  self->data = NULL;
}
#endif


void gui_init(dt_iop_module_t *self)
{
  dt_iop_diffuse_gui_data_t *g = IOP_GUI_ALLOC(diffuse);

  self->widget = dt_gui_vbox(dt_ui_section_label_new(C_("section", "properties")));

  g->iterations = dt_bauhaus_slider_from_params(self, "iterations");
  dt_bauhaus_slider_set_soft_range(g->iterations, 1., 128);
  gtk_widget_set_tooltip_text
    (g->iterations,
     _("more iterations make the effect stronger but the module slower.\n"
       "this is analogous to giving more time to the diffusion reaction.\n"
       "if you plan on sharpening or inpainting, \n"
       "more iterations help reconstruction."));

  g->radius_center = dt_bauhaus_slider_from_params(self, "radius_center");
  dt_bauhaus_slider_set_soft_range(g->radius_center, 0., 512.);
  dt_bauhaus_slider_set_format(g->radius_center, _(" px"));
  gtk_widget_set_tooltip_text(
      g->radius_center, _("main scale of the diffusion.\n"
                          "zero makes diffusion act on the finest details more heavily.\n"
                          "non-zero defines the size of the details to diffuse heavily.\n"
                          "for deblurring and denoising, set to zero.\n"
                          "increase to act on local contrast instead."));

  g->radius = dt_bauhaus_slider_from_params(self, "radius");
  dt_bauhaus_slider_set_soft_range(g->radius, 1., 512.);
  dt_bauhaus_slider_set_format(g->radius, _(" px"));
  gtk_widget_set_tooltip_text(
      g->radius, _("width of the diffusion around the central radius.\n"
                   "high values diffuse on a large band of radii.\n"
                   "low values diffuse closer to the central radius.\n"
                   "if you plan on deblurring, \n"
                   "the radius should be around the width of your lens blur."));

  dt_gui_box_add(self->widget, dt_ui_section_label_new(C_("section", "speed (sharpen ↔ diffuse)")));

  g->first = dt_bauhaus_slider_from_params(self, "first");
  dt_bauhaus_slider_set_digits(g->first, 4);
  dt_bauhaus_slider_set_format(g->first, "%");
  gtk_widget_set_tooltip_text
    (g->first, _("diffusion speed of low-frequency wavelet layers\n"
                 "in the direction of 1st order anisotropy (set below).\n\n"
                 "negative values sharpen, \n"
                 "positive values diffuse and blur, \n"
                 "zero does nothing."));

  g->second = dt_bauhaus_slider_from_params(self, "second");
  dt_bauhaus_slider_set_digits(g->second, 4);
  dt_bauhaus_slider_set_format(g->second, "%");
  gtk_widget_set_tooltip_text
    (g->second, _("diffusion speed of low-frequency wavelet layers\n"
                  "in the direction of 2nd order anisotropy (set below).\n\n"
                  "negative values sharpen, \n"
                  "positive values diffuse and blur, \n"
                  "zero does nothing."));

  g->third = dt_bauhaus_slider_from_params(self, "third");
  dt_bauhaus_slider_set_digits(g->third, 4);
  dt_bauhaus_slider_set_format(g->third, "%");
  gtk_widget_set_tooltip_text
    (g->third, _("diffusion speed of high-frequency wavelet layers\n"
                 "in the direction of 3rd order anisotropy (set below).\n\n"
                 "negative values sharpen, \n"
                 "positive values diffuse and blur, \n"
                 "zero does nothing."));

  g->fourth = dt_bauhaus_slider_from_params(self, "fourth");
  dt_bauhaus_slider_set_digits(g->fourth, 4);
  dt_bauhaus_slider_set_format(g->fourth, "%");
  gtk_widget_set_tooltip_text
    (g->fourth, _("diffusion speed of high-frequency wavelet layers\n"
                  "in the direction of 4th order anisotropy (set below).\n\n"
                  "negative values sharpen, \n"
                  "positive values diffuse and blur, \n"
                  "zero does nothing."));

  dt_gui_box_add(self->widget, dt_ui_section_label_new(C_("section", "direction")));

  g->anisotropy_first = dt_bauhaus_slider_from_params(self, "anisotropy_first");
  dt_bauhaus_slider_set_digits(g->anisotropy_first, 4);
  dt_bauhaus_slider_set_format(g->anisotropy_first, "%");
  gtk_widget_set_tooltip_text
    (g->anisotropy_first, _("direction of 1st order speed (set above).\n\n"
                            "negative values follow gradients more closely, \n"
                            "positive values rather avoid edges (isophotes), \n"
                            "zero affects both equally (isotropic)."));

  g->anisotropy_second = dt_bauhaus_slider_from_params(self, "anisotropy_second");
  dt_bauhaus_slider_set_digits(g->anisotropy_second, 4);
  dt_bauhaus_slider_set_format(g->anisotropy_second, "%");
  gtk_widget_set_tooltip_text
    (g->anisotropy_second,_("direction of 2nd order speed (set above).\n\n"
                            "negative values follow gradients more closely, \n"
                            "positive values rather avoid edges (isophotes), \n"
                            "zero affects both equally (isotropic)."));

  g->anisotropy_third = dt_bauhaus_slider_from_params(self, "anisotropy_third");
  dt_bauhaus_slider_set_digits(g->anisotropy_third, 4);
  dt_bauhaus_slider_set_format(g->anisotropy_third, "%");
  gtk_widget_set_tooltip_text
    (g->anisotropy_third,_("direction of 3rd order speed (set above).\n\n"
                           "negative values follow gradients more closely, \n"
                           "positive values rather avoid edges (isophotes), \n"
                           "zero affects both equally (isotropic)."));

  g->anisotropy_fourth = dt_bauhaus_slider_from_params(self, "anisotropy_fourth");
  dt_bauhaus_slider_set_digits(g->anisotropy_fourth, 4);
  dt_bauhaus_slider_set_format(g->anisotropy_fourth, "%");
  gtk_widget_set_tooltip_text
    (g->anisotropy_fourth,_("direction of 4th order speed (set above).\n\n"
                            "negative values follow gradients more closely, \n"
                            "positive values rather avoid edges (isophotes), \n"
                            "zero affects both equally (isotropic)."));

  dt_gui_box_add(self->widget, dt_ui_section_label_new(C_("section", "edge management")));

  g->sharpness = dt_bauhaus_slider_from_params(self, "sharpness");
  dt_bauhaus_slider_set_digits(g->sharpness, 3);
  dt_bauhaus_slider_set_soft_range(g->sharpness, -0.25, 0.25);
  dt_bauhaus_slider_set_format(g->sharpness, "%");
  gtk_widget_set_tooltip_text
    (g->sharpness,
     _("increase or decrease the sharpness of the highest frequencies.\n"
       "can be used to keep details after blooming,\n"
       "for standalone sharpening set speed to negative values."));

  g->regularization = dt_bauhaus_slider_from_params(self, "regularization");
  gtk_widget_set_tooltip_text
    (g->regularization,
     _("define the sensitivity of the variance penalty for edges.\n"
       "increase to exclude more edges from diffusion,\n"
       "if fringes or halos appear."));

  g->variance_threshold = dt_bauhaus_slider_from_params(self, "variance_threshold");
  gtk_widget_set_tooltip_text
    (g->variance_threshold,
     _("define the variance threshold between edge amplification and penalty.\n"
       "decrease if you want pixels on smooth surfaces get a boost,\n"
       "increase if you see noise appear on smooth surfaces or\n"
       "if dark areas seem oversharpened compared to bright areas."));

  dt_gui_box_add(self->widget, dt_ui_section_label_new(C_("section", "diffusion spatiality")));

  g->threshold = dt_bauhaus_slider_from_params(self, "threshold");
  dt_bauhaus_slider_set_format(g->threshold, "%");
  gtk_widget_set_tooltip_text
    (g->threshold,
     _("luminance threshold for the mask.\n"
       "0. disables the luminance masking and applies the module on the whole image.\n"
       "any higher value excludes pixels with luminance lower than the threshold.\n"
       "this can be used to inpaint highlights."));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
