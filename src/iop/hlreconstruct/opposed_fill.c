/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "common/gaussian.h"

enum
{
  OPPOSED_FILL_SELECTED = 1,
  OPPOSED_FILL_CLIPPED = 2,
  OPPOSED_FILL_OUTSIDE = 4,
  OPPOSED_FILL_VISITED = 8,
  OPPOSED_FILL_HOLE = 16
};

typedef struct _opposed_fill_stats_t
{
  size_t regions;
  size_t solved_regions;
  size_t largest_region;
  size_t iterations;
  size_t max_iterations;
  double solver_time;
  double max_relative_residual;
} _opposed_fill_stats_t;

static int _opposed_fill_float_compare(const void *a, const void *b)
{
  const float av = *(const float *)a;
  const float bv = *(const float *)b;
  return (av > bv) - (av < bv);
}

static float _opposed_fill_median(float *values, const size_t count)
{
  if(count == 0) return 0.0f;
  qsort(values, count, sizeof(float), _opposed_fill_float_compare);
  const size_t middle = count / 2;
  return (count & 1) ? values[middle] : 0.5f * (values[middle - 1] + values[middle]);
}

static size_t _opposed_fill_flood(uint8_t *flags,
                                  const size_t width,
                                  const size_t height,
                                  const size_t start,
                                  const uint8_t target,
                                  int *queue,
                                  size_t *xmin,
                                  size_t *ymin,
                                  size_t *xmax,
                                  size_t *ymax)
{
  size_t head = 0;
  size_t tail = 0;
  queue[tail++] = (int)start;
  flags[start] |= OPPOSED_FILL_VISITED;
  if(xmin)
  {
    *xmin = *xmax = start % width;
    *ymin = *ymax = start / width;
  }

  while(head < tail)
  {
    const size_t index = (size_t)queue[head++];
    const size_t row = index / width;
    const size_t col = index % width;
    if(xmin)
    {
      *xmin = MIN(*xmin, col);
      *xmax = MAX(*xmax, col);
      *ymin = MIN(*ymin, row);
      *ymax = MAX(*ymax, row);
    }

    const size_t y0 = row > 0 ? row - 1 : 0;
    const size_t y1 = MIN(height - 1, row + 1);
    const size_t x0 = col > 0 ? col - 1 : 0;
    const size_t x1 = MIN(width - 1, col + 1);
    for(size_t y = y0; y <= y1; y++)
      for(size_t x = x0; x <= x1; x++)
      {
        const size_t neighbor = y * width + x;
        if((flags[neighbor] & target) && !(flags[neighbor] & OPPOSED_FILL_VISITED))
        {
          flags[neighbor] |= OPPOSED_FILL_VISITED;
          queue[tail++] = (int)neighbor;
        }
      }
  }
  return tail;
}

static void _opposed_fill_mark_exterior(uint8_t *flags,
                                        const size_t width,
                                        const size_t height,
                                        int *queue)
{
  size_t head = 0;
  size_t tail = 0;
  for(size_t col = 0; col < width; col++)
    for(size_t row = 0; row < height; row += height - 1)
    {
      const size_t index = row * width + col;
      if(!(flags[index] & OPPOSED_FILL_SELECTED) && !(flags[index] & OPPOSED_FILL_OUTSIDE))
      {
        flags[index] |= OPPOSED_FILL_OUTSIDE;
        queue[tail++] = (int)index;
      }
      if(height == 1) break;
    }
  if(height > 2)
    for(size_t row = 1; row + 1 < height; row++)
      for(size_t side = 0; side < 2; side++)
      {
        const size_t col = side == 0 ? 0 : width - 1;
        const size_t index = row * width + col;
        if(!(flags[index] & OPPOSED_FILL_SELECTED) && !(flags[index] & OPPOSED_FILL_OUTSIDE))
        {
          flags[index] |= OPPOSED_FILL_OUTSIDE;
          queue[tail++] = (int)index;
        }
      }

  while(head < tail)
  {
    const size_t index = (size_t)queue[head++];
    const size_t row = index / width;
    const size_t col = index % width;
    const size_t neighbors[4] = { row > 0 ? index - width : index,
                                   row + 1 < height ? index + width : index,
                                   col > 0 ? index - 1 : index,
                                   col + 1 < width ? index + 1 : index };
    for(int k = 0; k < 4; k++)
    {
      const size_t neighbor = neighbors[k];
      if(neighbor != index && !(flags[neighbor] & OPPOSED_FILL_SELECTED)
         && !(flags[neighbor] & OPPOSED_FILL_OUTSIDE))
      {
        flags[neighbor] |= OPPOSED_FILL_OUTSIDE;
        queue[tail++] = (int)neighbor;
      }
    }
  }
}

static inline gboolean _opposed_fill_unknown(const uint8_t *region,
                                             const uint8_t *free_cells,
                                             const size_t index)
{
  return region[index] || free_cells[index];
}

static inline int _opposed_fill_degree(const size_t width,
                                       const size_t height,
                                       const size_t row,
                                       const size_t col)
{
  return (col > 0) + (col + 1 < width) + (row > 0) + (row + 1 < height);
}

// fixed samples contribute to both Laplacians in the biharmonic normal equations
static void _opposed_fill_laplacian(const size_t width,
                                    const size_t height,
                                    const double *values,
                                    double *output)
{
  const size_t count = width * height;
  DT_OMP_FOR(if(count > 16384))
  for(size_t row = 0; row < height; row++)
    for(size_t col = 0; col < width; col++)
    {
      const size_t i = row * width + col;
      double value = _opposed_fill_degree(width, height, row, col) * values[i];
      if(col > 0) value -= values[i - 1];
      if(col + 1 < width) value -= values[i + 1];
      if(row > 0) value -= values[i - width];
      if(row + 1 < height) value -= values[i + width];
      output[i] = value;
    }
}

#define OPPOSED_FILL_MG_LEVELS 32

typedef struct _opposed_fill_mg_level_t
{
  size_t width;
  size_t height;
  size_t count;
  uint8_t *unknown;
  double *matrix;
  double *diagonal;
  double *rhs;
  double *solution;
  double *residual;
  double *factor;
  int *indices;
  size_t active;
} _opposed_fill_mg_level_t;

typedef struct _opposed_fill_mg_t
{
  size_t count;
  _opposed_fill_mg_level_t levels[OPPOSED_FILL_MG_LEVELS];
} _opposed_fill_mg_t;

static void _opposed_fill_mg_free(_opposed_fill_mg_t *mg)
{
  for(size_t l = 0; l < mg->count; l++)
  {
    _opposed_fill_mg_level_t *level = &mg->levels[l];
    dt_free_align(level->unknown);
    dt_free_align(level->matrix);
    dt_free_align(level->diagonal);
    dt_free_align(level->rhs);
    dt_free_align(level->solution);
    dt_free_align(level->residual);
    dt_free_align(level->factor);
    dt_free_align(level->indices);
  }
}

