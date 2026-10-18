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

// what a module's gui_changed adjusts after the setting `field` changed,
// headless: m->params holds the new settings, old the settings before
typedef void (*dt_api_also_changed_t)(dt_iop_module_t *m, const char *field, const void *old);

// the function for a module, NULL when its gui_changed adjusts nothing (or
// isn't ported yet)
dt_api_also_changed_t dt_api_also_changed_find(const char *op);

// for the ports, from server.c: ashift's automatic crop fitted to the open
// session's ashift input, headless; FALSE when it can't be (yet)
gboolean dt_api_ashift_fit_crop(dt_iop_module_t *ashift);
