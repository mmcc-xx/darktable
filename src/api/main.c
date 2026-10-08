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

   Line-delimited JSON-RPC 2.0 over stdin/stdout: one request per line, one
   response per line. All arguments after --core go to dt_init, so it opens
   whichever library --configdir/--library name and holds its lock until it
   exits.

   methods:
     ping                        -> {"version"}
     session_open {imgid}        -> loads the image and replays its history
                                    up to history_end, as the darkroom does
     session_close               -> releases the open image
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
     library_status              -> owned or released, the lock holder's pid
     library_release             -> closes the session (unsaved changes kept
                                    as a draft), the caches and the library,
                                    removing the lock files: darktable's GUI
                                    can open the library now
     library_acquire             -> takes the library back (refused while
                                    another live process holds it), reopens
                                    database and caches, reloads darktablerc
                                    and restores the draft if the image was
                                    not changed meanwhile
     render {width, height, path, quality}
                                 -> writes an sRGB JPEG fitted inside
                                    width x height to path
     shutdown                    -> closes the session and exits

   edits stay in memory until save. the session keeps one pixelpipe with
   darktable's full-size cache, so a render after a change re-runs only the
   modules after the changed one. see src/api/README.md
*/

#include "common/darktable.h"
#include "common/database.h"
#include "common/file_location.h"
#include "common/history.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/iop_order.h"
#include "common/mipmap_cache.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/pixelpipe_hb.h"
#include "imageio/imageio_jpeg.h"

#include <errno.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <limits.h>
#include <signal.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// the open session, one at a time
static dt_develop_t *_dev = NULL;
static dt_dev_pixelpipe_t _pipe;
static dt_mipmap_buffer_t _buf;
static gboolean _pipe_changed = FALSE;
static gboolean _dirty = FALSE;       // edits since the last open or save
static volatile sig_atomic_t _stop = 0;