static gboolean _opposed_fill_mg_allocate(_opposed_fill_mg_level_t *level,
                                          const size_t width,
                                          const size_t height,
                                          const gboolean finest)
{
  level->width = width;
  level->height = height;
  level->count = width * height;
  level->unknown = dt_calloc_aligned(level->count);
  if(finest)
    level->diagonal = dt_calloc_aligned(level->count * sizeof(double));
  else
    level->matrix = dt_calloc_aligned(9 * level->count * sizeof(double));
  level->rhs = dt_calloc_aligned(level->count * sizeof(double));
  level->solution = dt_calloc_aligned(level->count * sizeof(double));
  level->residual = dt_calloc_aligned(level->count * sizeof(double));
  return level->unknown && (level->matrix || level->diagonal)
         && level->rhs && level->solution && level->residual;
}

// clamping the last interpolation node also makes restriction its exact transpose at crop edges
static double _opposed_fill_weight(const size_t fine, const size_t coarse, const size_t extent)
{
  const size_t lower = fine / 2;
  const size_t upper = MIN(lower + 1, extent - 1);
  if(!(fine & 1) || lower == upper) return coarse == lower ? 1.0 : 0.0;
  return coarse == lower || coarse == upper ? 0.5 : 0.0;
}

static double _opposed_fill_coefficient(const _opposed_fill_mg_level_t *level,
                                         const size_t x,
                                         const size_t y,
                                         const int dx,
                                         const int dy)
{
  const size_t i = y * level->width + x;
  if(level->matrix) return level->matrix[9 * i + 3 * (dy + 1) + dx + 1];
  if(dx == 0 && dy == 0) return level->diagonal[i];
  if(abs(dx) + abs(dy) != 1 || (int)x + dx < 0 || x + dx >= level->width
     || (int)y + dy < 0 || y + dy >= level->height) return 0.0;
  return level->unknown[i + dy * (ptrdiff_t)level->width + dx] ? -1.0 : 0.0;
}

static gboolean _opposed_fill_mg_init(_opposed_fill_mg_t *mg,
                                      const size_t width,
                                      const size_t height,
                                      const uint8_t *region,
                                      const uint8_t *free_cells)
{
  memset(mg, 0, sizeof(*mg));
  mg->count = 1;
  _opposed_fill_mg_level_t *fine = &mg->levels[0];
  if(!_opposed_fill_mg_allocate(fine, width, height, TRUE)) return FALSE;
  for(size_t y = 0; y < height; y++)
    for(size_t x = 0; x < width; x++)
    {
      const size_t i = y * width + x;
      fine->unknown[i] = _opposed_fill_unknown(region, free_cells, i);
      if(!fine->unknown[i]) continue;
      fine->diagonal[i] = _opposed_fill_degree(width, height, y, x)
                           + (free_cells[i] ? 1.0e-6 : 0.0);
    }

  while(fine->count > 64 && mg->count < OPPOSED_FILL_MG_LEVELS)
  {
    _opposed_fill_mg_level_t *coarse = &mg->levels[mg->count++];
    if(!_opposed_fill_mg_allocate(coarse, (fine->width + 1) / 2, (fine->height + 1) / 2, FALSE))
      return FALSE;
    for(size_t y = 0; y < coarse->height; y++)
      for(size_t x = 0; x < coarse->width; x++)
        coarse->unknown[y * coarse->width + x] = fine->unknown[2 * y * fine->width + 2 * x];

    // galerkin coarsening retains holes, fixed anchors and weak occluder ties at every scale
    const size_t coarse_count = coarse->count;
    DT_OMP_FOR(if(coarse_count > 4096))
    for(size_t cy = 0; cy < coarse->height; cy++)
      for(size_t cx = 0; cx < coarse->width; cx++)
      {
        const size_t ci = cy * coarse->width + cx;
        if(!coarse->unknown[ci]) continue;
        double *a = coarse->matrix + 9 * ci;
        const int y0 = MAX(0, (int)(2 * cy) - 1);
        const int x0 = MAX(0, (int)(2 * cx) - 1);
        for(int y = y0; y <= MIN((int)fine->height - 1, (int)(2 * cy) + 1); y++)
          for(int x = x0; x <= MIN((int)fine->width - 1, (int)(2 * cx) + 1); x++)
          {
            const size_t i = y * fine->width + x;
            if(!fine->unknown[i]) continue;
            const double p = _opposed_fill_weight(x, cx, coarse->width)
                             * _opposed_fill_weight(y, cy, coarse->height);
            for(int dy = -1; dy <= 1; dy++)
              for(int dx = -1; dx <= 1; dx++)
              {
                const double v = p * _opposed_fill_coefficient(fine, x, y, dx, dy);
                if(v == 0.0) continue;
                const size_t ny = y + dy, nx = x + dx;
                const size_t bx = nx / 2, by = ny / 2;
                for(size_t ty = by; ty <= MIN(by + (ny & 1), coarse->height - 1); ty++)
                  for(size_t tx = bx; tx <= MIN(bx + (nx & 1), coarse->width - 1); tx++)
                    if(coarse->unknown[ty * coarse->width + tx])
                    {
                      const int slot = 3 * ((int)ty - (int)cy + 1) + (int)tx - (int)cx + 1;
                      a[slot] += v * _opposed_fill_weight(nx, tx, coarse->width)
                                  * _opposed_fill_weight(ny, ty, coarse->height);
                    }
              }
          }
      }
    fine = coarse;
  }

  // the tiny bottom solve is exact and linear, keeping the V-cycle a symmetric CG preconditioner
  fine->indices = dt_alloc_align_int(fine->count);
  fine->factor = dt_calloc_aligned(fine->count * fine->count * sizeof(double));
  if(!fine->indices || !fine->factor) return FALSE;
  for(size_t i = 0; i < fine->count; i++)
    if(fine->unknown[i]) fine->indices[fine->active++] = i;
  const size_t n = fine->active;
  for(size_t i = 0; i < n; i++)
    for(size_t j = 0; j <= i; j++)
    {
      const size_t u = fine->indices[i], v = fine->indices[j];
      const int dy = (int)(v / fine->width) - (int)(u / fine->width);
      const int dx = (int)(v % fine->width) - (int)(u % fine->width);
      double value = abs(dx) <= 1 && abs(dy) <= 1
        ? _opposed_fill_coefficient(fine, u % fine->width, u / fine->width, dx, dy) : 0.0;
      for(size_t k = 0; k < j; k++) value -= fine->factor[i * n + k] * fine->factor[j * n + k];
      if(i == j)
      {
        if(!(value > 0.0)) return FALSE;
        fine->factor[i * n + j] = sqrt(value);
      }
      else
        fine->factor[i * n + j] = value / fine->factor[j * n + j];
    }
  return TRUE;
}

