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

/* darktable-api: a long-running edit engine (proof of concept).

   Line-delimited JSON-RPC 2.0: one request per line, one response per line.
   All arguments after --core go to dt_init, so it opens whichever library
   --configdir/--library name and holds its lock while it runs.

     darktable-api [--listen <socket>] [--max-sessions <n>] [--idle-exit <s>]
                   --core --configdir <dir> [darktable options]

   without --listen it serves one client on stdin/stdout and exits when stdin
   closes. with --listen it serves every client that connects to the unix
   socket (mode 0700), so several front ends share one library: requests are
   handled one at a time, edit sessions belong to the engine (one per open
   image, at most --max-sessions, default 3), and each change is announced to
   the other clients as a notification {"method": "event", "params": {"type",
   "imgid", "history_end", "unsaved", "client"}} with type edit, saved,
   reset, reopened, closed, image (rating/label), library_released,
   library_acquired, handover or (darktable's window) darkroom: the photo its
   darkroom shows now, imgid 0 for none. --idle-exit stops the engine that many seconds after the
   last client left, unless an image has unsaved changes.

   the session methods (module_*, history_*, geometry_*, save, reset,
   render, session_close) work on params.imgid if given, else on the image the
   client last opened.

   this server also runs in darktable's window (darktable --api, or
   --api-socket <path>; src/common/darktable.c): on GTK's main thread, so the
   image shown in the darkroom is edited through the darkroom's own path
   (its widgets and history panel follow) and changes made in the window are
   announced to the clients. at startup the window asks an engine serving the
   same socket to hand over (method handover: it releases the library, passes
   its unsaved edits along and exits). unsaved edits of other images are kept
   in <socket>.drafts.json when a server stops and restored by the next one.

   methods:
     ping                        -> {"version", "server", "methods"}
     film_rolls                  -> film rolls with their image counts
     images_list {film_id, rating, label, offset, limit}
                                 -> images in folder/filename order; rating
                                    "visible" (default), "all", "rejected"
                                    or "1".."5" (at least); label 0..4
     image_info {imgid}          -> one image's row as in images_list
     thumbnail {imgid, size, path, quality}
                                 -> a JPEG from darktable's mipmap cache,
                                    rendered with the current history
     set_rating {imgid, rating}  -> 0..5 or "reject", as the lighttable
     set_label {imgid, label, on}
                                 -> color label 0..4 on or off
     session_open {imgid, fresh} -> opens the image for editing: loads it and
                                    replays its history up to history_end, as
                                    the darkroom does, or joins the session
                                    another client has open (unsaved changes
                                    included); fresh reloads it as saved.
                                    replies joined and unsaved
     session_close               -> closes the image's session for everyone
     module_list                 -> the open image's modules: enabled ones and
                                    those in its history, in pipe order
     module_get {operation, instance}
                                 -> the module's settings by name: value,
                                    default, declared range, enum names and
                                    labels (introspection)
     module_set {operation, instance, values: {field: value}}
                                 -> changes settings by name (all or none),
                                    switches the module on, adds a history
                                    item; enums by name, label or number;
                                    lists whole (nested arrays) or by
                                    element ("grey[1]", "x[0][3]")
     preset_list {operation, instance}
                                 -> the module's presets for its version:
                                    name, label, builtin, autoapply
     preset_apply {operation, instance, name}
                                 -> applies a preset (by name or label) as
                                    the presets menu does, one history item
     module_enable {operation, instance, enabled}
                                 -> switches a module on or off (in memory)
     history_list                -> the open image's history items, and
                                    history_end
     history_end {end}           -> undo/redo to a step (0 = original)
     geometry_get                -> orientation {rotation, mirrored}, angle,
                                    autocrop, crop {left, top, right,
                                    bottom}, aspect, frame_width/height (what
                                    the crop box is a fraction of) and the
                                    output width/height, full size
     geometry_set {rotate, flip, angle, autocrop, crop, aspect}
                                 -> turns by 90 degrees (rotate, clockwise)
                                    or mirrors (flip) in flip, sets the angle
                                    in ashift and fits its automatic crop as
                                    the darkroom does, sets crop's box (null
                                    removes it) and aspect; all or nothing
     save                        -> writes the history to the library (and
                                    the sidecar, if write_sidecar_files
                                    asks for it), as leaving the darkroom does
     reset                       -> starts the open image over: deletes its
                                    history (saved and unsaved) and reloads
                                    it, so darktable applies the workflow
                                    defaults and auto-apply presets again
     library_status              -> owned or released, the lock holder's pid,
                                    the open sessions
     library_release             -> closes the sessions (unsaved changes kept
                                    as drafts), the caches and the library,
                                    removing the lock files: darktable's GUI
                                    can open the library now
     library_acquire             -> takes the library back (refused while
                                    another live process holds it), reopens
                                    database and caches, reloads darktablerc
                                    and restores each draft whose image was
                                    not changed meanwhile
     export {imgid, format, quality, max_width, max_height, high_quality,
             upscale, style, path, on_conflict, save}
                                 -> exports the image's saved edit as
                                    darktable's export module does (its
                                    settings unless given): format and disk
                                    storage modules, metadata, tags. path is
                                    a variable pattern without extension.
                                    on_conflict: unique, overwrite,
                                    overwrite_if_changed or skip.
                                    save: true saves unsaved changes first;
                                    without it they are refused. replies
                                    the file written, or skipped
     render {width, height, path, quality, uncropped, zoom, center_x,
             center_y}
                                 -> writes an sRGB JPEG fitted inside
                                    width x height to path; uncropped
                                    leaves crop's box out; zoom (1 = 100%)
                                    renders the width x height region
                                    around center_x/center_y (fractions)
                                    at that scale and replies its region
     handover                    -> (engine) releases the library to
                                    darktable's window, replies the unsaved
                                    edits as drafts, and exits
     shutdown                    -> closes the session and exits (engine)

   edits stay in memory until save. each session keeps one pixelpipe with
   darktable's full-size cache, so a render after a change re-runs only the
   modules after the changed one. see src/api/README.md
*/

#include "api/server.h"
#include "common/darktable.h"
#include "common/colorlabels.h"
#include "common/database.h"
#include "common/file_location.h"
#include "common/history.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/iop_order.h"
#include "common/mipmap_cache.h"
#include "common/ratings.h"
#include "common/styles.h"
#include "common/tags.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/pixelpipe_hb.h"
#include "gui/presets.h"
#include "imageio/imageio_jpeg.h"
#include "imageio/imageio_module.h"
#include "common/datetime.h"
#include "common/variables.h"
#include "control/signal.h"
#include "views/view.h"

#include <errno.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <poll.h>
#include <json-glib/json-glib.h>
#include <limits.h>
#include <signal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

// edit sessions: one per open image, shared by all clients. _cur is the
// session of the request being handled (_handle picks it)
typedef struct _session_t
{
  dt_develop_t *dev;
  dt_dev_pixelpipe_t pipe;
  dt_mipmap_buffer_t buf;
  gboolean pipe_changed;
  gboolean dirty;                    // edits since the last open or save
  gint64 last_use;
  // darktable's window: the darkroom's image, backed by darktable.develop;
  // renders go through mirror, a session reloaded when the darkroom's edit
  // changed (mirror_gen != _gui_gen)
  gboolean gui;
  struct _session_t *mirror;
  guint64 mirror_gen;
} _session_t;

static GList *_sessions = NULL;
static _session_t *_cur = NULL;

// ashift's automatic crop fit (iop/ashift.c: dt_iop_ashift_fit_crop)
typedef gboolean (*_fit_crop_t)(dt_iop_params_t *params, int width, int height);
static int _max_sessions = 3;        // each holds a full-size raw and a pipe cache

static gboolean _in_gui = FALSE;     // darktable's window serves the API
static _session_t _gui_session;      // the darkroom's image (in_gui only)
static guint64 _gui_gen = 1;         // bumped whenever the darkroom's edit changes
static guint32 _gui_hash_seen = 0;   // the darkroom state clients last heard of
static gboolean _api_editing = FALSE; // the API is changing the darkroom's edit

struct _client_t;
static void _notify(const struct _client_t *from, const char *type, const dt_imgid_t imgid);

// the image darktable's darkroom shows, when its window serves the API
static dt_imgid_t _darkroom_image(void)
{
  if(!_in_gui || !darktable.develop || !(dt_view_get_current() & DT_VIEW_DARKROOM))
    return NO_IMGID;
  return darktable.develop->image_storage.id;
}

// a cheap fingerprint of the darkroom's edit: tells darkroom changes apart
// from those the API made itself (both raise DT_SIGNAL_DEVELOP_HISTORY_CHANGE)
static guint32 _gui_hash(void)
{
  guint32 h = 2166136261u;
#define _MIX(p, n) for(size_t _k = 0; _k < (n); _k++) h = (h ^ ((const uint8_t *)(p))[_k]) * 16777619u
  const dt_develop_t *dev = darktable.develop;
  _MIX(&dev->history_end, sizeof(dev->history_end));
  for(const GList *l = dev->iop; l; l = g_list_next(l))
  {
    const dt_iop_module_t *m = l->data;
    _MIX(m->op, strlen(m->op));
    _MIX(&m->multi_priority, sizeof(m->multi_priority));
    _MIX(&m->enabled, sizeof(m->enabled));
    if(m->params && m->params_size > 0) _MIX(m->params, m->params_size);
  }
#undef _MIX
  return h;
}

// the API changed the darkroom's edit: renders need a new mirror, and the
// signal this raises must not be announced as a change made in the window
static void _gui_changed_by_api(void)
{
  _gui_gen++;
  _gui_hash_seen = _gui_hash();
}

static gboolean _in_history(const dt_iop_module_t *m)
{
  for(GList *h = _cur->dev->history; h; h = g_list_next(h))
  {
    const dt_dev_history_item_t *hi = h->data;
    if(hi->module == m && hi->num < _cur->dev->history_end) return TRUE;
  }
  return FALSE;
}

// asked for an unknown id, the image cache leaves a read lock behind and the
// next load of any image waits on it forever, so check the library first
static gboolean _image_exists(const dt_imgid_t imgid)
{
  sqlite3_stmt *st = NULL;
  gboolean found = FALSE;
  if(sqlite3_prepare_v2(dt_database_get(darktable.db),
                        "SELECT 1 FROM main.images WHERE id = ?1",
                        -1, &st, NULL) == SQLITE_OK)
  {
    sqlite3_bind_int(st, 1, imgid);
    found = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
  }
  return found;
}

static void _session_free(_session_t *s)
{
  dt_dev_pixelpipe_cleanup(&s->pipe);
  dt_mipmap_cache_release(&s->buf);
  dt_dev_cleanup(s->dev);
  g_free(s->dev);
  g_free(s);
}

// sessions other than the darkroom's
static _session_t *_session_find_listed(const dt_imgid_t imgid)
{
  for(GList *l = _sessions; l; l = g_list_next(l))
  {
    _session_t *s = l->data;
    if(s->dev->image_storage.id == imgid) return s;
  }
  return NULL;
}

static _session_t *_session_find(const dt_imgid_t imgid)
{
  // the image in darktable's darkroom is edited there, not in a copy
  if(dt_is_valid_imgid(imgid) && imgid == _darkroom_image())
  {
    _gui_session.dev = darktable.develop;
    _gui_session.gui = TRUE;
    return &_gui_session;
  }
  for(GList *l = _sessions; l; l = g_list_next(l))
  {
    _session_t *s = l->data;
    if(s->dev->image_storage.id == imgid) return s;
  }
  return NULL;
}

static void _session_close(_session_t *s)
{
  if(!s || s->gui) return;           // the darkroom's image stays open there
  _sessions = g_list_remove(_sessions, s);
  if(_cur == s) _cur = NULL;
  _session_free(s);
}

static void _session_close_all(void)
{
  while(_sessions) _session_close(_sessions->data);
}

// load an image into a new session: its history replayed up to history_end,
// as the darkroom does, and a pipe on its full-size buffer. listed: one of
// the open sessions (else a darkroom mirror)
static _session_t *_session_load_ext(const dt_imgid_t imgid, const gboolean listed, gchar **err)
{
  _session_t *s = g_new0(_session_t, 1);
  s->dev = g_new0(dt_develop_t, 1);
  dt_dev_init(s->dev, FALSE);
  dt_dev_load_image(s->dev, imgid);
  // dt_dev_load_image reads the history but leaves module params at their
  // defaults; replaying it is what the darkroom does when it shows an image
  dt_dev_pop_history_items_ext(s->dev, s->dev->history_end);
  dt_ioppr_resync_modules_order(s->dev);

  dt_mipmap_cache_get(&s->buf, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
  if(!s->buf.buf || !s->buf.width || !s->buf.height)
  {
    *err = g_strdup_printf("cannot load the image file of %d", imgid);
    dt_mipmap_cache_release(&s->buf);
    dt_dev_cleanup(s->dev);
    g_free(s->dev);
    g_free(s);
    return NULL;
  }
  // a full (darkroom) pipe: an export pipe keeps only DT_PIPECACHE_MIN cache
  // lines, so every render would start again from the raw file
  dt_dev_pixelpipe_init(&s->pipe);
  dt_dev_pixelpipe_set_icc(&s->pipe, DT_COLORSPACE_SRGB, NULL, DT_INTENT_PERCEPTUAL);
  dt_dev_pixelpipe_set_input(&s->pipe, s->dev, (float *)s->buf.buf,
                             s->buf.width, s->buf.height, s->buf.iscale);
  dt_dev_pixelpipe_create_nodes(&s->pipe, s->dev);
  s->pipe_changed = TRUE;
  s->last_use = g_get_monotonic_time();
  if(listed) _sessions = g_list_append(_sessions, s);
  return s;
}

static _session_t *_session_load(const dt_imgid_t imgid, gchar **err)
{
  return _session_load_ext(imgid, TRUE, err);
}

static void _session_info(JsonBuilder *b, const _session_t *s)
{
  json_builder_set_member_name(b, "imgid");
  json_builder_add_int_value(b, s->dev->image_storage.id);
  json_builder_set_member_name(b, "filename");
  json_builder_add_string_value(b, s->dev->image_storage.filename);
  json_builder_set_member_name(b, "width");
  json_builder_add_int_value(b, s->dev->image_storage.width);
  json_builder_set_member_name(b, "height");
  json_builder_add_int_value(b, s->dev->image_storage.height);
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, s->dev->history_end);
  json_builder_set_member_name(b, "history_items");
  json_builder_add_int_value(b, g_list_length(s->dev->history));
  json_builder_set_member_name(b, "unsaved");
  json_builder_add_boolean_value(b, s->dirty);
}

// open an image for editing. a session another client has open is joined,
// unsaved changes included, unless fresh asks to reload it as saved (which
// drops those changes for everyone)
static gboolean _session_open(const dt_imgid_t imgid, const gboolean fresh,
                              JsonBuilder *b, gchar **err)
{
  if(!dt_is_valid_imgid(imgid) || !_image_exists(imgid))
  {
    *err = g_strdup_printf("no image with id %d", imgid);
    return FALSE;
  }
  const gint64 t0 = g_get_monotonic_time();
  _session_t *s = _session_find(imgid);
  if(s && fresh && !s->gui)          // the darkroom's edit is reloaded there
  {
    _session_close(s);
    s = NULL;
  }
  const gboolean joined = s != NULL;
  if(!s)
  {
    if(g_list_length(_sessions) >= (guint)_max_sessions)
    {
      // make room: close the least recently used session without unsaved
      // changes; never drop someone's unsaved edit
      _session_t *victim = NULL;
      for(GList *l = _sessions; l; l = g_list_next(l))
      {
        _session_t *o = l->data;
        if(!o->dirty && (!victim || o->last_use < victim->last_use)) victim = o;
      }
      if(!victim)
      {
        *err = g_strdup_printf("%d images are open with unsaved changes: save one, or reopen it"
                               " with fresh to discard them, first", _max_sessions);
        return FALSE;
      }
      _session_close(victim);
    }
    if(!(s = _session_load(imgid, err))) return FALSE;
  }
  s->last_use = g_get_monotonic_time();
  _cur = s;
  _session_info(b, s);
  json_builder_set_member_name(b, "joined");
  json_builder_add_boolean_value(b, joined);
  json_builder_set_member_name(b, "ms");
  json_builder_add_int_value(b, (g_get_monotonic_time() - t0) / 1000);
  return TRUE;
}

static gboolean _module_list(JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  json_builder_set_member_name(b, "modules");
  json_builder_begin_array(b);
  for(GList *l = _cur->dev->iop; l; l = g_list_next(l))
  {
    dt_iop_module_t *m = l->data;
    const gboolean in_hist = _in_history(m);
    if(!m->enabled && !in_hist) continue;
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "operation");
    json_builder_add_string_value(b, m->op);
    json_builder_set_member_name(b, "instance");
    json_builder_add_int_value(b, m->multi_priority);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, m->multi_name);
    json_builder_set_member_name(b, "enabled");
    json_builder_add_boolean_value(b, m->enabled);
    json_builder_set_member_name(b, "in_history");
    json_builder_add_boolean_value(b, in_hist);
    json_builder_set_member_name(b, "iop_order");
    json_builder_add_int_value(b, m->iop_order);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  return TRUE;
}

static dt_iop_module_t *_find_module(JsonObject *params, gchar **err)
{
  const gchar *op = params ? json_object_get_string_member_with_default(params, "operation", NULL) : NULL;
  const int instance = params ? json_object_get_int_member_with_default(params, "instance", 0) : 0;
  dt_iop_module_t *m = op ? dt_iop_get_module_by_op_priority(_cur->dev->iop, op, instance) : NULL;
  if(!m) *err = g_strdup_printf("no module '%s' instance %d", op ? op : "", instance);
  return m;
}

