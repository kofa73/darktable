/* Extracted on 2026-10-24 from branch gradient-hl, commit 6ea0e47cd6, src/iop/hlreconstruct/gradient.c lines 1125-1326, 2394-2458.
   Mosaic (Bayer, X-Trans) to a reduced RGB grid with per-channel clip flags, and the write-back of a reduced-grid result to the photosites.
   Not compilable by itself: it was included by src/iop/highlights.c, which provides the
   darktable headers (dt_alloc_align_*, DT_OMP_FOR, dt_aligned_pixel_t, FC/FCxtrans). */

/* Least-squares plane fits to the photosites of each color in the 5x5 window
   centered on an X-Trans photosite, evaluated at the photosite. A 3x3 mean
   samples red and blue up to one photosite off-center, by an offset that
   changes from one photosite or block to the next and turns flat areas into
   false gradients and hues. The plane fit is exact on linear ramps at every
   phase, and its weights in a centered window are all positive, so its
   result stays within the values it uses. The weights depend only on the
   pattern phase: fit[75 * (6 * (row % 6) + col % 6) + 25 * color + 5 * (dy + 2) + dx + 2].
   Returns FALSE if a color does not span a plane in some window.
*/
static gboolean _gd_xtrans_fit(float *const fit,
                               const uint8_t (*const xtrans)[6])
{
  for(int pr = 0; pr < 6; pr++)
  {
    for(int pc = 0; pc < 6; pc++)
    {
      for(int c = 0; c < 3; c++)
      {
        // normal matrix of the plane a + b dx + c dy
        double m[3][3] = { { 0.0 } };
        for(int dy = -2; dy <= 2; dy++)
        {
          for(int dx = -2; dx <= 2; dx++)
          {
            if(FCNxtrans(pr + dy, pc + dx, xtrans) != c) continue;
            const double a[3] = { 1.0, dx, dy };
            for(int i = 0; i < 3; i++)
              for(int k = 0; k < 3; k++)
                m[i][k] += a[i] * a[k];
          }
        }
        // the plane's value at the photosite is a, so the weights come from
        // the first row of the inverse; all entries are small integers, so
        // a collinear set gives an exact 0
        const double r0 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
        const double r1 = m[0][2] * m[2][1] - m[0][1] * m[2][2];
        const double r2 = m[0][1] * m[1][2] - m[0][2] * m[1][1];
        const double det = m[0][0] * r0 + m[1][0] * r1 + m[2][0] * r2;
        if(det <= 0.0) return FALSE;

        float *const w = fit + 75 * (6 * pr + pc) + 25 * c;
        for(int dy = -2; dy <= 2; dy++)
          for(int dx = -2; dx <= 2; dx++)
            w[5 * (dy + 2) + dx + 2] = FCNxtrans(pr + dy, pc + dx, xtrans) == c
                                       ? (r0 + r1 * dx + r2 * dy) / det
                                       : 0.0f;
      }
    }
  }
  return TRUE;
}

