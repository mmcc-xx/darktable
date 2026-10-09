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
   reset, reopened, closed, image (rating/label), library_released or
   library_acquired. --idle-exit stops the engine that many seconds after the
   last client left, unless an image has unsaved changes.

   the session methods (module_*, history_*, save, reset, render,
   session_close) work on params.imgid if given, else on the image the
   client last opened.

   methods:
     ping                        -> {"version"}
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
                                    item; enums by name, label or number
     module_enable {operation, instance, enabled}
                                 -> switches a module on or off (in memory)
     history_list                -> the open image's history items, and
                                    history_end
     history_end {end}           -> undo/redo to a step (0 = original)
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
     render {width, height, path, quality}
                                 -> writes an sRGB JPEG fitted inside
                                    width x height to path
     shutdown                    -> closes the session and exits

   edits stay in memory until save. each session keeps one pixelpipe with
   darktable's full-size cache, so a render after a change re-runs only the
   modules after the changed one. see src/api/README.md
*/

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
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/pixelpipe_hb.h"
#include "imageio/imageio_jpeg.h"
#include "views/view.h"

#include <errno.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <limits.h>
#include <signal.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
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
} _session_t;

static GList *_sessions = NULL;
static _session_t *_cur = NULL;
static int _max_sessions = 3;        // each holds a full-size raw and a pipe cache
static volatile sig_atomic_t _stop = 0;

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

static _session_t *_session_find(const dt_imgid_t imgid)
{
  for(GList *l = _sessions; l; l = g_list_next(l))
  {
    _session_t *s = l->data;
    if(s->dev->image_storage.id == imgid) return s;
  }
  return NULL;
}

static void _session_close(_session_t *s)
{
  if(!s) return;
  _sessions = g_list_remove(_sessions, s);
  if(_cur == s) _cur = NULL;
  _session_free(s);
}

static void _session_close_all(void)
{
  while(_sessions) _session_close(_sessions->data);
}

// load an image into a new session: its history replayed up to history_end,
// as the darkroom does, and a pipe on its full-size buffer
static _session_t *_session_load(const dt_imgid_t imgid, gchar **err)
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
  _sessions = g_list_append(_sessions, s);
  return s;
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
  if(s && fresh)
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
  json_builder_end_array(b);
  return TRUE;
}