static gboolean _is_scalar(const dt_introspection_type_t t)
{
  switch(t)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:
    case DT_INTROSPECTION_TYPE_DOUBLE:
    case DT_INTROSPECTION_TYPE_INT:
    case DT_INTROSPECTION_TYPE_UINT:
    case DT_INTROSPECTION_TYPE_INT8:
    case DT_INTROSPECTION_TYPE_UINT8:
    case DT_INTROSPECTION_TYPE_SHORT:
    case DT_INTROSPECTION_TYPE_USHORT:
    case DT_INTROSPECTION_TYPE_BOOL:
    case DT_INTROSPECTION_TYPE_ENUM:
      return TRUE;
    default:
      return FALSE;
  }
}

// fields a client can set by name: scalars, not array elements (arrays are
// listed once, as their first element, so a name alone can't address them)
static gboolean _settable(const dt_introspection_field_t *f)
{
  return _is_scalar(f->header.type) && !strchr(f->header.name, '[');
}

static double _get_num(const dt_introspection_field_t *f, const void *p)
{
  switch(f->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:  return *(const float *)p;
    case DT_INTROSPECTION_TYPE_DOUBLE: return *(const double *)p;
    case DT_INTROSPECTION_TYPE_INT:    return *(const int *)p;
    case DT_INTROSPECTION_TYPE_UINT:   return *(const unsigned int *)p;
    case DT_INTROSPECTION_TYPE_INT8:   return *(const int8_t *)p;
    case DT_INTROSPECTION_TYPE_UINT8:  return *(const uint8_t *)p;
    case DT_INTROSPECTION_TYPE_SHORT:  return *(const short *)p;
    case DT_INTROSPECTION_TYPE_USHORT: return *(const unsigned short *)p;
    case DT_INTROSPECTION_TYPE_BOOL:   return *(const gboolean *)p != 0;
    case DT_INTROSPECTION_TYPE_ENUM:   return *(const int *)p;
    default:                           return 0.0;
  }
}

static void _set_num(const dt_introspection_field_t *f, void *p, const double v)
{
  switch(f->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:  *(float *)p = v; break;
    case DT_INTROSPECTION_TYPE_DOUBLE: *(double *)p = v; break;
    case DT_INTROSPECTION_TYPE_INT:    *(int *)p = v; break;
    case DT_INTROSPECTION_TYPE_UINT:   *(unsigned int *)p = v; break;
    case DT_INTROSPECTION_TYPE_INT8:   *(int8_t *)p = v; break;
    case DT_INTROSPECTION_TYPE_UINT8:  *(uint8_t *)p = v; break;
    case DT_INTROSPECTION_TYPE_SHORT:  *(short *)p = v; break;
    case DT_INTROSPECTION_TYPE_USHORT: *(unsigned short *)p = v; break;
    case DT_INTROSPECTION_TYPE_BOOL:   *(gboolean *)p = v != 0.0; break;
    case DT_INTROSPECTION_TYPE_ENUM:   *(int *)p = v; break;
    default: break;
  }
}

// the declared range ($MIN/$MAX in the params struct); FALSE for bool/enum
static gboolean _range(const dt_introspection_field_t *f, double *lo, double *hi, double *def)
{
  switch(f->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:  *lo = f->Float.Min;  *hi = f->Float.Max;  *def = f->Float.Default;  return TRUE;
    case DT_INTROSPECTION_TYPE_DOUBLE: *lo = f->Double.Min; *hi = f->Double.Max; *def = f->Double.Default; return TRUE;
    case DT_INTROSPECTION_TYPE_INT:    *lo = f->Int.Min;    *hi = f->Int.Max;    *def = f->Int.Default;    return TRUE;
    case DT_INTROSPECTION_TYPE_UINT:   *lo = f->UInt.Min;   *hi = f->UInt.Max;   *def = f->UInt.Default;   return TRUE;
    case DT_INTROSPECTION_TYPE_INT8:   *lo = f->Int8.Min;   *hi = f->Int8.Max;   *def = f->Int8.Default;   return TRUE;
    case DT_INTROSPECTION_TYPE_UINT8:  *lo = f->UInt8.Min;  *hi = f->UInt8.Max;  *def = f->UInt8.Default;  return TRUE;
    case DT_INTROSPECTION_TYPE_SHORT:  *lo = f->Short.Min;  *hi = f->Short.Max;  *def = f->Short.Default;  return TRUE;
    case DT_INTROSPECTION_TYPE_USHORT: *lo = f->UShort.Min; *hi = f->UShort.Max; *def = f->UShort.Default; return TRUE;
    default: return FALSE;
  }
}

static void _add_field_value(JsonBuilder *b, const dt_introspection_field_t *f, const void *params)
{
  const void *p = (const uint8_t *)params + f->header.offset;
  if(f->header.type == DT_INTROSPECTION_TYPE_BOOL)
    json_builder_add_boolean_value(b, *(const gboolean *)p != 0);
  else if(f->header.type == DT_INTROSPECTION_TYPE_ENUM)
  {
    const int v = *(const int *)p;
    const char *name = NULL;
    for(const dt_introspection_type_enum_tuple_t *e = f->Enum.values; e && e->name; e++)
      if(e->value == v) { name = e->name; break; }
    if(name) json_builder_add_string_value(b, name);
    else json_builder_add_int_value(b, v);
  }
  else if(f->header.type == DT_INTROSPECTION_TYPE_FLOAT || f->header.type == DT_INTROSPECTION_TYPE_DOUBLE)
    json_builder_add_double_value(b, _get_num(f, p));
  else
    json_builder_add_int_value(b, (gint64)_get_num(f, p));
}

// a list setting a client can address: an array (of arrays) of floats,
// bools or enums at the top of the params struct. not arrays of structs
// (curve nodes) or chars (strings), nor of integers: those are counts and
// indexes into other lists (rgbcurve's curve_num_nodes), whose declared
// range doesn't keep them inside the lists they count
static gboolean _settable_list(const dt_introspection_field_t *f)
{
  if(f->header.type != DT_INTROSPECTION_TYPE_ARRAY || strchr(f->header.name, '[') || strchr(f->header.name, '.'))
    return FALSE;
  const dt_introspection_field_t *e = f;
  while(e->header.type == DT_INTROSPECTION_TYPE_ARRAY) e = e->Array.field;
  const dt_introspection_type_t t = e->header.type;
  return t == DT_INTROSPECTION_TYPE_FLOAT || t == DT_INTROSPECTION_TYPE_DOUBLE
         || t == DT_INTROSPECTION_TYPE_BOOL || t == DT_INTROSPECTION_TYPE_ENUM;
}

static dt_introspection_field_t *_list_field(const dt_iop_module_t *m, const char *name)
{
  for(dt_introspection_field_t *f = m->so->get_introspection_linear();
      f && f->header.type != DT_INTROSPECTION_TYPE_NONE; f++)
    if(_settable_list(f) && !g_strcmp0(f->header.name, name)) return f;
  return NULL;
}

// the element a name like "grey[1]" or "x[0][3]" addresses: its scalar
// descriptor and offset in the params. a list's element descriptor carries
// the first element's offset (dev-doc/introspection.md), so step from the
// list's own
static dt_introspection_field_t *_element(const dt_iop_module_t *m, const char *name, size_t *offset)
{
  const char *br = strchr(name, '[');
  if(!br) return NULL;
  gchar *base = g_strndup(name, br - name);
  dt_introspection_field_t *f = _list_field(m, base);
  g_free(base);
  if(!f) return NULL;
  size_t off = f->header.offset;
  const char *p = br;
  while(f->header.type == DT_INTROSPECTION_TYPE_ARRAY)
  {
    char *end = NULL;
    if(*p != '[') return NULL;
    const long k = strtol(p + 1, &end, 10);
    if(end == p + 1 || *end != ']' || k < 0 || (size_t)k >= f->Array.count) return NULL;
    off += k * f->Array.field->header.size;
    f = f->Array.field;
    p = end + 1;
  }
  if(*p) return NULL;
  *offset = off;
  return f;
}

// a list's values as nested JSON arrays
static void _add_list_value(JsonBuilder *b, const dt_introspection_field_t *f, const void *p)
{
  json_builder_begin_array(b);
  for(size_t k = 0; k < f->Array.count; k++)
  {
    const void *e = (const uint8_t *)p + k * f->Array.field->header.size;
    if(f->Array.field->header.type == DT_INTROSPECTION_TYPE_ARRAY)
      _add_list_value(b, f->Array.field, e);
    else
      _add_field_value(b, f->Array.field, (const uint8_t *)e - f->Array.field->header.offset);
  }
  json_builder_end_array(b);
}

static void _add_list_shape(JsonBuilder *b, const dt_introspection_field_t *f)
{
  json_builder_begin_array(b);
  for(; f->header.type == DT_INTROSPECTION_TYPE_ARRAY; f = f->Array.field)
    json_builder_add_int_value(b, f->Array.count);
  json_builder_end_array(b);
}

static dt_introspection_field_t *_field(const dt_iop_module_t *m, const char *name)
{
  for(dt_introspection_field_t *f = m->so->get_introspection_linear();
      f && f->header.type != DT_INTROSPECTION_TYPE_NONE; f++)
    if(_settable(f) && !g_strcmp0(f->header.name, name)) return f;
  return NULL;
}

static gboolean _module_get(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  dt_iop_module_t *m = _find_module(params, err);
  if(!m) return FALSE;
  if(!m->so->have_introspection)
  {
    *err = g_strdup_printf("module '%s' has no introspection", m->op);
    return FALSE;
  }
  json_builder_set_member_name(b, "operation");
  json_builder_add_string_value(b, m->op);
  json_builder_set_member_name(b, "instance");
  json_builder_add_int_value(b, m->multi_priority);
  json_builder_set_member_name(b, "enabled");
  json_builder_add_boolean_value(b, m->enabled);
  json_builder_set_member_name(b, "version");
  json_builder_add_int_value(b, m->version());
  json_builder_set_member_name(b, "fields");
  json_builder_begin_array(b);
  for(dt_introspection_field_t *f = m->so->get_introspection_linear();
      f && f->header.type != DT_INTROSPECTION_TYPE_NONE; f++)
  {
    if(!_settable(f)) continue;
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, f->header.name);
    json_builder_set_member_name(b, "description");
    json_builder_add_string_value(b, f->header.description ? f->header.description : "");
    json_builder_set_member_name(b, "value");
    _add_field_value(b, f, m->params);
    json_builder_set_member_name(b, "default");
    _add_field_value(b, f, m->default_params);
    double lo, hi, def;
    if(_range(f, &lo, &hi, &def))
    {
      json_builder_set_member_name(b, "min");
      json_builder_add_double_value(b, lo);
      json_builder_set_member_name(b, "max");
      json_builder_add_double_value(b, hi);
    }
    if(f->header.type == DT_INTROSPECTION_TYPE_ENUM)
    {
      json_builder_set_member_name(b, "values");
      json_builder_begin_array(b);
      for(const dt_introspection_type_enum_tuple_t *e = f->Enum.values; e && e->name; e++)
      {
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "name");
        json_builder_add_string_value(b, e->name);
        json_builder_set_member_name(b, "label");
        json_builder_add_string_value(b, e->description ? e->description : "");
        json_builder_end_object(b);
      }
      json_builder_end_array(b);
    }
    json_builder_end_object(b);
  }
  // lists: their values nested by dimension, the element's range; set
  // whole, or one element by "name[i]" or "name[i][j]"
  for(dt_introspection_field_t *f = m->so->get_introspection_linear();
      f && f->header.type != DT_INTROSPECTION_TYPE_NONE; f++)
  {
    if(!_settable_list(f)) continue;
    const dt_introspection_field_t *e = f;
    while(e->header.type == DT_INTROSPECTION_TYPE_ARRAY) e = e->Array.field;
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, f->header.name);
    json_builder_set_member_name(b, "description");
    json_builder_add_string_value(b, f->header.description ? f->header.description : "");
    json_builder_set_member_name(b, "shape");
    _add_list_shape(b, f);
    json_builder_set_member_name(b, "value");
    _add_list_value(b, f, (const uint8_t *)m->params + f->header.offset);
    json_builder_set_member_name(b, "default");
    _add_list_value(b, f, (const uint8_t *)m->default_params + f->header.offset);
    double lo, hi, def;
    if(_range(e, &lo, &hi, &def))
    {
      json_builder_set_member_name(b, "min");
      json_builder_add_double_value(b, lo);
      json_builder_set_member_name(b, "max");
      json_builder_add_double_value(b, hi);
    }
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  return TRUE;
}

// parse one requested value into the field's number, or explain why not
static gboolean _parse_value_named(const dt_introspection_field_t *f, const char *name, JsonNode *v,
                                   double *out, gchar **err)
{
  if(f->header.type == DT_INTROSPECTION_TYPE_ENUM)
  {
    if(JSON_NODE_HOLDS_VALUE(v) && json_node_get_value_type(v) == G_TYPE_STRING)
    {
      const char *s = json_node_get_string(v);
      for(const dt_introspection_type_enum_tuple_t *e = f->Enum.values; e && e->name; e++)
        if(!g_ascii_strcasecmp(e->name, s)
           || (e->description && !g_ascii_strcasecmp(e->description, s)))
        {
          *out = e->value;
          return TRUE;
        }
      *err = g_strdup_printf("'%s': no value '%s'", name, s);
      return FALSE;
    }
  }
  if(!JSON_NODE_HOLDS_VALUE(v))
  {
    *err = g_strdup_printf("'%s': value must be a number%s", name,
                           f->header.type == DT_INTROSPECTION_TYPE_ENUM ? " or a name" : "");
    return FALSE;
  }
  const GType t = json_node_get_value_type(v);
  if(t == G_TYPE_BOOLEAN)
    *out = json_node_get_boolean(v) ? 1.0 : 0.0;
  else if(t == G_TYPE_INT64 || t == G_TYPE_DOUBLE)
    *out = json_node_get_double(v);
  else
  {
    *err = g_strdup_printf("'%s': value must be a number", name);
    return FALSE;
  }
  if(f->header.type == DT_INTROSPECTION_TYPE_ENUM)
  {
    for(const dt_introspection_type_enum_tuple_t *e = f->Enum.values; e && e->name; e++)
      if(e->value == (int)*out) return TRUE;
    *err = g_strdup_printf("'%s': no value %d", name, (int)*out);
    return FALSE;
  }
  double lo, hi, def;
  if(_range(f, &lo, &hi, &def) && (*out < lo || *out > hi))
  {
    *err = g_strdup_printf("'%s': %g is outside %g..%g", name, *out, lo, hi);
    return FALSE;
  }
  return TRUE;
}

static gboolean _parse_value(const dt_introspection_field_t *f, JsonNode *v, double *out, gchar **err)
{
  return _parse_value_named(f, f->header.name, v, out, err);
}

// a whole list from nested JSON arrays of exactly its shape, into p
static gboolean _parse_list(const dt_introspection_field_t *f, const char *name, JsonNode *v,
                            uint8_t *p, gchar **err)
{
  JsonArray *a = JSON_NODE_HOLDS_ARRAY(v) ? json_node_get_array(v) : NULL;
  if(!a || json_array_get_length(a) != f->Array.count)
  {
    *err = g_strdup_printf("'%s': needs a list of %zu", name, f->Array.count);
    return FALSE;
  }
  for(size_t k = 0; k < f->Array.count; k++)
  {
    gchar *ename = g_strdup_printf("%s[%zu]", name, k);
    uint8_t *e = p + k * f->Array.field->header.size;
    gboolean ok;
    if(f->Array.field->header.type == DT_INTROSPECTION_TYPE_ARRAY)
      ok = _parse_list(f->Array.field, ename, json_array_get_element(a, k), e, err);
    else
    {
      double x;
      ok = _parse_value_named(f->Array.field, ename, json_array_get_element(a, k), &x, err);
      if(ok) _set_num(f->Array.field, e, x);
    }
    g_free(ename);
    if(!ok) return FALSE;
  }
  return TRUE;
}

// a module change becomes a history item, as in the darkroom. in darktable's
// window, the darkroom's own path for its image, so its widgets, history
// panel and pipes follow
static void _record(dt_iop_module_t *m, const gboolean enable)
{
  if(_cur->gui)
  {
    _api_editing = TRUE;
    dt_iop_gui_update(m);
    dt_dev_add_history_item(_cur->dev, m, enable);
    dt_iop_gui_set_enable_button(m);
    _api_editing = FALSE;
    _gui_changed_by_api();
  }
  else
  {
    dt_dev_add_history_item_ext(_cur->dev, m, enable, TRUE);
    _cur->pipe_changed = TRUE;
    _cur->dirty = TRUE;
  }
}