/* One RGB pixel at the photosite (row, col). Its own color keeps the
   photosite's value and flag. Each other color comes from its photosites in
   the window of radius rad around the photosite, by the plane fit fit (X-Trans)
   or by their mean (Bayer, where a centered 3x3 mean equals bilinear
   interpolation), and is flagged if any of them is clipped.
   At the image edges the window moves inward until it lies inside the image.
   The photosite is then off the window's center, where a plane fit
   extrapolates and can leave the range of its values, so the mean replaces it.
   With block_flag, the own color is flagged if any photosite of that color in
   the 3x3 block around the photosite is clipped (X-Trans reduced resolution
   experiment), like the other colors are over their window.
*/
static inline void _gd_window_pixel(float *const restrict o,
                                    uint8_t *const restrict flags,
                                    const float *const restrict in,
                                    const uint32_t filters,
                                    const uint8_t (*const xtrans)[6],
                                    const float *const restrict fit,
                                    const dt_aligned_pixel_t clips,
                                    const int iwidth,
                                    const int iheight,
                                    const int row,
                                    const int col,
                                    const int rad,
                                    const gboolean block_flag)
{
  const int cy = CLAMP(row, rad, iheight - 1 - rad);
  const int cx = CLAMP(col, rad, iwidth - 1 - rad);
  const float *const w = fit && cy == row && cx == col ? fit + 75 * (6 * (row % 6) + col % 6) : NULL;

  dt_aligned_pixel_t sum = { 0.0f, 0.0f, 0.0f, 0.0f };
  dt_aligned_pixel_t cnt = { 0.0f, 0.0f, 0.0f, 0.0f };
  int clipped = 0;
  int block_clipped = 0;
  int k = 0;
  for(int y = cy - rad; y <= cy + rad; y++)
  {
    for(int x = cx - rad; x <= cx + rad; x++, k++)
    {
      const int c = fcol(y, x, filters, xtrans);
      const float v = in[(size_t)y * iwidth + x];
      sum[c] += w ? w[25 * c + k] * v : v;
      cnt[c] += 1.0f;
      if(v >= clips[c])
      {
        clipped |= 1 << c;
        // the moved window still holds the whole block: the photosite is at
        // most one off its center
        if(block_flag && abs(y - row) <= 1 && abs(x - col) <= 1) block_clipped |= 1 << c;
      }
    }
  }

  for_three_channels(c) o[c] = w ? sum[c] : sum[c] / cnt[c];
  o[3] = 0.0f;

  const int own = fcol(row, col, filters, xtrans);
  o[own] = in[(size_t)row * iwidth + col];
  clipped &= ~(1 << own);
  if(o[own] >= clips[own] || (block_clipped & (1 << own))) clipped |= 1 << own;
  *flags = clipped;
}

/* Builds the RGB pixels and their clipped flags (bits 0-2 of state) from the
   input, on a grid of width x height pixels:
   - linear raws: the input pixels as they are
   - Bayer at reduced resolution: 2x2 blocks, each channel the mean of the
     block's photosites of that color and flagged if any of them is clipped.
     A 2x2 block holds one period of the pattern, so all blocks sample the
     colors at the same positions; leftover photosites at the right and bottom
     edges join the last block of their row or column
   - Bayer at full resolution: a 3x3 window at every photosite
   - X-Trans: a 5x5 plane fit at every photosite, or at the center photosite
     of each 3x3 block at reduced resolution; block_flag, see
     _gd_window_pixel()
*/
static void _gd_to_rgb(float *const restrict rgb,
                       uint8_t *const restrict state,
                       const float *const restrict in,
                       const uint32_t filters,
                       const uint8_t (*const xtrans)[6],
                       const float *const restrict fit,
                       const dt_aligned_pixel_t clips,
                       const int iwidth,
                       const int iheight,
                       const int step,
                       const int width,
                       const int height,
                       const gboolean block_flag)
{
  if(!filters)
  {
    DT_OMP_FOR()
    for(size_t i = 0; i < (size_t)width * height; i++)
    {
      int clipped = 0;
      for(int c = 0; c < 3; c++)
        if(in[4 * i + c] >= clips[c]) clipped |= 1 << c;
      copy_pixel(rgb + 4 * i, in + 4 * i);
      state[i] = clipped;
    }
  }
  else if(step == 2)
  {
    DT_OMP_FOR(collapse(2))
    for(int row = 0; row < height; row++)
    {
      for(int col = 0; col < width; col++)
      {
        const int y1 = row == height - 1 ? iheight : 2 * row + 2;
        const int x1 = col == width - 1 ? iwidth : 2 * col + 2;
        dt_aligned_pixel_t sum = { 0.0f, 0.0f, 0.0f, 0.0f };
        dt_aligned_pixel_t cnt = { 0.0f, 0.0f, 0.0f, 0.0f };
        int clipped = 0;
        for(int y = 2 * row; y < y1; y++)
        {
          for(int x = 2 * col; x < x1; x++)
          {
            const int c = FC(y, x, filters);
            const float v = in[(size_t)y * iwidth + x];
            sum[c] += v;
            cnt[c] += 1.0f;
            if(v >= clips[c]) clipped |= 1 << c;
          }
        }
        const size_t i = (size_t)row * width + col;
        for_three_channels(c) rgb[4 * i + c] = sum[c] / cnt[c];
        rgb[4 * i + 3] = 0.0f;
        state[i] = clipped;
      }
    }
  }
  else
  {
    const int off = step / 2;
    const int rad = filters == 9u ? 2 : 1;
    DT_OMP_FOR(collapse(2))
    for(int row = 0; row < height; row++)
    {
      for(int col = 0; col < width; col++)
      {
        const size_t i = (size_t)row * width + col;
        _gd_window_pixel(rgb + 4 * i, state + i, in, filters, xtrans, fit, clips,
                         iwidth, iheight, step * row + off, step * col + off, rad, block_flag);
      }
    }
  }
}