static void _opposed_fill_mg_apply(const _opposed_fill_mg_level_t *level,
                                   const double *values,
                                   double *output)
{
  if(!level->matrix)
  {
    const size_t count = level->count;
    DT_OMP_FOR(if(count > 16384))
    for(size_t y = 0; y < level->height; y++)
      for(size_t x = 0; x < level->width; x++)
      {
        const size_t i = y * level->width + x;
        double value = 0.0;
        if(level->unknown[i])
        {
          value = level->diagonal[i] * values[i];
          if(x > 0) value -= values[i - 1];
          if(x + 1 < level->width) value -= values[i + 1];
          if(y > 0) value -= values[i - level->width];
          if(y + 1 < level->height) value -= values[i + level->width];
        }
        output[i] = value;
      }
    return;
  }
  const size_t count = level->count;
  DT_OMP_FOR(if(count > 16384))
  for(size_t y = 0; y < level->height; y++)
    for(size_t x = 0; x < level->width; x++)
    {
      const size_t i = y * level->width + x;
      double value = 0.0;
      if(level->unknown[i])
        for(int dy = -1; dy <= 1; dy++)
          for(int dx = -1; dx <= 1; dx++)
          {
            const double a = level->matrix[9 * i + 3 * (dy + 1) + dx + 1];
            if(a != 0.0) value += a * values[i + dy * (ptrdiff_t)level->width + dx];
          }
      output[i] = value;
    }
}

static void _opposed_fill_mg_smooth(_opposed_fill_mg_level_t *level, const gboolean reverse)
{
  if(!level->matrix)
  {
    const size_t count = level->count;
    for(int pass = 0; pass < 2; pass++)
    {
      const int color = reverse ? 1 - pass : pass;
      DT_OMP_FOR(if(count > 16384))
      for(size_t y = 0; y < level->height; y++)
        for(size_t x = (color + y) & 1; x < level->width; x += 2)
        {
          const size_t i = y * level->width + x;
          if(!level->unknown[i]) continue;
          double value = level->rhs[i];
          if(x > 0) value += level->solution[i - 1];
          if(x + 1 < level->width) value += level->solution[i + 1];
          if(y > 0) value += level->solution[i - level->width];
          if(y + 1 < level->height) value += level->solution[i + level->width];
          level->solution[i] = value / level->diagonal[i];
        }
    }
    return;
  }
  const size_t count = level->count;
  for(int pass = 0; pass < 4; pass++)
  {
    const int color = reverse ? 3 - pass : pass;
    DT_OMP_FOR(if(count > 16384))
    for(size_t y = (color >> 1); y < level->height; y += 2)
      for(size_t x = (color & 1); x < level->width; x += 2)
      {
        const size_t i = y * level->width + x;
        if(!level->unknown[i]) continue;
        const double *a = level->matrix + 9 * i;
        double value = level->rhs[i];
        for(int dy = -1; dy <= 1; dy++)
          for(int dx = -1; dx <= 1; dx++)
          {
            const int slot = 3 * (dy + 1) + dx + 1;
            if(slot != 4 && a[slot] != 0.0)
              value -= a[slot] * level->solution[i + dy * (ptrdiff_t)level->width + dx];
          }
        level->solution[i] = value / a[4];
      }
  }
}

static void _opposed_fill_mg_vcycle(_opposed_fill_mg_t *mg, const size_t l)
{
  _opposed_fill_mg_level_t *fine = &mg->levels[l];
  memset(fine->solution, 0, fine->count * sizeof(double));
  if(l + 1 == mg->count)
  {
    const size_t n = fine->active;
    for(size_t i = 0; i < n; i++)
    {
      double v = fine->rhs[fine->indices[i]];
      for(size_t j = 0; j < i; j++) v -= fine->factor[i * n + j] * fine->solution[fine->indices[j]];
      fine->solution[fine->indices[i]] = v / fine->factor[i * n + i];
    }
    for(size_t i = n; i-- > 0;)
    {
      double v = fine->solution[fine->indices[i]];
      for(size_t j = i + 1; j < n; j++) v -= fine->factor[j * n + i] * fine->solution[fine->indices[j]];
      fine->solution[fine->indices[i]] = v / fine->factor[i * n + i];
    }
    return;
  }
  _opposed_fill_mg_smooth(fine, FALSE);
  _opposed_fill_mg_apply(fine, fine->solution, fine->residual);
  const size_t count = fine->count;
  DT_OMP_FOR(if(count > 16384))
  for(size_t i = 0; i < count; i++) fine->residual[i] = fine->rhs[i] - fine->residual[i];
  _opposed_fill_mg_level_t *coarse = &mg->levels[l + 1];
  const size_t coarse_count = coarse->count;
  DT_OMP_FOR(if(coarse_count > 4096))
  for(size_t cy = 0; cy < coarse->height; cy++)
    for(size_t cx = 0; cx < coarse->width; cx++)
    {
      const size_t ci = cy * coarse->width + cx;
      double value = 0.0;
      if(coarse->unknown[ci])
        for(int y = MAX(0, (int)(2 * cy) - 1); y <= MIN((int)fine->height - 1, (int)(2 * cy) + 1); y++)
          for(int x = MAX(0, (int)(2 * cx) - 1); x <= MIN((int)fine->width - 1, (int)(2 * cx) + 1); x++)
            value += _opposed_fill_weight(x, cx, coarse->width) * _opposed_fill_weight(y, cy, coarse->height)
                     * fine->residual[y * fine->width + x];
      coarse->rhs[ci] = value;
    }
  _opposed_fill_mg_vcycle(mg, l + 1);
  DT_OMP_FOR(if(count > 16384))
  for(size_t y = 0; y < fine->height; y++)
    for(size_t x = 0; x < fine->width; x++)
    {
      const size_t i = y * fine->width + x;
      if(!fine->unknown[i]) continue;
      for(size_t cy = y / 2; cy <= MIN(y / 2 + (y & 1), coarse->height - 1); cy++)
        for(size_t cx = x / 2; cx <= MIN(x / 2 + (x & 1), coarse->width - 1); cx++)
          fine->solution[i] += _opposed_fill_weight(x, cx, coarse->width)
                               * _opposed_fill_weight(y, cy, coarse->height)
                               * coarse->solution[cy * coarse->width + cx];
    }
  _opposed_fill_mg_smooth(fine, TRUE);
}