static gboolean _module_set(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  dt_iop_module_t *m = _find_module(params, err);
  if(!m) return FALSE;
  JsonObject *values = params && json_object_has_member(params, "values")
                       && JSON_NODE_HOLDS_OBJECT(json_object_get_member(params, "values"))
                         ? json_object_get_object_member(params, "values") : NULL;
  if(!values || !json_object_get_size(values))
  {
    *err = g_strdup("module_set needs values {field: value}");
    return FALSE;
  }
  if(!m->so->have_introspection)
  {
    *err = g_strdup_printf("module '%s' has no introspection", m->op);
    return FALSE;
  }

  // all or nothing: check every field before changing any
  void *p = g_malloc(m->params_size);
  memcpy(p, m->params, m->params_size);
  GList *names = json_object_get_members(values);
  gboolean ok = TRUE;
  for(GList *n = names; n && ok; n = g_list_next(n))
  {
    const char *name = n->data;
    JsonNode *node = json_object_get_member(values, name);
    const dt_introspection_field_t *f = _field(m, name);
    const dt_introspection_field_t *list = f ? NULL : _list_field(m, name);
    size_t off = 0;
    const dt_introspection_field_t *e = f || list ? NULL : _element(m, name, &off);
    double v;
    if(f)
    {
      if((ok = _parse_value(f, node, &v, err)))
        _set_num(f, (uint8_t *)p + f->header.offset, v);
    }
    else if(list)
      ok = _parse_list(list, name, node, (uint8_t *)p + list->header.offset, err);
    else if(e)
    {
      if((ok = _parse_value_named(e, name, node, &v, err)))
        _set_num(e, (uint8_t *)p + off, v);
    }
    else
    {
      *err = g_strdup_printf("module '%s' has no field '%s'", m->op, name);
      ok = FALSE;
    }
  }
  if(ok)
  {
    memcpy(m->params, p, m->params_size);
    // as in the darkroom: changing a module's setting switches it on
    _record(m, TRUE);
    json_builder_set_member_name(b, "enabled");
    json_builder_add_boolean_value(b, m->enabled);
    json_builder_set_member_name(b, "history_end");
    json_builder_add_int_value(b, _cur->dev->history_end);
    json_builder_set_member_name(b, "values");
    json_builder_begin_object(b);
    for(GList *n = names; n; n = g_list_next(n))
    {
      const char *name = n->data;
      const dt_introspection_field_t *f = _field(m, name), *list = f ? NULL : _list_field(m, name);
      size_t off = 0;
      const dt_introspection_field_t *e = f || list ? NULL : _element(m, name, &off);
      json_builder_set_member_name(b, name);
      if(f) _add_field_value(b, f, m->params);
      else if(list) _add_list_value(b, list, (const uint8_t *)m->params + list->header.offset);
      else _add_field_value(b, e, (const uint8_t *)m->params + off - e->header.offset);
    }
    json_builder_end_object(b);
  }
  g_list_free(names);
  g_free(p);
  return ok;
}

static gboolean _module_enable(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  dt_iop_module_t *m = _find_module(params, err);
  if(!m) return FALSE;
  m->enabled = json_object_get_boolean_member_with_default(params, "enabled", TRUE);
  _record(m, FALSE);
  json_builder_set_member_name(b, "enabled");
  json_builder_add_boolean_value(b, m->enabled);
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, _cur->dev->history_end);
  return TRUE;
}

static gboolean _history_list(JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, _cur->dev->history_end);
  json_builder_set_member_name(b, "items");
  json_builder_begin_array(b);
  int i = 0;
  for(GList *h = _cur->dev->history; h; h = g_list_next(h), i++)
  {
    const dt_dev_history_item_t *hi = h->data;
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "num");
    json_builder_add_int_value(b, i);
    json_builder_set_member_name(b, "operation");
    json_builder_add_string_value(b, hi->op_name);
    json_builder_set_member_name(b, "instance");
    json_builder_add_int_value(b, hi->multi_priority);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, hi->multi_name);
    json_builder_set_member_name(b, "enabled");
    json_builder_add_boolean_value(b, hi->enabled);
    json_builder_set_member_name(b, "applied");
    json_builder_add_boolean_value(b, i < _cur->dev->history_end);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  return TRUE;
}

// undo/redo to a step, as darktable's history panel: items above it stay
// until the next edit, which drops them
static gboolean _history_end(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  const int n = params ? json_object_get_int_member_with_default(params, "end", -1) : -1;
  const int count = g_list_length(_cur->dev->history);
  if(n < 0 || n > count)
  {
    *err = g_strdup_printf("end must be 0..%d", count);
    return FALSE;
  }
  if(_cur->gui)
  {
    // what darktable's history panel does on a click (libs/history.c)
    _api_editing = TRUE;
    dt_dev_undo_start_record(_cur->dev);
    dt_dev_pop_history_items(_cur->dev, n);
    dt_dev_reorder_gui_module_list(_cur->dev);
    dt_image_update_final_size(_cur->dev->image_storage.id);
    dt_dev_pixelpipe_cache_invalidate_later(_cur->dev->preview_pipe, 0, "api history_end: ");
    dt_dev_undo_end_record(_cur->dev);
    _api_editing = FALSE;
    _gui_changed_by_api();
  }
  else
  {
    dt_dev_pop_history_items_ext(_cur->dev, n);
    _cur->pipe_changed = TRUE;
    _cur->dirty = TRUE;
  }
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, _cur->dev->history_end);
  return TRUE;
}

static gboolean _save_session(_session_t *s)
{
  const dt_imgid_t imgid = s->dev->image_storage.id;
  // the history hash alone can miss a change: a draft restored after a
  // release can match the hash of an older thumbnail
  const gboolean was_dirty = s->dirty;
  dt_dev_write_history(s->dev);
  s->dirty = FALSE;
  const gboolean changed = was_dirty || !dt_history_hash_is_mipmap_synced(imgid);
  if(changed)
  {
    dt_image_cache_set_change_timestamp(imgid);
    dt_mipmap_cache_remove(imgid);
    dt_image_update_final_size(imgid);
    dt_image_write_sidecar_file(imgid);
    dt_history_hash_set_mipmap(imgid);
  }
  return changed;
}

// what the darkroom does when it leaves an image (views/darkroom.c, leave()):
// write the history, then invalidate the thumbnails and write the sidecar if
// the edit changed. the sidecar follows write_sidecar_files, so a library
// configured with "never" gets none
static gboolean _save(JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  const gint64 t0 = g_get_monotonic_time();
  const gboolean changed = _save_session(_cur);
  json_builder_set_member_name(b, "changed");
  json_builder_add_boolean_value(b, changed);
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, _cur->dev->history_end);
  json_builder_set_member_name(b, "history_items");
  json_builder_add_int_value(b, g_list_length(_cur->dev->history));
  json_builder_set_member_name(b, "ms");
  json_builder_add_int_value(b, (g_get_monotonic_time() - t0) / 1000);
  return TRUE;
}

// darktable's "discard history" (lighttable), then load the image again:
// _dev_auto_apply_presets (develop.c) applies the workflow's default modules
// and the user's auto-apply presets to an image with no history
static gboolean _reset(JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  const dt_imgid_t imgid = _cur->dev->image_storage.id;
  _session_t *s = _cur;
  if(s->gui)
  {
    // reloads the darkroom's history too (history.c: current image)
    _api_editing = TRUE;
    dt_history_delete_on_image_ext(imgid, FALSE, TRUE);
    _api_editing = FALSE;
  }
  else
  {
    _session_close(s);
    dt_history_delete_on_image_ext(imgid, FALSE, TRUE);
    if(!(s = _session_load(imgid, err))) return FALSE;
    _cur = s;
  }
  // the defaults exist only in memory until saved, as when darktable first
  // opens an image; write them so the library and thumbnails agree
  dt_dev_write_history(s->dev);
  dt_image_cache_set_change_timestamp(imgid);
  dt_mipmap_cache_remove(imgid);
  dt_image_write_sidecar_file(imgid);
  dt_history_hash_set_mipmap(imgid);
  if(s->gui) _gui_changed_by_api();
  _session_info(b, s);
  return TRUE;
}

// ---- presets ---------------------------------------------------------------
// a module's presets as its presets menu offers them: those stored for its
// current version (gui/presets.c, dt_gui_presets_apply_preset). built-in
// ones are stored as "_builtin_group | name"; label is what the menu shows

static gboolean _preset_list(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  dt_iop_module_t *m = _find_module(params, err);
  if(!m) return FALSE;
  sqlite3_stmt *st = NULL;
  // clang-format off
  sqlite3_prepare_v2(dt_database_get(darktable.db),
                     "SELECT name, description, writeprotect, autoapply"
                     " FROM data.presets"
                     " WHERE operation = ?1 AND op_version = ?2"
                     " ORDER BY writeprotect DESC, LOWER(name)",
                     -1, &st, NULL);
  // clang-format on
  sqlite3_bind_text(st, 1, m->op, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, m->version());
  json_builder_set_member_name(b, "operation");
  json_builder_add_string_value(b, m->op);
  json_builder_set_member_name(b, "presets");
  json_builder_begin_array(b);
  while(sqlite3_step(st) == SQLITE_ROW)
  {
    const char *name = (const char *)sqlite3_column_text(st, 0);
    const char *desc = (const char *)sqlite3_column_text(st, 1);
    gchar *label = dt_util_localize_segmented_name(name, TRUE);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, name);
    json_builder_set_member_name(b, "label");
    json_builder_add_string_value(b, label);
    json_builder_set_member_name(b, "description");
    json_builder_add_string_value(b, desc ? desc : "");
    json_builder_set_member_name(b, "builtin");
    json_builder_add_boolean_value(b, sqlite3_column_int(st, 2));
    json_builder_set_member_name(b, "autoapply");
    json_builder_add_boolean_value(b, sqlite3_column_int(st, 3));
    json_builder_end_object(b);
    g_free(label);
  }
  json_builder_end_array(b);
  sqlite3_finalize(st);
  return TRUE;
}

// apply a preset by its name or its label, as the presets menu does: the
// preset's settings, on/off state and blending, and its name as the
// module's label unless that was typed by hand
static gboolean _preset_apply(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  dt_iop_module_t *m = _find_module(params, err);
  if(!m) return FALSE;
  const char *want = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
  if(!want || !*want)
  {
    *err = g_strdup("preset_apply needs name (from preset_list)");
    return FALSE;
  }
  sqlite3_stmt *st = NULL;
  // clang-format off
  sqlite3_prepare_v2(dt_database_get(darktable.db),
                     "SELECT name, op_params, enabled, blendop_params, blendop_version,"
                     "       multi_name, multi_name_hand_edited"
                     " FROM data.presets"
                     " WHERE operation = ?1 AND op_version = ?2",
                     -1, &st, NULL);
  // clang-format on
  sqlite3_bind_text(st, 1, m->op, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, m->version());
  gboolean found = FALSE;
  gchar *name = NULL;
  while(!found && sqlite3_step(st) == SQLITE_ROW)
  {
    const char *n = (const char *)sqlite3_column_text(st, 0);
    gchar *label = dt_util_localize_segmented_name(n, TRUE);
    found = !g_strcmp0(n, want) || !g_ascii_strcasecmp(label, want);
    g_free(label);
    if(!found) continue;
    name = g_strdup(n);
    if(_cur->gui)
      break;                         // the darkroom applies it itself, below

    const void *op = sqlite3_column_blob(st, 1);
    const int op_len = sqlite3_column_bytes(st, 1);
    const void *bl = sqlite3_column_blob(st, 3);
    const int bl_len = sqlite3_column_bytes(st, 3);
    const int bl_version = sqlite3_column_int(st, 4);
    const char *multi_name = (const char *)sqlite3_column_text(st, 5);
    memcpy(m->params, op && op_len == m->params_size ? op : m->default_params, m->params_size);
    m->enabled = sqlite3_column_int(st, 2);
    if(bl && bl_version == dt_develop_blend_version() && bl_len == sizeof(dt_develop_blend_params_t))
      dt_iop_commit_blend_params(m, bl, NULL);
    else if(!bl || dt_develop_blend_legacy_params(m, bl, bl_version, m->blend_params,
                                                  dt_develop_blend_version(), bl_len))
      dt_iop_commit_blend_params(m, m->default_blendop_params, NULL);
    // dt_iop_update_multi_name, without its history item and header update
    if(dt_conf_get_bool("darkroom/ui/auto_module_name_update") && !m->multi_name_hand_edited)
    {
      gchar *mname = g_strdup(multi_name && *multi_name ? multi_name : n);
      g_strlcpy(m->multi_name, g_strstrip(mname), sizeof(m->multi_name));
      m->multi_name_hand_edited = sqlite3_column_int(st, 6);
      g_free(mname);
    }
  }
  sqlite3_finalize(st);
  if(!found)
  {
    *err = g_strdup_printf("module '%s' has no preset '%s'", m->op, want);
    return FALSE;
  }
  if(_cur->gui)
  {
    _api_editing = TRUE;
    dt_gui_presets_apply_preset(name, m);
    _api_editing = FALSE;
    _gui_changed_by_api();
  }
  else
    _record(m, FALSE);
  json_builder_set_member_name(b, "name");
  json_builder_add_string_value(b, name);
  json_builder_set_member_name(b, "enabled");
  json_builder_add_boolean_value(b, m->enabled);
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, _cur->dev->history_end);
  g_free(name);
  return TRUE;
}

// ---- geometry: orientation, rotation and crop ------------------------------
// three modules, as in the darkroom: flip (steps of 90 degrees and mirroring),
// ashift (rotate and perspective: the angle, and its automatic crop) and crop.
// crop's box is in fractions of its input: the image as oriented and rotated

// the session whose pipe shows the open image's edit: the darkroom's image
// renders through a mirror, reloaded from the darkroom's history (written
// first, as its autosave does) when that changed
static _session_t *_pipe_session(gchar **err)
{
  if(!_cur->gui) return _cur;
  _session_t *gui = _cur;
  const dt_imgid_t imgid = gui->dev->image_storage.id;
  if(!gui->mirror || gui->mirror_gen != _gui_gen || gui->mirror->dev->image_storage.id != imgid)
  {
    dt_dev_write_history(gui->dev);
    if(gui->mirror) _session_free(gui->mirror);
    gui->mirror = _session_load_ext(imgid, FALSE, err);
    gui->mirror_gen = _gui_gen;
  }
  return gui->mirror;
}

// parameters changed or modules switched: syncing the nodes is enough, and
// the cache keeps every line whose inputs are unchanged
static void _pipe_sync(_session_t *s)
{
  if(!s->pipe_changed) return;
  dt_dev_pixelpipe_synch_all(&s->pipe, s->dev);
  dt_dev_pixelpipe_get_dimensions(&s->pipe, s->dev, s->pipe.iwidth, s->pipe.iheight,
                                  &s->pipe.processed_width, &s->pipe.processed_height);
  s->pipe_changed = FALSE;
}

// a module's node in a synced pipe; buf_in is its full-size input
static dt_dev_pixelpipe_iop_t *_piece(_session_t *s, const char *op)
{
  for(GList *n = s->pipe.nodes; n; n = g_list_next(n))
  {
    dt_dev_pixelpipe_iop_t *piece = n->data;
    if(!g_strcmp0(piece->module->op, op) && piece->module->multi_priority == 0) return piece;
  }
  return NULL;
}

static double _param(const dt_iop_module_t *m, const char *name)
{
  const dt_introspection_field_t *f = _field(m, name);
  return f ? _get_num(f, (const uint8_t *)m->params + f->header.offset) : 0.0;
}

static void _set_param(dt_iop_module_t *m, const char *name, const double v)
{
  const dt_introspection_field_t *f = _field(m, name);
  if(f) _set_num(f, (uint8_t *)m->params + f->header.offset, v);
}

// what flip shows: its orientation, the image's own one if "autodetect"
static dt_image_orientation_t _orientation(const dt_iop_module_t *flip)
{
  if(!flip->enabled) return ORIENTATION_NONE;
  const dt_image_orientation_t o = _param(flip, "orientation");
  return o == ORIENTATION_NULL ? dt_image_orientation(&flip->dev->image_storage) : o;
}

// an orientation as clockwise degrees after an optional horizontal mirror
static const struct { dt_image_orientation_t o; int degrees; gboolean mirrored; } _orientations[] = {
  { ORIENTATION_NONE, 0, FALSE }, { ORIENTATION_ROTATE_CW_90_DEG, 90, FALSE },
  { ORIENTATION_ROTATE_180_DEG, 180, FALSE }, { ORIENTATION_ROTATE_CCW_90_DEG, 270, FALSE },
  { ORIENTATION_FLIP_HORIZONTALLY, 0, TRUE }, { ORIENTATION_TRANSVERSE, 90, TRUE },
  { ORIENTATION_FLIP_VERTICALLY, 180, TRUE }, { ORIENTATION_TRANSPOSE, 270, TRUE } };

// what flip's buttons do (iop/flip.c: do_rotate, _flip_h, _flip_v), and what
// crop does to its box then (iop/crop.c: _crop_handle_flip)
static dt_image_orientation_t _turn(dt_image_orientation_t o, const dt_image_orientation_t mode, float box[4])
{
  const gboolean swapped = o & ORIENTATION_SWAP_XY;
  if(mode == ORIENTATION_ROTATE_CW_90_DEG)
    o = (o ^ (swapped ? ORIENTATION_FLIP_X : ORIENTATION_FLIP_Y)) ^ ORIENTATION_SWAP_XY;
  else if(mode == ORIENTATION_ROTATE_CCW_90_DEG)
    o = (o ^ (swapped ? ORIENTATION_FLIP_Y : ORIENTATION_FLIP_X)) ^ ORIENTATION_SWAP_XY;
  else if(mode == ORIENTATION_FLIP_HORIZONTALLY)
    o ^= swapped ? ORIENTATION_FLIP_VERTICALLY : ORIENTATION_FLIP_HORIZONTALLY;
  else
    o ^= swapped ? ORIENTATION_FLIP_HORIZONTALLY : ORIENTATION_FLIP_VERTICALLY;

  // box: left, top, right, bottom
  const float l = box[0], t = box[1], r = box[2], b = box[3];
  if(mode == ORIENTATION_FLIP_HORIZONTALLY)      { box[0] = 1.f - r; box[2] = 1.f - l; }
  else if(mode == ORIENTATION_FLIP_VERTICALLY)   { box[1] = 1.f - b; box[3] = 1.f - t; }
  else if(mode == ORIENTATION_ROTATE_CW_90_DEG)  { box[0] = 1.f - b; box[1] = l; box[2] = 1.f - t; box[3] = r; }
  else                                           { box[0] = t; box[1] = 1.f - r; box[2] = b; box[3] = 1.f - l; }
  return o;
}

