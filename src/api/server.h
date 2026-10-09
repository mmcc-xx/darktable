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

#include <glib.h>

G_BEGIN_DECLS

// the darktable-api request server (src/api/server.c), served on the
// default GLib main context: by the darktable-api engine, or by darktable's
// window when started with --api

typedef struct dt_api_options_t
{
  int listen_fd;           // a socket from dt_api_listen(), or -1: serve stdin/stdout
  const char *socket_path; // removed again by dt_api_stop()
  int out_fd;              // stdout for the stdin/stdout client
  int max_sessions;        // images kept open at once
  int idle_exit_s;         // quit after this long with no client and nothing unsaved; 0: never
  gboolean in_gui;         // darktable's window serves: no release, no shutdown
  void (*quit)(void);      // asked to stop (shutdown, stdin closed, idle)
} dt_api_options_t;

// a listening unix socket only this user can connect to, or -1 (another
// server is listening there, or the path can't be used)
int dt_api_listen(const char *path);

// the socket path the example clients use for a config dir
gchar *dt_api_default_socket(const char *configdir);

// darktable's window, before it opens the library: if an engine serves the
// library on path, ask it to hand over (it releases the library, passes its
// unsaved edits along and exits). TRUE if it did; the edits are restored by
// dt_api_start
gboolean dt_api_handover(const char *path);

void dt_api_start(const dt_api_options_t *options);

// closes clients, the socket and every session; returns TRUE if the library
// was left released (no database to clean up)
gboolean dt_api_stop(void);

G_END_DECLS
