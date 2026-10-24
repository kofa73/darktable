/* Extracted on 2026-10-24 from branch gradient-hl, commit 6ea0e47cd6, src/iop/hlreconstruct/gradient.c lines 871-975.
   Pull-push (multiscale) fill of unknown pixels from known ones.
   Not compilable by itself: it was included by src/iop/highlights.c, which provides the
   darktable headers (dt_alloc_align_*, DT_OMP_FOR, dt_aligned_pixel_t, FC/FCxtrans). */

/* Starting point for the hue Laplace solve, which converges slowly on large
   regions from a flat guess: a pull-push pyramid spreads the known values
   into the pixels to fill. Coarser levels sum the values and counts of 2x2
   children; on the way back, a pixel without data takes its parent's mean.
   Returns FALSE if out of memory before the first level.
*/
static gboolean _gd_pullpush_fill(float *const plane[3],
                                  const uint8_t *const known,
                                  const uint8_t *const fill,
                                  const int width,
                                  const int height)
{
  float *lev[GD_MAX_LEVELS] = { NULL };
  int lw[GD_MAX_LEVELS], lh[GD_MAX_LEVELS];
  int nlev = 0;
  int w = width;
  int h = height;

  while((w > 1 || h > 1) && nlev < GD_MAX_LEVELS)
  {
    const int cw = (w + 1) / 2;
    const int chh = (h + 1) / 2;
    float *const c = dt_calloc_align_float((size_t)4 * cw * chh);
    if(!c) break;
    const float *const f = nlev ? lev[nlev - 1] : NULL;
    DT_OMP_FOR()
    for(int row = 0; row < chh; row++)
    {
      for(int col = 0; col < cw; col++)
      {
        float *const o = c + (size_t)4 * (row * cw + col);
        for(int y = 2 * row; y < MIN(2 * row + 2, h); y++)
        {
          for(int x = 2 * col; x < MIN(2 * col + 2, w); x++)
          {
            const size_t i = (size_t)y * w + x;
            if(f)
            {
              for(int k = 0; k < 4; k++) o[k] += f[4 * i + k];
            }
            else if(known[i])
            {
              for_three_channels(k) o[k] += plane[k][i];
              o[3] += 1.0f;
            }
          }
        }
      }
    }
    lev[nlev] = c;
    lw[nlev] = cw;
    lh[nlev] = chh;
    nlev++;
    w = cw;
    h = chh;
  }

  for(int l = nlev - 1; l >= 0; l--)
  {
    float *const c = lev[l];
    const float *const parent = l + 1 < nlev ? lev[l + 1] : NULL;
    const int cw = lw[l];
    const int pw = parent ? lw[l + 1] : 0;
    DT_OMP_FOR()
    for(int row = 0; row < lh[l]; row++)
    {
      for(int col = 0; col < cw; col++)
      {
        float *const o = c + (size_t)4 * (row * cw + col);
        if(o[3] > 0.0f)
        {
          for_three_channels(k) o[k] /= o[3];
        }
        else if(parent)
        {
          const float *const pp = parent + (size_t)4 * ((row / 2) * pw + col / 2);
          for_three_channels(k) o[k] = pp[k];
        }
      }
    }
  }

  if(nlev)
  {
    const float *const c = lev[0];
    const int cw = lw[0];
    DT_OMP_FOR()
    for(int row = 0; row < height; row++)
    {
      for(int col = 0; col < width; col++)
      {
        const size_t i = (size_t)row * width + col;
        if(fill[i])
        {
          const float *const pp = c + (size_t)4 * ((row / 2) * cw + col / 2);
          for_three_channels(k) plane[k][i] = pp[k];
        }
      }
    }
  }

  for(int l = 0; l < nlev; l++)
    dt_free_align(lev[l]);
  return nlev > 0 || (width == 1 && height == 1);
}