static void _crop_box(const dt_iop_module_t *crop, float box[4])
{
  box[0] = _param(crop, "cx");
  box[1] = _param(crop, "cy");
  box[2] = _param(crop, "cw");
  box[3] = _param(crop, "ch");
}

static gboolean _full_box(const float box[4])
{
  return box[0] == 0.f && box[1] == 0.f && box[2] == 1.f && box[3] == 1.f;
}

// crop's ratio_d/ratio_n as darktable's aspect presets store them: d is the
// long side, negative when the box's orientation differs from the image's
// (iop/crop.c: _aspect_ratio_get, _aspect_apply)
static gchar *_aspect_name(const int d, const int n, const gboolean landscape)
{
  if(n == 0 && abs(d) == 1) return g_strdup("original");
  if(n <= 0 || d == 0) return g_strdup("free");
  const gboolean wide = (d > 0) == landscape;
  const int lo = MIN(abs(d), n), hi = MAX(abs(d), n);
  return wide ? g_strdup_printf("%d:%d", hi, lo) : g_strdup_printf("%d:%d", lo, hi);
}

// "free", "original", "square" or "W:H" (box width:height, e.g. "3:2" or
// "2:3"); *w/*h is the box's ratio, 0 for free and original
static gboolean _parse_aspect(const char *s, int *d, int *n, double *w, double *h, gchar **err)
{
  *w = *h = 0.0;
  if(!g_ascii_strcasecmp(s, "free") || !g_ascii_strcasecmp(s, "freehand")) { *d = *n = 0; return TRUE; }
  if(!g_ascii_strcasecmp(s, "original")) { *d = 1; *n = 0; return TRUE; }
  if(!g_ascii_strcasecmp(s, "square")) s = "1:1";
  gchar **p = g_strsplit(s, ":", 2);
  gchar *e0 = NULL, *e1 = NULL;
  const double a = p[0] ? g_ascii_strtod(p[0], &e0) : 0.0;
  const double b = p[0] && p[1] ? g_ascii_strtod(p[1], &e1) : 0.0;
  const gboolean ok = e0 && e1 && !*e0 && !*e1 && a > 0.0 && b > 0.0 && a / b < 100.0 && b / a < 100.0;
  g_strfreev(p);
  if(!ok)
  {
    *err = g_strdup_printf("aspect '%s': use free, original, square or W:H such as 3:2", s);
    return FALSE;
  }
  // crop stores integers: 1.91:1 becomes 191:100
  const double k = (a == floor(a) && b == floor(b) && a < 100000 && b < 100000) ? 1.0 : 100.0;
  *w = a;
  *h = b;
  *d = (int)round(MAX(a, b) * k);  // the sign is set once the image's orientation is known
  *n = (int)round(MIN(a, b) * k);
  return TRUE;
}

static void _add_box(JsonBuilder *b, const float box[4])
{
  static const char *names[] = { "left", "top", "right", "bottom" };
  json_builder_begin_object(b);
  for(int k = 0; k < 4; k++)
  {
    json_builder_set_member_name(b, names[k]);
    json_builder_add_double_value(b, round(box[k] * 1e5) / 1e5);
  }
  json_builder_end_object(b);
}

static gboolean _geometry_get(JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  dt_iop_module_t *flip = dt_iop_get_module_by_op_priority(_cur->dev->iop, "flip", 0);
  dt_iop_module_t *ashift = dt_iop_get_module_by_op_priority(_cur->dev->iop, "ashift", 0);
  dt_iop_module_t *crop = dt_iop_get_module_by_op_priority(_cur->dev->iop, "crop", 0);
  _session_t *s = _pipe_session(err);
  if(!s) return FALSE;
  _pipe_sync(s);
  const dt_dev_pixelpipe_iop_t *cp = _piece(s, "crop");

  json_builder_set_member_name(b, "orientation");
  json_builder_begin_object(b);
  const dt_image_orientation_t o = flip ? _orientation(flip) : ORIENTATION_NONE;
  for(int k = 0; k < (int)G_N_ELEMENTS(_orientations); k++)
    if(_orientations[k].o == o)
    {
      json_builder_set_member_name(b, "rotation");
      json_builder_add_int_value(b, _orientations[k].degrees);
      json_builder_set_member_name(b, "mirrored");
      json_builder_add_boolean_value(b, _orientations[k].mirrored);
    }
  json_builder_end_object(b);

  json_builder_set_member_name(b, "angle");
  json_builder_add_double_value(b, ashift && ashift->enabled ? round(_param(ashift, "rotation") * 1e4) / 1e4 : 0.0);
  if(ashift)
  {
    json_builder_set_member_name(b, "autocrop");
    _add_field_value(b, _field(ashift, "cropmode"), ashift->params);
  }

  float box[4] = { 0.f, 0.f, 1.f, 1.f };
  if(crop && crop->enabled) _crop_box(crop, box);
  json_builder_set_member_name(b, "crop");
  _add_box(b, box);
  const int fw = cp ? cp->buf_in.width : s->pipe.processed_width;
  const int fh = cp ? cp->buf_in.height : s->pipe.processed_height;
  gchar *aspect = crop && crop->enabled ? _aspect_name(_param(crop, "ratio_d"), _param(crop, "ratio_n"), fw >= fh)
                                        : g_strdup("free");
  json_builder_set_member_name(b, "aspect");
  json_builder_add_string_value(b, aspect);
  g_free(aspect);
  json_builder_set_member_name(b, "frame_width");
  json_builder_add_int_value(b, fw);
  json_builder_set_member_name(b, "frame_height");
  json_builder_add_int_value(b, fh);
  json_builder_set_member_name(b, "width");
  json_builder_add_int_value(b, s->pipe.processed_width);
  json_builder_set_member_name(b, "height");
  json_builder_add_int_value(b, s->pipe.processed_height);
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, _cur->dev->history_end);
  return TRUE;
}

// orientation, rotation and crop in one request, all or nothing, applied in
// pipe order: each module changed becomes a history item, as in the darkroom
static gboolean _geometry_set(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  dt_iop_module_t *flip = dt_iop_get_module_by_op_priority(_cur->dev->iop, "flip", 0);
  dt_iop_module_t *ashift = dt_iop_get_module_by_op_priority(_cur->dev->iop, "ashift", 0);
  dt_iop_module_t *crop = dt_iop_get_module_by_op_priority(_cur->dev->iop, "crop", 0);
  if(!flip || !ashift || !crop)
  {
    *err = g_strdup("this darktable lacks the flip, ashift or crop module");
    return FALSE;
  }
  JsonNode *n_rotate = params ? json_object_get_member(params, "rotate") : NULL;
  JsonNode *n_flip = params ? json_object_get_member(params, "flip") : NULL;
  JsonNode *n_angle = params ? json_object_get_member(params, "angle") : NULL;
  JsonNode *n_autocrop = params ? json_object_get_member(params, "autocrop") : NULL;
  JsonNode *n_crop = params ? json_object_get_member(params, "crop") : NULL;
  JsonNode *n_aspect = params ? json_object_get_member(params, "aspect") : NULL;
  if(!n_rotate && !n_flip && !n_angle && !n_autocrop && !n_crop && !n_aspect)
  {
    *err = g_strdup("geometry_set needs rotate, flip, angle, autocrop, crop or aspect");
    return FALSE;
  }

  // check everything before changing anything
  int turns = 0;
  if(n_rotate)
  {
    const int deg = JSON_NODE_HOLDS_VALUE(n_rotate) ? json_node_get_int(n_rotate) : 1;
    if(!JSON_NODE_HOLDS_VALUE(n_rotate) || json_node_get_value_type(n_rotate) != G_TYPE_INT64 || deg % 90)
    {
      *err = g_strdup("rotate: degrees clockwise, a multiple of 90 (-90 turns left)");
      return FALSE;
    }
    turns = ((deg / 90) % 4 + 4) % 4;
  }
  dt_image_orientation_t mirror = 0;
  if(n_flip)
  {
    const char *f = JSON_NODE_HOLDS_VALUE(n_flip) && json_node_get_value_type(n_flip) == G_TYPE_STRING
                    ? json_node_get_string(n_flip) : "";
    if(!g_ascii_strcasecmp(f, "horizontal")) mirror = ORIENTATION_FLIP_HORIZONTALLY;
    else if(!g_ascii_strcasecmp(f, "vertical")) mirror = ORIENTATION_FLIP_VERTICALLY;
    else
    {
      *err = g_strdup("flip: \"horizontal\" or \"vertical\"");
      return FALSE;
    }
  }
  double angle = 0.0, autocrop = 0.0;
  if(n_angle && !_parse_value(_field(ashift, "rotation"), n_angle, &angle, err)) return FALSE;
  if(n_autocrop && !_parse_value(_field(ashift, "cropmode"), n_autocrop, &autocrop, err)) return FALSE;
  float want[4] = { 0.f, 0.f, 1.f, 1.f };
  const gboolean uncrop = n_crop && JSON_NODE_HOLDS_NULL(n_crop);
  if(n_crop && !uncrop)
  {
    JsonObject *c = JSON_NODE_HOLDS_OBJECT(n_crop) ? json_node_get_object(n_crop) : NULL;
    static const char *names[] = { "left", "top", "right", "bottom" };
    for(int k = 0; k < 4; k++)
    {
      JsonNode *v = c ? json_object_get_member(c, names[k]) : NULL;
      if(!v || !JSON_NODE_HOLDS_VALUE(v)
         || (json_node_get_value_type(v) != G_TYPE_DOUBLE && json_node_get_value_type(v) != G_TYPE_INT64))
      {
        *err = g_strdup("crop: {left, top, right, bottom} as fractions 0..1 of the image, or null for none");
        return FALSE;
      }
      want[k] = json_node_get_double(v);
    }
    if(want[0] < 0.f || want[1] < 0.f || want[2] > 1.f || want[3] > 1.f
       || want[2] - want[0] < 0.01f || want[3] - want[1] < 0.01f)
    {
      *err = g_strdup("crop: needs 0 <= left < right <= 1 and 0 <= top < bottom <= 1");
      return FALSE;
    }
  }
  int ratio_d = 0, ratio_n = 0;
  double aw = 0.0, ah = 0.0;
  if(n_aspect)
  {
    const char *a = JSON_NODE_HOLDS_VALUE(n_aspect) && json_node_get_value_type(n_aspect) == G_TYPE_STRING
                    ? json_node_get_string(n_aspect) : "";
    if(!_parse_aspect(a, &ratio_d, &ratio_n, &aw, &ah, err)) return FALSE;
    if(uncrop)
    {
      *err = g_strdup("crop null removes the crop: no aspect with it");
      return FALSE;
    }
  }
  _fit_crop_t fit = NULL;
  if((n_angle || n_autocrop)
     && !g_module_symbol(ashift->so->module, "dt_iop_ashift_fit_crop", (gpointer *)&fit))
    fit = NULL;

  // flip first, then ashift, then crop: each one changes what the next sees
  float box[4];
  _crop_box(crop, box);
  const gboolean had_box = !_full_box(box);
  if(turns || mirror)
  {
    dt_image_orientation_t o = _orientation(flip);
    const dt_image_orientation_t mode = turns == 3 ? ORIENTATION_ROTATE_CCW_90_DEG : ORIENTATION_ROTATE_CW_90_DEG;
    for(int k = 0; k < (turns == 3 ? 1 : turns); k++) o = _turn(o, mode, box);
    if(mirror) o = _turn(o, mirror, box);
    _set_param(flip, "orientation", o);
    _record(flip, TRUE);
    if(had_box && !n_crop)
    {
      // as the darkroom does: the box follows the image
      _set_param(crop, "cx", box[0]);
      _set_param(crop, "cy", box[1]);
      _set_param(crop, "cw", box[2]);
      _set_param(crop, "ch", box[3]);
      _record(crop, FALSE);
    }
  }

  gboolean autocropped = TRUE;
  if(n_angle || n_autocrop)
  {
    if(n_angle) _set_param(ashift, "rotation", angle);
    if(n_autocrop) _set_param(ashift, "cropmode", autocrop);
    // what gui_changed() does in the darkroom: fit the automatic crop to the
    // new rotation. ashift's input doesn't depend on any change above
    _session_t *s = _pipe_session(err);
    if(!s) return FALSE;
    _pipe_sync(s);
    const dt_dev_pixelpipe_iop_t *ap = _piece(s, "ashift");
    if(fit && ap)
      autocropped = fit(ashift->params, ap->buf_in.width, ap->buf_in.height);
    else
      autocropped = FALSE;
    _record(ashift, TRUE);
  }

  if(n_crop || n_aspect)
  {
    if(n_crop) memcpy(box, want, sizeof(box));
    else _crop_box(crop, box);
    if(uncrop)
    {
      memcpy(box, (float[4]){ 0.f, 0.f, 1.f, 1.f }, sizeof(box));
    }
    else if(n_aspect)
    {
      // the largest box of that aspect inside the given (or current) one,
      // centered on it, in pixels of crop's input
      _session_t *s = _pipe_session(err);
      if(!s) return FALSE;
      _pipe_sync(s);
      const dt_dev_pixelpipe_iop_t *cp = _piece(s, "crop");
      const double fw = cp ? cp->buf_in.width : s->pipe.processed_width;
      const double fh = cp ? cp->buf_in.height : s->pipe.processed_height;
      const gboolean landscape = fw >= fh;
      if(ratio_n == 0 && ratio_d == 1)
      {
        // the sensor's ratio, oriented as the image
        const double pw = _cur->dev->image_storage.p_width, ph = _cur->dev->image_storage.p_height;
        aw = landscape ? MAX(pw, ph) : MIN(pw, ph);
        ah = landscape ? MIN(pw, ph) : MAX(pw, ph);
      }
      else if(ratio_d)
        ratio_d = (aw >= ah) == landscape ? ratio_d : -ratio_d;
      if(aw > 0.0 && ah > 0.0 && fw > 0.0 && fh > 0.0)
      {
        double bw = (box[2] - box[0]) * fw, bh = (box[3] - box[1]) * fh;
        const double cx = (box[0] + box[2]) / 2.0 * fw, cy = (box[1] + box[3]) / 2.0 * fh;
        if(bw / bh > aw / ah) bw = bh * aw / ah;
        else bh = bw * ah / aw;
        box[0] = (cx - bw / 2.0) / fw;
        box[2] = (cx + bw / 2.0) / fw;
        box[1] = (cy - bh / 2.0) / fh;
        box[3] = (cy + bh / 2.0) / fh;
      }
      _set_param(crop, "ratio_d", ratio_d);
      _set_param(crop, "ratio_n", ratio_n);
    }
    else
    {
      // a box without an aspect is freehand
      _set_param(crop, "ratio_d", 0);
      _set_param(crop, "ratio_n", 0);
    }
    _set_param(crop, "cx", box[0]);
    _set_param(crop, "cy", box[1]);
    _set_param(crop, "cw", box[2]);
    _set_param(crop, "ch", box[3]);
    if(uncrop) crop->enabled = FALSE;
    _record(crop, !uncrop);
  }

  if(!_geometry_get(b, err)) return FALSE;
  if(n_angle || n_autocrop)
  {
    json_builder_set_member_name(b, "autocrop_fitted");
    json_builder_add_boolean_value(b, autocropped);
  }
  return TRUE;
}

// ---- export ----------------------------------------------------------------
// darktable's export: the disk storage and a format module, with the export
// module's settings (libs/export.c) unless the request overrides them, as
// _control_export_job_run (control/jobs/control_jobs.c) runs it. it exports
// the image's saved history, as the lighttable does

