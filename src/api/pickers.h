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

#pragma once

#include "develop/imageop.h"

// a picker's sample of the module's input, in the picker's color space, as
// the pipe gives it to the module (dt_iop_module_t: picked_color,
// picked_color_min, picked_color_max)
typedef struct dt_api_pick_t
{
  dt_aligned_pixel_t mean, min, max;
} dt_api_pick_t;

// what a module's color_picker_apply does with a sample, headless: writes
// m->params; pipe is the pipe that sampled (its profiles)
typedef void (*dt_api_picker_apply_t)(dt_iop_module_t *m, struct dt_dev_pixelpipe_t *pipe,
                                      const dt_api_pick_t *pick);

typedef struct dt_api_picker_t
{
  const char *op;
  const char *control;            // as picker_list names it in darktable's window
  dt_iop_colorspace_type_t cst;   // the picker's color space; IOP_CS_NONE: the module's default
  gboolean denoise;               // DT_COLOR_PICKER_DENOISE
  dt_api_picker_apply_t apply;
} dt_api_picker_t;

// the module's pickers ported headless, in order: the n-th, NULL past the
// last
const dt_api_picker_t *dt_api_picker_nth(const char *op, const int n);

// a module's picker by its control's name; NULL when it isn't ported
const dt_api_picker_t *dt_api_picker_find(const char *op, const char *control);