static gboolean _in_history(const dt_iop_module_t *m)
{
  for(GList *h = _dev->history; h; h = g_list_next(h))
  {
    const dt_dev_history_item_t *hi = h->data;
    if(hi->module == m && hi->num < _dev->history_end) return TRUE;
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

static void _session_close(void)
{
  if(!_dev) return;
  dt_dev_pixelpipe_cleanup(&_pipe);
  dt_mipmap_cache_release(&_buf);
  dt_dev_cleanup(_dev);
  g_free(_dev);
  _dev = NULL;
}

static gboolean _session_open(const dt_imgid_t imgid, JsonBuilder *b, gchar **err)
{
  if(!dt_is_valid_imgid(imgid) || !_image_exists(imgid))
  {
    *err = g_strdup_printf("no image with id %d", imgid);
    return FALSE;
  }

  _session_close();
  const gint64 t0 = g_get_monotonic_time();
  _dev = g_new0(dt_develop_t, 1);
  dt_dev_init(_dev, FALSE);
  dt_dev_load_image(_dev, imgid);
  // dt_dev_load_image reads the history but leaves module params at their
  // defaults; replaying it is what the darkroom does when it shows an image
  dt_dev_pop_history_items_ext(_dev, _dev->history_end);
  dt_ioppr_resync_modules_order(_dev);

  dt_mipmap_cache_get(&_buf, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
  if(!_buf.buf || !_buf.width || !_buf.height)
  {
    *err = g_strdup_printf("cannot load the image file of %d", imgid);
    dt_mipmap_cache_release(&_buf);
    dt_dev_cleanup(_dev);
    g_free(_dev);
    _dev = NULL;
    return FALSE;
  }
  // a full (darkroom) pipe: an export pipe keeps only DT_PIPECACHE_MIN cache
  // lines, so every render would start again from the raw file
  dt_dev_pixelpipe_init(&_pipe);
  dt_dev_pixelpipe_set_icc(&_pipe, DT_COLORSPACE_SRGB, NULL, DT_INTENT_PERCEPTUAL);
  dt_dev_pixelpipe_set_input(&_pipe, _dev, (float *)_buf.buf,
                             _buf.width, _buf.height, _buf.iscale);
  dt_dev_pixelpipe_create_nodes(&_pipe, _dev);
  _pipe_changed = TRUE;
  _dirty = FALSE;

  json_builder_set_member_name(b, "imgid");
  json_builder_add_int_value(b, imgid);
  json_builder_set_member_name(b, "filename");
  json_builder_add_string_value(b, _dev->image_storage.filename);
  json_builder_set_member_name(b, "width");
  json_builder_add_int_value(b, _dev->image_storage.width);
  json_builder_set_member_name(b, "height");
  json_builder_add_int_value(b, _dev->image_storage.height);
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, _dev->history_end);
  json_builder_set_member_name(b, "history_items");
  json_builder_add_int_value(b, g_list_length(_dev->history));
  json_builder_set_member_name(b, "ms");
  json_builder_add_int_value(b, (g_get_monotonic_time() - t0) / 1000);
  return TRUE;
}

static gboolean _module_list(JsonBuilder *b, gchar **err)
{
  if(!_dev)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  json_builder_set_member_name(b, "modules");
  json_builder_begin_array(b);
  for(GList *l = _dev->iop; l; l = g_list_next(l))
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
  dt_iop_module_t *m = op ? dt_iop_get_module_by_op_priority(_dev->iop, op, instance) : NULL;
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
  if(!_dev)
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
  if(!_dev)
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
    dt_dev_add_history_item_ext(_dev, m, TRUE, TRUE);
    _pipe_changed = TRUE;
    _dirty = TRUE;
    json_builder_set_member_name(b, "enabled");
    json_builder_add_boolean_value(b, m->enabled);
    json_builder_set_member_name(b, "history_end");
    json_builder_add_int_value(b, _dev->history_end);
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
  if(!_dev)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  dt_iop_module_t *m = _find_module(params, err);
  if(!m) return FALSE;
  m->enabled = json_object_get_boolean_member_with_default(params, "enabled", TRUE);
  dt_dev_add_history_item_ext(_dev, m, FALSE, TRUE);
  _pipe_changed = TRUE;
  _dirty = TRUE;
  json_builder_set_member_name(b, "enabled");
  json_builder_add_boolean_value(b, m->enabled);
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, _dev->history_end);
  return TRUE;
}

static gboolean _history_list(JsonBuilder *b, gchar **err)
{
  if(!_dev)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, _dev->history_end);
  json_builder_set_member_name(b, "items");
  json_builder_begin_array(b);
  int i = 0;
  for(GList *h = _dev->history; h; h = g_list_next(h), i++)
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
    json_builder_add_boolean_value(b, i < _dev->history_end);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  return TRUE;
}

// undo/redo to a step, as darktable's history panel: items above it stay
// until the next edit, which drops them
static gboolean _history_end(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_dev)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  const int n = params ? json_object_get_int_member_with_default(params, "end", -1) : -1;
  const int count = g_list_length(_dev->history);
  if(n < 0 || n > count)
  {
    *err = g_strdup_printf("end must be 0..%d", count);
    return FALSE;
  }
  dt_dev_pop_history_items_ext(_dev, n);
  _pipe_changed = TRUE;
  _dirty = TRUE;
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, _dev->history_end);
  return TRUE;
}

// what the darkroom does when it leaves an image (views/darkroom.c, leave()):
// write the history, then invalidate the thumbnails and write the sidecar if
// the edit changed. the sidecar follows write_sidecar_files, so a library
// configured with "never" gets none
static gboolean _save(JsonBuilder *b, gchar **err)
{
  if(!_dev)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  const dt_imgid_t imgid = _dev->image_storage.id;
  const gint64 t0 = g_get_monotonic_time();
  dt_dev_write_history(_dev);
  _dirty = FALSE;
  const gboolean changed = !dt_history_hash_is_mipmap_synced(imgid);
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
  json_builder_add_int_value(b, _dev->history_end);
  json_builder_set_member_name(b, "history_items");
  json_builder_add_int_value(b, g_list_length(_dev->history));
  json_builder_set_member_name(b, "ms");
  json_builder_add_int_value(b, (g_get_monotonic_time() - t0) / 1000);
  return TRUE;
}