// the file the disk storage would write for imgid (imageio/storage/disk.c,
// store()): the pattern expanded, a directory getting $(FILE_NAME), the
// folder created, the extension added and the conflict setting applied.
// worked out here because the storage doesn't report its file, and its
// signal (DT_SIGNAL_IMAGE_EXPORT_TMPFILE) isn't raised without darktable's
// control running, as in the engine. NULL with *skipped when the conflict
// setting leaves an existing file alone
static gchar *_export_target(const dt_imgid_t imgid, const char *pattern_in, const int on_conflict,
                             dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata,
                             const gboolean upscale, gboolean *skipped, gchar **err)
{
  *skipped = FALSE;
  char input[PATH_MAX] = { 0 };
  dt_image_full_path(imgid, input, sizeof(input), NULL);
  dt_variables_params_t *vp = NULL;
  dt_variables_params_init(&vp);
  dt_variables_set_max_width_height(vp, fdata->max_width, fdata->max_height);
  dt_variables_set_upscale(vp, upscale);
  vp->filename = input;
  vp->jobcode = "export";
  vp->imgid = imgid;
  vp->sequence = 1;

  gchar *pattern = dt_util_fix_path(pattern_in);
  gchar *base = dt_variables_expand_path(vp, pattern, TRUE);
  const size_t n = base ? strlen(base) : 0;
  if(n && (base[n - 1] == '/' || base[n - 1] == '\\'))
  {
    // a pattern that expands to a directory gets $(FILE_NAME)
    size_t k = strlen(pattern);
    while(k > 1 && (pattern[k - 1] == '/' || pattern[k - 1] == '\\')) pattern[--k] = '\0';
    gchar *p2 = g_strconcat(pattern, G_DIR_SEPARATOR_S "$(FILE_NAME)", NULL);
    g_free(base);
    base = dt_variables_expand_path(vp, p2, TRUE);
    g_free(p2);
  }
  g_free(pattern);
  dt_variables_params_destroy(vp);
  if(!base || !*base)
  {
    g_free(base);
    *err = g_strdup_printf("the path pattern '%s' expands to nothing", pattern_in);
    return NULL;
  }

  gchar *dir = g_path_get_dirname(base);
  if(g_mkdir_with_parents(dir, 0755) || g_access(dir, W_OK | X_OK))
  {
    *err = g_strdup_printf("cannot write to the folder '%s'", dir);
    g_free(dir);
    g_free(base);
    return NULL;
  }
  g_free(dir);

  const char *ext = format->extension(fdata);
  gchar *file = g_strdup_printf("%s.%s", base, ext);
  if(g_file_test(file, G_FILE_TEST_EXISTS))
  {
    if(on_conflict == 0)          // a unique name: _01, _02, ...
      for(int seq = 1; g_file_test(file, G_FILE_TEST_EXISTS); seq++)
      {
        g_free(file);
        file = g_strdup_printf("%s_%.2d.%s", base, seq, ext);
      }
    else if(on_conflict == 3)     // skip
      *skipped = TRUE;
    else if(on_conflict == 2)     // overwrite if the edit changed since
    {
      GStatBuf st;
      const dt_image_t *img = dt_image_cache_get(imgid, 'r');
      const GTimeSpan changed = img ? img->change_timestamp : 0;
      dt_image_cache_read_release(img);
      if(!g_stat(file, &st))
      {
        GDateTime *gdt = g_date_time_new_from_unix_local(st.st_mtime);
        *skipped = dt_datetime_gdatetime_to_gtimespan(gdt) > changed;
        g_date_time_unref(gdt);
      }
    }
  }
  g_free(base);
  if(*skipped)
  {
    g_free(file);
    return NULL;
  }
  return file;
}

// a conf key set for one request, then put back
typedef struct _conf_override_t
{
  gchar *key;
  gchar *saved;
} _conf_override_t;

static void _conf_override(GList **list, const char *key, const char *value)
{
  _conf_override_t *o = g_new0(_conf_override_t, 1);
  o->key = g_strdup(key);
  o->saved = dt_conf_key_exists(key) ? dt_conf_get_string(key) : NULL;
  dt_conf_set_string(key, value);
  *list = g_list_prepend(*list, o);
}

static void _conf_restore(GList *list)
{
  for(GList *l = list; l; l = g_list_next(l))
  {
    _conf_override_t *o = l->data;
    dt_conf_set_string(o->key, o->saved ? o->saved : "");
    g_free(o->key);
    g_free(o->saved);
    g_free(o);
  }
  g_list_free(list);
}

static gboolean _export(JsonObject *params, const dt_imgid_t current, gboolean *saved,
                        JsonBuilder *b, gchar **err)
{
  const dt_imgid_t imgid = params && json_object_has_member(params, "imgid")
    ? json_object_get_int_member(params, "imgid") : current;
  if(!dt_is_valid_imgid(imgid) || !_image_exists(imgid))
  {
    *err = g_strdup_printf("no image with id %d", imgid);
    return FALSE;
  }
  const gchar *fmt_name = params ? json_object_get_string_member_with_default(params, "format", NULL) : NULL;
  const gchar *pattern = params ? json_object_get_string_member_with_default(params, "path", NULL) : NULL;
  const gchar *conflict = params ? json_object_get_string_member_with_default(params, "on_conflict", NULL) : NULL;
  const gchar *style = params ? json_object_get_string_member_with_default(params, "style", NULL) : NULL;
  const int quality = params ? json_object_get_int_member_with_default(params, "quality", 0) : 0;
  const int max_w = params ? json_object_get_int_member_with_default(params, "max_width", -1) : -1;
  const int max_h = params ? json_object_get_int_member_with_default(params, "max_height", -1) : -1;
  const gboolean save = params ? json_object_get_boolean_member_with_default(params, "save", FALSE) : FALSE;
  const gboolean hq = params && json_object_has_member(params, "high_quality")
    ? json_object_get_boolean_member(params, "high_quality")
    : dt_conf_get_bool("plugins/lighttable/export/high_quality_processing");
  const gboolean upscale = params && json_object_has_member(params, "upscale")
    ? json_object_get_boolean_member(params, "upscale")
    : dt_conf_get_bool("plugins/lighttable/export/upscale");

  // in the disk storage's numbering (imageio/storage/disk.c:54); the
  // default is its setting
  static const char *conflicts[] = { "unique", "overwrite", "overwrite_if_changed", "skip", NULL };
  int conflict_action = -1;
  for(int k = 0; conflict && conflicts[k]; k++)
    if(!g_strcmp0(conflict, conflicts[k])) conflict_action = k;
  if(conflict && conflict_action < 0)
  {
    *err = g_strdup("on_conflict: unique, overwrite, overwrite_if_changed or skip");
    return FALSE;
  }
  if(max_w < -1 || max_h < -1)
  {
    *err = g_strdup("max_width/max_height: pixels, 0 for no limit");
    return FALSE;
  }

  // darktable knows these by other names (cli/main.c)
  const char *want = fmt_name ? fmt_name : dt_conf_get_string_const("plugins/lighttable/export/format_name");
  if(!g_ascii_strcasecmp(want, "jpg")) want = "jpeg";
  else if(!g_ascii_strcasecmp(want, "tif")) want = "tiff";
  else if(!g_ascii_strcasecmp(want, "jxl")) want = "jpegxl";
  dt_imageio_module_format_t *format = dt_imageio_get_format_by_name(want);
  dt_imageio_module_storage_t *storage = dt_imageio_get_storage_by_name("disk");
  if(!format || !storage)
  {
    *err = g_strdup_printf(format ? "no disk storage module" : "unknown format '%s'", want);
    return FALSE;
  }
  if(style && *style && !dt_styles_exists(style))
  {
    *err = g_strdup_printf("no style '%s'", style);
    return FALSE;
  }

  // the export reads the saved history: unsaved changes are saved first on
  // request, else refused. the darkroom's edit is saved as its autosave does
  _session_t *s = _session_find(imgid);
  *saved = FALSE;
  if(s && s->gui)
    dt_dev_write_history(s->dev);
  else if(s && s->dirty)
  {
    if(!save)
    {
      *err = g_strdup("the image has unsaved changes, and export uses the saved edit: save first,"
                      " or pass save: true");
      return FALSE;
    }
    _save_session(s);
    *saved = TRUE;
  }

  const gint64 t0 = g_get_monotonic_time();
  // the format module reads its settings from darktablerc in get_params,
  // so the request's quality goes there for the duration
  GList *overrides = NULL;
  gchar *qkey = g_strdup_printf("plugins/imageio/format/%s/quality", format->plugin_name);
  if(quality > 0 && dt_conf_key_exists(qkey))
  {
    gchar *v = g_strdup_printf("%d", CLAMP(quality, 1, 100));
    _conf_override(&overrides, qkey, v);
    g_free(v);
  }
  g_free(qkey);
  dt_imageio_module_data_t *sdata = storage->get_params(storage);
  dt_imageio_module_data_t *fdata = format->get_params(format);
  _conf_restore(overrides);
  if(!sdata || !fdata)
  {
    if(sdata) storage->free_params(storage, sdata);
    if(fdata) format->free_params(format, fdata);
    *err = g_strdup("the export modules returned no settings");
    return FALSE;
  }

  // the size: the request's, else the export module's, within what the
  // format and storage allow (as _control_export_job_run)
  uint32_t mw = max_w >= 0 ? max_w : dt_conf_get_int("plugins/lighttable/export/width");
  uint32_t mh = max_h >= 0 ? max_h : dt_conf_get_int("plugins/lighttable/export/height");
  if(upscale)
  {
    if(mw == 0 && mh != 0) mw = mh * 100;
    else if(mh == 0 && mw != 0) mh = mw * 100;
  }
  uint32_t fw = 0, fh = 0, sw = 0, sh = 0;
  storage->dimension(storage, sdata, &sw, &sh);
  format->dimension(format, fdata, &fw, &fh);
  const uint32_t w = (sw == 0 || fw == 0) ? MAX(sw, fw) : MIN(sw, fw);
  const uint32_t h = (sh == 0 || fh == 0) ? MAX(sh, fh) : MIN(sh, fh);
  fdata->max_width = (mw != 0 && w != 0) ? MIN(w, mw) : MAX(w, mw);
  fdata->max_height = (mh != 0 && h != 0) ? MIN(h, mh) : MAX(h, mh);
  g_strlcpy(fdata->style, style ? style : dt_conf_get_string_const("plugins/lighttable/export/style"),
            sizeof(fdata->style));
  fdata->style_append = dt_conf_get_bool("plugins/lighttable/export/style_append");

  // the metadata the export module is set to write
  gchar *mconf = dt_lib_export_metadata_get_conf();
  if(!g_strstr_len(mconf, -1, "Iptc.Envelope.CharacterSet"))
    dt_util_str_cat(&mconf, "\1%s\1%s", "Iptc.Envelope.CharacterSet", "\x1b%G");
  dt_export_metadata_t metadata = { 0 };
  metadata.list = dt_util_str_to_glist("\1", mconf);
  g_free(mconf);
  if(metadata.list)
  {
    metadata.flags = strtol(metadata.list->data, NULL, 16);
    metadata.list = g_list_remove(metadata.list, metadata.list->data);
  }

  const dt_colorspaces_color_profile_type_t icc_type = dt_conf_get_int("plugins/lighttable/export/icctype");
  gchar *icc_filename = dt_conf_get_string("plugins/lighttable/export/iccprofile");
  const dt_iop_color_intent_t icc_intent = dt_conf_get_int("plugins/lighttable/export/iccintent");

  const char *pat = pattern ? pattern : dt_conf_get_string_const("plugins/imageio/storage/disk/file_directory");
  const int on_conflict = conflict ? conflict_action : dt_conf_get_int("plugins/imageio/storage/disk/overwrite");
  gboolean skipped = FALSE;
  gchar *file = _export_target(imgid, pat, on_conflict, format, fdata, upscale, &skipped, err);
  // as the disk storage's store() does
  const gboolean failed = file && dt_imageio_export(imgid, file, format, fdata, hq, upscale, FALSE, 1.0,
                                                    TRUE, FALSE, icc_type, icc_filename, icc_intent,
                                                    storage, sdata, 1, 1, &metadata);
  g_list_free_full(metadata.list, g_free);
  g_free(icc_filename);
  storage->free_params(storage, sdata);
  format->free_params(format, fdata);
  if(!file && !skipped) return FALSE;
  if(failed)
  {
    *err = g_strdup_printf("could not export to '%s'", file);
    g_free(file);
    return FALSE;
  }
  if(file)
  {
    // as the export job: tagged exported, no longer changed
    guint tagid = 0, etagid = 0;
    dt_tag_new("darktable|changed", &tagid);
    dt_tag_new("darktable|exported", &etagid);
    dt_tag_detach(tagid, imgid, FALSE, FALSE);
    dt_tag_attach(etagid, imgid, FALSE, FALSE);
    dt_image_cache_set_export_timestamp(imgid);
  }

  json_builder_set_member_name(b, "imgid");
  json_builder_add_int_value(b, imgid);
  json_builder_set_member_name(b, "file");
  if(file) json_builder_add_string_value(b, file);
  else json_builder_add_null_value(b);
  // the conflict setting can leave an existing file alone
  json_builder_set_member_name(b, "skipped");
  json_builder_add_boolean_value(b, skipped);
  json_builder_set_member_name(b, "format");
  json_builder_add_string_value(b, format->plugin_name);
  json_builder_set_member_name(b, "saved");
  json_builder_add_boolean_value(b, *saved);
  json_builder_set_member_name(b, "ms");
  json_builder_add_int_value(b, (g_get_monotonic_time() - t0) / 1000);
  g_free(file);
  return TRUE;
}

// ---- releasing and taking back the library -------------------------------
// while released, darktable's GUI (or another engine) may own the library.
// the engine keeps running with no database, no image cache and no mipmap
// cache; acquiring opens all three again, so nothing read before the release
// is trusted afterwards

typedef struct _draft_module_t
{
  char op[20];
  int multi_priority;
  gboolean enabled;
  int params_size;
  void *params;
} _draft_module_t;

// the unsaved state of one image at release
typedef struct _draft_t
{
  dt_imgid_t imgid;
  gchar *fingerprint;
  GList *modules;
} _draft_t;

static gboolean _released = FALSE;
static gchar *_library_path = NULL;   // the library file to reopen
static GList *_drafts = NULL;

static void _draft_free(_draft_t *d)
{
  for(GList *l = d->modules; l; l = g_list_next(l))
  {
    _draft_module_t *m = l->data;
    g_free(m->params);
    g_free(m);
  }
  g_list_free(d->modules);
  g_free(d->fingerprint);
  g_free(d);
}

static void _drafts_free(void)
{
  g_list_free_full(_drafts, (GDestroyNotify)_draft_free);
  _drafts = NULL;
}

// what tells us whether someone else edited the image: its saved history
static gchar *_fingerprint(const dt_imgid_t imgid)
{
  sqlite3_stmt *st = NULL;
  gchar *fp = NULL;
  if(sqlite3_prepare_v2(dt_database_get(darktable.db),
                        "SELECT change_timestamp, history_end,"
                        "       (SELECT COUNT(*) FROM main.history WHERE imgid = ?1),"
                        "       (SELECT COUNT(*) FROM main.masks_history WHERE imgid = ?1)"
                        " FROM main.images WHERE id = ?1",
                        -1, &st, NULL) == SQLITE_OK)
  {
    sqlite3_bind_int(st, 1, imgid);
    if(sqlite3_step(st) == SQLITE_ROW)
      fp = g_strdup_printf("%lld/%d/%d/%d", (long long)sqlite3_column_int64(st, 0),
                           sqlite3_column_int(st, 1), sqlite3_column_int(st, 2),
                           sqlite3_column_int(st, 3));
    sqlite3_finalize(st);
  }
  return fp;
}

// pid of a live process holding <db>.lock, or 0
static int _lock_holder(const char *dbfile)
{
  gchar *lockfile = g_strconcat(dbfile, ".lock", NULL);
  gchar *buf = NULL;
  int pid = 0;
  if(g_file_get_contents(lockfile, &buf, NULL, NULL))
  {
    pid = atoi(buf);
    if(pid > 0 && kill(pid, 0) != 0 && errno == ESRCH) pid = 0;
  }
  g_free(buf);
  g_free(lockfile);
  return pid;
}

static gchar *_data_path(void)
{
  gchar *dir = g_path_get_dirname(_library_path);
  gchar *p = g_build_filename(dir, "data.db", NULL);
  g_free(dir);
  return p;
}

// keep a session's unsaved state of every module, to restore it later if
// nobody changed the image meanwhile
static void _draft_add(const _session_t *s)
{
  // keep the unsaved state of every module, to restore it on acquire if
  // nobody changed the image meanwhile
  _draft_t *d = g_new0(_draft_t, 1);
  d->imgid = s->dev->image_storage.id;
  d->fingerprint = _fingerprint(d->imgid);
  for(GList *m = s->dev->iop; m; m = g_list_next(m))
  {
    const dt_iop_module_t *mod = m->data;
    _draft_module_t *dm = g_new0(_draft_module_t, 1);
    g_strlcpy(dm->op, mod->op, sizeof(dm->op));
    dm->multi_priority = mod->multi_priority;
    dm->enabled = mod->enabled;
    dm->params_size = mod->params_size;
    dm->params = g_malloc(mod->params_size);
    memcpy(dm->params, mod->params, mod->params_size);
    d->modules = g_list_prepend(d->modules, dm);
  }
  _drafts = g_list_append(_drafts, d);
}

static gboolean _library_status(JsonBuilder *b, const dt_imgid_t current, gchar **err)
{
  json_builder_set_member_name(b, "state");
  json_builder_add_string_value(b, _released ? "released" : "owned");
  json_builder_set_member_name(b, "server");
  json_builder_add_string_value(b, _in_gui ? "gui" : "engine");
  if(_in_gui)
  {
    // the photo the user has open in darktable's darkroom, 0 if none
    const dt_imgid_t dr = _darkroom_image();
    json_builder_set_member_name(b, "darkroom_imgid");
    json_builder_add_int_value(b, dt_is_valid_imgid(dr) ? dr : 0);
  }
  json_builder_set_member_name(b, "library");
  json_builder_add_string_value(b, _released ? _library_path : dt_database_get_path(darktable.db));
  if(_released)
  {
    gchar *data = _data_path();
    const int pid = MAX(_lock_holder(_library_path), _lock_holder(data));
    g_free(data);
    json_builder_set_member_name(b, "holder_pid");
    json_builder_add_int_value(b, pid);
    json_builder_set_member_name(b, "drafts");
    json_builder_begin_array(b);
    for(GList *l = _drafts; l; l = g_list_next(l))
      json_builder_add_int_value(b, ((_draft_t *)l->data)->imgid);
    json_builder_end_array(b);
  }
  else
  {
    // this client's image, and every open session
    const _session_t *mine = _session_find(current);
    json_builder_set_member_name(b, "open_imgid");
    json_builder_add_int_value(b, mine ? current : NO_IMGID);
    json_builder_set_member_name(b, "unsaved");
    json_builder_add_boolean_value(b, mine && mine->dirty);
    json_builder_set_member_name(b, "sessions");
    json_builder_begin_array(b);
    for(GList *l = _sessions; l; l = g_list_next(l))
    {
      const _session_t *s = l->data;
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "imgid");
      json_builder_add_int_value(b, s->dev->image_storage.id);
      json_builder_set_member_name(b, "unsaved");
      json_builder_add_boolean_value(b, s->dirty);
      json_builder_end_object(b);
    }
    json_builder_end_array(b);
  }
  return TRUE;
}

