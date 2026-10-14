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

/* darktable-api: the headless engine. the requests are served by
   src/api/server.c; see src/api/README.md */

#include "api/server.h"
#include "common/darktable.h"
#include "common/file_location.h"

#include <glib.h>
#include <glib-unix.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static GMainLoop *_loop = NULL;

static void _quit(void)
{
  g_main_loop_quit(_loop);
}

static gboolean _on_signal(gpointer data)
{
  g_main_loop_quit(_loop);
  return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
  dt_loc_init(NULL, NULL, NULL, NULL, NULL, NULL);
  char localedir[PATH_MAX] = { 0 };
  dt_loc_get_localedir(localedir, sizeof(localedir));
  bindtextdomain(GETTEXT_PACKAGE, localedir);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);

  // our options come before --core; everything after it goes to dt_init
  const char *listen_path = NULL;
  int max_sessions = 3, idle_exit_s = 0;
  int core_start = argc;
  for(int i = 1; i < argc; i++)
  {
    if(!g_strcmp0(argv[i], "--core")) { core_start = i + 1; break; }
    else if(!g_strcmp0(argv[i], "--listen") && i + 1 < argc) listen_path = argv[++i];
    else if(!g_strcmp0(argv[i], "--max-sessions") && i + 1 < argc) max_sessions = atoi(argv[++i]);
    else if(!g_strcmp0(argv[i], "--idle-exit") && i + 1 < argc) idle_exit_s = atoi(argv[++i]);
  }
  gboolean has_configdir = FALSE;
  for(int i = core_start; i < argc; i++)
    if(!g_strcmp0(argv[i], "--configdir") || !g_strcmp0(argv[i], "--library"))
      has_configdir = TRUE;
  // never fall back to the user's real ~/.config/darktable by accident
  if(!has_configdir)
  {
    fprintf(stderr, "usage: darktable-api [--listen <socket>] [--max-sessions <n>]"
                    " [--idle-exit <seconds>] --core --configdir <dir> [darktable options]\n");
    return 1;
  }

  GPtrArray *m = g_ptr_array_new();
  g_ptr_array_add(m, (gpointer)"darktable-api");
  for(int i = core_start; i < argc; i++) g_ptr_array_add(m, argv[i]);

  // libdarktable prints to stdout in places: keep the real stdout for replies
  fflush(stdout);
  const int out_fd = dup(STDOUT_FILENO);
  dup2(STDERR_FILENO, STDOUT_FILENO);

  // listen before dt_init: a second engine for the same socket gives up here
  // instead of waiting for the library lock
  int listen_fd = -1;
  if(listen_path && (listen_fd = dt_api_listen(listen_path)) < 0)
  {
    fprintf(stderr, "darktable-api: cannot listen on %s\n", listen_path);
    return 1;
  }

  if(dt_init(m->len, (char **)m->pdata, FALSE, TRUE, NULL))
  {
    fprintf(stderr, "darktable-api: dt_init failed\n");
    if(listen_path) g_unlink(listen_path);
    g_ptr_array_free(m, TRUE);
    return 1;
  }

  // a client that went away must not kill us; SIGTERM/SIGINT (launchd, a
  // restart) stop like a shutdown request
  signal(SIGPIPE, SIG_IGN);
  _loop = g_main_loop_new(NULL, FALSE);
  g_unix_signal_add(SIGTERM, _on_signal, NULL);
  g_unix_signal_add(SIGINT, _on_signal, NULL);

  const dt_api_options_t options = {
    .listen_fd = listen_fd, .socket_path = listen_path, .out_fd = out_fd,
    .max_sessions = max_sessions, .idle_exit_s = idle_exit_s,
    .in_gui = FALSE, .quit = _quit };
  dt_api_start(&options);
  g_main_loop_run(_loop);

  if(dt_api_stop())
  {
    // dt_cleanup would tear down the closed database and caches again;
    // there's nothing left to save
    _exit(0);
  }
  dt_cleanup();
  g_ptr_array_free(m, TRUE);
  return 0;
}
