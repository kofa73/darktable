/*
    This file is part of darktable,
    Copyright (C) 2022-2026 darktable developers.

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

/* The refavg values are calculated in raw-RGB-cube3 space
   We calculate all color channels in the 3x3 photosite area, this can be understaood as a "superpixel",
   the "asking" location is in the centre.
   As this works for bayer and xtrans sensors we don't have a fixed ratio but calculate the average
   for every color channel first.
   refavg for one of red, green or blue is defined as means of both other color channels (opposing).

   The basic idea / observation for the _process_opposed algorithm is, the refavg is a good estimate
   for any clipped color channel in the vast majority of images, working mostly fine both for small specular
   highlighted spots and large areas.

   The correction via some sort of global chrominance further helps to correct color casts.
   The chrominace data are taken from the areas morphologically very close to clipped data.
   Failures of the algorithm (color casts) are in most cases related to
    a) very large differences between optimal white balance coefficients vs what we have as D65 in the darktable pipeline
    b) complicated lightings so the gradients are not well related
    c) a wrong whitepoint setting in the rawprepare module.
    d) the maths might not be best

   Again the algorithm has been developed in collaboration by @garagecoder and @Iain from gmic team and @jenshannoschwalm from dt.
*/

static dt_hash_t _opposed_hash(dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_highlights_data_t *d = piece->data;
  dt_hash_t hash = dt_dev_pixelpipe_piece_hash(piece, NULL, FALSE);
  hash = dt_hash(hash, &d->clip, sizeof(d->clip));
  hash = dt_hash(hash, &piece->module->dev->chroma.late_correction, sizeof(int));
  const gboolean cbrt_space = d->mode == DT_IOP_HIGHLIGHTS_OPPOSED && d->opp_cbrt;
  hash = dt_hash(hash, &cbrt_space, sizeof(cbrt_space));
  const gboolean local_chroma = d->mode == DT_IOP_HIGHLIGHTS_OPPOSED && piece->filters != 0
                                && d->opp_local == DT_HIGHLIGHTS_OPP_CHROMA_PER_REGION;
  return dt_hash(hash, &local_chroma, sizeof(local_chroma));
}

static inline float _calc_linear_refavg(const float *in, const int color)
{
  const dt_aligned_pixel_t ins = { cbrtf(fmaxf(0.0f, in[0])), cbrtf(fmaxf(0.0f, in[1])), cbrtf(fmaxf(0.0f, in[2])), 0.0f };
  const dt_aligned_pixel_t opp = { 0.5f*(ins[1]+ins[2]), 0.5f*(ins[0]+ins[2]), 0.5f*(ins[0]+ins[1]), 0.0f};

  return fcube(opp[color]);
}

static inline size_t _raw_to_cmap(const size_t width, const size_t row, const size_t col)
{
  return (row / 3) * width + (col / 3);
}

static inline float _opp_estimate(const float *in,
                                  const uint8_t(*const xtrans)[6],
                                  const uint32_t filters,
                                  const int row,
                                  const int col,
                                  const dt_iop_roi_t *const roi,
                                  const dt_aligned_pixel_t correction,
                                  const float chroma,
                                  const gboolean cbrt_space)
{
  if(cbrt_space)
    return fcube(fmaxf(0.0f, _calc_refavg(in, xtrans, filters, row, col, roi, correction, FALSE) + chroma));
  return _calc_refavg(in, xtrans, filters, row, col, roi, correction, TRUE) + chroma;
}

static inline char _mask_dilated(const char *in, const size_t w1)
{
  if(in[0])
    return 1;

  if(in[-w1-1] | in[-w1] | in[-w1+1] | in[-1] | in[1] | in[w1-1] | in[w1] | in[w1+1])
    return 1;

  const size_t w2 = 2*w1;
  const size_t w3 = 3*w1;
  return (in[-w3-2] | in[-w3-1] | in[-w3]   | in[-w3+1] | in[-w3+2] |
          in[-w2-3] | in[-w2-2] | in[-w2-1] | in[-w2]   | in[-w2+1] | in[-w2+2] | in[-w2+3] |
          in[-w1-3] | in[-w1-2] | in[-w1+2] | in[-w1+3] |
          in[-3]    | in[-2]    | in[2]     | in[3]     |
          in[w1-3]  | in[w1-2]  | in[w1+2]  | in[w1+3]  |
          in[w2-3]  | in[w2-2]  | in[w2-1]  | in[w2]    | in[w2+1]  | in[w2+2]  | in[w2+3] |
          in[w3-2]  | in[w3-1]  | in[w3]    | in[w3+1]  | in[w3+2]) ? 1 : 0;
}

typedef struct opp_region_chroma_t
{
  double sum[3];
  double count[3];
  float offset[3];
} opp_region_chroma_t;

