/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable. If not, see <http://www.gnu.org/licenses/>.
*/

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include "gui/draw.h"
#include "common/curve_tools.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static void test_curve_anchor_bounds(void **state)
{
  dt_draw_curve_t *curve = dt_draw_curve_new(0.0f, 1.0f, MONOTONE_HERMITE);
  uint16_t *const samples = curve->csample.m_Samples;

  for(int i = 0; i < MAX_ANCHORS + 5; i++)
    dt_draw_curve_add_point(curve, (float)i / MAX_ANCHORS, (float)i / MAX_ANCHORS);

  assert_int_equal(curve->c.m_numAnchors, MAX_ANCHORS);
  assert_ptr_equal(curve->csample.m_Samples, samples);

  const CurveAnchorPoint first = curve->c.m_anchors[0];
  const CurveAnchorPoint last = curve->c.m_anchors[MAX_ANCHORS - 1];
  dt_draw_curve_set_point(curve, -1, 2.0f, 3.0f);
  dt_draw_curve_set_point(curve, MAX_ANCHORS, 4.0f, 5.0f);
  dt_draw_curve_set_point(curve, MAX_ANCHORS + 5, 6.0f, 7.0f);
  assert_memory_equal(&curve->c.m_anchors[0], &first, sizeof(first));
  assert_memory_equal(&curve->c.m_anchors[MAX_ANCHORS - 1], &last, sizeof(last));
  assert_ptr_equal(curve->csample.m_Samples, samples);

  curve->c.m_numAnchors = MAX_ANCHORS + 5;
  (void)dt_draw_curve_calc_value(curve, 0.5f);
  dt_draw_curve_calc_values(curve, 0.0f, 1.0f, 256, NULL, NULL);
  assert_ptr_equal(curve->csample.m_Samples, samples);

  dt_draw_curve_destroy(curve);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_curve_anchor_bounds)
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