// darktable's "discard history" (lighttable), then load the image again:
// _dev_auto_apply_presets (develop.c) applies the workflow's default modules
// and the user's auto-apply presets to an image with no history
static gboolean _reset(JsonBuilder *b, gchar **err)
{
  if(!_dev)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  const dt_imgid_t imgid = _dev->image_storage.id;
  _session_close();
  dt_history_delete_on_image_ext(imgid, FALSE, TRUE);
  if(!_session_open(imgid, b, err)) return FALSE;
  // the defaults exist only in memory until saved, as when darktable first
  // opens an image; write them so the library and thumbnails agree
  dt_dev_write_history(_dev);
  dt_image_cache_set_change_timestamp(imgid);
  dt_mipmap_cache_remove(imgid);
  dt_image_write_sidecar_file(imgid);
  dt_history_hash_set_mipmap(imgid);
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

static gboolean _released = FALSE;
static gchar *_library_path = NULL;   // the library file to reopen
static GList *_draft = NULL;          // unsaved module state at release
static dt_imgid_t _draft_imgid = NO_IMGID;
static gchar *_draft_fingerprint = NULL;

static void _draft_free(void)
{
  for(GList *l = _draft; l; l = g_list_next(l))
  {
    _draft_module_t *d = l->data;
    g_free(d->params);
    g_free(d);
  }
  g_list_free(_draft);
  _draft = NULL;
  _draft_imgid = NO_IMGID;
  g_free(_draft_fingerprint);
  _draft_fingerprint = NULL;
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

static gboolean _library_status(JsonBuilder *b, gchar **err)
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
    json_builder_set_member_name(b, "draft_imgid");
    json_builder_add_int_value(b, _draft_imgid);
  }
  else
  {
    json_builder_set_member_name(b, "open_imgid");
    json_builder_add_int_value(b, _dev ? _dev->image_storage.id : NO_IMGID);
    json_builder_set_member_name(b, "unsaved");
    json_builder_add_boolean_value(b, _dev && _dirty);
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
  _draft_free();
  if(_dev && _dirty)
  {
    // keep the unsaved state of every module, to restore it on acquire if
    // nobody changed the image meanwhile
    _draft_imgid = _dev->image_storage.id;
    _draft_fingerprint = _fingerprint(_draft_imgid);
    for(GList *l = _dev->iop; l; l = g_list_next(l))
    {
      const dt_iop_module_t *m = l->data;
      _draft_module_t *d = g_new0(_draft_module_t, 1);
      g_strlcpy(d->op, m->op, sizeof(d->op));
      d->multi_priority = m->multi_priority;
      d->enabled = m->enabled;
      d->params_size = m->params_size;
      d->params = g_malloc(m->params_size);
      memcpy(d->params, m->params, m->params_size);
      _draft = g_list_prepend(_draft, d);
    }
  }
  const dt_imgid_t was_open = _dev ? _dev->image_storage.id : NO_IMGID;
  _session_close();

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
  json_builder_set_member_name(b, "closed_imgid");
  json_builder_add_int_value(b, was_open);
  json_builder_set_member_name(b, "draft_kept");
  json_builder_add_boolean_value(b, _draft != NULL);
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
  if(dt_is_valid_imgid(_draft_imgid) && _image_exists(_draft_imgid))
  {
    gchar *fp = _fingerprint(_draft_imgid);
    const gboolean unchanged = !g_strcmp0(fp, _draft_fingerprint);
    g_free(fp);
    gchar *open_err = NULL;
    JsonBuilder *ob = json_builder_new();
    json_builder_begin_object(ob);
    const gboolean opened = _session_open(_draft_imgid, ob, &open_err);
    json_builder_end_object(ob);
    g_object_unref(ob);
    int restored = 0;
    if(opened && unchanged)
    {
      for(GList *l = _draft; l; l = g_list_next(l))
      {
        const _draft_module_t *d = l->data;
        dt_iop_module_t *m = dt_iop_get_module_by_op_priority(_dev->iop, d->op, d->multi_priority);
        if(!m || m->params_size != d->params_size) continue;
        if(m->enabled == d->enabled && !memcmp(m->params, d->params, d->params_size)) continue;
        memcpy(m->params, d->params, d->params_size);
        m->enabled = d->enabled;
        dt_dev_add_history_item_ext(_dev, m, FALSE, TRUE);
        restored++;
      }
      _dirty = restored > 0;
      _pipe_changed = TRUE;
    }
    json_builder_set_member_name(b, "draft_imgid");
    json_builder_add_int_value(b, _draft_imgid);
    json_builder_set_member_name(b, "draft");
    json_builder_add_string_value(b, !opened ? "image could not be opened"
                                     : unchanged ? "restored"
                                     : "dropped: the image was changed while released");
    json_builder_set_member_name(b, "modules_restored");
    json_builder_add_int_value(b, restored);
    g_free(open_err);
  }
  _draft_free();
  json_builder_set_member_name(b, "ms");
  json_builder_add_int_value(b, (g_get_monotonic_time() - t0) / 1000);
  return TRUE;
}

static gboolean _render(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_dev)
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
  if(_pipe_changed)
  {
    // parameters changed or modules switched: syncing the nodes is enough,
    // and the cache keeps every line whose inputs are unchanged
    dt_dev_pixelpipe_synch_all(&_pipe, _dev);
    dt_dev_pixelpipe_get_dimensions(&_pipe, _dev, _pipe.iwidth, _pipe.iheight,
                                    &_pipe.processed_width, &_pipe.processed_height);
    _pipe_changed = FALSE;
  }
  const double scale = fmin(1.0, fmin((double)max_w / _pipe.processed_width,
                                      (double)max_h / _pipe.processed_height));
  const int w = floor(scale * _pipe.processed_width);
  const int h = floor(scale * _pipe.processed_height);

  // as a non-hq export does: downscale right after demosaic, not in finalscale
  dt_dev_pixelpipe_iop_t *finalscale = NULL;
  for(GList *n = g_list_last(_pipe.nodes); n; n = g_list_previous(n))
  {
    dt_dev_pixelpipe_iop_t *piece = n->data;
    if(dt_iop_module_is_finalscale(piece->module)) { finalscale = piece; break; }
  }
  if(finalscale) finalscale->enabled = FALSE;
  dt_dev_pixelpipe_process(&_pipe, _dev, 0, 0, w, h, scale, DT_DEVICE_NONE);
  if(finalscale) finalscale->enabled = TRUE;
  const gint64 t1 = g_get_monotonic_time();

  if(!_pipe.backbuf || _pipe.backbuf_width != w || _pipe.backbuf_height != h)
  {
    *err = g_strdup("the pipe produced no output");
    return FALSE;
  }
  // backbuf is a cache line in display byte order (BGRA): convert a copy
  const size_t npix = (size_t)w * h;
  uint8_t *rgba = g_malloc(npix * 4);
  const uint8_t *src = _pipe.backbuf;
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

static void _reply(FILE *out, JsonNode *id, JsonNode *result, const gchar *err)
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
  JsonGenerator *g = json_generator_new();
  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(g, root);
  gchar *s = json_generator_to_data(g, NULL);
  fprintf(out, "%s\n", s);
  fflush(out);
  g_free(s);
  json_node_unref(root);
  g_object_unref(g);
  g_object_unref(b);
}