static uint32_t *_opp_region_labels(const char *mask,
                                    const size_t mwidth,
                                    const size_t mheight,
                                    const size_t msize,
                                    size_t *region_count)
{
  *region_count = 0;
  if(mwidth == 0 || mheight == 0 || mwidth > SIZE_MAX / mheight
     || msize > SIZE_MAX / sizeof(uint32_t))
    return NULL;

  const size_t cell_count = mwidth * mheight;
  if(cell_count > SIZE_MAX / sizeof(size_t))
    return NULL;

  uint32_t *labels = calloc(msize, sizeof(*labels));
  uint32_t *grown = calloc(msize, sizeof(*grown));
  size_t *queue = malloc(cell_count * sizeof(*queue));
  if(!labels || !grown || !queue)
  {
    free(labels);
    free(grown);
    free(queue);
    return NULL;
  }

  uint32_t label_count = 0;
  for(size_t row = 0; row < mheight; row++)
  {
    for(size_t col = 0; col < mwidth; col++)
    {
      const size_t cell = row * mwidth + col;
      if(labels[cell] || !(mask[cell] || mask[msize + cell] || mask[2 * msize + cell]))
        continue;
      if(label_count == UINT32_MAX)
      {
        free(labels);
        free(grown);
        free(queue);
        return NULL;
      }

      const uint32_t label = ++label_count;
      size_t head = 0;
      size_t tail = 0;
      labels[cell] = label;
      queue[tail++] = cell;
      while(head < tail)
      {
        const size_t current = queue[head++];
        const size_t current_row = current / mwidth;
        const size_t current_col = current % mwidth;
        const size_t row0 = current_row > 0 ? current_row - 1 : current_row;
        const size_t row1 = MIN(current_row + 1, mheight - 1);
        const size_t col0 = current_col > 0 ? current_col - 1 : current_col;
        const size_t col1 = MIN(current_col + 1, mwidth - 1);
        for(size_t r = row0; r <= row1; r++)
        {
          for(size_t c = col0; c <= col1; c++)
          {
            const size_t neighbor = r * mwidth + c;
            if(!labels[neighbor]
               && (mask[neighbor] || mask[msize + neighbor] || mask[2 * msize + neighbor]))
            {
              labels[neighbor] = label;
              queue[tail++] = neighbor;
            }
          }
        }
      }
    }
  }

  memcpy(grown, labels, msize * sizeof(*grown));
  // clipped cells keep their labels; row-major seeds claim empty overlap cells
  for(size_t row = 0; row < mheight; row++)
  {
    for(size_t col = 0; col < mwidth; col++)
    {
      const size_t cell = row * mwidth + col;
      const uint32_t label = labels[cell];
      if(!label) continue;

      const size_t row0 = row > 3 ? row - 3 : 0;
      const size_t row1 = MIN(row + 3, mheight - 1);
      const size_t col0 = col > 3 ? col - 3 : 0;
      const size_t col1 = MIN(col + 3, mwidth - 1);
      for(size_t r = row0; r <= row1; r++)
      {
        const int dy = (int)r - (int)row;
        for(size_t c = col0; c <= col1; c++)
        {
          const int dx = (int)c - (int)col;
          if((dy == -3 || dy == 3) && (dx == -3 || dx == 3)) continue;
          const size_t grown_cell = r * mwidth + c;
          if(!grown[grown_cell]) grown[grown_cell] = label;
        }
      }
    }
  }

  free(labels);
  free(queue);
  *region_count = label_count;
  return grown;
}

static inline float _opp_chroma_for(const dt_aligned_pixel_t chrominance,
                                    const uint32_t *grown_regions,
                                    const opp_region_chroma_t *region_chrominance,
                                    const size_t mwidth,
                                    const size_t mheight,
                                    const size_t row,
                                    const size_t col,
                                    const int color)
{
  if(grown_regions && region_chrominance)
  {
    const size_t mrow = row / 3;
    const size_t mcol = col / 3;
    if(mrow < mheight && mcol < mwidth)
    {
      const uint32_t label = grown_regions[mrow * mwidth + mcol];
      if(label) return region_chrominance[label].offset[color];
    }
  }
  return chrominance[color];
}