static double _opposed_fill_dot(const double *a, const double *b, const size_t count)
{
  double sum = 0.0;
  DT_OMP_FOR(reduction(+ : sum) if(count > 16384))
  for(size_t i = 0; i < count; i++) sum += a[i] * b[i];
  return sum;
}

static void _opposed_fill_apply(const _opposed_fill_mg_level_t *fine,
                                const uint8_t *free_cells,
                                const int order,
                                const double *values,
                                double *output,
                                double *work)
{
  if(order == 1)
    _opposed_fill_mg_apply(fine, values, output);
  else
  {
    _opposed_fill_laplacian(fine->width, fine->height, values, work);
    _opposed_fill_laplacian(fine->width, fine->height, work, output);
    const size_t count = fine->count;
    DT_OMP_FOR(if(count > 16384))
    for(size_t i = 0; i < count; i++)
      output[i] = fine->unknown[i] ? output[i] + (free_cells[i] ? 1.0e-6 * values[i] : 0.0) : 0.0;
  }
}

static gboolean _opposed_fill_solve_level(const size_t width,
                                          const size_t height,
                                          const int order,
                                          const float source,
                                          const float *luminance,
                                          const uint8_t *region,
                                          const uint8_t *free_cells,
                                          float *solution,
                                          _opposed_fill_stats_t *stats,
                                          double *relative_residual)
{
  const double start = dt_get_wtime();
  const size_t count = width * height;
  *relative_residual = INFINITY;
  size_t anchors = 0;
  double mean = 0.0;
  for(size_t i = 0; i < count; i++)
    if(!_opposed_fill_unknown(region, free_cells, i))
    {
      anchors++;
      mean += luminance[i];
    }
  if(!anchors)
  {
    for(size_t i = 0; i < count; i++) anchors += free_cells[i];
    if(!anchors) return FALSE;
    for(size_t i = 0; i < count; i++) if(free_cells[i]) mean += luminance[i];
  }
  mean /= anchors;

  _opposed_fill_mg_t mg;
  double *vectors = dt_calloc_aligned(7 * count * sizeof(double));
  gboolean ok = vectors && _opposed_fill_mg_init(&mg, width, height, region, free_cells);
  if(!vectors) memset(&mg, 0, sizeof(mg));
  size_t iterations = 0;
  if(ok)
  {
    _opposed_fill_mg_level_t *fine = &mg.levels[0];
    double *x = vectors, *rhs = x + count, *r = rhs + count, *p = r + count;
    double *ap = p + count, *work = ap + count, *fixed = work + count;
    const double *z = fine->solution;
    for(size_t i = 0; i < count; i++)
    {
      fixed[i] = fine->unknown[i] ? 0.0 : luminance[i];
      x[i] = fine->unknown[i] ? mean : 0.0;
    }
    _opposed_fill_laplacian(width, height, fixed, work);
    if(order == 2) _opposed_fill_laplacian(width, height, work, fixed);
    else memcpy(fixed, work, count * sizeof(double));
    for(size_t i = 0; i < count; i++)
      rhs[i] = fine->unknown[i] ? -fixed[i] + (region[i] ? source : 0.0)
                                 + (free_cells[i] ? 1.0e-6 * luminance[i] : 0.0) : 0.0;
    const double norm_b = sqrt(_opposed_fill_dot(rhs, rhs, count));
    const double scale = fmax(norm_b, 1.0e-20);
    // the tighter internal target leaves room for conditioning and the final float conversion
    const double tolerance = 1.0e-8 * scale;
    double previous = 0.0;
    gboolean restart = TRUE;
    const size_t limit = order == 1 ? 2000 : 50000;
    while(iterations <= limit)
    {
      if(restart)
      {
        _opposed_fill_apply(fine, free_cells, order, x, ap, work);
        for(size_t i = 0; i < count; i++) r[i] = rhs[i] - ap[i];
      }
      const double norm_r = sqrt(_opposed_fill_dot(r, r, count));
      if(!isfinite(norm_r)) break;
      if(norm_r <= tolerance)
      {
        if(restart) break;
        restart = TRUE;
        continue;
      }
      if(iterations == limit) break;
      memcpy(fine->rhs, r, count * sizeof(double));
      _opposed_fill_mg_vcycle(&mg, 0);
      if(order == 2)
      {
        memcpy(fine->rhs, fine->solution, count * sizeof(double));
        _opposed_fill_mg_vcycle(&mg, 0);
      }
      const double rz = _opposed_fill_dot(r, z, count);
      if(!(rz > 0.0) || !isfinite(rz)) break;
      const double beta = restart ? 0.0 : rz / previous;
      DT_OMP_FOR(if(count > 16384))
      for(size_t i = 0; i < count; i++) p[i] = z[i] + beta * p[i];
      _opposed_fill_apply(fine, free_cells, order, p, ap, work);
      const double pap = _opposed_fill_dot(p, ap, count);
      if(!(pap > 0.0) || !isfinite(pap)) break;
      const double alpha = rz / pap;
      DT_OMP_FOR(if(count > 16384))
      for(size_t i = 0; i < count; i++)
      {
        x[i] += alpha * p[i];
        r[i] -= alpha * ap[i];
      }
      iterations++;
      previous = rz;
      // recheck b - A x periodically and before accepting convergence
      restart = iterations % 256 == 0;
    }
    _opposed_fill_apply(fine, free_cells, order, x, ap, work);
    for(size_t i = 0; i < count; i++) r[i] = rhs[i] - ap[i];
    *relative_residual = sqrt(_opposed_fill_dot(r, r, count)) / scale;
    ok = isfinite(*relative_residual) && *relative_residual <= 1.0e-6;
    if(ok)
      for(size_t i = 0; i < count; i++) solution[i] = fine->unknown[i] ? x[i] : luminance[i];
  }
  dt_free_align(vectors);
  _opposed_fill_mg_free(&mg);
  stats->iterations += iterations;
  stats->max_iterations = MAX(stats->max_iterations, iterations);
  stats->solver_time += dt_get_wtime() - start;
  return ok;
}

static size_t _opposed_fill_collect_ring(const size_t width,
                                        const size_t height,
                                        const size_t image_width,
                                        const size_t x0,
                                        const size_t y0,
                                        const uint8_t *flags,
                                        const uint8_t *region,
                                        const uint8_t *free_cells,
                                        const float *luminance,
                                        const float *distance,
                                        const float ring_min,
                                        const float ring_max,
                                        const gboolean first_ring,
                                        float *values)
{
  size_t count = 0;
  const size_t crop_count = width * height;
  for(size_t index = 0; index < crop_count; index++)
  {
    if(region[index] || free_cells[index]) continue;
    const size_t row = index / width;
    const size_t col = index % width;
    const size_t global = (y0 + row) * image_width + x0 + col;
    if(flags[global] & OPPOSED_FILL_SELECTED) continue;
    const float dist = distance[index];
    if((first_ring && dist <= ring_max)
       || (!first_ring && dist > ring_min && dist <= ring_max))
      values[count++] = luminance[index];
  }
  return count;
}

