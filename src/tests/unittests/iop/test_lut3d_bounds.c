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

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#ifdef HAVE_GMIC
static unsigned int decompress_calls;

#define lut3d_decompress_clut test_lut3d_decompress_clut
#define lut3d_get_cached_clut test_lut3d_get_cached_clut
#define lut3d_read_gmz test_lut3d_read_gmz
#endif

#define dt_conf_get_string test_lut3d_conf_get_string
#include "iop/lut3d.c"
#undef dt_conf_get_string

#ifdef HAVE_GMIC
#undef lut3d_decompress_clut
#undef lut3d_get_cached_clut
#undef lut3d_read_gmz

void test_lut3d_decompress_clut(const unsigned char *const input_keypoints,
                                const unsigned int nb_input_keypoints,
                                const unsigned int output_resolution,
                                float *const output_clut_data,
                                const char *const filename)
{
  decompress_calls++;
}

unsigned int test_lut3d_get_cached_clut(float *const output_clut_data,
                                        const unsigned int output_resolution,
                                        const char *const filename)
{
  return 0;
}

gboolean test_lut3d_read_gmz(int *const nb_keypoints,
                             unsigned char *const keypoints,
                             const char *const filename,
                             int *const nb_lut,
                             void *widget,
                             const char *const lutname,
                             const gboolean newlutname)
{
  return FALSE;
}
#endif

char *test_lut3d_conf_get_string(const char *const key)
{
  return g_strdup("");
}

static void test_lut3d_path_components(void **state)
{
  assert_true(_filepath_is_safe("nested/plain.cube"));
  assert_false(_filepath_is_safe("../plain.cube"));
  assert_false(_filepath_is_safe("plain\\cube"));
  assert_false(_filepath_is_safe("plain\"cube"));

#ifdef HAVE_GMIC
  assert_true(_lutname_is_safe("plain.cimgz"));
  assert_false(_lutname_is_safe(""));
  assert_false(_lutname_is_safe("../plain.cimgz"));
  assert_false(_lutname_is_safe("plain/name.cimgz"));
  assert_false(_lutname_is_safe("plain\\name.cimgz"));
  assert_false(_lutname_is_safe("plain\"name.cimgz"));
#endif
}

#ifdef HAVE_GMIC
static void test_lut3d_compressed_branch(void **state)
{
  const int counts[] = { -1, 0, DT_IOP_LUT3D_MAX_KEYPOINTS + 1 };

  for(size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
  {
    dt_iop_lut3d_params_t params = { 0 };
    float *clut = NULL;
    params.nb_keypoints = counts[i];
    g_strlcpy(params.filepath, "plain.cube", sizeof(params.filepath));
    g_strlcpy(params.lutname, "plain.cimgz", sizeof(params.lutname));
    decompress_calls = 0;

    (void)_calculate_clut(&params, &clut);
    assert_int_equal(decompress_calls, 0);
    dt_free_align(clut);
  }

  dt_iop_lut3d_params_t params = { 0 };
  float *clut = NULL;
  params.nb_keypoints = 1;
  g_strlcpy(params.filepath, "plain.cube", sizeof(params.filepath));
  g_strlcpy(params.lutname, "plain.cimgz", sizeof(params.lutname));
  decompress_calls = 0;
  (void)_calculate_clut(&params, &clut);
  assert_int_equal(decompress_calls, 1);
  dt_free_align(clut);

  clut = NULL;
  g_strlcpy(params.lutname, "plain\"name.cimgz", sizeof(params.lutname));
  decompress_calls = 0;
  (void)_calculate_clut(&params, &clut);
  assert_int_equal(decompress_calls, 0);
  dt_free_align(clut);
}

static void test_lut3d_unterminated_lutname(void **state)
{
  dt_iop_lut3d_params_t params = { 0 };
  float *clut = NULL;
  params.nb_keypoints = 1;
  g_strlcpy(params.filepath, "plain.cube", sizeof(params.filepath));
  memset(params.lutname, 'x', sizeof(params.lutname));
  decompress_calls = 0;

  (void)_calculate_clut(&params, &clut);
  assert_int_equal(decompress_calls, 0);
  assert_null(clut);
}
#endif

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_lut3d_path_components),
#ifdef HAVE_GMIC
    cmocka_unit_test(test_lut3d_compressed_branch),
    cmocka_unit_test(test_lut3d_unterminated_lutname),
#endif
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