static gboolean _library_release(JsonBuilder *b, gchar **err)
{
  if(_released)
  {
    *err = g_strdup("the library is already released");
    return FALSE;
  }
  const gint64 t0 = g_get_monotonic_time();
  _drafts_free();
  json_builder_set_member_name(b, "closed");
  json_builder_begin_array(b);
  for(GList *l = _sessions; l; l = g_list_next(l))
  {
    const _session_t *s = l->data;
    const dt_imgid_t imgid = s->dev->image_storage.id;
    json_builder_add_int_value(b, imgid);
    if(s->dirty) _draft_add(s);
  }
  json_builder_end_array(b);
  _session_close_all();

  // as dt_cleanup does: the caches first (the mipmap cache writes its
  // thumbnails to disk), then the database, which removes the lock files
  dt_image_cache_cleanup();
  dt_mipmap_cache_cleanup();
  g_free(_library_path);
  _library_path = g_strdup(dt_database_get_path(darktable.db));
  dt_database_cleanup_busy_statements(darktable.db);
  dt_database_destroy(darktable.db);
  darktable.db = NULL;
  _released = TRUE;

  json_builder_set_member_name(b, "state");
  json_builder_add_string_value(b, "released");
  json_builder_set_member_name(b, "drafts_kept");
  json_builder_add_int_value(b, g_list_length(_drafts));
  json_builder_set_member_name(b, "ms");
  json_builder_add_int_value(b, (g_get_monotonic_time() - t0) / 1000);
  return TRUE;
}

// reopen the images of _drafts and reapply their unsaved changes, unless
// someone changed the image in the meantime; reports each as "drafts"
static void _restore_drafts(JsonBuilder *b)
{
  json_builder_set_member_name(b, "drafts");
  json_builder_begin_array(b);
  for(GList *l = _drafts; l; l = g_list_next(l))
  {
    const _draft_t *d = l->data;
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "imgid");
    json_builder_add_int_value(b, d->imgid);
    const char *what;
    int restored = 0;
    if(!_image_exists(d->imgid))
      what = "dropped: the image was removed while released";
    else
    {
      gchar *fp = _fingerprint(d->imgid);
      const gboolean unchanged = !g_strcmp0(fp, d->fingerprint);
      g_free(fp);
      gchar *open_err = NULL;
      _session_t *s = unchanged ? _session_load(d->imgid, &open_err) : NULL;
      if(!unchanged)
        what = "dropped: the image was changed while released";
      else if(!s)
        what = "dropped: the image could not be opened";
      else
      {
        for(GList *m = d->modules; m; m = g_list_next(m))
        {
          const _draft_module_t *dm = m->data;
          dt_iop_module_t *mod = dt_iop_get_module_by_op_priority(s->dev->iop, dm->op, dm->multi_priority);
          if(!mod || mod->params_size != dm->params_size) continue;
          if(mod->enabled == dm->enabled && !memcmp(mod->params, dm->params, dm->params_size)) continue;
          memcpy(mod->params, dm->params, dm->params_size);
          mod->enabled = dm->enabled;
          dt_dev_add_history_item_ext(s->dev, mod, FALSE, TRUE);
          restored++;
        }
        s->dirty = restored > 0;
        s->pipe_changed = TRUE;
        what = "restored";
      }
      g_free(open_err);
    }
    json_builder_set_member_name(b, "draft");
    json_builder_add_string_value(b, what);
    json_builder_set_member_name(b, "modules_restored");
    json_builder_add_int_value(b, restored);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  _drafts_free();
}

static gboolean _library_acquire(JsonBuilder *b, gchar **err)
{
  if(!_released)
  {
    *err = g_strdup("the library is not released");
    return FALSE;
  }
  // check first: when dt_database_init can't lock it returns a db whose
  // lock file names are those of the other process, and destroying that
  // db would delete its lock files
  gchar *data = _data_path();
  const int pid = MAX(_lock_holder(_library_path), _lock_holder(data));
  g_free(data);
  if(pid)
  {
    *err = g_strdup_printf("the library is held by process %d", pid);
    return FALSE;
  }
  const gint64 t0 = g_get_monotonic_time();
  const struct dt_database_t *db = dt_database_init(_library_path, TRUE, FALSE);
  if(!db || !dt_database_get_lock_acquired(db))
  {
    *err = g_strdup("could not lock the library");   // db leaks; see above
    return FALSE;
  }
  darktable.db = db;
  _released = FALSE;
  // the GUI rewrites darktablerc when it quits: read it again
  dt_conf_init(darktable.conf, darktable.conf->filename, FALSE, NULL);
  dt_image_cache_init();
  dt_mipmap_cache_init();

  json_builder_set_member_name(b, "state");
  json_builder_add_string_value(b, "owned");
  _restore_drafts(b);
  json_builder_set_member_name(b, "ms");
  json_builder_add_int_value(b, (g_get_monotonic_time() - t0) / 1000);
  return TRUE;
}

// ---- handing the library over to darktable's window ----------------------
// darktable started with --api asks a running engine to step aside: the
// engine releases the library, sends its drafts along and exits; darktable
// restores the drafts once its own startup is done (dt_api_start)

static void _drafts_to_json(JsonBuilder *b)
{
  json_builder_set_member_name(b, "drafts");
  json_builder_begin_array(b);
  for(GList *l = _drafts; l; l = g_list_next(l))
  {
    const _draft_t *d = l->data;
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "imgid");
    json_builder_add_int_value(b, d->imgid);
    json_builder_set_member_name(b, "fingerprint");
    json_builder_add_string_value(b, d->fingerprint ? d->fingerprint : "");
    json_builder_set_member_name(b, "modules");
    json_builder_begin_array(b);
    for(GList *m = d->modules; m; m = g_list_next(m))
    {
      const _draft_module_t *dm = m->data;
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "operation");
      json_builder_add_string_value(b, dm->op);
      json_builder_set_member_name(b, "instance");
      json_builder_add_int_value(b, dm->multi_priority);
      json_builder_set_member_name(b, "enabled");
      json_builder_add_boolean_value(b, dm->enabled);
      gchar *hex = g_malloc(dm->params_size * 2 + 1);
      for(int i = 0; i < dm->params_size; i++)
        snprintf(hex + 2 * i, 3, "%02x", ((const uint8_t *)dm->params)[i]);
      json_builder_set_member_name(b, "params");
      json_builder_add_string_value(b, hex);
      g_free(hex);
      json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
}

// appends to _drafts
static void _drafts_from_json(JsonArray *a)
{
  for(guint i = 0; a && i < json_array_get_length(a); i++)
  {
    JsonObject *o = json_array_get_object_element(a, i);
    _draft_t *d = g_new0(_draft_t, 1);
    d->imgid = json_object_get_int_member_with_default(o, "imgid", NO_IMGID);
    d->fingerprint = g_strdup(json_object_get_string_member_with_default(o, "fingerprint", ""));
    JsonArray *mods = json_object_get_array_member(o, "modules");
    for(guint k = 0; mods && k < json_array_get_length(mods); k++)
    {
      JsonObject *mo = json_array_get_object_element(mods, k);
      const char *hex = json_object_get_string_member_with_default(mo, "params", "");
      _draft_module_t *dm = g_new0(_draft_module_t, 1);
      g_strlcpy(dm->op, json_object_get_string_member_with_default(mo, "operation", ""), sizeof(dm->op));
      dm->multi_priority = json_object_get_int_member_with_default(mo, "instance", 0);
      dm->enabled = json_object_get_boolean_member_with_default(mo, "enabled", TRUE);
      dm->params_size = strlen(hex) / 2;
      dm->params = g_malloc0(MAX(dm->params_size, 1));
      for(int j = 0; j < dm->params_size; j++)
      {
        const char byte[3] = { hex[2 * j], hex[2 * j + 1], 0 };
        ((uint8_t *)dm->params)[j] = strtol(byte, NULL, 16);
      }
      d->modules = g_list_append(d->modules, dm);
    }
    _drafts = g_list_append(_drafts, d);
  }
}

static gboolean _handover(JsonBuilder *b, gchar **err)
{
  if(!_released)
  {
    JsonBuilder *rb = json_builder_new();
    json_builder_begin_object(rb);
    const gboolean ok = _library_release(rb, err);
    json_builder_end_object(rb);
    g_object_unref(rb);
    if(!ok) return FALSE;
  }
  _drafts_to_json(b);
  _drafts_free();
  return TRUE;
}

// ---- browsing the library -------------------------------------------------

static gboolean _film_rolls(JsonBuilder *b, gchar **err)
{
  sqlite3_stmt *st = NULL;
  // clang-format off
  if(sqlite3_prepare_v2(dt_database_get(darktable.db),
                        "SELECT f.id, f.folder, COUNT(i.id)"
                        " FROM main.film_rolls AS f"
                        " LEFT JOIN main.images AS i ON i.film_id = f.id"
                        " GROUP BY f.id"
                        " ORDER BY f.folder",
                        -1, &st, NULL) != SQLITE_OK)
  // clang-format on
  {
    *err = g_strdup("cannot read film rolls");
    return FALSE;
  }
  json_builder_set_member_name(b, "film_rolls");
  json_builder_begin_array(b);
  while(sqlite3_step(st) == SQLITE_ROW)
  {
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "id");
    json_builder_add_int_value(b, sqlite3_column_int(st, 0));
    json_builder_set_member_name(b, "folder");
    json_builder_add_string_value(b, (const char *)sqlite3_column_text(st, 1));
    json_builder_set_member_name(b, "count");
    json_builder_add_int_value(b, sqlite3_column_int(st, 2));
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  sqlite3_finalize(st);
  return TRUE;
}

// rating filters: the clause is chosen from fixed strings, never built from input
static const char *_rating_clause(const char *rating)
{
  // rejected: the reject flag, or the old reject rating (DT_VIEW_REJECT)
  static const char *rejected = "((i.flags & 8) = 8 OR (i.flags & 7) = 6)";
  if(!g_strcmp0(rating, "all")) return "1";
  if(!g_strcmp0(rating, "rejected")) return rejected;
  if(rating && rating[0] >= '1' && rating[0] <= '5' && !rating[1])
  {
    static const char *atleast[] = {
      "(NOT ((i.flags & 8) = 8 OR (i.flags & 7) = 6) AND (i.flags & 7) >= 1)",
      "(NOT ((i.flags & 8) = 8 OR (i.flags & 7) = 6) AND (i.flags & 7) >= 2)",
      "(NOT ((i.flags & 8) = 8 OR (i.flags & 7) = 6) AND (i.flags & 7) >= 3)",
      "(NOT ((i.flags & 8) = 8 OR (i.flags & 7) = 6) AND (i.flags & 7) >= 4)",
      "(NOT ((i.flags & 8) = 8 OR (i.flags & 7) = 6) AND (i.flags & 7) >= 5)" };
    return atleast[rating[0] - '1'];
  }
  return "NOT ((i.flags & 8) = 8 OR (i.flags & 7) = 6)"; // "visible", the default
}

static void _add_image_row(JsonBuilder *b, sqlite3_stmt *st)
{
  const int flags = sqlite3_column_int(st, 4);
  const gboolean rejected = (flags & DT_IMAGE_REJECTED) || (flags & DT_VIEW_RATINGS_MASK) == DT_VIEW_REJECT;
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "id");
  json_builder_add_int_value(b, sqlite3_column_int(st, 0));
  json_builder_set_member_name(b, "filename");
  json_builder_add_string_value(b, (const char *)sqlite3_column_text(st, 1));
  json_builder_set_member_name(b, "folder");
  json_builder_add_string_value(b, (const char *)sqlite3_column_text(st, 2));
  json_builder_set_member_name(b, "version");
  json_builder_add_int_value(b, sqlite3_column_int(st, 3));
  json_builder_set_member_name(b, "rating");
  json_builder_add_int_value(b, rejected ? 0 : (flags & DT_VIEW_RATINGS_MASK));
  json_builder_set_member_name(b, "rejected");
  json_builder_add_boolean_value(b, rejected);
  json_builder_set_member_name(b, "width");
  json_builder_add_int_value(b, sqlite3_column_int(st, 5));
  json_builder_set_member_name(b, "height");
  json_builder_add_int_value(b, sqlite3_column_int(st, 6));
  json_builder_set_member_name(b, "changed");
  json_builder_add_int_value(b, sqlite3_column_int64(st, 7));
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, sqlite3_column_int(st, 9));
  json_builder_set_member_name(b, "labels");
  json_builder_begin_array(b);
  const char *labels = (const char *)sqlite3_column_text(st, 8);
  if(labels)
  {
    gchar **parts = g_strsplit(labels, ",", -1);
    for(gchar **p = parts; *p; p++) json_builder_add_int_value(b, atoi(*p));
    g_strfreev(parts);
  }
  json_builder_end_array(b);
  json_builder_end_object(b);
}

static gboolean _images_list(JsonObject *params, JsonBuilder *b, gchar **err)
{
  const int film_id = params ? json_object_get_int_member_with_default(params, "film_id", -1) : -1;
  const int label = params ? json_object_get_int_member_with_default(params, "label", -1) : -1;
  const int offset = MAX(0, params ? json_object_get_int_member_with_default(params, "offset", 0) : 0);
  const int limit = CLAMP(params ? json_object_get_int_member_with_default(params, "limit", 100) : 100, 1, 1000);
  const char *rating = params ? json_object_get_string_member_with_default(params, "rating", "visible") : "visible";

  // clang-format off
  gchar *where = g_strdup_printf(" WHERE (?1 < 0 OR i.film_id = ?1)"
                                 "   AND %s"
                                 "   AND (?2 < 0 OR EXISTS (SELECT 1 FROM main.color_labels AS c"
                                 "                          WHERE c.imgid = i.id AND c.color = ?2))",
                                 _rating_clause(rating));
  gchar *q_count = g_strconcat("SELECT COUNT(*) FROM main.images AS i", where, NULL);
  gchar *q_rows = g_strconcat("SELECT i.id, i.filename, f.folder, i.version, i.flags, i.width, i.height,"
                              "       i.change_timestamp,"
                              "       (SELECT GROUP_CONCAT(color) FROM main.color_labels WHERE imgid = i.id),"
                              "       i.history_end"
                              " FROM main.images AS i"
                              " JOIN main.film_rolls AS f ON f.id = i.film_id",
                              where,
                              " ORDER BY f.folder, i.filename, i.version"
                              " LIMIT ?3 OFFSET ?4", NULL);
  // clang-format on
  sqlite3 *db = dt_database_get(darktable.db);
  sqlite3_stmt *st = NULL;
  gboolean ok = sqlite3_prepare_v2(db, q_count, -1, &st, NULL) == SQLITE_OK;
  if(ok)
  {
    sqlite3_bind_int(st, 1, film_id);
    sqlite3_bind_int(st, 2, label);
    json_builder_set_member_name(b, "total");
    json_builder_add_int_value(b, sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : 0);
    sqlite3_finalize(st);
    ok = sqlite3_prepare_v2(db, q_rows, -1, &st, NULL) == SQLITE_OK;
  }
  if(ok)
  {
    sqlite3_bind_int(st, 1, film_id);
    sqlite3_bind_int(st, 2, label);
    sqlite3_bind_int(st, 3, limit);
    sqlite3_bind_int(st, 4, offset);
    json_builder_set_member_name(b, "images");
    json_builder_begin_array(b);
    while(sqlite3_step(st) == SQLITE_ROW) _add_image_row(b, st);
    json_builder_end_array(b);
    sqlite3_finalize(st);
  }
  else
    *err = g_strdup("cannot read the image list");
  g_free(where);
  g_free(q_count);
  g_free(q_rows);
  return ok;
}

static gboolean _image_info(JsonObject *params, JsonBuilder *b, gchar **err)
{
  const dt_imgid_t imgid = params ? json_object_get_int_member_with_default(params, "imgid", -1) : -1;
  if(!dt_is_valid_imgid(imgid) || !_image_exists(imgid))
  {
    *err = g_strdup_printf("no image with id %d", imgid);
    return FALSE;
  }
  sqlite3_stmt *st = NULL;
  // clang-format off
  if(sqlite3_prepare_v2(dt_database_get(darktable.db),
                        "SELECT i.id, i.filename, f.folder, i.version, i.flags, i.width, i.height,"
                        "       i.change_timestamp,"
                        "       (SELECT GROUP_CONCAT(color) FROM main.color_labels WHERE imgid = i.id),"
                        "       i.history_end"
                        " FROM main.images AS i"
                        " JOIN main.film_rolls AS f ON f.id = i.film_id"
                        " WHERE i.id = ?1",
                        -1, &st, NULL) != SQLITE_OK)
  // clang-format on
  {
    *err = g_strdup("cannot read the image");
    return FALSE;
  }
  sqlite3_bind_int(st, 1, imgid);
  if(sqlite3_step(st) == SQLITE_ROW)
  {
    json_builder_set_member_name(b, "image");
    _add_image_row(b, st);
  }
  sqlite3_finalize(st);
  return TRUE;
}