/* Clipped photosites take the larger of their value and the solution of
   their color, since a clipped photosite was at least as bright as its
   clipping level; all others keep their value. On a grid coarser than the
   photosites, the solution is interpolated bilinearly between the grid
   points, which sit at the centers of their step x step blocks. Beyond the
   outermost grid points, each grid coordinate is clamped on its own, so the
   solution is still interpolated along the edge; only at the corners does a
   single grid point apply.
*/
static void _gd_write_back(float *const restrict out,
                           const float *const restrict in,
                           const float *const restrict rgb,
                           const uint32_t filters,
                           const uint8_t (*const xtrans)[6],
                           const dt_aligned_pixel_t clips,
                           const int step,
                           const int width,
                           const int height,
                           const dt_iop_roi_t *const roi_in,
                           const dt_iop_roi_t *const roi_out)
{
  const int dx = roi_out->x - roi_in->x;
  const int dy = roi_out->y - roi_in->y;
  const float off = 0.5f * (step - 1);

  DT_OMP_FOR()
  for(int row = 0; row < roi_out->height; row++)
  {
    const int irow = row + dy;
    if(irow < 0 || irow >= roi_in->height) continue;
    const float gy = CLAMPF((irow - off) / step, 0.0f, height - 1);
    const int y0 = (int)gy;
    const int y1 = MIN(y0 + 1, height - 1);
    const float fy = gy - y0;

    for(int col = 0; col < roi_out->width; col++)
    {
      const int icol = col + dx;
      if(icol < 0 || icol >= roi_in->width) continue;
      const size_t ip = (size_t)irow * roi_in->width + icol;
      const size_t op = (size_t)row * roi_out->width + col;

      if(!filters)
      {
        // linear raws are solved on their own pixels
        for_three_channels(c)
          if(in[4 * ip + c] >= clips[c])
            out[4 * op + c] = fmaxf(in[4 * ip + c], rgb[4 * ip + c]);
        continue;
      }

      const int c = fcol(irow, icol, filters, xtrans);
      if(in[ip] < clips[c]) continue;
      const float gx = CLAMPF((icol - off) / step, 0.0f, width - 1);
      const int x0 = (int)gx;
      const int x1 = MIN(x0 + 1, width - 1);
      const float fx = gx - x0;
      const float top = (1.0f - fx) * rgb[4 * ((size_t)y0 * width + x0) + c]
                               + fx * rgb[4 * ((size_t)y0 * width + x1) + c];
      const float bottom = (1.0f - fx) * rgb[4 * ((size_t)y1 * width + x0) + c]
                                  + fx * rgb[4 * ((size_t)y1 * width + x1) + c];
      out[op] = fmaxf(in[ip], (1.0f - fy) * top + fy * bottom);
    }
  }
}
