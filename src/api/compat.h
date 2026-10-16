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

// what differs between the darktable versions src/api builds against
// (master and releases from 5.6.2 on). The HAVE_ flags come from the
// feature checks in compat.cmake; macros darktable defines are tested
// directly

#include "common/darktable.h"
#include "common/exif.h"
#include "common/variables.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/pixelpipe_hb.h"
#include "dtgtk/paint.h"
#include "dtgtk/togglebutton.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "libs/lib.h"

static inline void dt_api_commit_blend_params(dt_iop_module_t *module,
                                              const dt_develop_blend_params_t *params)
{
#ifdef HAVE_COMMIT_BLEND_PARAMS_PIPE
  dt_iop_commit_blend_params(module, params, NULL);
#else
  dt_iop_commit_blend_params(module, params);
#endif
}

static inline gchar *dt_api_expand_path(dt_variables_params_t *params,
                                        gchar *source,
                                        const gboolean iterate)
{
#ifdef HAVE_DT_VARIABLES_EXPAND_PATH
  return dt_variables_expand_path(params, source, iterate);
#else
  return dt_variables_expand(params, source, iterate);
#endif
}

// marks a file as AI denoise output; nothing where darktable has no marker
static inline void dt_api_mark_neural_restore(const char *path, const char *task)
{
#ifdef HAVE_DT_EXIF_XMP_WRITE_NEURAL_RESTORE
  dt_exif_xmp_write_neural_restore(path, task);
#else
  (void)path;
  (void)task;
#endif
}

static inline gboolean dt_api_pipe_processing(dt_dev_pixelpipe_t *pipe)
{
#ifdef HAVE_DT_PIPE_PROCESSING
  return dt_pipe_processing(pipe);
#else
  return pipe->processing;
#endif
}

// whether the module's picker on this widget is the one picking now
static inline gboolean dt_api_picker_is_active(GtkWidget *w)
{
  const dt_iop_color_picker_t *p = darktable.lib->proxy.colorpicker.picker_proxy;
  return p && p->colorpick == w;
}

// a picker of its own (a button, not a slider's quad)
static inline gboolean dt_api_is_picker_button(GtkWidget *w)
{
#ifdef DT_COLOR_PICKER_INSTANCE_KEY
  return g_object_get_data(G_OBJECT(w), DT_COLOR_PICKER_INSTANCE_KEY) != NULL;
#else
  return DTGTK_IS_TOGGLEBUTTON(w)
         && DTGTK_TOGGLEBUTTON(w)->icon == dtgtk_cairo_paint_colorpicker;
#endif
}

// a button darktable's actions press through a gesture (not a click)
static inline gboolean dt_api_button_has_gesture(GtkWidget *w)
{
#ifdef DT_ACTION_GESTURE_KEY
  return g_object_get_data(G_OBJECT(w), DT_ACTION_GESTURE_KEY) != NULL;
#else
  (void)w;
  return FALSE;
#endif
}

// the drawn mask: flexi masks where darktable has them (master, 5.8 on),
// else the classic group
#ifdef HAVE_MASKS_FLEXI
#define DT_API_MASK_DRAWN DEVELOP_MASK_FLEXI
#else
#define DT_API_MASK_DRAWN DEVELOP_MASK_MASK
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