static void _opposed_fill_region(const float *cell_y,
                                 const float *occluder_y,
                                 const uint8_t *flags,
                                 float *output,
                                 const size_t grid_width,
                                 const size_t photosite_width,
                                 const size_t cell_size,
                                 const size_t grid_height,
                                 const int *queue,
                                 const size_t area,
                                 const size_t xmin,
                                 const size_t ymin,
                                 const size_t xmax,
                                 const size_t ymax,
                                 const float cell_pixels,
                                 const dt_iop_highlights_data_t *d,
                                 _opposed_fill_stats_t *stats)
{
  const size_t x0 = xmin > 4 ? xmin - 4 : 0;
  const size_t y0 = ymin > 4 ? ymin - 4 : 0;
  const size_t x1 = MIN(grid_width, xmax + 5);
  const size_t y1 = MIN(grid_height, ymax + 5);
  const size_t width = x1 - x0;
  const size_t height = y1 - y0;
  const size_t count = width * height;
  const gboolean log_luminance = d->fill_log == DT_HIGHLIGHTS_FILL_LOG;
  const float occluder = CLAMP(d->fill_occluder, 0.0f, 1.0f);
  const float strength = CLAMP(d->fill_strength, 0.0f, 4.0f);
  const float max_rise = CLAMP(d->fill_max_rise, 0.0f, 4.0f);
  float *luminance = dt_alloc_align_float(count);
  float *solution = dt_alloc_align_float(count);
  float *distance = NULL;
  uint8_t *region = dt_calloc_aligned(count);
  uint8_t *free_cells = dt_calloc_aligned(count);
  if(!luminance || !solution || !region || !free_cells)
  {
    dt_free_align(luminance);
    dt_free_align(solution);
    dt_free_align(region);
    dt_free_align(free_cells);
    dt_print(DT_DEBUG_ALWAYS,
             "highlights opposed fill region unchanged: %zu cells, could not allocate crop data",
             area);
    return;
  }

  for(size_t index = 0; index < count; index++)
  {
    const size_t row = index / width;
    const size_t col = index % width;
    const float y = cell_y[(y0 + row) * grid_width + x0 + col];
    luminance[index] = log_luminance ? logf(y) : y;
  }
  for(size_t j = 0; j < area; j++)
  {
    const size_t global = (size_t)queue[j];
    const size_t row = global / grid_width;
    const size_t col = global % grid_width;
    region[(row - y0) * width + col - x0] = 1;
    solution[j] = occluder_y[global];
  }
  const float median_y = _opposed_fill_median(solution, area);
  for(size_t index = 0; index < count; index++)
  {
    const size_t row = index / width;
    const size_t col = index % width;
    const size_t global = (y0 + row) * grid_width + x0 + col;
    if(!region[index] && !(flags[global] & OPPOSED_FILL_SELECTED)
       && occluder > 0.0f && cell_y[global] < occluder * median_y)
      free_cells[index] = 1;
  }

  const gboolean needs_rim = d->fill == DT_HIGHLIGHTS_FILL_POISSON
                             || d->fill == DT_HIGHLIGHTS_FILL_DISTANCE;
  float slope = 0.0f;
  float median_ring1 = 0.0f;
  size_t ring1_count = 0;
  gboolean has_outside = FALSE;
  for(size_t index = 0; index < count; index++)
    has_outside |= !region[index];
  if(needs_rim && has_outside)
  {
    distance = dt_alloc_align_float(count);
    if(!distance)
    {
      dt_free_align(luminance);
      dt_free_align(solution);
      dt_free_align(region);
      dt_free_align(free_cells);
      dt_print(DT_DEBUG_ALWAYS,
               "highlights opposed fill region unchanged: %zu cells, could not allocate rim distances",
               area);
      return;
    }

    for(size_t index = 0; index < count; index++)
      distance[index] = region[index] ? 0.0f : DT_DISTANCE_TRANSFORM_MAX;
    dt_image_distance_transform(NULL, distance, width, height, 1.0f, DT_DISTANCE_TRANSFORM_NONE);

    // widen either ring only when it is narrower than one cell in prototype pixels
    const float inner_radius = fmaxf(1.5f, cell_pixels);
    const float outer_radius = fmaxf(3.5f, inner_radius + cell_pixels);
    DT_OMP_FOR(if(count > 16384))
    for(size_t index = 0; index < count; index++)
      distance[index] *= cell_pixels;
    ring1_count = _opposed_fill_collect_ring(width, height, grid_width, x0, y0,
                                              flags, region, free_cells, luminance,
                                              distance, 0.0f, inner_radius, TRUE, solution);
    median_ring1 = _opposed_fill_median(solution, ring1_count);
    const size_t ring2_count = _opposed_fill_collect_ring(width, height, grid_width, x0, y0,
                                                           flags, region, free_cells, luminance,
                                                           distance, inner_radius, outer_radius,
                                                           FALSE, solution);
    const float median_ring2 = _opposed_fill_median(solution, ring2_count);
    if(ring1_count >= 8 && ring2_count >= 8)
      slope = fmaxf((median_ring1 - median_ring2) / 2.0f, 0.0f) * strength;
  }

  float limit = log_luminance ? max_rise * logf(2.0f)
                              : (exp2f(max_rise) - 1.0f) * median_ring1;
  limit = fmaxf(limit, 0.0f);
  int order = (d->fill == DT_HIGHLIGHTS_FILL_BIHARMONIC) ? 2 : 1;
  float source = 0.0f;
  if(d->fill == DT_HIGHLIGHTS_FILL_POISSON && ring1_count > 0 && area > 0)
  {
    const float area_cells = (float)area;
    // the cell spacing converts the prototype rim slope to finite-difference source units
    const float poisson = slope * ring1_count / area_cells * cell_pixels * cell_pixels;
    const float cap = 4.0f * limit / (area_cells / 3.14159265358979323846f);
    source = fminf(poisson, cap);
  }

  double relative_residual = INFINITY;
  const gboolean solved = _opposed_fill_solve_level(width, height, order, source,
                                                     luminance, region, free_cells,
                                                     solution, stats, &relative_residual);
  if(solved) stats->max_relative_residual = fmax(stats->max_relative_residual, relative_residual);
  if(!solved)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "highlights opposed fill region unchanged: %zu cells, crop %zux%zu, relative residual %.3g",
             area, width, height, relative_residual);
    dt_free_align(distance);
    dt_free_align(luminance);
    dt_free_align(solution);
    dt_free_align(region);
    dt_free_align(free_cells);
    return;
  }
  if(solved && d->fill == DT_HIGHLIGHTS_FILL_DISTANCE && slope > 0.0f && limit > 0.0f)
  {
    for(size_t index = 0; index < count; index++)
      distance[index] = region[index] ? DT_DISTANCE_TRANSFORM_MAX : 0.0f;
    const float max_distance = dt_image_distance_transform(NULL, distance, width, height,
                                                            1.0f, DT_DISTANCE_TRANSFORM_NONE)
                               * cell_pixels;
    DT_OMP_FOR(if(count > 16384))
    for(size_t index = 0; index < count; index++)
      distance[index] *= cell_pixels;
    if(d->fill_round && max_distance > 0.0f)
    {
      float *blurred = dt_alloc_align_float(count);
      if(!blurred)
      {
        dt_free_align(distance);
        dt_free_align(luminance);
        dt_free_align(solution);
        dt_free_align(region);
        dt_free_align(free_cells);
        dt_print(DT_DEBUG_ALWAYS,
                 "highlights opposed fill region unchanged: %zu cells, could not allocate ridge blur",
                 area);
        return;
      }
      const float sigma = fmaxf(1.0f, 0.25f * max_distance) / cell_pixels;
      const float max_value[1] = { max_distance + 1.0f };
      const float min_value[1] = { 0.0f };
      dt_gaussian_t *gaussian = dt_gaussian_init(width, height, 1, max_value, min_value,
                                                 sigma, DT_IOP_GAUSSIAN_ZERO);
      if(!gaussian)
      {
        dt_free_align(blurred);
        dt_free_align(distance);
        dt_free_align(luminance);
        dt_free_align(solution);
        dt_free_align(region);
        dt_free_align(free_cells);
        dt_print(DT_DEBUG_ALWAYS,
                 "highlights opposed fill region unchanged: %zu cells, could not initialize ridge blur",
                 area);
        return;
      }
      dt_gaussian_blur(gaussian, distance, blurred);
      dt_gaussian_free(gaussian);
      DT_OMP_FOR(if(count > 16384))
      for(size_t index = 0; index < count; index++)
        if(region[index]) distance[index] = fminf(distance[index], blurred[index]);
      dt_free_align(blurred);
    }
    DT_OMP_FOR(if(count > 16384))
    for(size_t index = 0; index < count; index++)
      if(region[index])
        solution[index] += -limit * expm1f(-slope * distance[index] / limit);
  }

  if(solved)
  {
    const float soft_cap = max_rise * logf(2.0f);
    DT_OMP_FOR(if(count > 16384))
    for(size_t index = 0; index < count; index++)
      if(region[index])
      {
        const float current = luminance[index];
        float rise = log_luminance ? solution[index] - current
                                   : logf(fmaxf(solution[index], 1.0e-6f) / current);
        rise = fmaxf(rise, 0.0f);
        if(d->fill_cap == DT_HIGHLIGHTS_FILL_SOFT_CAP)
          rise = soft_cap > 0.0f ? soft_cap * tanhf(rise / soft_cap) : 0.0f;
        else
          rise = fminf(rise, soft_cap);
        solution[index] = expf(rise);
      }

    DT_OMP_FOR(if(count > 16384))
    for(size_t j = 0; j < area; j++)
    {
      const size_t global = (size_t)queue[j];
      const size_t row = global / grid_width;
      const size_t col = global % grid_width;
      const float gain = solution[(row - y0) * width + col - x0];
      for(size_t dy = 0; dy < cell_size; dy++)
        for(size_t dx = 0; dx < cell_size; dx++)
          output[(row * cell_size + dy) * photosite_width
                 + col * cell_size + dx] *= gain;
    }
    stats->solved_regions++;
  }

  dt_free_align(distance);
  dt_free_align(luminance);
  dt_free_align(solution);
  dt_free_align(region);
  dt_free_align(free_cells);
}