// a thumbnail from darktable's mipmap cache (rendered with the image's
// current history if the cache has none), as darktable's lighttable shows it
static gboolean _thumbnail(JsonObject *params, JsonBuilder *b, gchar **err)
{
  const dt_imgid_t imgid = params ? json_object_get_int_member_with_default(params, "imgid", -1) : -1;
  const int size = params ? json_object_get_int_member_with_default(params, "size", 400) : 400;
  const int quality = params ? json_object_get_int_member_with_default(params, "quality", 80) : 80;
  const gchar *path = params ? json_object_get_string_member_with_default(params, "path", NULL) : NULL;
  if(!path || size <= 0)
  {
    *err = g_strdup("thumbnail needs path and size > 0");
    return FALSE;
  }
  if(!dt_is_valid_imgid(imgid) || !_image_exists(imgid))
  {
    *err = g_strdup_printf("no image with id %d", imgid);
    return FALSE;
  }
  const gint64 t0 = g_get_monotonic_time();
  const dt_mipmap_size_t level = dt_mipmap_cache_get_matching_size(size, size);
  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(&buf, imgid, level, DT_MIPMAP_BLOCKING, 'r');
  gboolean ok = buf.buf && buf.width > 0 && buf.height > 0;
  // the mipmap cache keeps 8-bit levels in the byte order its own jpeg
  // writer takes (mipmap_cache.c, disk cache)
  if(ok && dt_imageio_jpeg_write(path, buf.buf, buf.width, buf.height, quality, NULL, 0))
    ok = FALSE;
  if(ok)
  {
    json_builder_set_member_name(b, "width");
    json_builder_add_int_value(b, buf.width);
    json_builder_set_member_name(b, "height");
    json_builder_add_int_value(b, buf.height);
    json_builder_set_member_name(b, "level");
    json_builder_add_int_value(b, level);
    json_builder_set_member_name(b, "ms");
    json_builder_add_int_value(b, (g_get_monotonic_time() - t0) / 1000);
  }
  else
    *err = g_strdup_printf("no thumbnail for image %d", imgid);
  dt_mipmap_cache_release(&buf);
  return ok;
}

static gboolean _set_rating(JsonObject *params, JsonBuilder *b, gchar **err)
{
  const dt_imgid_t imgid = params ? json_object_get_int_member_with_default(params, "imgid", -1) : -1;
  if(!dt_is_valid_imgid(imgid) || !_image_exists(imgid))
  {
    *err = g_strdup_printf("no image with id %d", imgid);
    return FALSE;
  }
  JsonNode *v = params ? json_object_get_member(params, "rating") : NULL;
  int rating = -2;
  if(v && JSON_NODE_HOLDS_VALUE(v) && json_node_get_value_type(v) == G_TYPE_STRING
     && !g_strcmp0(json_node_get_string(v), "reject"))
    rating = DT_VIEW_REJECT;
  else if(v && JSON_NODE_HOLDS_VALUE(v) && json_node_get_value_type(v) == G_TYPE_INT64)
  {
    // 6 is DT_VIEW_REJECT: a number must be a star rating
    const gint64 n = json_node_get_int(v);
    rating = n >= 0 && n <= 5 ? (int)n : -2;
  }
  if(rating != DT_VIEW_REJECT && (rating < 0 || rating > 5))
  {
    *err = g_strdup("rating must be 0..5 or \"reject\"");
    return FALSE;
  }
  // as the lighttable: rejecting keeps the stars, a rating clears the reject
  dt_ratings_apply_on_image(imgid, rating, FALSE, FALSE, FALSE);
  return _image_info(params, b, err);
}

static gboolean _set_label(JsonObject *params, JsonBuilder *b, gchar **err)
{
  const dt_imgid_t imgid = params ? json_object_get_int_member_with_default(params, "imgid", -1) : -1;
  const int label = params ? json_object_get_int_member_with_default(params, "label", -1) : -1;
  const gboolean on = params ? json_object_get_boolean_member_with_default(params, "on", TRUE) : TRUE;
  if(!dt_is_valid_imgid(imgid) || !_image_exists(imgid))
  {
    *err = g_strdup_printf("no image with id %d", imgid);
    return FALSE;
  }
  if(label < 0 || label > 4)
  {
    *err = g_strdup("label must be 0..4 (red, yellow, green, blue, purple)");
    return FALSE;
  }
  if(on)
    dt_colorlabels_set_label(imgid, label);
  else
    dt_colorlabels_remove_label(imgid, label);
  dt_image_write_sidecar_file(imgid);
  return _image_info(params, b, err);
}

static gboolean _render_session(JsonObject *params, JsonBuilder *b, gchar **err);

// the darkroom's image renders through its mirror (_pipe_session)
static gboolean _render(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_cur || !_cur->gui) return _render_session(params, b, err);
  _session_t *gui = _cur;
  _session_t *mirror = _pipe_session(err);
  if(!mirror) return FALSE;
  _cur = mirror;
  const gboolean ok = _render_session(params, b, err);
  _cur = gui;
  return ok;
}

static gboolean _render_session(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  const int max_w = params ? json_object_get_int_member_with_default(params, "width", 1200) : 1200;
  const int max_h = params ? json_object_get_int_member_with_default(params, "height", 1200) : 1200;
  const int quality = params ? json_object_get_int_member_with_default(params, "quality", 85) : 85;
  const gchar *path = params ? json_object_get_string_member_with_default(params, "path", NULL) : NULL;
  if(!path || max_w <= 0 || max_h <= 0)
  {
    *err = g_strdup("render needs path, width > 0 and height > 0");
    return FALSE;
  }

  const gint64 t0 = g_get_monotonic_time();
  _pipe_sync(_cur);
  // uncropped: without crop's box, as the darkroom shows the image while
  // crop has the focus, for drawing a box on
  dt_dev_pixelpipe_iop_t *crop = params && json_object_get_boolean_member_with_default(params, "uncropped", FALSE)
                                 ? _piece(_cur, "crop") : NULL;
  if(crop && !crop->enabled) crop = NULL;
  if(crop)
  {
    crop->enabled = FALSE;
    dt_dev_pixelpipe_get_dimensions(&_cur->pipe, _cur->dev, _cur->pipe.iwidth, _cur->pipe.iheight,
                                    &_cur->pipe.processed_width, &_cur->pipe.processed_height);
  }
  // zoom: a region at that scale of the full-size image (1 = 100%), as the
  // darkroom shows it zoomed in, centered on center_x/center_y (fractions);
  // else the whole image fitted inside max_w x max_h
  const double zoom = params ? json_object_get_double_member_with_default(params, "zoom", 0.0) : 0.0;
  const double pw = _cur->pipe.processed_width, ph = _cur->pipe.processed_height;
  double scale;
  int x = 0, y = 0, w, h;
  if(zoom > 0.0)
  {
    scale = CLAMP(zoom, 0.01, 2.0);
    const double cx = params ? json_object_get_double_member_with_default(params, "center_x", 0.5) : 0.5;
    const double cy = params ? json_object_get_double_member_with_default(params, "center_y", 0.5) : 0.5;
    w = MIN(max_w, (int)floor(scale * pw));
    h = MIN(max_h, (int)floor(scale * ph));
    x = CLAMP((int)round(cx * scale * pw - w / 2.0), 0, (int)floor(scale * pw) - w);
    y = CLAMP((int)round(cy * scale * ph - h / 2.0), 0, (int)floor(scale * ph) - h);
  }
  else
  {
    scale = fmin(1.0, fmin(max_w / pw, max_h / ph));
    w = floor(scale * pw);
    h = floor(scale * ph);
  }

  // as a non-hq export does: downscale right after demosaic, not in finalscale
  dt_dev_pixelpipe_iop_t *finalscale = NULL;
  for(GList *n = g_list_last(_cur->pipe.nodes); n; n = g_list_previous(n))
  {
    dt_dev_pixelpipe_iop_t *piece = n->data;
    if(dt_iop_module_is_finalscale(piece->module)) { finalscale = piece; break; }
  }
  if(finalscale) finalscale->enabled = FALSE;
  dt_dev_pixelpipe_process(&_cur->pipe, _cur->dev, x, y, w, h, scale, DT_DEVICE_NONE);
  if(finalscale) finalscale->enabled = TRUE;
  if(crop)
  {
    crop->enabled = TRUE;
    dt_dev_pixelpipe_get_dimensions(&_cur->pipe, _cur->dev, _cur->pipe.iwidth, _cur->pipe.iheight,
                                    &_cur->pipe.processed_width, &_cur->pipe.processed_height);
  }
  const gint64 t1 = g_get_monotonic_time();

  if(!_cur->pipe.backbuf || _cur->pipe.backbuf_width != w || _cur->pipe.backbuf_height != h)
  {
    *err = g_strdup("the pipe produced no output");
    return FALSE;
  }
  // backbuf is a cache line in display byte order (BGRA): convert a copy
  const size_t npix = (size_t)w * h;
  uint8_t *rgba = g_malloc(npix * 4);
  const uint8_t *src = _cur->pipe.backbuf;
  for(size_t k = 0; k < npix; k++)
  {
    rgba[4 * k + 0] = src[4 * k + 2];
    rgba[4 * k + 1] = src[4 * k + 1];
    rgba[4 * k + 2] = src[4 * k + 0];
    rgba[4 * k + 3] = 255;
  }
  const int jerr = dt_imageio_jpeg_write(path, rgba, w, h, quality, NULL, 0);
  g_free(rgba);
  if(jerr)
  {
    *err = g_strdup_printf("cannot write %s", path);
    return FALSE;
  }
  const gint64 t2 = g_get_monotonic_time();

  json_builder_set_member_name(b, "width");
  json_builder_add_int_value(b, w);
  json_builder_set_member_name(b, "height");
  json_builder_add_int_value(b, h);
  json_builder_set_member_name(b, "uncropped");
  json_builder_add_boolean_value(b, crop != NULL);
  if(zoom > 0.0)
  {
    // the region shown, in fractions of the whole image
    json_builder_set_member_name(b, "zoom");
    json_builder_add_double_value(b, scale);
    json_builder_set_member_name(b, "region");
    _add_box(b, (float[4]){ x / (scale * pw), y / (scale * ph), (x + w) / (scale * pw), (y + h) / (scale * ph) });
  }
  json_builder_set_member_name(b, "process_ms");
  json_builder_add_int_value(b, (t1 - t0) / 1000);
  json_builder_set_member_name(b, "jpeg_ms");
  json_builder_add_int_value(b, (t2 - t1) / 1000);
  return TRUE;
}

// ---- clients ------------------------------------------------------------
// stdin/stdout is one client; with a socket, every connection is one. the
// sockets are GLib sources of the default main context: the engine's own
// main loop, or darktable's GTK main loop when its window serves the API,
// so requests run on that thread, one at a time, in arrival order

typedef struct _client_t _client_t;
struct _client_t
{
  int id;
  int in_fd, out_fd;
  GString *inbuf;
  dt_imgid_t current;               // the image this client last opened
  gboolean dead;
  guint source;
};

static GList *_clients = NULL;
static int _next_client_id = 1;
static dt_api_options_t _opt;
static guint _listen_source = 0;
static guint _idle_source = 0;
static gint64 _last_activity = 0;
static gchar *_socket_path = NULL;    // our copy of _opt.socket_path

static void _request_quit(void)
{
  if(_opt.quit) _opt.quit();
}

static gboolean _client_cb(gint fd, GIOCondition cond, gpointer data);

static _client_t *_client_new(const int in_fd, const int out_fd)
{
  _client_t *c = g_new0(_client_t, 1);
  c->id = _next_client_id++;
  c->in_fd = in_fd;
  c->out_fd = out_fd;
  c->inbuf = g_string_new(NULL);
  c->current = NO_IMGID;
  c->source = g_unix_fd_add(in_fd, G_IO_IN | G_IO_HUP | G_IO_ERR, _client_cb, c);
  _clients = g_list_append(_clients, c);
  return c;
}

static void _client_free(_client_t *c)
{
  _clients = g_list_remove(_clients, c);
  if(c->source) g_source_remove(c->source);
  if(c->in_fd > STDERR_FILENO) close(c->in_fd);
  if(c->out_fd != c->in_fd && c->out_fd > STDERR_FILENO) close(c->out_fd);
  g_string_free(c->inbuf, TRUE);
  g_free(c);
}

static void _send_line(_client_t *c, const gchar *s)
{
  if(c->dead) return;
  const size_t n = strlen(s);
  gchar *line = g_strconcat(s, "\n", NULL);
  size_t done = 0;
  while(done < n + 1)
  {
    const ssize_t w = write(c->out_fd, line + done, n + 1 - done);
    if(w < 0 && errno == EINTR) continue;
    if(w <= 0)
    {
      c->dead = TRUE;  // gone (EPIPE); dropped when its source fires
      break;
    }
    done += w;
  }
  g_free(line);
}

static void _send_node(_client_t *c, JsonNode *root)
{
  JsonGenerator *g = json_generator_new();
  json_generator_set_root(g, root);
  gchar *s = json_generator_to_data(g, NULL);
  _send_line(c, s);
  g_free(s);
  g_object_unref(g);
}

static void _reply(_client_t *c, JsonNode *id, JsonNode *result, const gchar *err)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "jsonrpc");
  json_builder_add_string_value(b, "2.0");
  json_builder_set_member_name(b, "id");
  json_builder_add_value(b, id ? json_node_copy(id) : json_node_new(JSON_NODE_NULL));
  if(err)
  {
    json_builder_set_member_name(b, "error");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "code");
    json_builder_add_int_value(b, -32000);
    json_builder_set_member_name(b, "message");
    json_builder_add_string_value(b, err);
    json_builder_end_object(b);
    if(result) json_node_unref(result);
  }
  else
  {
    json_builder_set_member_name(b, "result");
    json_builder_add_value(b, result);
  }
  json_builder_end_object(b);
  JsonNode *root = json_builder_get_root(b);
  _send_node(c, root);
  json_node_unref(root);
  g_object_unref(b);
}

// tell the other clients what changed: a JSON-RPC notification
// {"method": "event", "params": {"type", "imgid", "history_end", "client"}}.
// from is NULL for changes made in darktable's window
static void _notify(const struct _client_t *from, const char *type, const dt_imgid_t imgid)
{
  if(!_clients || (from && !_clients->next)) return;
  const _session_t *s = dt_is_valid_imgid(imgid) && !_released ? _session_find(imgid) : NULL;
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "jsonrpc");
  json_builder_add_string_value(b, "2.0");
  json_builder_set_member_name(b, "method");
  json_builder_add_string_value(b, "event");
  json_builder_set_member_name(b, "params");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "type");
  json_builder_add_string_value(b, type);
  json_builder_set_member_name(b, "imgid");
  json_builder_add_int_value(b, imgid);
  if(s)
  {
    json_builder_set_member_name(b, "history_end");
    json_builder_add_int_value(b, s->dev->history_end);
    json_builder_set_member_name(b, "unsaved");
    json_builder_add_boolean_value(b, s->dirty);
  }
  json_builder_set_member_name(b, "client");
  json_builder_add_int_value(b, from ? from->id : 0);
  json_builder_end_object(b);
  json_builder_end_object(b);
  JsonNode *root = json_builder_get_root(b);
  for(GList *l = _clients; l; l = g_list_next(l))
    if(l->data != from) _send_node(l->data, root);
  json_node_unref(root);
  g_object_unref(b);
}

static const char *_methods[] = {
  "ping", "film_rolls", "images_list", "image_info", "thumbnail", "set_rating", "set_label",
  "session_open", "session_close", "module_list", "module_get", "module_set", "module_set.lists",
  "module_enable", "preset_list", "preset_apply", "history_list", "history_end", "geometry_get",
  "geometry_set", "save", "reset", "render", "render.uncropped", "render.zoom", "export",
  "library_status", "library_release", "library_acquire", "handover", "shutdown", NULL };

// methods that work on an edit session, and the event each one sends
static const struct { const char *method; const char *event; } _session_methods[] = {
  { "module_list", NULL }, { "module_get", NULL }, { "history_list", NULL }, { "render", NULL },
  { "geometry_get", NULL }, { "preset_list", NULL }, { "preset_apply", "edit" },
  { "module_set", "edit" }, { "module_enable", "edit" }, { "history_end", "edit" }, { "geometry_set", "edit" },
  { "save", "saved" }, { "reset", "reset" }, { "session_close", "closed" }, { NULL, NULL } };