// A slightly modified version for sraws
static void _process_linear_opposed(dt_iop_module_t *self,
                                    dt_dev_pixelpipe_iop_t *piece,
                                    const float *const input,
                                    float *const output,
                                    const dt_iop_roi_t *const roi_in)
{
  dt_iop_highlights_data_t *d = piece->data;
  dt_iop_highlights_gui_data_t *g = self->gui_data;
  const float clipval = highlights_clip_magics[DT_IOP_HIGHLIGHTS_OPPOSED] * d->clip;
  const dt_iop_buffer_dsc_t *dsc = &piece->pipe->dsc;
  const gboolean wbon = dsc->temperature.enabled;
  const dt_aligned_pixel_t icoeffs = { wbon ? dsc->temperature.coeffs[0] : 1.0f,
                                       wbon ? dsc->temperature.coeffs[1] : 1.0f,
                                       wbon ? dsc->temperature.coeffs[2] : 1.0f};
  const dt_aligned_pixel_t clips = { clipval * icoeffs[0], clipval * icoeffs[1], clipval * icoeffs[2]};

  const size_t width = roi_in->width;
  const size_t height = roi_in->height;
  const size_t mwidth  = width / 3;
  const size_t mheight = height / 3;
  const size_t msize = dt_round_size((size_t) mwidth+2, 8) * dt_round_size(mheight+2, 8);

  dt_aligned_pixel_t chrominance = {0.0f, 0.0f, 0.0f, 0.0f};
  const dt_hash_t opphash = _opposed_hash(piece);
  const gboolean fullpipe = dt_pipe_is_full(piece->pipe);

  gboolean is_hashed = FALSE;
  gboolean unclipped = FALSE;
  if(self->dev->gui_attached && g)
  {
    dt_iop_gui_enter_critical_section(self);
    is_hashed = opphash == g->opphash;
    unclipped = !g->oppclipped;
    if(is_hashed)
    {
      for_three_channels(c)
        chrominance[c] = g->oppchroma[c];
    }
    dt_iop_gui_leave_critical_section(self);
  }

  if(is_hashed && unclipped)
  {
    dt_iop_image_copy(output, input, width * height * 4);
    return;
  }

  if(!is_hashed)
  {
    char *mask = dt_calloc_aligned(6 * msize);
    if(mask)
    {
      gboolean anyclipped = FALSE;
      DT_OMP_FOR(reduction( | : anyclipped))
      for(size_t row = 0; row < height; row++)
      {
        for(size_t col = 0; col < width; col++)
        {
          const size_t idx = (row * width + col) * 4;
          const size_t mdx = _raw_to_cmap(mwidth, row, col);
          for_three_channels(c)
          {
            if((input[idx] >= clips[c]) && (mask[c*msize + mdx] == 0))
            {
              mask[c * msize + mdx] |= 1;
              anyclipped |= TRUE;
            }
          }
        }
      }
      /* We want to use the photosites closely around clipped data to be taken into account.
         The mask buffers holds data for each color channel, we dilate the mask buffer slightly
         to get those locations.
      */

      dt_aligned_pixel_t sums = {0.0f, 0.0f, 0.0f, 0.0f};
      dt_aligned_pixel_t cnts = {0.0f, 0.0f, 0.0f, 0.0f};

      if(anyclipped)
      {
        DT_OMP_FOR(collapse(2))
        for(size_t row = 3; row < mheight - 3; row++)
        {
          for(size_t col = 3; col < mwidth - 3; col++)
          {
            const size_t mx = row * mwidth + col;
            mask[3*msize + mx] = _mask_dilated(mask + mx, mwidth);
            mask[4*msize + mx] = _mask_dilated(mask + msize + mx, mwidth);
            mask[5*msize + mx] = _mask_dilated(mask + 2*msize + mx, mwidth);
          }
        }

        DT_OMP_FOR(reduction(+ : sums, cnts))
        for(size_t row = 3; row < height - 3; row++)
        {
          for(size_t col = 3; col < width - 3; col++)
          {
            const size_t idx = (row * width + col) * 4;
            for_three_channels(c)
            {
              const float inval = input[idx+c];
              if((inval > 0.2f * clips[c]) && (inval < clips[c]) && (mask[(c+3) * msize + _raw_to_cmap(mwidth, row, col)]))
              {
                sums[c] += inval - _calc_linear_refavg(&input[idx], c);
                cnts[c] += 1.0f;
              }
            }
          }
        }
        for_three_channels(c)
          chrominance[c] = (cnts[c] > 30.0f) ? sums[c] / cnts[c] : 0.0f;

        if(self->dev->gui_attached && g && fullpipe)
        {
          dt_iop_gui_enter_critical_section(self);
          for_three_channels(c)
            g->oppchroma[c] = chrominance[c];
          g->opphash = opphash;
          g->oppclipped = anyclipped;
          g->oppresult = TRUE;
          dt_iop_gui_leave_critical_section(self);
        }
        dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE,
            "opposed chroma", piece->pipe, self, DT_DEVICE_CPU, roi_in, NULL,
            "RGB %3.4f %3.4f %3.4f clipped=%s",
            chrominance[0], chrominance[1], chrominance[2],
            STR_YESNO(anyclipped));
      }
      dt_free_align(mask);
    }
    else
      dt_print_pipe(DT_DEBUG_ALWAYS,
          "invalid opposed chroma", piece->pipe, self, DT_DEVICE_CPU, NULL, NULL,
          "allocation for mask generation failed");
  }

  DT_OMP_FOR(collapse(2))
  for(size_t row = 0; row < height; row++)
  {
    for(size_t col = 0; col < width; col++)
    {
      const size_t idx = (row * width + col) * 4;
      for_three_channels(c)
      {
        const float ref = _calc_linear_refavg(&input[idx], c);
        const float inval = fmaxf(0.0f, input[idx+c]);
        output[idx+c] = (inval >= clips[c]) ? fmaxf(inval, ref + chrominance[c]) : inval;
      }
    }
  }
}