/* chromaticity of a cell: the channels relative to the largest one, so white is (1, 1, 1). This
   is the white-balanced space highlight reconstruction works in, not opposed's D65-corrected
   reference space: with the scene-referred defaults the data is as-shot balanced and color
   calibration adapts from the as-shot illuminant, so equal channels here render neutral, while
   D65-neutral cells came out light blue under a warm as-shot illuminant */
static inline void _opposed_fill_chroma(const float *rgb, float *chroma)
{
  const float v[3] = { fmaxf(rgb[0], 0.0f), fmaxf(rgb[1], 0.0f), fmaxf(rgb[2], 0.0f) };
  const float largest = fmaxf(v[0], fmaxf(v[1], v[2]));
  for(int c = 0; c < 3; c++) chroma[c] = largest > 0.0f ? v[c] / largest : 1.0f;
}

/* gives the cells of one fully clipped region a new chromaticity: neutral for "white", or for
   "from rim" each channel's share solved inward from the cells around the region (Laplace, as the
   flat lift). Clipped values are lower bounds, so a cell gets the smallest brightness at which
   the new chromaticity keeps every channel at or above its current value, limited to "max rise"
   above its largest channel; channels that would need more keep their value. */
static void _opposed_fill_color_region(const float *cell_rgb,
                                       const float *cell_y,
                                       const uint8_t *flags,
                                       float *output,
                                       const size_t grid_width,
                                       const size_t photosite_width,
                                       const size_t cell_size,
                                       const size_t grid_height,
                                       const uint32_t filters,
                                       const uint8_t(*const xtrans)[6],
                                       const int *queue,
                                       const size_t area,
                                       const size_t xmin,
                                       const size_t ymin,
                                       const size_t xmax,
                                       const size_t ymax,
                                       const dt_iop_highlights_data_t *d,
                                       _opposed_fill_stats_t *stats)
{
  const size_t x0 = xmin > 2 ? xmin - 2 : 0;
  const size_t y0 = ymin > 2 ? ymin - 2 : 0;
  const size_t x1 = MIN(grid_width, xmax + 3);
  const size_t y1 = MIN(grid_height, ymax + 3);
  const size_t width = x1 - x0;
  const size_t height = y1 - y0;
  const size_t count = width * height;
  const float max_rise = CLAMP(d->fill_max_rise, 0.0f, 4.0f);

  // per crop cell and channel the chromaticity to give the region (the region's cells) or to
  // solve from (the cells around it)
  float *chroma = dt_alloc_align_float(3 * count);
  if(!chroma)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "highlights opposed color region unchanged: %zu cells, could not allocate crop data", area);
    return;
  }
  for(size_t i = 0; i < 3 * count; i++) chroma[i] = 1.0f;

  if(d->fill_color == DT_HIGHLIGHTS_FILL_COLOR_RIM)
  {
    float *values = dt_alloc_align_float(count);
    float *solution = dt_alloc_align_float(count);
    uint8_t *region = dt_calloc_aligned(count);
    uint8_t *free_cells = dt_calloc_aligned(count);
    gboolean solved = values && solution && region && free_cells;
    if(solved)
    {
      for(size_t j = 0; j < area; j++)
      {
        const size_t global = (size_t)queue[j];
        region[(global / grid_width - y0) * width + global % grid_width - x0] = 1;
        values[j] = cell_y[global];
      }
      // dark cells around the region (leaves, roofs against a clipped sky) do not set its color:
      // as for the dome they are free, so the color is carried from the bright part of the rim
      const float dark = CLAMP(d->fill_occluder, 0.0f, 1.0f) * _opposed_fill_median(values, area);
      for(size_t index = 0; index < count; index++)
        if(!region[index] && cell_y[(y0 + index / width) * grid_width + x0 + index % width] < dark)
          free_cells[index] = 1;
      // a fixed cell carries the mean chromaticity of the unselected cells in its 3x3
      // neighborhood: one noisy or clipped rim cell would otherwise set the color of the
      // region's edge on its own
      for(size_t index = 0; index < count; index++)
      {
        if(region[index]) continue;
        const size_t row = y0 + index / width;
        const size_t col = x0 + index % width;
        float sum[3] = { 0.0f, 0.0f, 0.0f };
        int n = 0;
        for(size_t r = row > 0 ? row - 1 : 0; r <= MIN(row + 1, grid_height - 1); r++)
          for(size_t c = col > 0 ? col - 1 : 0; c <= MIN(col + 1, grid_width - 1); c++)
          {
            const size_t cell = r * grid_width + c;
            if((flags[cell] & OPPOSED_FILL_SELECTED) || cell_y[cell] < dark) continue;
            float k[3];
            _opposed_fill_chroma(cell_rgb + 3 * cell, k);
            for(int ch = 0; ch < 3; ch++) sum[ch] += k[ch];
            n++;
          }
        for(int ch = 0; ch < 3; ch++) chroma[3 * index + ch] = n ? sum[ch] / n : 1.0f;
      }
      for(int ch = 0; ch < 3 && solved; ch++)
      {
        for(size_t index = 0; index < count; index++) values[index] = chroma[3 * index + ch];
        double relative_residual = INFINITY;
        solved = _opposed_fill_solve_level(width, height, 1, 0.0f, values, region, free_cells,
                                           solution, stats, &relative_residual);
        if(solved)
          for(size_t index = 0; index < count; index++)
            if(region[index]) chroma[3 * index + ch] = solution[index];
      }
    }
    if(!solved)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "highlights opposed color region: %zu cells, rim color not solved, using white", area);
      for(size_t i = 0; i < 3 * count; i++) chroma[i] = 1.0f;
    }
    dt_free_align(values);
    dt_free_align(solution);
    dt_free_align(region);
    dt_free_align(free_cells);
  }

  const float rise = exp2f(max_rise);
  DT_OMP_FOR(if(area > 4096))
  for(size_t j = 0; j < area; j++)
  {
    const size_t global = (size_t)queue[j];
    const size_t row = global / grid_width;
    const size_t col = global % grid_width;
    const float *k = chroma + 3 * ((row - y0) * width + col - x0);
    const float *m = cell_rgb + 3 * global;
    float needed = 0.0f;
    float largest = 0.0f;
    for(int c = 0; c < 3; c++)
    {
      const float v = fmaxf(m[c], 0.0f);
      needed = fmaxf(needed, v / fmaxf(k[c], 0.02f));
      largest = fmaxf(largest, v);
    }
    const float level = fminf(needed, largest * rise);
    float gain[3];
    for(int c = 0; c < 3; c++)
      gain[c] = m[c] > 0.0f ? fmaxf(1.0f, level * fmaxf(k[c], 0.02f) / m[c]) : 1.0f;
    for(size_t dy = 0; dy < cell_size; dy++)
      for(size_t dx = 0; dx < cell_size; dx++)
      {
        const size_t py = row * cell_size + dy;
        const size_t px = col * cell_size + dx;
        output[py * photosite_width + px] *= gain[fcol((int)py, (int)px, filters, xtrans)];
      }
  }
  dt_free_align(chroma);
}

