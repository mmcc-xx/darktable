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

// what a module's color_picker_apply does with a picker's sample, for a
// session without darktable's window: there the module has no controls,
// and its color_picker_apply reads them and works on the darkroom
// (darktable.develop). server.c takes the sample (_pick_sample); each
// function mirrors the branch of one module's color_picker_apply that
// writes its params, the settings its controls hold taken as darktable
// keeps them

#include "api/pickers.h"
#include "api/also_changed.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/iop_profile.h"
#include "common/math.h"
#include "control/conf.h"
#include "develop/pixelpipe_hb.h"

#include <math.h>

// a setting by name; NULL when this darktable's module doesn't have it
#define _P(T, m, name) ((T *)(m)->get_p((m)->params, (name)))

// ---- monochrome (monochrome.c) ----------------------------------------------

static void _monochrome(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  float *a = _P(float, m, "a"), *b = _P(float, m, "b"), *size = _P(float, m, "size");
  if(!a || !b || !size) return;
  if(fabsf(*a - pick->mean[1]) < 0.0001f && fabsf(*b - pick->mean[2]) < 0.0001f) return;
  *a = pick->mean[1];
  *b = pick->mean[2];
  const float da = pick->max[1] - pick->min[1];
  const float db = pick->max[2] - pick->min[2];
  *size = CLAMP((da + db) / 128.0, .5, 3.0);
}

// ---- exposure (exposure.c) --------------------------------------------------

// _auto_set_exposure in its area mode "correction", the one the module
// shows on every update (gui_update): exposure that brings the area's
// lightness to the target darktable keeps for the module's lightness slider
static void _exposure(dt_iop_module_t *m, dt_dev_pixelpipe_t *pipe, const dt_api_pick_t *pick)
{
  if(pick->max[0] < pick->min[0]) return;
  const dt_iop_order_iccprofile_info_t *const input_profile = dt_ioppr_get_pipe_input_profile_info(pipe);
  if(!input_profile) return;

  dt_aligned_pixel_t XYZ, Lab;
  dot_product(pick->mean, input_profile->matrix_in, XYZ);
  dt_XYZ_to_Lab(XYZ, Lab);
  Lab[1] = Lab[2] = Lab[3] = 0.f;
  dt_Lab_to_XYZ(Lab, XYZ);

  // the slider (0-100 %, one decimal) holds the saved lightness
  const float lightness = CLAMP(dt_conf_get_float("darkroom/modules/exposure/lightness"), 0.0f, 100.0f);
  dt_aligned_pixel_t Lch_target = { roundf(10.0f * lightness) / 10.0f, 0.f, 0.f, 0.f };
  dt_aligned_pixel_t Lab_target = { 0.f }, XYZ_target = { 0.f };
  dt_LCH_2_Lab(Lch_target, Lab_target);
  dt_Lab_to_XYZ(Lab_target, XYZ_target);

  if(XYZ[1] > 1e-5f && XYZ_target[1] > 1e-5f) dt_api_exposure_set_white(m, XYZ[1] / XYZ_target[1]);
}

// -----------------------------------------------------------------------------

static const dt_api_picker_t _pickers[] = {
  { "exposure", "exposure", IOP_CS_NONE, FALSE, _exposure },
  { "monochrome", "highlights", IOP_CS_NONE, FALSE, _monochrome },
};

const dt_api_picker_t *dt_api_picker_nth(const char *op, const int n)
{
  int k = 0;
  for(size_t i = 0; i < G_N_ELEMENTS(_pickers); i++)
    if(!g_strcmp0(op, _pickers[i].op) && k++ == n) return &_pickers[i];
  return NULL;
}

const dt_api_picker_t *dt_api_picker_find(const char *op, const char *control)
{
  for(size_t i = 0; i < G_N_ELEMENTS(_pickers); i++)
    if(!g_strcmp0(op, _pickers[i].op) && !g_strcmp0(control, _pickers[i].control)) return &_pickers[i];
  return NULL;
}