static float *_process_opposed(dt_iop_module_t *self,
                               dt_dev_pixelpipe_iop_t *piece,
                               const float *const input,
                               float *const output,
                               const dt_iop_roi_t *const roi_in,
                               const dt_iop_roi_t *const roi_out,
                               const gboolean keep,
                               const float clipval)
{
  dt_iop_highlights_data_t *d = piece->data;
  dt_iop_highlights_gui_data_t *g = self->gui_data;
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->xtrans;
  const uint32_t filters = piece->filters;

  const dt_iop_buffer_dsc_t *dsc = &piece->pipe->dsc;
  const gboolean wbon = dsc->temperature.enabled;
  const dt_aligned_pixel_t icoeffs = { wbon ? dsc->temperature.coeffs[0] : 1.0f,
                                       wbon ? dsc->temperature.coeffs[1] : 1.0f,
                                       wbon ? dsc->temperature.coeffs[2] : 1.0f};
  const dt_aligned_pixel_t clips = { clipval * icoeffs[0], clipval * icoeffs[1], clipval * icoeffs[2]};

  const dt_dev_chroma_t *chr = &self->dev->chroma;
  const gboolean late = chr->late_correction;
  const dt_aligned_pixel_t correction = { late ? (float)(chr->D65coeffs[0] / chr->as_shot[0]) : 1.0f,
                                          late ? (float)(chr->D65coeffs[1] / chr->as_shot[1]) : 1.0f,
                                          late ? (float)(chr->D65coeffs[2] / chr->as_shot[2]) : 1.0f,
                                          1.0f };

  const size_t iwidth = roi_in->width;
  const size_t iheight = roi_in->height;
  const size_t mwidth  = iwidth / 3;
  const size_t mheight = iheight / 3;
  // we have to over-allocate making sure all later tests fit for all width&height combinations
  const size_t msize = dt_round_size((size_t) mwidth, 8) * dt_round_size(mheight, 8);

  dt_aligned_pixel_t chrominance = {0.0f, 0.0f, 0.0f, 0.0f};

  const gboolean experiments = d->mode == DT_IOP_HIGHLIGHTS_OPPOSED;
  const int opp_iter = experiments ? CLAMP(d->opp_passes, 0, 20) : 0;
  const gboolean iter_rg = experiments
                           && d->opp_iterate == DT_HIGHLIGHTS_OPP_ITERATE_WARM_LIGHTS;
  const gboolean cbrt_space = experiments && d->opp_cbrt;
  const gboolean local_chroma = experiments && filters != 0
                                && d->opp_local == DT_HIGHLIGHTS_OPP_CHROMA_PER_REGION;
  const gboolean fill_active = experiments
                               && (d->fill != DT_HIGHLIGHTS_FILL_OFF
                                   || d->fill_color != DT_HIGHLIGHTS_FILL_COLOR_OPPOSED);

  const dt_hash_t opphash = _opposed_hash(piece);
  const gboolean fullpipe = dt_pipe_is_full(piece->pipe);

  uint32_t *grown_regions = NULL;
  opp_region_chroma_t *region_chrominance = NULL;
  gboolean is_hashed = FALSE;
  gboolean unclipped = FALSE;
  if(self->dev->gui_attached && g)
  {
    dt_iop_gui_enter_critical_section(self);
    is_hashed = !local_chroma && opphash == g->opphash;
    unclipped = !g->oppclipped;
    if(is_hashed)
    {
      for_three_channels(c)
        chrominance[c] = g->oppchroma[c];
    }
    dt_iop_gui_leave_critical_section(self);
  }

  if(is_hashed && unclipped)
  {
    dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE,
                "copy unclipped opposed",
                piece->pipe, self, piece->pipe->devid, roi_in, roi_out);
    dt_iop_copy_image_roi(output, input, 1, roi_in, roi_out);
    return NULL;
  }

  if(!is_hashed)
  {
    char *mask = dt_calloc_aligned(6 * msize);
    if(mask)
    {
      gboolean anyclipped = FALSE;
      DT_OMP_FOR(reduction( | : anyclipped))
      for(int mrow = 0; mrow < mheight; mrow++)
      {
        const int irow = 3*mrow;
        for(int mcol = 0; mcol < mwidth; mcol++)
        {
          const int icol = 3*mcol;
          char mbuff[3] = { 0, 0, 0 };
          for(int y = irow; y < irow + 3; y++)
          {
            for(int x = icol; x < icol + 3; x++)
            {
              if(y < iheight && x < iwidth)
              {
                const size_t idx = y * iwidth + x;
                const int color = fcol(y, x, filters, xtrans);
                const gboolean clipped = input[idx] >= clips[color];
                mbuff[color] += clipped ? 1 : 0;
              }
            }
          }
          for_three_channels(c)
          {
            mask[c * msize + mrow * mwidth + mcol] = mbuff[c] ? 1 : 0;
            anyclipped |= mbuff[c] ? 1 : 0;
          }
        }
      }

      dt_aligned_pixel_t sums = {0.0f, 0.0f, 0.0f, 0.0f};
      dt_aligned_pixel_t cnts = {0.0f, 0.0f, 0.0f, 0.0f};
      if(anyclipped)
      {
        size_t region_count = 0;
        if(local_chroma)
        {
          grown_regions = _opp_region_labels(mask, mwidth, mheight, msize, &region_count);
          if(grown_regions && region_count > 0
             && region_count < SIZE_MAX / sizeof(*region_chrominance))
            region_chrominance = calloc(region_count + 1, sizeof(*region_chrominance));
          if(!region_chrominance)
          {
            free(grown_regions);
            grown_regions = NULL;
          }
        }

        /* We want to use the photosites closely around clipped data to be taken into account.
         The mask buffers holds data for each color channel, we dilate the mask buffer slightly
         to get those locations.
         If there are no clipped locations we keep the chrominance correction at 0 but make it valid
        */
        DT_OMP_FOR(collapse(2))
        for(size_t row = 0; row < mheight; row++)
        {
          for(size_t col = 0; col < mwidth; col++)
          {
            const size_t mx = row * mwidth + col;
            const gboolean safe = col >= 3 && row >= 3 && col < mwidth - 3 && row < mheight - 3;
            mask[3*msize + mx] = safe ? _mask_dilated(mask + mx, mwidth)          : mask[mx];
            mask[4*msize + mx] = safe ? _mask_dilated(mask + msize + mx, mwidth)  : mask[mx + msize];
            mask[5*msize + mx] = safe ? _mask_dilated(mask + 2*msize + mx, mwidth): mask[mx + 2*msize];
          }
        }

        const dt_aligned_pixel_t lo_clips = { 0.2f * clips[0], 0.2f * clips[1], 0.2f * clips[2], 1.0f };
        /* After having the surrounding mask for each color channel we can calculate the chrominance corrections. */
        DT_OMP_FOR(reduction(+ : sums, cnts) collapse(2))
        for(size_t row = 0; row < iheight; row++)
        {
          for(size_t col = 0; col < iwidth; col++)
          {
            const size_t idx = row * iwidth + col;
            const int color = fcol(row, col, filters, xtrans);
            const float inval = input[idx];

            /* we only use the unclipped photosites very close the true clipped data to calculate the chrominance offset */
            if((inval < clips[color]) && (inval > lo_clips[color])
               && (mask[(color+3) * msize + _raw_to_cmap(mwidth, row, col)]))
            {
              sums[color] += cbrt_space
                ? cbrtf(inval) - _calc_refavg(input, xtrans, filters, row, col, roi_in, correction, FALSE)
                : inval - _calc_refavg(input, xtrans, filters, row, col, roi_in, correction, TRUE);
              cnts[color] += 1.0f;
            }
          }
        }
        for_three_channels(c)
          chrominance[c] = (cnts[c] > 100.0f) ? sums[c] / cnts[c] : 0.0f;

        if(region_chrominance)
        {
          for(size_t row = 0; row < iheight; row++)
          {
            for(size_t col = 0; col < iwidth; col++)
            {
              const size_t idx = row * iwidth + col;
              const int color = fcol(row, col, filters, xtrans);
              const float inval = input[idx];
              const size_t mrow = row / 3;
              const size_t mcol = col / 3;
              if(mrow < mheight && mcol < mwidth
                 && (inval < clips[color]) && (inval > lo_clips[color]))
              {
                const size_t cell = mrow * mwidth + mcol;
                if(mask[(color+3) * msize + cell])
                {
                  const uint32_t label = grown_regions[cell];
                  if(label)
                  {
                    region_chrominance[label].sum[color] += cbrt_space
                      ? cbrtf(inval) - _calc_refavg(input, xtrans, filters, row, col, roi_in, correction, FALSE)
                      : inval - _calc_refavg(input, xtrans, filters, row, col, roi_in, correction, TRUE);
                    region_chrominance[label].count[color] += 1.0;
                  }
                }
              }
            }
          }

          for(size_t region = 1; region <= region_count; region++)
          {
            for_three_channels(c)
              region_chrominance[region].offset[c] = region_chrominance[region].count[c] > 30
                ? (float)(region_chrominance[region].sum[c] / region_chrominance[region].count[c])
                : chrominance[c];
          }
        }
      }

      if(self->dev->gui_attached && g && fullpipe)
      {
        dt_iop_gui_enter_critical_section(self);
        for_three_channels(c)
          g->oppchroma[c] = chrominance[c];
        g->opphash = opphash;
        g->oppclipped = anyclipped;
        g->oppresult = TRUE;
        dt_iop_gui_leave_critical_section(self);
      }
      dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE,
          "opposed chroma", piece->pipe, self, DT_DEVICE_CPU, NULL, NULL,
           "%12.7f (%d)%12.7f (%d)%12.7f (%d) clipped=%s",
          chrominance[0], (int)cnts[0],
          chrominance[1], (int)cnts[1],
          chrominance[2], (int)cnts[2],
          STR_YESNO(anyclipped));
      dt_free_align(mask);
    }
    else
      dt_print_pipe(DT_DEBUG_ALWAYS,
          "invalid opposed chroma", piece->pipe, self, DT_DEVICE_CPU, NULL, NULL,
          "allocation for mask generation failed");
  }

  const gboolean fill_mosaic = fill_active && filters != 0;
  if(opp_iter > 0 || cbrt_space)
  {
    float *cur = dt_alloc_align_float(iwidth * iheight);
    float *nxt = opp_iter > 0 ? dt_alloc_align_float(iwidth * iheight) : NULL;
    if(cur && (nxt || opp_iter == 0))
    {
      DT_OMP_FOR(collapse(2))
      for(size_t row = 0; row < iheight; row++)
      {
        for(size_t col = 0; col < iwidth; col++)
        {
          const size_t idx = row * iwidth + col;
          const int color = fcol(row, col, filters, xtrans);
          const float inval = input[idx];
          cur[idx] = inval >= clips[color]
            ? MAX(inval, _opp_estimate(input, xtrans, filters, row, col, roi_in,
                                       correction, _opp_chroma_for(chrominance, grown_regions,
                                                                  region_chrominance, mwidth, mheight,
                                                                  row, col, color), cbrt_space))
            : inval;
        }
      }

      for(int pass = 0; pass < opp_iter; pass++)
      {
        DT_OMP_FOR(collapse(2))
        for(size_t row = 0; row < iheight; row++)
        {
          for(size_t col = 0; col < iwidth; col++)
          {
            const size_t idx = row * iwidth + col;
            const int color = fcol(row, col, filters, xtrans);
            const float inval = input[idx];
            float oval = cur[idx];
            if(inval >= clips[color])
            {
              gboolean partial = FALSE;
              gboolean measured[3] = { FALSE, FALSE, FALSE };
              for(int y = MAX(0, (int)row - 1); y <= MIN((int)iheight - 1, (int)row + 1); y++)
                for(int x = MAX(0, (int)col - 1); x <= MIN((int)iwidth - 1, (int)col + 1); x++)
                {
                  const int c = fcol(y, x, filters, xtrans);
                  if(input[(size_t)y * iwidth + x] < clips[c])
                    measured[c] = TRUE;
                  if(c != color && input[(size_t)y * iwidth + x] < clips[c])
                    partial = TRUE;
                }
              if(iter_rg)
                partial = color != 2 && measured[2] && !measured[color == 0 ? 1 : 0];
              if(partial)
                oval = MAX(inval, _opp_estimate(cur, xtrans, filters, row, col, roi_in,
                                                correction, _opp_chroma_for(chrominance, grown_regions,
                                                                           region_chrominance, mwidth, mheight,
                                                                           row, col, color), cbrt_space));
            }
            nxt[idx] = oval;
          }
        }
        float *t = cur;
        cur = nxt;
        nxt = t;
      }

      dt_free_align(nxt);
      if(fill_mosaic)
        _opposed_fill(input, cur, iwidth, iheight, filters, xtrans, clips, d);

      DT_OMP_FOR(collapse(2))
      for(size_t row = 0; row < roi_out->height; row++)
      {
        for(size_t col = 0; col < roi_out->width; col++)
        {
          const size_t irow = row + roi_out->y;
          const size_t icol = col + roi_out->x;
          output[row * roi_out->width + col] = (irow < iheight && icol < iwidth)
            ? cur[irow * iwidth + icol] : 0.0f;
        }
      }
      if(keep)
      {
        free(grown_regions);
        free(region_chrominance);
        return cur;
      }
      dt_free_align(cur);
      free(grown_regions);
      free(region_chrominance);
      return NULL;
    }
    dt_free_align(cur);
    dt_free_align(nxt);
    dt_print(DT_DEBUG_ALWAYS, "highlights opposed experiment skipped: could not allocate working buffers");
  }

  const gboolean need_full = keep || fill_mosaic;
  float *tmpout = need_full ? dt_alloc_align_float(iwidth * iheight) : NULL;
  if(tmpout)
  {
    DT_OMP_FOR(collapse(2))
    for(size_t row = 0; row < iheight; row++)
    {
      for(size_t col = 0; col < iwidth; col++)
      {
        const size_t idx = row * iwidth + col;
        const int color = fcol(row, col, filters, xtrans);
        const float inval = input[idx];
        if(inval >= clips[color])
        {
          tmpout[idx] = MAX(inval, _opp_estimate(input, xtrans, filters, row, col,
                                                  roi_in, correction,
                                                  _opp_chroma_for(chrominance, grown_regions,
                                                                  region_chrominance, mwidth, mheight,
                                                                  row, col, color), cbrt_space));
        }
        else
          tmpout[idx] = inval;
      }
    }
    if(fill_mosaic)
      _opposed_fill(input, tmpout, iwidth, iheight, filters, xtrans, clips, d);
  }
  else if(fill_mosaic)
    dt_print(DT_DEBUG_ALWAYS, "highlights opposed fill skipped: could not allocate the full image buffer");

  DT_OMP_FOR(collapse(2))
  for(size_t row = 0; row < roi_out->height; row++)
  {
    for(size_t col = 0; col < roi_out->width; col++)
    {
      const size_t odx = row * roi_out->width + col;
      const size_t irow = row + roi_out->y;
      const size_t icol = col + roi_out->x;
      const size_t ix = irow * roi_in->width + icol;
      float oval = 0.0f;
      if(irow < iheight && icol < iwidth)
      {
        if(tmpout)
          oval = tmpout[ix];
        else
        {
          const int color = fcol(irow, icol, filters, xtrans);
          oval = input[ix];
          if(oval >= clips[color])
            oval = MAX(oval, _opp_estimate(input, xtrans, filters, irow, icol,
                                            roi_in, correction,
                                            _opp_chroma_for(chrominance, grown_regions,
                                                            region_chrominance, mwidth, mheight,
                                                            irow, icol, color), cbrt_space));
        }
      }
      output[odx] = oval;
    }
  }
  if(keep)
  {
    free(grown_regions);
    free(region_chrominance);
    return tmpout;
  }
  dt_free_align(tmpout);
  free(grown_regions);
  free(region_chrominance);
  return NULL;
}