// cell means and luminance of the reconstructed mosaic, and with flags its fully clipped and clipped cells
static void _opposed_fill_cells(const float *input,
                                const float *output,
                                const size_t width,
                                const size_t cells_w,
                                const size_t count,
                                const size_t cell_width,
                                const uint32_t filters,
                                const uint8_t(*const xtrans)[6],
                                const dt_aligned_pixel_t clips,
                                const float threshold,
                                float *cell_rgb,
                                float *cell_y,
                                uint8_t *flags)
{
  // rec2020 weights only approximate luminance in white-balanced camera rgb
  DT_OMP_FOR()
  for(size_t cell = 0; cell < count; cell++)
  {
    const size_t row = cell / cells_w;
    const size_t col = cell % cells_w;
    float sums[3] = { 0.0f, 0.0f, 0.0f };
    int samples[3] = { 0, 0, 0 };
    int clipped[3] = { 0, 0, 0 };
    for(size_t dy = 0; dy < cell_width; dy++)
      for(size_t dx = 0; dx < cell_width; dx++)
      {
        const size_t py = row * cell_width + dy;
        const size_t px = col * cell_width + dx;
        const size_t index = py * width + px;
        const int color = fcol((int)py, (int)px, filters, xtrans);
        sums[color] += output[index];
        samples[color]++;
        clipped[color] += input[index] >= clips[color];
      }
    if(flags)
    {
      gboolean full = TRUE;
      gboolean any = FALSE;
      for(int color = 0; color < 3; color++)
      {
        full &= samples[color] > 0 && (float)clipped[color] / samples[color] >= threshold;
        any |= clipped[color] > 0;
      }
      flags[cell] = (full ? OPPOSED_FILL_SELECTED : 0) | (any ? OPPOSED_FILL_CLIPPED : 0);
    }
    for(int color = 0; color < 3; color++)
      cell_rgb[3 * cell + color] = samples[color] ? sums[color] / samples[color] : 0.0f;
    cell_y[cell] = fmaxf(0.2627f * cell_rgb[3 * cell] + 0.6780f * cell_rgb[3 * cell + 1]
                         + 0.0593f * cell_rgb[3 * cell + 2], 1.0e-6f);
  }
}