// returns FALSE when the engine should stop
static gboolean _handle(_client_t *c, const gchar *line)
{
  JsonParser *p = json_parser_new();
  if(!json_parser_load_from_data(p, line, -1, NULL)
     || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p)))
  {
    _reply(c, NULL, NULL, "parse error");
    g_object_unref(p);
    return TRUE;
  }
  JsonObject *req = json_node_get_object(json_parser_get_root(p));
  JsonNode *id = json_object_get_member(req, "id");
  const gchar *method = json_object_get_string_member_with_default(req, "method", "");
  JsonObject *params = json_object_has_member(req, "params")
                       && JSON_NODE_HOLDS_OBJECT(json_object_get_member(req, "params"))
                         ? json_object_get_object_member(req, "params") : NULL;

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  gchar *err = NULL;
  gboolean go_on = TRUE;
  const char *event = NULL;
  dt_imgid_t event_img = NO_IMGID;

  // a session method works on params.imgid, else on the client's image
  gboolean session_method = FALSE;
  for(int i = 0; _session_methods[i].method; i++)
    if(!g_strcmp0(method, _session_methods[i].method))
    {
      session_method = TRUE;
      event = _session_methods[i].event;
    }
  _cur = NULL;
  if(session_method && !_released)
  {
    const dt_imgid_t imgid = params && json_object_has_member(params, "imgid")
      ? json_object_get_int_member(params, "imgid") : c->current;
    _cur = _session_find(imgid);
    if(_cur)
      _cur->last_use = g_get_monotonic_time();
    else if(dt_is_valid_imgid(imgid))
      err = g_strdup_printf("image %d is not open: call session_open first", imgid);
    event_img = imgid;
  }

  if(err)
    ;
  else if(_released && g_strcmp0(method, "ping") && g_strcmp0(method, "library_status")
     && g_strcmp0(method, "library_acquire") && g_strcmp0(method, "shutdown")
     && g_strcmp0(method, "handover"))
    err = g_strdup("the library is released: call library_acquire first");
  else if(!g_strcmp0(method, "ping"))
  {
    json_builder_set_member_name(b, "version");
    json_builder_add_string_value(b, darktable_package_version);
    json_builder_set_member_name(b, "client");
    json_builder_add_int_value(b, c->id);
    json_builder_set_member_name(b, "server");
    json_builder_add_string_value(b, _opt.in_gui ? "gui" : "engine");
    // what this server can do, so a client can check before calling: names
    // of methods, and of options a method gained later ("render.zoom")
    json_builder_set_member_name(b, "methods");
    json_builder_begin_array(b);
    for(int k = 0; _methods[k]; k++) json_builder_add_string_value(b, _methods[k]);
    json_builder_end_array(b);
  }
  else if(!g_strcmp0(method, "film_rolls"))
    _film_rolls(b, &err);
  else if(!g_strcmp0(method, "images_list"))
    _images_list(params, b, &err);
  else if(!g_strcmp0(method, "image_info"))
    _image_info(params, b, &err);
  else if(!g_strcmp0(method, "thumbnail"))
    _thumbnail(params, b, &err);
  else if(!g_strcmp0(method, "export"))
  {
    gboolean saved = FALSE;
    if(_export(params, c->current, &saved, b, &err) && saved)
    {
      event = "saved";
      event_img = params && json_object_has_member(params, "imgid")
        ? json_object_get_int_member(params, "imgid") : c->current;
    }
  }
  else if(!g_strcmp0(method, "set_rating") || !g_strcmp0(method, "set_label"))
  {
    if(!g_strcmp0(method, "set_rating") ? _set_rating(params, b, &err) : _set_label(params, b, &err))
    {
      event = "image";
      event_img = json_object_get_int_member_with_default(params, "imgid", NO_IMGID);
    }
  }
  else if(!g_strcmp0(method, "session_open"))
  {
    const dt_imgid_t imgid = params ? json_object_get_int_member_with_default(params, "imgid", -1) : -1;
    const gboolean fresh = params ? json_object_get_boolean_member_with_default(params, "fresh", FALSE) : FALSE;
    const gboolean had = _session_find(imgid) != NULL;
    if(_session_open(imgid, fresh, b, &err))
    {
      c->current = imgid;
      if(fresh && had)
      {
        event = "reopened";  // unsaved changes dropped for everyone
        event_img = imgid;
      }
    }
  }
  else if(!g_strcmp0(method, "session_close"))
  {
    json_builder_set_member_name(b, "closed");
    json_builder_add_boolean_value(b, _cur != NULL);
    _session_close(_cur);
  }
  else if(!g_strcmp0(method, "module_list"))
    _module_list(b, &err);
  else if(!g_strcmp0(method, "module_get"))
    _module_get(params, b, &err);
  else if(!g_strcmp0(method, "module_set"))
    _module_set(params, b, &err);
  else if(!g_strcmp0(method, "module_enable"))
    _module_enable(params, b, &err);
  else if(!g_strcmp0(method, "history_list"))
    _history_list(b, &err);
  else if(!g_strcmp0(method, "preset_list"))
    _preset_list(params, b, &err);
  else if(!g_strcmp0(method, "preset_apply"))
    _preset_apply(params, b, &err);
  else if(!g_strcmp0(method, "geometry_get"))
    _geometry_get(b, &err);
  else if(!g_strcmp0(method, "geometry_set"))
    _geometry_set(params, b, &err);
  else if(!g_strcmp0(method, "history_end"))
    _history_end(params, b, &err);
  else if(!g_strcmp0(method, "save"))
    _save(b, &err);
  else if(!g_strcmp0(method, "library_status"))
    _library_status(b, c->current, &err);
  else if(!g_strcmp0(method, "handover") && !_opt.in_gui)
  {
    // darktable's window takes over: everyone else waits for it
    if(_handover(b, &err))
    {
      event = "handover";
      go_on = FALSE;
    }
  }
  else if(_opt.in_gui && (!g_strcmp0(method, "library_release") || !g_strcmp0(method, "library_acquire")
                          || !g_strcmp0(method, "handover")))
    err = g_strdup("darktable's window has the library; quit darktable to hand it to the engine");
  else if(!g_strcmp0(method, "library_release") || !g_strcmp0(method, "library_acquire"))
  {
    if(!g_strcmp0(method, "library_release") ? _library_release(b, &err) : _library_acquire(b, &err))
      event = !g_strcmp0(method, "library_release") ? "library_released" : "library_acquired";
  }
  else if(!g_strcmp0(method, "reset"))
    _reset(b, &err);
  else if(!g_strcmp0(method, "render"))
    _render(params, b, &err);
  else if(!g_strcmp0(method, "shutdown"))
  {
    if(_opt.in_gui)
      err = g_strdup("darktable's window serves the library; it can't be shut down from here");
    else
      go_on = FALSE;
  }
  else
    err = g_strdup_printf("unknown method '%s'", method);

  json_builder_end_object(b);
  // notifications (no id) get no reply
  if(id) _reply(c, id, json_builder_get_root(b), err);
  if(!err && event) _notify(c, event, event_img);
  g_free(err);
  g_object_unref(b);
  g_object_unref(p);
  _cur = NULL;
  return go_on;
}

static gboolean _client_cb(gint fd, GIOCondition cond, gpointer data)
{
  _client_t *c = data;
  _last_activity = g_get_monotonic_time();
  char buf[65536];
  const ssize_t n = (cond & G_IO_IN) ? read(c->in_fd, buf, sizeof(buf)) : 0;
  if(n < 0 && errno == EINTR) return G_SOURCE_CONTINUE;
  if(n <= 0) c->dead = TRUE;
  else
    g_string_append_len(c->inbuf, buf, n);
  gboolean go_on = TRUE;
  gchar *nl;
  while(go_on && !c->dead && (nl = memchr(c->inbuf->str, '\n', c->inbuf->len)))
  {
    gchar *line = g_strndup(c->inbuf->str, nl - c->inbuf->str);
    g_string_erase(c->inbuf, 0, nl - c->inbuf->str + 1);
    g_strstrip(line);
    go_on = !*line || _handle(c, line);
    g_free(line);
  }
  if(!go_on) _request_quit();
  if(c->dead)
  {
    // the source is removed by returning G_SOURCE_REMOVE
    c->source = 0;
    const gboolean stdio = _opt.listen_fd < 0;
    _client_free(c);
    if(stdio) _request_quit();   // stdin closed: the client is gone
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static gboolean _accept_cb(gint fd, GIOCondition cond, gpointer data)
{
  const int cfd = accept(fd, NULL, NULL);
  if(cfd >= 0) _client_new(cfd, cfd);
  _last_activity = g_get_monotonic_time();
  return G_SOURCE_CONTINUE;
}

static gboolean _any_unsaved(void)
{
  for(GList *l = _sessions; l; l = g_list_next(l))
    if(((_session_t *)l->data)->dirty) return TRUE;
  return FALSE;
}

// idle exit: nobody connected, nothing unsaved, nothing happened for a while
static gboolean _idle_cb(gpointer data)
{
  if(!_clients && !_any_unsaved()
     && g_get_monotonic_time() - _last_activity > (gint64)_opt.idle_exit_s * G_USEC_PER_SEC)
    _request_quit();
  return G_SOURCE_CONTINUE;
}

// ---- public -------------------------------------------------------------

int dt_api_listen(const char *path)
{
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if(strlen(path) >= sizeof(addr.sun_path))
  {
    dt_print(DT_DEBUG_ALWAYS, "[api] socket path too long: %s", path);
    return -1;
  }
  g_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
  // refuse if another server answers; remove a stale socket left by a crash
  const int probe = socket(AF_UNIX, SOCK_STREAM, 0);
  if(probe >= 0 && connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0)
  {
    close(probe);
    dt_print(DT_DEBUG_ALWAYS, "[api] another server is listening on %s", path);
    return -1;
  }
  if(probe >= 0) close(probe);
  g_unlink(path);
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  // only this user may connect
  const mode_t old = umask(0077);
  const int bound = fd >= 0 ? bind(fd, (struct sockaddr *)&addr, sizeof(addr)) : -1;
  umask(old);
  if(bound != 0 || listen(fd, 16) != 0)
  {
    dt_print(DT_DEBUG_ALWAYS, "[api] cannot listen on %s: %s", path, strerror(errno));
    if(fd >= 0) close(fd);
    return -1;
  }
  return fd;
}

gchar *dt_api_default_socket(const char *configdir)
{
  // the same rule as the example clients: next to the config dir, or in /tmp
  // when that path is too long for a unix socket
  // resolved like the clients' Path.resolve(), symlinks included
  char *resolved = realpath(configdir, NULL);
  gchar *real = g_strdup(resolved ? resolved : configdir);
  free(resolved);
  gchar *parent = g_path_get_dirname(real);
  gchar *path = g_build_filename(parent, "darktable-api.sock", NULL);
  if(strlen(path) >= 100)
  {
    g_free(path);
    gchar *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA1, real, -1);
    sum[12] = '\0';
    path = g_strdup_printf("/tmp/darktable-api-%d-%s.sock", (int)getuid(), sum);
    g_free(sum);
  }
  g_free(parent);
  g_free(real);
  return path;
}

// ---- darktable's window ----------------------------------------------------

static void _gui_history_cb(gpointer instance, gpointer user_data)
{
  const dt_imgid_t imgid = _darkroom_image();
  if(!dt_is_valid_imgid(imgid)) return;
  _gui_gen++;
  if(_api_editing) return;          // raised by the API's own change, told already
  const guint32 h = _gui_hash();
  if(h == _gui_hash_seen) return;   // the API's own change, raised later
  _gui_hash_seen = h;
  _notify(NULL, "edit", imgid);
}

// tell the clients which photo the darkroom shows (event "darkroom", imgid
// 0 when none) whenever that changes
static dt_imgid_t _darkroom_seen = NO_IMGID;

static void _darkroom_announce(void)
{
  const dt_imgid_t imgid = _darkroom_image();
  if(imgid == _darkroom_seen) return;
  _darkroom_seen = imgid;
  _notify(NULL, "darkroom", dt_is_valid_imgid(imgid) ? imgid : 0);
}

static void _gui_view_cb(gpointer instance, gpointer old_view, gpointer new_view, gpointer user_data)
{
  _gui_gen++;
  _darkroom_announce();
}

static void _gui_image_cb(gpointer instance, gpointer user_data)
{
  _gui_gen++;
  const dt_imgid_t imgid = _darkroom_image();
  _gui_hash_seen = dt_is_valid_imgid(imgid) ? _gui_hash() : 0;
  // the darkroom now edits this image: a session of it here would be a
  // second, diverging edit. its unsaved changes are dropped
  _session_t *s = dt_is_valid_imgid(imgid) ? _session_find_listed(imgid) : NULL;
  if(s)
  {
    const gboolean dirty = s->dirty;
    _session_close(s);
    if(dirty) _notify(NULL, "reopened", imgid);
  }
  _darkroom_announce();
}

static void _gui_info_cb(gpointer instance, GList *imgs, gpointer user_data)
{
  for(GList *l = imgs; l; l = g_list_next(l))
    _notify(NULL, "image", GPOINTER_TO_INT(l->data));
}

static void _drafts_load(void);

gboolean dt_api_handover(const char *path)
{
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  g_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if(fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
  {
    if(fd >= 0) close(fd);
    return FALSE;                     // nobody serves the library
  }
  // one request, then read until the engine has replied and gone (EOF):
  // by then it has released the library and removed its socket
  const char *req = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"handover\"}\n";
  gboolean ok = write(fd, req, strlen(req)) == (ssize_t)strlen(req);
  GString *in = g_string_new(NULL);
  const gint64 until = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
  char buf[65536];
  while(ok && g_get_monotonic_time() < until)
  {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if(poll(&pfd, 1, 500) <= 0) continue;
    const ssize_t n = read(fd, buf, sizeof(buf));
    if(n <= 0) break;
    g_string_append_len(in, buf, n);
  }
  close(fd);
  // the reply is the line with "id": 1; notifications may come first
  gboolean took = FALSE;
  gchar **lines = g_strsplit(in->str, "\n", -1);
  for(gchar **l = lines; *l; l++)
  {
    JsonParser *p = json_parser_new();
    if(**l && json_parser_load_from_data(p, *l, -1, NULL)
       && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p)))
    {
      JsonObject *o = json_node_get_object(json_parser_get_root(p));
      if(json_object_has_member(o, "id") && json_object_has_member(o, "result"))
      {
        JsonObject *r = json_object_get_object_member(o, "result");
        if(json_object_has_member(r, "drafts"))
          _drafts_from_json(json_object_get_array_member(r, "drafts"));
        took = TRUE;
      }
      else if(json_object_has_member(o, "error"))
        dt_print(DT_DEBUG_ALWAYS, "[api] the engine on %s refused to hand over", path);
    }
    g_object_unref(p);
  }
  g_strfreev(lines);
  g_string_free(in, TRUE);
  if(took)
    dt_print(DT_DEBUG_ALWAYS, "[api] took the library over from the engine on %s (%d drafts)",
             path, g_list_length(_drafts));
  return took;
}

void dt_api_start(const dt_api_options_t *options)
{
  _opt = *options;
  _in_gui = _opt.in_gui;
  if(_in_gui)
  {
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_DEVELOP_HISTORY_CHANGE, _gui_history_cb, NULL);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_DEVELOP_IMAGE_CHANGED, _gui_image_cb, NULL);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_IMAGE_INFO_CHANGED, _gui_info_cb, NULL);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED, _gui_view_cb, NULL);
  }
  // unsaved edits handed over by an engine (dt_api_handover) or kept by the
  // last server that stopped
  _socket_path = g_strdup(options->socket_path);
  _drafts_load();
  if(_drafts)
  {
    JsonBuilder *rb = json_builder_new();
    json_builder_begin_object(rb);
    _restore_drafts(rb);
    json_builder_end_object(rb);
    g_object_unref(rb);
  }
  _max_sessions = MAX(_opt.max_sessions, 1);
  _last_activity = g_get_monotonic_time();
  if(_opt.listen_fd >= 0)
  {
    _listen_source = g_unix_fd_add(_opt.listen_fd, G_IO_IN, _accept_cb, NULL);
    dt_print(DT_DEBUG_ALWAYS, "[api] listening on %s", _socket_path);
  }
  else
    _client_new(STDIN_FILENO, _opt.out_fd);
  if(_opt.idle_exit_s > 0 && _opt.listen_fd >= 0)
    _idle_source = g_timeout_add_seconds(1, _idle_cb, NULL);
}

// unsaved edits outlive the server: written next to its socket when it
// stops, restored by the next one (engine or darktable's window) at start
static gchar *_drafts_file(void)
{
  return _socket_path ? g_strconcat(_socket_path, ".drafts.json", NULL) : NULL;
}

static void _drafts_save(void)
{
  gchar *file = _drafts_file();
  _drafts_free();
  for(GList *l = _sessions; l; l = g_list_next(l))
    if(((_session_t *)l->data)->dirty) _draft_add(l->data);
  if(file && _drafts)
  {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    _drafts_to_json(b);
    json_builder_end_object(b);
    JsonGenerator *g = json_generator_new();
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(g, root);
    if(json_generator_to_file(g, file, NULL))
      dt_print(DT_DEBUG_ALWAYS, "[api] kept %d unsaved edits in %s", g_list_length(_drafts), file);
    json_node_unref(root);
    g_object_unref(g);
    g_object_unref(b);
  }
  _drafts_free();
  g_free(file);
}

static void _drafts_load(void)
{
  gchar *file = _drafts_file();
  JsonParser *p = json_parser_new();
  if(file && g_file_test(file, G_FILE_TEST_EXISTS) && json_parser_load_from_file(p, file, NULL)
     && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p)))
  {
    JsonObject *o = json_node_get_object(json_parser_get_root(p));
    if(json_object_has_member(o, "drafts"))
      _drafts_from_json(json_object_get_array_member(o, "drafts"));
    g_unlink(file);
  }
  g_object_unref(p);
  g_free(file);
}

gboolean dt_api_stop(void)
{
  if(!_released) _drafts_save();
  if(_in_gui)
  {
    DT_CONTROL_SIGNAL_DISCONNECT(_gui_history_cb, NULL);
    DT_CONTROL_SIGNAL_DISCONNECT(_gui_image_cb, NULL);
    DT_CONTROL_SIGNAL_DISCONNECT(_gui_info_cb, NULL);
    DT_CONTROL_SIGNAL_DISCONNECT(_gui_view_cb, NULL);
    if(_gui_session.mirror) _session_free(_gui_session.mirror);
    _gui_session.mirror = NULL;
  }
  while(_clients) _client_free(_clients->data);
  if(_listen_source) g_source_remove(_listen_source);
  if(_idle_source) g_source_remove(_idle_source);
  _listen_source = _idle_source = 0;
  if(_opt.listen_fd >= 0)
  {
    close(_opt.listen_fd);
    g_unlink(_socket_path);
    _opt.listen_fd = -1;
  }
  g_free(_socket_path);
  _socket_path = NULL;
  _session_close_all();
  return _released;
}