#ifdef HAVE_OPENCL
static cl_int process_opposed_cl(dt_iop_module_t *self,
                                 dt_dev_pixelpipe_iop_t *piece,
                                 cl_mem dev_in,
                                 cl_mem dev_out,
                                 const dt_iop_roi_t *const roi_in,
                                 const dt_iop_roi_t *const roi_out)
{
  dt_iop_highlights_data_t *d = piece->data;
  const dt_iop_highlights_global_data_t *gd = self->global_data;
  dt_iop_highlights_gui_data_t *g = self->gui_data;
  const int devid = piece->pipe->devid;
  const uint32_t filters = piece->filters;

  const float clipval = highlights_clip_magics[DT_IOP_HIGHLIGHTS_OPPOSED] * d->clip;
  const dt_iop_buffer_dsc_t *dsc = &piece->pipe->dsc;
  const gboolean wbon = dsc->temperature.enabled;
  const dt_aligned_pixel_t icoeffs = { wbon ? dsc->temperature.coeffs[0] : 1.0f,
                                       wbon ? dsc->temperature.coeffs[1] : 1.0f,
                                       wbon ? dsc->temperature.coeffs[2] : 1.0f};

  dt_aligned_pixel_t clips = { clipval * icoeffs[0], clipval * icoeffs[1], clipval * icoeffs[2], 1.0f};

  const dt_dev_chroma_t *chr = &self->dev->chroma;
  const gboolean late = chr->late_correction;
  dt_aligned_pixel_t correction = { late ? (float)(chr->D65coeffs[0] / chr->as_shot[0]) : 1.0f,
                                    late ? (float)(chr->D65coeffs[1] / chr->as_shot[1]) : 1.0f,
                                    late ? (float)(chr->D65coeffs[2] / chr->as_shot[2]) : 1.0f,
                                    1.0f };

  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  cl_mem dev_chrominance = NULL;
  cl_mem dev_xtrans = NULL;
  cl_mem dev_clips = NULL;
  cl_mem dev_inmask = NULL;
  cl_mem dev_outmask = NULL;
  cl_mem dev_accu = NULL;
  cl_mem dev_correction = NULL;
  float *claccu = NULL;

  dt_aligned_pixel_t chrominance = {0.0f, 0.0f, 0.0f, 0.0f};

  const dt_hash_t opphash = _opposed_hash(piece);
  const gboolean fullpipe = dt_pipe_is_full(piece->pipe);

  gboolean is_hashed = FALSE;
  gboolean unclipped = FALSE;
  if(self->dev->gui_attached && g)
  {
    dt_iop_gui_enter_critical_section(self);
    is_hashed = opphash == g->opphash;
    unclipped = !g->oppclipped;
    if(is_hashed)
    {
      for_three_channels(c)
        chrominance[c] = g->oppchroma[c];
    }
    dt_iop_gui_leave_critical_section(self);
  }

  if(is_hashed && unclipped)
  {
    const size_t iorigin[2] = { roi_out->x, roi_out->y };
    const size_t region[2] = { roi_out->width, roi_out->height };
    dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE,
                "copy unclipped opposed",
                piece->pipe, self, devid, roi_in, roi_out);
    return dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, iorigin, CLIMG_ORIGIN, region);
  }

  dev_xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->xtrans), piece->xtrans);
  if(dev_xtrans == NULL) goto error;
  dev_clips = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), clips);
  if(dev_clips == NULL) goto error;
  dev_correction = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), correction);
  if(dev_correction == NULL) goto error;

  if(!is_hashed)
  {
    // We don't have valid chrominance correction so go the hard way
    const int iwidth = roi_in->width;
    const int iheight = roi_in->height;
    const int mwidth  = iwidth / 3;
    const int mheight = iheight / 3;
    const int msize = dt_round_size((size_t) mwidth, 4) * dt_round_size(mheight, 4);
    const size_t mbufsize = sizeof(float) * msize;

    dev_inmask = dt_opencl_alloc_device_buffer(devid, mbufsize);
    if(dev_inmask == NULL) goto error;

    dev_outmask =  dt_opencl_alloc_device_buffer(devid, mbufsize);
    if(dev_outmask == NULL) goto error;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_highlights_initmask, mwidth, mheight,
            CLARG(dev_in), CLARG(dev_inmask),
            CLARG(msize), CLARG(mwidth), CLARG(mheight), CLARG(iwidth), CLARG(iheight),
            CLARG(filters), CLARG(dev_xtrans),
            CLARG(dev_clips));
    if(err != CL_SUCCESS) goto error;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_highlights_dilatemask, mwidth, mheight,
            CLARG(dev_inmask), CLARG(dev_outmask),
            CLARG(msize), CLARG(mwidth), CLARG(mheight));
    if(err != CL_SUCCESS) goto error;

    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    const size_t accu_floats = 8 * iheight;
    const size_t accusize = sizeof(float) * accu_floats;
    dev_accu = dt_opencl_alloc_device_buffer(devid, accusize);
    if(dev_accu == NULL) goto error;

    claccu = dt_alloc_align_float(accu_floats);
    if(claccu == NULL)
    {
      err = DT_OPENCL_SYSMEM_ALLOCATION;
      goto error;
    }

    err = dt_opencl_enqueue_kernel_1d_args(devid, gd->kernel_highlights_chroma, iheight,
            CLARG(dev_in), CLARG(dev_outmask), CLARG(dev_accu),
            CLARG(iwidth), CLARG(iheight),
            CLARG(msize), CLARG(mwidth),
            CLARG(filters), CLARG(dev_xtrans), CLARG(dev_clips), CLARG(dev_correction));
    if(err != CL_SUCCESS) goto error;

    err = dt_opencl_read_buffer_from_device(devid, claccu, dev_accu, 0, accusize, TRUE);
    if(err != CL_SUCCESS) goto error;

    // collect row data and accumulate
    dt_aligned_pixel_t sums = { 0.0f, 0.0f, 0.0f};
    dt_aligned_pixel_t cnts = { 0.0f, 0.0f, 0.0f};
    float clipped = 0.0f;

    for(int row = 0; row < iheight; row++)
    {
      for_three_channels(c)
      {
        sums[c] += claccu[8*row + 2*c];
        cnts[c] += claccu[8*row + 2*c +1];
        clipped += claccu[8*row + 6];
      }
    }
    for_three_channels(c)
      chrominance[c] = (cnts[c] > 100.0f) ? sums[c] / cnts[c] : 0.0f;

    if(self->dev->gui_attached && g && fullpipe)
    {
      dt_iop_gui_enter_critical_section(self);
      for_three_channels(c)
        g->oppchroma[c] = chrominance[c];
      g->opphash = opphash;
      g->oppclipped = clipped > 0.0f;
      g->oppresult = TRUE;
      dt_iop_gui_leave_critical_section(self);
    }
    dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE,
        "opposed chroma", piece->pipe, self, piece->pipe->devid, NULL, NULL,
        "%12.7f (%d)%12.7f (%d)%12.7f (%d) clipped=%s",
        chrominance[0], (int)cnts[0],
        chrominance[1], (int)cnts[1],
        chrominance[2], (int)cnts[2],
        STR_YESNO(clipped > 0.0f));
  }

  err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  dev_chrominance = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), chrominance);
  if(dev_chrominance == NULL) goto error;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_highlights_opposed, roi_out->width, roi_out->height,
          CLARG(dev_in), CLARG(dev_out),
          CLARG(roi_out->width), CLARG(roi_out->height),
          CLARG(roi_in->width), CLARG(roi_in->height),
          CLARG(roi_out->x), CLARG(roi_out->y),
          CLARG(filters), CLARG(dev_xtrans),
          CLARG(dev_clips),
          CLARG(dev_chrominance),
          CLARG(dev_correction));

  error:
  if(err != CL_SUCCESS)
    dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_OPENCL,
          "opposed processing failed", piece->pipe, self, devid, NULL, NULL,
          "error=%s", cl_errstr(err));

  dt_opencl_release_mem_object(dev_clips);
  dt_opencl_release_mem_object(dev_xtrans);
  dt_opencl_release_mem_object(dev_chrominance);
  dt_opencl_release_mem_object(dev_inmask);
  dt_opencl_release_mem_object(dev_outmask);
  dt_opencl_release_mem_object(dev_accu);
  dt_opencl_release_mem_object(dev_correction);
  dt_free_align(claccu);
  return err;
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