// returns FALSE when the loop should stop
static gboolean _handle(const gchar *line, FILE *out)
{
  JsonParser *p = json_parser_new();
  if(!json_parser_load_from_data(p, line, -1, NULL)
     || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p)))
  {
    _reply(out, NULL, NULL, "parse error");
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

  if(_released && g_strcmp0(method, "ping") && g_strcmp0(method, "library_status")
     && g_strcmp0(method, "library_acquire") && g_strcmp0(method, "shutdown"))
    err = g_strdup("the library is released: call library_acquire first");
  else if(!g_strcmp0(method, "ping"))
  {
    json_builder_set_member_name(b, "version");
    json_builder_add_string_value(b, darktable_package_version);
  }
  else if(!g_strcmp0(method, "session_open"))
  {
    const dt_imgid_t imgid = params ? json_object_get_int_member_with_default(params, "imgid", -1) : -1;
    _session_open(imgid, b, &err);
  }
  else if(!g_strcmp0(method, "session_close"))
  {
    json_builder_set_member_name(b, "closed");
    json_builder_add_boolean_value(b, _dev != NULL);
    _session_close();
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
    _library_status(b, &err);
  else if(!g_strcmp0(method, "library_release"))
    _library_release(b, &err);
  else if(!g_strcmp0(method, "library_acquire"))
    _library_acquire(b, &err);
  else if(!g_strcmp0(method, "reset"))
    _reset(b, &err);
  else if(!g_strcmp0(method, "render"))
    _render(params, b, &err);
  else if(!g_strcmp0(method, "shutdown"))
  {
    _session_close();
    go_on = FALSE;
  }
  else
    err = g_strdup_printf("unknown method '%s'", method);

  json_builder_end_object(b);
  // notifications (no id) get no reply
  if(id) _reply(out, id, json_builder_get_root(b), err);
  g_free(err);
  g_object_unref(b);
  g_object_unref(p);
  return go_on;
}

int main(int argc, char **argv)
{
  dt_loc_init(NULL, NULL, NULL, NULL, NULL, NULL);
  char localedir[PATH_MAX] = { 0 };
  dt_loc_get_localedir(localedir, sizeof(localedir));
  bindtextdomain(GETTEXT_PACKAGE, localedir);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);

  int core_start = argc;
  for(int i = 1; i < argc; i++)
    if(!g_strcmp0(argv[i], "--core")) { core_start = i + 1; break; }
  gboolean has_configdir = FALSE;
  for(int i = core_start; i < argc; i++)
    if(!g_strcmp0(argv[i], "--configdir") || !g_strcmp0(argv[i], "--library"))
      has_configdir = TRUE;
  // never fall back to the user's real ~/.config/darktable by accident
  if(!has_configdir)
  {
    fprintf(stderr, "usage: darktable-api --core --configdir <dir> [darktable options]\n");
    return 1;
  }

  GPtrArray *m = g_ptr_array_new();
  g_ptr_array_add(m, (gpointer)"darktable-api");
  for(int i = core_start; i < argc; i++) g_ptr_array_add(m, argv[i]);

  // libdarktable prints to stdout in places: keep the real stdout for replies
  fflush(stdout);
  FILE *out = fdopen(dup(STDOUT_FILENO), "w");
  dup2(STDERR_FILENO, STDOUT_FILENO);

  if(dt_init(m->len, (char **)m->pdata, FALSE, TRUE, NULL))
  {
    fprintf(stderr, "darktable-api: dt_init failed\n");
    g_ptr_array_free(m, TRUE);
    return 1;
  }

  // stop like a shutdown request on SIGTERM/SIGINT (launchd, the web app
  // restarting): no SA_RESTART, so the blocked read returns
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = _on_signal;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  gchar *line = NULL;
  size_t cap = 0;
  ssize_t n;
  while(!_stop && (n = getline(&line, &cap, stdin)) > 0)
  {
    g_strstrip(line);
    if(!*line) continue;
    if(!_handle(line, out)) break;
  }
  free(line);

  _session_close();
  if(_released)
  {
    // dt_cleanup would tear down the closed database and caches again;
    // there's nothing left to save
    fflush(out);
    _exit(0);
  }
  dt_cleanup();
  fflush(out);
  g_ptr_array_free(m, TRUE);
  return 0;
}