// parse one requested value into the field's number, or explain why not
static gboolean _parse_value(const dt_introspection_field_t *f, JsonNode *v, double *out, gchar **err)
{
  const char *name = f->header.name;
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
    const dt_introspection_field_t *f = _field(m, n->data);
    double v;
    if(!f)
    {
      *err = g_strdup_printf("module '%s' has no field '%s'", m->op, (char *)n->data);
      ok = FALSE;
    }
    else if((ok = _parse_value(f, json_object_get_member(values, n->data), &v, err)))
      _set_num(f, (uint8_t *)p + f->header.offset, v);
  }
  if(ok)
  {
    memcpy(m->params, p, m->params_size);
    // as in the darkroom: changing a module's setting switches it on
    dt_dev_add_history_item_ext(_cur->dev, m, TRUE, TRUE);
    _cur->pipe_changed = TRUE;
    _cur->dirty = TRUE;
    json_builder_set_member_name(b, "enabled");
    json_builder_add_boolean_value(b, m->enabled);
    json_builder_set_member_name(b, "history_end");
    json_builder_add_int_value(b, _cur->dev->history_end);
    json_builder_set_member_name(b, "values");
    json_builder_begin_object(b);
    for(GList *n = names; n; n = g_list_next(n))
    {
      json_builder_set_member_name(b, n->data);
      _add_field_value(b, _field(m, n->data), m->params);
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
  dt_dev_add_history_item_ext(_cur->dev, m, FALSE, TRUE);
  _cur->pipe_changed = TRUE;
  _cur->dirty = TRUE;
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
  dt_dev_pop_history_items_ext(_cur->dev, n);
  _cur->pipe_changed = TRUE;
  _cur->dirty = TRUE;
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, _cur->dev->history_end);
  return TRUE;
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
  const dt_imgid_t imgid = _cur->dev->image_storage.id;
  const gint64 t0 = g_get_monotonic_time();
  // the history hash alone can miss a change: a draft restored after a
  // release can match the hash of an older thumbnail
  const gboolean was_dirty = _cur->dirty;
  dt_dev_write_history(_cur->dev);
  _cur->dirty = FALSE;
  const gboolean changed = was_dirty || !dt_history_hash_is_mipmap_synced(imgid);
  if(changed)
  {
    dt_image_cache_set_change_timestamp(imgid);
    dt_mipmap_cache_remove(imgid);
    dt_image_update_final_size(imgid);
    dt_image_write_sidecar_file(imgid);
    dt_history_hash_set_mipmap(imgid);
  }
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
  _session_close(_cur);
  dt_history_delete_on_image_ext(imgid, FALSE, TRUE);
  _session_t *s = _session_load(imgid, err);
  if(!s) return FALSE;
  _cur = s;
  // the defaults exist only in memory until saved, as when darktable first
  // opens an image; write them so the library and thumbnails agree
  dt_dev_write_history(s->dev);
  dt_image_cache_set_change_timestamp(imgid);
  dt_mipmap_cache_remove(imgid);
  dt_image_write_sidecar_file(imgid);
  dt_history_hash_set_mipmap(imgid);
  _session_info(b, s);
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

static gboolean _library_status(JsonBuilder *b, const dt_imgid_t current, gchar **err)
{
  json_builder_set_member_name(b, "state");
  json_builder_add_string_value(b, _released ? "released" : "owned");
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
    if(!s->dirty) continue;
    // keep the unsaved state of every module, to restore it on acquire if
    // nobody changed the image meanwhile
    _draft_t *d = g_new0(_draft_t, 1);
    d->imgid = imgid;
    d->fingerprint = _fingerprint(imgid);
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
  json_builder_set_member_name(b, "ms");
  json_builder_add_int_value(b, (g_get_monotonic_time() - t0) / 1000);
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
    rating = json_node_get_int(v);
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

static gboolean _render(JsonObject *params, JsonBuilder *b, gchar **err)
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
  if(_cur->pipe_changed)
  {
    // parameters changed or modules switched: syncing the nodes is enough,
    // and the cache keeps every line whose inputs are unchanged
    dt_dev_pixelpipe_synch_all(&_cur->pipe, _cur->dev);
    dt_dev_pixelpipe_get_dimensions(&_cur->pipe, _cur->dev, _cur->pipe.iwidth, _cur->pipe.iheight,
                                    &_cur->pipe.processed_width, &_cur->pipe.processed_height);
    _cur->pipe_changed = FALSE;
  }
  const double scale = fmin(1.0, fmin((double)max_w / _cur->pipe.processed_width,
                                      (double)max_h / _cur->pipe.processed_height));
  const int w = floor(scale * _cur->pipe.processed_width);
  const int h = floor(scale * _cur->pipe.processed_height);

  // as a non-hq export does: downscale right after demosaic, not in finalscale
  dt_dev_pixelpipe_iop_t *finalscale = NULL;
  for(GList *n = g_list_last(_cur->pipe.nodes); n; n = g_list_previous(n))
  {
    dt_dev_pixelpipe_iop_t *piece = n->data;
    if(dt_iop_module_is_finalscale(piece->module)) { finalscale = piece; break; }
  }
  if(finalscale) finalscale->enabled = FALSE;
  dt_dev_pixelpipe_process(&_cur->pipe, _cur->dev, 0, 0, w, h, scale, DT_DEVICE_NONE);
  if(finalscale) finalscale->enabled = TRUE;
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
  json_builder_set_member_name(b, "process_ms");
  json_builder_add_int_value(b, (t1 - t0) / 1000);
  json_builder_set_member_name(b, "jpeg_ms");
  json_builder_add_int_value(b, (t2 - t1) / 1000);
  return TRUE;
}

static void _on_signal(int sig)
{
  _stop = 1;
}

// ---- clients ------------------------------------------------------------
// stdin/stdout is one client; with --listen, every connection to the unix
// socket is one. requests are handled one at a time, in arrival order

typedef struct _client_t
{
  int id;
  int in_fd, out_fd;
  GString *inbuf;
  dt_imgid_t current;               // the image this client last opened
  gboolean dead;
} _client_t;

static GList *_clients = NULL;
static int _next_client_id = 1;

static _client_t *_client_new(const int in_fd, const int out_fd)
{
  _client_t *c = g_new0(_client_t, 1);
  c->id = _next_client_id++;
  c->in_fd = in_fd;
  c->out_fd = out_fd;
  c->inbuf = g_string_new(NULL);
  c->current = NO_IMGID;
  _clients = g_list_append(_clients, c);
  return c;
}

static void _client_free(_client_t *c)
{
  _clients = g_list_remove(_clients, c);
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
      c->dead = TRUE;  // gone (EPIPE); dropped by the main loop
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
// {"method": "event", "params": {"type", "imgid", "history_end", "client"}}
static void _notify(const _client_t *from, const char *type, const dt_imgid_t imgid)
{
  if(!_clients || !_clients->next) return;
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
  json_builder_add_int_value(b, from->id);
  json_builder_end_object(b);
  json_builder_end_object(b);
  JsonNode *root = json_builder_get_root(b);
  for(GList *l = _clients; l; l = g_list_next(l))
    if(l->data != from) _send_node(l->data, root);
  json_node_unref(root);
  g_object_unref(b);
}

// methods that work on an edit session, and the event each one sends
static const struct { const char *method; const char *event; } _session_methods[] = {
  { "module_list", NULL }, { "module_get", NULL }, { "history_list", NULL }, { "render", NULL },
  { "module_set", "edit" }, { "module_enable", "edit" }, { "history_end", "edit" },
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
     && g_strcmp0(method, "library_acquire") && g_strcmp0(method, "shutdown"))
    err = g_strdup("the library is released: call library_acquire first");
  else if(!g_strcmp0(method, "ping"))
  {
    json_builder_set_member_name(b, "version");
    json_builder_add_string_value(b, darktable_package_version);
    json_builder_set_member_name(b, "client");
    json_builder_add_int_value(b, c->id);
  }
  else if(!g_strcmp0(method, "film_rolls"))
    _film_rolls(b, &err);
  else if(!g_strcmp0(method, "images_list"))
    _images_list(params, b, &err);
  else if(!g_strcmp0(method, "image_info"))
    _image_info(params, b, &err);
  else if(!g_strcmp0(method, "thumbnail"))
    _thumbnail(params, b, &err);
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
  else if(!g_strcmp0(method, "history_end"))
    _history_end(params, b, &err);
  else if(!g_strcmp0(method, "save"))
    _save(b, &err);
  else if(!g_strcmp0(method, "library_status"))
    _library_status(b, c->current, &err);
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
    go_on = FALSE;
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

// read what a client sent and handle every complete line. FALSE: stop
static gboolean _client_read(_client_t *c)
{
  char buf[65536];
  const ssize_t n = read(c->in_fd, buf, sizeof(buf));
  if(n < 0 && errno == EINTR) return TRUE;
  if(n <= 0)
  {
    c->dead = TRUE;
    return TRUE;
  }
  g_string_append_len(c->inbuf, buf, n);
  gchar *nl;
  while(!c->dead && (nl = memchr(c->inbuf->str, '\n', c->inbuf->len)))
  {
    gchar *line = g_strndup(c->inbuf->str, nl - c->inbuf->str);
    g_string_erase(c->inbuf, 0, nl - c->inbuf->str + 1);
    g_strstrip(line);
    const gboolean go_on = !*line || _handle(c, line);
    g_free(line);
    if(!go_on) return FALSE;
  }
  return TRUE;
}

static gboolean _any_unsaved(void)
{
  for(GList *l = _sessions; l; l = g_list_next(l))
    if(((_session_t *)l->data)->dirty) return TRUE;
  return FALSE;
}

// a unix socket only this user can use. refuses if another engine already
// listens on it; removes a stale one left by a crash
static int _listen_on(const char *path)
{
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if(strlen(path) >= sizeof(addr.sun_path))
  {
    fprintf(stderr, "darktable-api: socket path too long: %s\n", path);
    return -1;
  }
  g_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
  const int probe = socket(AF_UNIX, SOCK_STREAM, 0);
  if(probe >= 0 && connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0)
  {
    close(probe);
    fprintf(stderr, "darktable-api: another engine is listening on %s\n", path);
    return -1;
  }
  if(probe >= 0) close(probe);
  g_unlink(path);
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  const mode_t old = umask(0077);
  const int bound = fd >= 0 ? bind(fd, (struct sockaddr *)&addr, sizeof(addr)) : -1;
  umask(old);
  if(bound != 0 || listen(fd, 16) != 0)
  {
    fprintf(stderr, "darktable-api: cannot listen on %s: %s\n", path, strerror(errno));
    if(fd >= 0) close(fd);
    return -1;
  }
  return fd;
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
  int idle_exit_s = 0;
  int core_start = argc;
  for(int i = 1; i < argc; i++)
  {
    if(!g_strcmp0(argv[i], "--core")) { core_start = i + 1; break; }
    else if(!g_strcmp0(argv[i], "--listen") && i + 1 < argc) listen_path = argv[++i];
    else if(!g_strcmp0(argv[i], "--max-sessions") && i + 1 < argc) _max_sessions = atoi(argv[++i]);
    else if(!g_strcmp0(argv[i], "--idle-exit") && i + 1 < argc) idle_exit_s = atoi(argv[++i]);
  }
  _max_sessions = MAX(_max_sessions, 1);
  idle_exit_s = MAX(idle_exit_s, 0);
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

  int listen_fd = -1;
  if(listen_path && (listen_fd = _listen_on(listen_path)) < 0) return 1;

  if(dt_init(m->len, (char **)m->pdata, FALSE, TRUE, NULL))
  {
    fprintf(stderr, "darktable-api: dt_init failed\n");
    if(listen_path) g_unlink(listen_path);
    g_ptr_array_free(m, TRUE);
    return 1;
  }

  // stop like a shutdown request on SIGTERM/SIGINT (launchd, a restart): no
  // SA_RESTART, so poll returns. a client that went away must not kill us
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = _on_signal;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);

  if(listen_fd < 0)
    _client_new(STDIN_FILENO, out_fd);
  else
    fprintf(stderr, "darktable-api: listening on %s\n", listen_path);

  gint64 last_activity = g_get_monotonic_time();
  gboolean go_on = TRUE;
  while(go_on && !_stop)
  {
    const int nfds = g_list_length(_clients) + (listen_fd >= 0);
    struct pollfd *fds = g_new0(struct pollfd, MAX(nfds, 1));
    int k = 0;
    if(listen_fd >= 0)
    {
      fds[k].fd = listen_fd;
      fds[k++].events = POLLIN;
    }
    for(GList *l = _clients; l; l = g_list_next(l))
    {
      fds[k].fd = ((_client_t *)l->data)->in_fd;
      fds[k++].events = POLLIN;
    }
    const int ready = poll(fds, k, 1000);
    if(ready > 0)
    {
      last_activity = g_get_monotonic_time();
      k = 0;
      if(listen_fd >= 0 && (fds[k++].revents & POLLIN))
      {
        const int cfd = accept(listen_fd, NULL, NULL);
        if(cfd >= 0) _client_new(cfd, cfd);
      }
      // the list may change while handling (new clients); go by fd
      for(; k < nfds && go_on; k++)
      {
        if(!(fds[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        for(GList *l = _clients; l; l = g_list_next(l))
        {
          _client_t *c = l->data;
          if(c->in_fd == fds[k].fd && !c->dead)
          {
            go_on = _client_read(c);
            break;
          }
        }
      }
    }
    g_free(fds);

    // drop clients that went away; on stdin/stdout that ends the engine
    for(GList *l = _clients; l;)
    {
      GList *next = g_list_next(l);
      _client_t *c = l->data;
      if(c->dead)
      {
        if(listen_fd < 0) go_on = FALSE;
        _client_free(c);
      }
      l = next;
    }
    // with --idle-exit: stop when nobody is connected, nothing is unsaved
    // and nothing happened for that long
    if(listen_fd >= 0 && idle_exit_s && !_clients && !_any_unsaved()
       && g_get_monotonic_time() - last_activity > (gint64)idle_exit_s * G_USEC_PER_SEC)
      go_on = FALSE;
  }

  while(_clients) _client_free(_clients->data);
  if(listen_fd >= 0)
  {
    close(listen_fd);
    g_unlink(listen_path);
  }
  _session_close_all();
  if(_released)
  {
    // dt_cleanup would tear down the closed database and caches again;
    // there's nothing left to save
    _exit(0);
  }
  dt_cleanup();
  g_ptr_array_free(m, TRUE);
  return 0;
}