static gboolean _opposed_fill(const float *input,
                              float *output,
                              const size_t width,
                              const size_t height,
                              const uint32_t filters,
                              const uint8_t(*const xtrans)[6],
                              const dt_aligned_pixel_t clips,
                              const dt_iop_highlights_data_t *d)
{
  const gboolean do_fill = d->fill >= DT_HIGHLIGHTS_FILL_FLAT && d->fill <= DT_HIGHLIGHTS_FILL_DISTANCE;
  const gboolean do_color = d->fill_color == DT_HIGHLIGHTS_FILL_COLOR_WHITE
                            || d->fill_color == DT_HIGHLIGHTS_FILL_COLOR_RIM;
  if(filters == 0 || (!do_fill && !do_color))
    return FALSE;

  const double start = dt_get_wtime();
  const size_t cell_width = filters == 9u ? 3 : 2;
  const size_t cells_w = width / cell_width;
  const size_t cells_h = height / cell_width;
  const size_t count = cells_w * cells_h;
  if(count == 0 || count > INT_MAX) return FALSE;

  const float prototype_scale = (float)MAX(width, height) / 1600.0f;
  const float cell_pixels = cell_width / fmaxf(prototype_scale, 1.0e-6f);
  const float threshold = CLAMP(d->fill_threshold, 0.05f, 1.0f);
  float *cell_y = dt_alloc_align_float(count);
  float *cell_rgb = dt_alloc_align_float(3 * count);
  uint8_t *flags = dt_calloc_aligned(count);
  int *queue = dt_alloc_align_int(count);
  // the color step raises the region's cells (up to "max rise" for a warm rim), so the dome's
  // occluder threshold, relative to the region's median, would free the bright ring around a
  // recolored lamp and leave no rim slope: the threshold uses the luminance before the color step
  float *occluder_y = do_color && do_fill ? dt_alloc_align_float(count) : NULL;
  if(!cell_y || !cell_rgb || !flags || !queue || (do_color && do_fill && !occluder_y))
  {
    dt_free_align(occluder_y);
    dt_free_align(cell_y);
    dt_free_align(cell_rgb);
    dt_free_align(flags);
    dt_free_align(queue);
    dt_print(DT_DEBUG_ALWAYS, "highlights opposed fill skipped: could not allocate cell data");
    return FALSE;
  }

  _opposed_fill_cells(input, output, width, cells_w, count, cell_width, filters, xtrans, clips,
                      threshold, cell_rgb, cell_y, flags);

  // fully clipped cells, with the enclosed clipped specks merged, are the selection of the color
  // and, unless partly clipped cells are filled too, of the dome
  if(d->fill_specks > 0)
  {
    _opposed_fill_mark_exterior(flags, cells_w, cells_h, queue);
    for(size_t cell = 0; cell < count; cell++)
      if(!(flags[cell] & (OPPOSED_FILL_SELECTED | OPPOSED_FILL_OUTSIDE))
         && (flags[cell] & OPPOSED_FILL_CLIPPED))
        flags[cell] |= OPPOSED_FILL_HOLE;

    const float speck_limit = d->fill_specks * prototype_scale * prototype_scale;
    for(size_t cell = 0; cell < count; cell++)
      if((flags[cell] & OPPOSED_FILL_HOLE) && !(flags[cell] & OPPOSED_FILL_VISITED))
      {
        const size_t area = _opposed_fill_flood(flags, cells_w, cells_h, cell,
                                                 OPPOSED_FILL_HOLE, queue, NULL, NULL, NULL, NULL);
        const float photosite_area = area * (float)(cell_width * cell_width);
        if(photosite_area <= speck_limit)
          for(size_t j = 0; j < area; j++)
            flags[(size_t)queue[j]] |= OPPOSED_FILL_SELECTED;
      }
    for(size_t cell = 0; cell < count; cell++)
      flags[cell] &= (uint8_t)~(OPPOSED_FILL_OUTSIDE | OPPOSED_FILL_VISITED | OPPOSED_FILL_HOLE);
  }

  _opposed_fill_stats_t stats = { 0 };
  size_t color_regions = 0;
  if(do_color)
  {
    for(size_t cell = 0; cell < count; cell++)
      if((flags[cell] & OPPOSED_FILL_SELECTED) && !(flags[cell] & OPPOSED_FILL_VISITED))
      {
        size_t xmin, ymin, xmax, ymax;
        const size_t area = _opposed_fill_flood(flags, cells_w, cells_h, cell,
                                                 OPPOSED_FILL_SELECTED, queue,
                                                 &xmin, &ymin, &xmax, &ymax);
        _opposed_fill_color_region(cell_rgb, cell_y, flags, output, cells_w, width, cell_width, cells_h,
                                   filters, xtrans, queue, area, xmin, ymin, xmax, ymax,
                                   d, &stats);
        color_regions++;
      }
    for(size_t cell = 0; cell < count; cell++)
      flags[cell] &= (uint8_t)~OPPOSED_FILL_VISITED;
    if(occluder_y) memcpy(occluder_y, cell_y, count * sizeof(float));
    // the dome measures and scales the recolored cells
    _opposed_fill_cells(input, output, width, cells_w, count, cell_width, filters, xtrans, clips,
                        threshold, cell_rgb, cell_y, NULL);
  }

  if(do_fill)
  {
    if(d->fill_partial)
      for(size_t cell = 0; cell < count; cell++)
        if(flags[cell] & OPPOSED_FILL_CLIPPED) flags[cell] |= OPPOSED_FILL_SELECTED;

    for(size_t cell = 0; cell < count; cell++)
      if((flags[cell] & OPPOSED_FILL_SELECTED) && !(flags[cell] & OPPOSED_FILL_VISITED))
      {
        size_t xmin, ymin, xmax, ymax;
        const size_t area = _opposed_fill_flood(flags, cells_w, cells_h, cell,
                                                 OPPOSED_FILL_SELECTED, queue,
                                                 &xmin, &ymin, &xmax, &ymax);
        stats.regions++;
        stats.largest_region = MAX(stats.largest_region, area);
        _opposed_fill_region(cell_y, occluder_y ? occluder_y : cell_y, flags, output, cells_w, width,
                             cell_width, cells_h,
                             queue, area, xmin, ymin, xmax, ymax,
                             cell_pixels, d, &stats);
      }
  }

  dt_print(DT_DEBUG_PERF,
           "highlights opposed fill: %zu color regions, %zu regions, %zu solved, largest %zu cells, %zu solver iterations (max %zu), residual %.3g, solve %.3f s, total %.3f s",
           color_regions, stats.regions, stats.solved_regions, stats.largest_region, stats.iterations,
           stats.max_iterations, stats.max_relative_residual, stats.solver_time, dt_get_wtime() - start);
  dt_free_align(occluder_y);
  dt_free_align(cell_y);
  dt_free_align(cell_rgb);
  dt_free_align(flags);
  dt_free_align(queue);
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
