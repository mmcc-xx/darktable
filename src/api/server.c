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
     coords {points, from, to}   -> maps [x, y] points between "raw" (drawn
                                    mask space: the pipe's input, normalized),
                                    "image" (the rendered image, fractions)
                                    and "uncropped" (crop's input)
     retouch_list                -> retouch's shapes: type, circle center,
                                    radius and feather, source (raw space),
                                    algorithm and its options
     retouch_add {spots: [{x, y, r, algorithm, sx, sy, blur_type,
                  blur_radius, fill_mode, fill_color, fill_brightness}]}
                                 -> adds circles (raw space; r relative to
                                    the shorter side) that clone or heal
                                    from a source, blur or fill, as one
                                    history item; retouch_heal: heal default
     retouch_set {formid, x, y, r, sx, sy, algorithm, ...}
                                 -> moves, resizes or changes one spot
     retouch_remove {formids}    -> deletes spots, as one history item
     module_add {operation, instance, copy}
                                 -> a new instance after the given one ("new
                                    instance", or "duplicate" with copy)
     ai_denoise {imgid, strength, path}
                                 -> AI raw denoise (neural restore) as a
                                    background job: a DNG beside the photo,
                                    imported into its group; replies the job
     job_status {job} / job_list / job_cancel {job}
                                 -> background jobs; a "job" event goes to
                                    every client when one ends
     module_move {operation, instance, before | after: {operation, instance}}
                                 -> moves a module in the pipe, as dragging it
                                    in the darkroom (darktable's rules apply)
     curve_get / curve_set {operation, instance, channel, points, type}
                                 -> curves of rgbcurve, tonecurve, colorzones,
                                    basecurve: points [[x, y], ...] per channel
     image_duplicate {imgid, virgin, save}
                                 -> a duplicate (virtual copy) with the saved
                                    edit, or none (virgin)
     style_list {filter} / style_create {name, imgid, modules} /
     style_delete {name}         -> darktable's styles
     style_apply {name, imgids, save}
     history_paste {from, imgids, mode, modules, save}
                                 -> a style, or an image's saved edit, onto
                                    images in the library as the lighttable
                                    does; open ones saved first, reopened
     image_metadata / set_tags / set_metadata / set_location / tag_list
                                 -> tags, metadata fields and location, as
                                    darktable's tagging, metadata editor
                                    and geotagging set them
     mask_ai_encode              -> encodes the photo for AI masks (a job)
     picker_list / picker_apply {operation, instance, control, box | point}
                                 -> darktable's window, the darkroom's photo:
                                    a module's pickers and auto buttons,
                                    pressed as a click does (a job)
     module_remove {operation, instance}
                                 -> deletes an instance (not a module's last)
     module_rename {operation, instance, name}
                                 -> the instance's label ("" for darktable's)
     history_compress {truncate} -> compresses the history stack as the
                                    history panel does (truncate: only drops
                                    the items above history_end); saves first
     blend_get / blend_set {operation, instance, values}
                                 -> a module's blending in the darkroom's
                                    names and units: masks, blend_mode,
                                    reverse, opacity, blend_parameter,
                                    feathering_radius, blur_radius,
                                    brightness, contrast, details, combine,
                                    feathering_guide, parametric ranges per
                                    channel ("g_in": {range, inverted,
                                    boost}); all or none, one history item
     mask_add {operation, instance, shape, combine, inverted}
                                 -> a drawn shape (circle, ellipse, gradient,
                                    raw space) on a module's mask
     mask_ai {operation, instance, points: [{x, y, include}], combine,
              inverted}
                                 -> an AI object mask (SAM) around points on
                                    the photo, traced into path shapes on the
                                    module's mask (needs USE_AI, AI on)
     mask_list / mask_remove {operation, instance, formid}
                                 -> the module's shapes; take one out
     sample {width, height, zoom, center_x, center_y, uncropped, points,
             radius, boxes, bins}
                                 -> renders as render does and reads the
                                    output: sRGB and Lab at points (fractions
                                    of the image, averaged over radius
                                    pixels) and in boxes, R/G/B/luminance
                                    histograms, clipped fractions
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
             center_y, history_end}
                                 -> writes an sRGB JPEG fitted inside
                                    width x height to path; uncropped
                                    leaves crop's box out; zoom (1 = 100%)
                                    renders the width x height region
                                    around center_x/center_y (fractions)
                                    at that scale and replies its region;
                                    history_end renders an earlier step
                                    (before/after) without undoing
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
#include "common/utility.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/masks.h"
#include "develop/pixelpipe_hb.h"
#include "gui/presets.h"
#include "imageio/imageio_jpeg.h"
#include "imageio/imageio_module.h"
#include "common/datetime.h"
#include "common/variables.h"
#include "common/ras2vect.h"
#include "common/collection.h"
#include "common/exif.h"
#include "common/film.h"
#include "common/grouping.h"
#include "common/metadata.h"
#include "imageio/imageio_dng.h"
#include "control/jobs.h"
#include "bauhaus/bauhaus.h"
#include "common/color_picker.h"
#include "dtgtk/paint.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "libs/colorpicker.h"
#include "libs/lib.h"
#ifdef HAVE_AI
#include "common/ai/segmentation.h"
#include "common/ai/restore.h"
#include "common/ai/restore_raw_bayer.h"
#include "common/ai/restore_raw_linear.h"
#include "common/ai_models.h"
#endif
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
  // as if a module had been focused, which the darkroom always does before
  // an edit (imageop.c, dt_iop_request_focus): history items read from the
  // library carry focus_hash FALSE, so the first edit of a module starts a
  // new item instead of merging into a saved one (develop.c,
  // _dev_add_history_item_ext), which undo couldn't go back from
  s->dev->focus_hash = TRUE;

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
// in darktable's window, an API edit counts as working in the module: a
// darkroom just opened (or reloaded, dt_dev_reload_history_items) has no
// focus yet, and its first edit would merge into the top saved item
static void _gui_focus(void)
{
  if(_cur->gui && !_cur->dev->focus_hash) _cur->dev->focus_hash = TRUE;
}

static void _record(dt_iop_module_t *m, const gboolean enable)
{
  if(_cur->gui)
  {
    _gui_focus();
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
    _gui_focus();
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

// ---- coordinates and retouch -----------------------------------------------
// drawn masks and retouch shapes live in the pipe's input, normalized: x
// times its width, y times its height, a circle's radius times the shorter
// side (develop/masks/circle.c). clients see the image as rendered: these
// map between the two through the pipe's distortions (lens, rotation,
// crop...), as the darkroom does for the mouse

// "raw": mask space; "image": fractions of the rendered image; "uncropped":
// fractions of crop's input (render uncropped)
static gboolean _coords(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  const char *from = params ? json_object_get_string_member_with_default(params, "from", "image") : "image";
  const char *to = params ? json_object_get_string_member_with_default(params, "to", "raw") : "raw";
  JsonArray *in = params && json_object_has_member(params, "points")
                  && JSON_NODE_HOLDS_ARRAY(json_object_get_member(params, "points"))
                    ? json_object_get_array_member(params, "points") : NULL;
  static const char *spaces[] = { "raw", "image", "uncropped", NULL };
  int fs = -1, ts = -1;
  for(int k = 0; spaces[k]; k++)
  {
    if(!g_strcmp0(from, spaces[k])) fs = k;
    if(!g_strcmp0(to, spaces[k])) ts = k;
  }
  if(fs < 0 || ts < 0 || !in)
  {
    *err = g_strdup("coords needs points [[x, y], ...] and from/to: raw, image or uncropped");
    return FALSE;
  }
  const guint n = json_array_get_length(in);
  float *pts = g_new0(float, 2 * MAX(n, 1));
  for(guint k = 0; k < n; k++)
  {
    JsonArray *p = JSON_NODE_HOLDS_ARRAY(json_array_get_element(in, k))
                   ? json_array_get_array_element(in, k) : NULL;
    if(!p || json_array_get_length(p) != 2)
    {
      g_free(pts);
      *err = g_strdup_printf("point %u: needs [x, y]", k);
      return FALSE;
    }
    pts[2 * k] = json_array_get_double_element(p, 0);
    pts[2 * k + 1] = json_array_get_double_element(p, 1);
  }
  _session_t *s = _pipe_session(err);
  if(!s)
  {
    g_free(pts);
    return FALSE;
  }
  _pipe_sync(s);
  // pixel sizes of the three spaces in this full-size pipe
  const dt_dev_pixelpipe_iop_t *cp = _piece(s, "crop");
  const double w[3] = { s->pipe.iwidth, s->pipe.processed_width, cp ? cp->buf_in.width : s->pipe.processed_width };
  const double h[3] = { s->pipe.iheight, s->pipe.processed_height, cp ? cp->buf_in.height : s->pipe.processed_height };
  const int crop_order = cp ? cp->module->iop_order : INT_MAX;
  for(guint k = 0; k < n; k++)
  {
    pts[2 * k] *= w[fs];
    pts[2 * k + 1] *= h[fs];
  }
  // to raw first, then out: uncropped stops before crop, image goes through
  if(fs == 1)
    dt_dev_distort_backtransform_plus(s->dev, &s->pipe, 0, DT_DEV_TRANSFORM_DIR_ALL, pts, n);
  else if(fs == 2)
    dt_dev_distort_backtransform_plus(s->dev, &s->pipe, crop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL, pts, n);
  if(ts == 1)
    dt_dev_distort_transform_plus(s->dev, &s->pipe, 0, DT_DEV_TRANSFORM_DIR_ALL, pts, n);
  else if(ts == 2)
    dt_dev_distort_transform_plus(s->dev, &s->pipe, crop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL, pts, n);
  json_builder_set_member_name(b, "points");
  json_builder_begin_array(b);
  for(guint k = 0; k < n; k++)
  {
    json_builder_begin_array(b);
    json_builder_add_double_value(b, pts[2 * k] / w[ts]);
    json_builder_add_double_value(b, pts[2 * k + 1] / h[ts]);
    json_builder_end_array(b);
  }
  json_builder_end_array(b);
  // a length in raw space is a fraction of this many raw pixels
  json_builder_set_member_name(b, "raw_width");
  json_builder_add_int_value(b, s->pipe.iwidth);
  json_builder_set_member_name(b, "raw_height");
  json_builder_add_int_value(b, s->pipe.iheight);
  g_free(pts);
  return TRUE;
}

// a field of retouch's rt_forms[k] (array of structs: introspection gives
// the first element's layout, dev-doc/introspection.md)
static void *_rt_form_field(dt_iop_module_t *m, const int k, const char *name,
                            dt_introspection_field_t **f)
{
  dt_introspection_field_t *forms = NULL;
  for(dt_introspection_field_t *i = m->so->get_introspection_linear();
      i && i->header.type != DT_INTROSPECTION_TYPE_NONE; i++)
    if(!g_strcmp0(i->header.name, "rt_forms")) { forms = i; break; }
  if(!forms) return NULL;
  dt_introspection_field_t *elem = NULL;
  void *p = dt_introspection_access_array(forms, (uint8_t *)m->params + forms->header.offset, k, &elem);
  return dt_introspection_get_child(elem, p, name, f);
}

static int _rt_forms_count(dt_iop_module_t *m)
{
  for(dt_introspection_field_t *i = m->so->get_introspection_linear();
      i && i->header.type != DT_INTROSPECTION_TYPE_NONE; i++)
    if(!g_strcmp0(i->header.name, "rt_forms")) return i->Array.count;
  return 0;
}

static int _enum_value(const dt_introspection_field_t *f, const char *name)
{
  for(const dt_introspection_type_enum_tuple_t *e = f->Enum.values; e && e->name; e++)
    if(!g_strcmp0(e->name, name)) return e->value;
  return -1;
}

// a retouch enum value by its short name: "blur" in "DT_IOP_RETOUCH_" is
// DT_IOP_RETOUCH_BLUR
static int _rt_enum(dt_iop_module_t *m, const char *field, const char *prefix, const char *name)
{
  dt_introspection_field_t *f = _field(m, field);
  gchar *up = g_ascii_strup(name, -1);
  // the full names retouch_list shows work too
  gchar *full = g_str_has_prefix(up, prefix) ? g_strdup(up) : g_strconcat(prefix, up, NULL);
  const int v = f ? _enum_value(f, full) : -1;
  g_free(up);
  g_free(full);
  return v;
}

// retouch's shapes: type, circle center/radius/source in raw space, and the
// algorithm retouch applies to each
static gboolean _retouch_list(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  dt_iop_module_t *m = dt_iop_get_module_by_op_priority(_cur->dev->iop, "retouch", 0);
  if(!m)
  {
    *err = g_strdup("this darktable has no retouch module");
    return FALSE;
  }
  json_builder_set_member_name(b, "enabled");
  json_builder_add_boolean_value(b, m->enabled);
  json_builder_set_member_name(b, "spots");
  json_builder_begin_array(b);
  dt_masks_form_t *grp = dt_masks_get_from_id(_cur->dev, m->blend_params->mask_id);
  const int count = _rt_forms_count(m);
  for(GList *l = grp && (grp->type & DT_MASKS_GROUP) ? grp->points : NULL; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *gp = l->data;
    const dt_masks_form_t *form = dt_masks_get_from_id(_cur->dev, gp->formid);
    if(!form) continue;
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "formid");
    json_builder_add_int_value(b, gp->formid);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, form->type & DT_MASKS_CIRCLE ? "circle" : form->type & DT_MASKS_ELLIPSE
                                     ? "ellipse" : form->type & DT_MASKS_PATH ? "path" : form->type & DT_MASKS_BRUSH
                                     ? "brush" : "other");
    json_builder_set_member_name(b, "active");
    json_builder_add_boolean_value(b, (gp->state & DT_MASKS_STATE_USE) != 0);
    if((form->type & DT_MASKS_CIRCLE) && form->points)
    {
      const dt_masks_point_circle_t *c = form->points->data;
      json_builder_set_member_name(b, "x");
      json_builder_add_double_value(b, c->center[0]);
      json_builder_set_member_name(b, "y");
      json_builder_add_double_value(b, c->center[1]);
      json_builder_set_member_name(b, "r");
      json_builder_add_double_value(b, c->radius);
      // the feather around it, in the same unit as r
      json_builder_set_member_name(b, "border");
      json_builder_add_double_value(b, c->border);
    }
    json_builder_set_member_name(b, "sx");
    json_builder_add_double_value(b, form->source[0]);
    json_builder_set_member_name(b, "sy");
    json_builder_add_double_value(b, form->source[1]);
    for(int k = 0; k < count; k++)
    {
      dt_introspection_field_t *f = NULL;
      const dt_mask_id_t *id = _rt_form_field(m, k, "formid", &f);
      if(!id || *id != gp->formid) continue;
      const int *algo = _rt_form_field(m, k, "algorithm", &f);
      if(algo)
      {
        json_builder_set_member_name(b, "algorithm");
        _add_field_value(b, f, (const uint8_t *)algo - f->header.offset);
        // the tool's options, as retouch shows them for a selected spot
        static const char *blur[] = { "blur_type", "blur_radius", NULL };
        static const char *fill[] = { "fill_mode", "fill_color", "fill_brightness", NULL };
        const char **opts = *algo == _rt_enum(m, "algorithm", "DT_IOP_RETOUCH_", "blur") ? blur
                            : *algo == _rt_enum(m, "algorithm", "DT_IOP_RETOUCH_", "fill") ? fill : NULL;
        for(; opts && *opts; opts++)
        {
          const void *p = _rt_form_field(m, k, *opts, &f);
          if(!p) continue;
          json_builder_set_member_name(b, *opts);
          if(f->header.type == DT_INTROSPECTION_TYPE_ARRAY)
          {
            json_builder_begin_array(b);
            for(int c = 0; c < f->Array.count; c++) json_builder_add_double_value(b, ((const float *)p)[c]);
            json_builder_end_array(b);
          }
          else
            _add_field_value(b, f, (const uint8_t *)p - f->header.offset);
        }
      }
      break;
    }
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  return TRUE;
}

// retouch's spots: circles in its mask group, and per spot in rt_forms[]
// (in group order, retouch.c rt_resynch_params) the tool and its options
static dt_introspection_field_t *_rt_forms_field(dt_iop_module_t *m)
{
  for(dt_introspection_field_t *i = m->so->get_introspection_linear();
      i && i->header.type != DT_INTROSPECTION_TYPE_NONE; i++)
    if(!g_strcmp0(i->header.name, "rt_forms")) return i;
  return NULL;
}

static int _rt_index(dt_iop_module_t *m, const dt_mask_id_t formid)
{
  const int count = _rt_forms_count(m);
  for(int k = 0; k < count; k++)
  {
    dt_introspection_field_t *f = NULL;
    const dt_mask_id_t *id = _rt_form_field(m, k, "formid", &f);
    if(id && *id == formid) return k;
  }
  return -1;
}

static gboolean _rt_cloning(dt_iop_module_t *m, const int algorithm)
{
  return algorithm == _rt_enum(m, "algorithm", "DT_IOP_RETOUCH_", "clone")
         || algorithm == _rt_enum(m, "algorithm", "DT_IOP_RETOUCH_", "heal");
}

// rt_resynch_params for any session: entries follow the group's shapes,
// new shapes get the current scale and distortion mode 2. retouch's own
// version runs on darktable.develop, which is only the darkroom's image
static void _rt_resync(dt_iop_module_t *m)
{
  dt_introspection_field_t *forms = _rt_forms_field(m);
  const int count = forms ? forms->Array.count : 0;
  if(!count) return;
  const size_t size = forms->header.size / count;
  uint8_t *base = (uint8_t *)m->params + forms->header.offset;
  uint8_t *out = g_malloc0(forms->header.size);
  gboolean *fresh = g_new0(gboolean, count);
  dt_mask_id_t *ids = g_new0(dt_mask_id_t, count);
  int n = 0;
  dt_masks_form_t *grp = dt_masks_get_from_id(_cur->dev, m->blend_params->mask_id);
  for(GList *l = grp && (grp->type & DT_MASKS_GROUP) ? grp->points : NULL; l && n < count; l = g_list_next(l))
  {
    const dt_masks_point_group_t *gp = l->data;
    if(!dt_masks_get_from_id(_cur->dev, gp->formid)) continue;
    const int k = _rt_index(m, gp->formid);
    if(k >= 0)
      memcpy(out + n * size, base + k * size, size);
    else
      fresh[n] = TRUE;
    ids[n++] = gp->formid;
  }
  memcpy(base, out, forms->header.size);
  const int *curr = m->get_p(m->params, "curr_scale");
  const int *algo = m->get_p(m->params, "algorithm");
  for(int k = 0; k < n; k++)
  {
    if(!fresh[k]) continue;
    dt_introspection_field_t *f = NULL;
    dt_mask_id_t *id = _rt_form_field(m, k, "formid", &f);
    if(id) *id = ids[k];
    int *scale = _rt_form_field(m, k, "scale", &f);
    if(scale && curr) *scale = *curr;
    int *a = _rt_form_field(m, k, "algorithm", &f);
    if(a && algo) *a = *algo;
    int *mode = _rt_form_field(m, k, "distort_mode", &f);
    if(mode) *mode = 2;
  }
  g_free(out);
  g_free(fresh);
  g_free(ids);
}

// a spot from a request: where (circle, raw space) and the tool with its
// options; has[] says which were given
enum { RT_X, RT_Y, RT_R, RT_SX, RT_SY, RT_ALGO, RT_BLUR_TYPE, RT_BLUR_RADIUS, RT_FILL_MODE, RT_FILL_COLOR,
       RT_FILL_BRIGHTNESS, RT_N };

typedef struct _rt_spot_t
{
  gboolean has[RT_N];
  float v[5]; // x, y, r, sx, sy
  int algorithm, blur_type, fill_mode;
  float blur_radius, fill_color[3], fill_brightness;
} _rt_spot_t;

static gboolean _rt_number(JsonNode *x, float *out)
{
  if(!x || !JSON_NODE_HOLDS_VALUE(x)
     || (json_node_get_value_type(x) != G_TYPE_DOUBLE && json_node_get_value_type(x) != G_TYPE_INT64))
    return FALSE;
  *out = json_node_get_double(x);
  return TRUE;
}

static gboolean _rt_choice(dt_iop_module_t *m, JsonObject *o, const char *key, const char *field,
                           const char *prefix, const char *names, int *v, gboolean *has, gchar **err)
{
  if(!json_object_has_member(o, key)) return TRUE;
  JsonNode *x = json_object_get_member(o, key);
  const char *s = JSON_NODE_HOLDS_VALUE(x) && json_node_get_value_type(x) == G_TYPE_STRING
                  ? json_node_get_string(x) : "";
  *v = _rt_enum(m, field, prefix, s);
  // DT_IOP_RETOUCH_NONE is an empty slot, not a tool
  if(*v < 0 || (*v == 0 && !g_strcmp0(key, "algorithm")))
  {
    *err = g_strdup_printf("%s is one of %s", key, names);
    return FALSE;
  }
  *has = TRUE;
  return TRUE;
}

static gboolean _rt_parse(dt_iop_module_t *m, JsonObject *o, _rt_spot_t *s, gchar **err)
{
  static const char *keys[5] = { "x", "y", "r", "sx", "sy" };
  memset(s, 0, sizeof(*s));
  if(!o)
  {
    *err = g_strdup("a spot is an object");
    return FALSE;
  }
  for(int k = 0; k < 5; k++)
  {
    if(!json_object_has_member(o, keys[k])) continue;
    if(!_rt_number(json_object_get_member(o, keys[k]), &s->v[k])
       || (k == RT_R ? !(s->v[k] > 0.f && s->v[k] < 0.5f) : !(s->v[k] >= 0.f && s->v[k] <= 1.f)))
    {
      *err = g_strdup("x, y, sx, sy are in 0..1 and r in 0..0.5 (raw space, see coords)");
      return FALSE;
    }
    s->has[k] = TRUE;
  }
  if(!_rt_choice(m, o, "algorithm", "algorithm", "DT_IOP_RETOUCH_", "clone, heal, blur, fill",
                 &s->algorithm, &s->has[RT_ALGO], err)
     || !_rt_choice(m, o, "blur_type", "blur_type", "DT_IOP_RETOUCH_BLUR_", "gaussian, bilateral",
                    &s->blur_type, &s->has[RT_BLUR_TYPE], err)
     || !_rt_choice(m, o, "fill_mode", "fill_mode", "DT_IOP_RETOUCH_FILL_", "erase, color",
                    &s->fill_mode, &s->has[RT_FILL_MODE], err))
    return FALSE;
  if(json_object_has_member(o, "blur_radius"))
  {
    if(!_rt_number(json_object_get_member(o, "blur_radius"), &s->blur_radius)
       || s->blur_radius < 0.1f || s->blur_radius > 200.f)
    {
      *err = g_strdup("blur_radius is 0.1..200");
      return FALSE;
    }
    s->has[RT_BLUR_RADIUS] = TRUE;
  }
  if(json_object_has_member(o, "fill_brightness"))
  {
    if(!_rt_number(json_object_get_member(o, "fill_brightness"), &s->fill_brightness)
       || fabsf(s->fill_brightness) > 1.f)
    {
      *err = g_strdup("fill_brightness is -1..1");
      return FALSE;
    }
    s->has[RT_FILL_BRIGHTNESS] = TRUE;
  }
  if(json_object_has_member(o, "fill_color"))
  {
    JsonNode *n = json_object_get_member(o, "fill_color");
    JsonArray *a = JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : NULL;
    gboolean ok = a && json_array_get_length(a) == 3;
    for(int c = 0; ok && c < 3; c++)
      ok = _rt_number(json_array_get_element(a, c), &s->fill_color[c])
           && s->fill_color[c] >= 0.f && s->fill_color[c] <= 1.f;
    if(!ok)
    {
      *err = g_strdup("fill_color is [r, g, b] in 0..1, the module's working RGB");
      return FALSE;
    }
    s->has[RT_FILL_COLOR] = TRUE;
  }
  return TRUE;
}

// options that don't belong to the spot's tool are refused, as are a
// source for blur and fill (they have none)
static gboolean _rt_check(dt_iop_module_t *m, const _rt_spot_t *s, const int algorithm, gchar **err)
{
  const gboolean blur = algorithm == _rt_enum(m, "algorithm", "DT_IOP_RETOUCH_", "blur");
  const gboolean fill = algorithm == _rt_enum(m, "algorithm", "DT_IOP_RETOUCH_", "fill");
  if(!_rt_cloning(m, algorithm) && (s->has[RT_SX] || s->has[RT_SY]))
    *err = g_strdup("blur and fill spots have no source (sx, sy)");
  else if(!blur && (s->has[RT_BLUR_TYPE] || s->has[RT_BLUR_RADIUS]))
    *err = g_strdup("blur_type and blur_radius are for blur spots");
  else if(!fill && (s->has[RT_FILL_MODE] || s->has[RT_FILL_COLOR] || s->has[RT_FILL_BRIGHTNESS]))
    *err = g_strdup("fill_mode, fill_color and fill_brightness are for fill spots");
  return *err == NULL;
}

// a spot's tool and options into rt_forms[k]. a new spot takes options
// not given from the module's current ones, as drawing it in the darkroom
// does (rt_resynch_params)
static void _rt_apply(dt_iop_module_t *m, const int k, const _rt_spot_t *s, const gboolean fresh)
{
  dt_introspection_field_t *f = NULL;
  int *algo = _rt_form_field(m, k, "algorithm", &f);
  if(algo && s->has[RT_ALGO]) *algo = s->algorithm;
  if(!algo) return;
  if(*algo == _rt_enum(m, "algorithm", "DT_IOP_RETOUCH_", "blur"))
  {
    int *t = _rt_form_field(m, k, "blur_type", &f);
    float *r = _rt_form_field(m, k, "blur_radius", &f);
    const int *pt = m->get_p(m->params, "blur_type");
    const float *pr = m->get_p(m->params, "blur_radius");
    if(t) *t = s->has[RT_BLUR_TYPE] ? s->blur_type : fresh && pt ? *pt : *t;
    if(r) *r = s->has[RT_BLUR_RADIUS] ? s->blur_radius : fresh && pr ? *pr : *r;
  }
  else if(*algo == _rt_enum(m, "algorithm", "DT_IOP_RETOUCH_", "fill"))
  {
    int *mode = _rt_form_field(m, k, "fill_mode", &f);
    float *color = _rt_form_field(m, k, "fill_color", &f);
    float *bright = _rt_form_field(m, k, "fill_brightness", &f);
    const int *pm = m->get_p(m->params, "fill_mode");
    const float *pc = m->get_p(m->params, "fill_color");
    const float *pb = m->get_p(m->params, "fill_brightness");
    if(mode) *mode = s->has[RT_FILL_MODE] ? s->fill_mode : fresh && pm ? *pm : *mode;
    for(int c = 0; color && c < 3; c++)
      color[c] = s->has[RT_FILL_COLOR] ? s->fill_color[c] : fresh && pc ? pc[c] : color[c];
    if(bright) *bright = s->has[RT_FILL_BRIGHTNESS] ? s->fill_brightness : fresh && pb ? *pb : *bright;
  }
}

static dt_iop_module_t *_rt_module(gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return NULL;
  }
  dt_iop_module_t *m = dt_iop_get_module_by_op_priority(_cur->dev->iop, "retouch", 0);
  if(!m) *err = g_strdup("this darktable has no retouch module");
  return m;
}

// recording a retouch change: in the window through the darkroom's history
// (retouch then lists the shapes in its own view, dt_masks_iop_update),
// elsewhere as a history item carrying the forms; no_image, as _record
static void _rt_begin(void)
{
  if(!_cur->gui) return;
  _gui_focus();
  _api_editing = TRUE;
  dt_dev_undo_start_record(_cur->dev);
}

static void _rt_end(dt_iop_module_t *m)
{
  if(_cur->gui)
  {
    dt_masks_iop_update(m);
    dt_dev_add_masks_history_item(_cur->dev, m, TRUE);
    dt_dev_undo_end_record(_cur->dev);
    _api_editing = FALSE;
    _gui_changed_by_api();
  }
  else
  {
    m->enabled = TRUE;
    dt_dev_add_masks_history_item_ext(_cur->dev, m, TRUE, TRUE);
    _cur->pipe_changed = TRUE;
    _cur->dirty = TRUE;
  }
}

// spots in retouch, as its circle tool draws them: a circle per spot with
// the user's circle feather, cloning or healing from a source, or blurring
// or filling, all in one history step
static gboolean _retouch_add(JsonObject *params, const char *algorithm, JsonBuilder *b, gchar **err)
{
  dt_iop_module_t *m = _rt_module(err);
  if(!m) return FALSE;
  dt_develop_t *dev = _cur->dev;
  JsonArray *spots = params && json_object_has_member(params, "spots")
                     && JSON_NODE_HOLDS_ARRAY(json_object_get_member(params, "spots"))
                       ? json_object_get_array_member(params, "spots") : NULL;
  const guint n = spots ? json_array_get_length(spots) : 0;
  if(!n)
  {
    *err = g_strdup("needs spots [{x, y, r, algorithm, sx, sy, ...}] in raw space (coords)");
    return FALSE;
  }
  const int dflt = _rt_enum(m, "algorithm", "DT_IOP_RETOUCH_", algorithm);
  _rt_spot_t *s = g_new0(_rt_spot_t, n);
  for(guint i = 0; i < n; i++)
  {
    JsonNode *e = json_array_get_element(spots, i);
    gchar *e_err = NULL;
    if(_rt_parse(m, JSON_NODE_HOLDS_OBJECT(e) ? json_node_get_object(e) : NULL, &s[i], &e_err))
    {
      if(!s[i].has[RT_ALGO])
      {
        s[i].algorithm = dflt;
        s[i].has[RT_ALGO] = TRUE;
      }
      if(!s[i].has[RT_X] || !s[i].has[RT_Y] || !s[i].has[RT_R])
        e_err = g_strdup("needs x, y and r");
      else if(_rt_cloning(m, s[i].algorithm) && (!s[i].has[RT_SX] || !s[i].has[RT_SY]))
        e_err = g_strdup("clone and heal spots need a source (sx, sy)");
      else
        _rt_check(m, &s[i], s[i].algorithm, &e_err);
    }
    if(e_err)
    {
      *err = g_strdup_printf("spot %u: %s", i, e_err);
      g_free(e_err);
      g_free(s);
      return FALSE;
    }
  }
  const int count = _rt_forms_count(m);
  int used = 0;
  for(int k = 0; k < count; k++)
  {
    dt_introspection_field_t *f = NULL;
    const dt_mask_id_t *id = _rt_form_field(m, k, "formid", &f);
    if(id && *id) used++;
  }
  if(used + (int)n > count)
  {
    g_free(s);
    *err = g_strdup_printf("retouch holds at most %d shapes; it has %d", count, used);
    return FALSE;
  }

  const float border = dt_conf_get_float("plugins/darkroom/spots/circle_border");
  _rt_begin();
  // in the window, a history item of its own, so the spots don't merge into
  // a retouch step on top of the history (as heal_spots does)
  if(_cur->gui) dt_dev_add_new_history_item(dev, m, TRUE);
  JsonBuilder *ids = json_builder_new();
  json_builder_begin_array(ids);
  dt_mask_id_t *formids = g_new0(dt_mask_id_t, n);
  for(guint i = 0; i < n; i++)
  {
    const gboolean cloning = _rt_cloning(m, s[i].algorithm);
    dt_masks_form_t *form = dt_masks_create(DT_MASKS_CIRCLE | (cloning ? DT_MASKS_CLONE : DT_MASKS_NON_CLONE));
    dt_masks_point_circle_t *circle = malloc(sizeof(dt_masks_point_circle_t));
    circle->center[0] = s[i].v[RT_X];
    circle->center[1] = s[i].v[RT_Y];
    circle->radius = s[i].v[RT_R];
    circle->border = border;
    form->points = g_list_append(form->points, circle);
    if(cloning)
    {
      form->source[0] = s[i].v[RT_SX];
      form->source[1] = s[i].v[RT_SY];
    }
    dt_masks_gui_form_save_creation(dev, m, form, NULL);
    formids[i] = form->formid;
    json_builder_add_int_value(ids, form->formid);
  }
  json_builder_end_array(ids);
  _rt_resync(m);
  for(guint i = 0; i < n; i++)
  {
    const int k = _rt_index(m, formids[i]);
    if(k >= 0) _rt_apply(m, k, &s[i], TRUE);
  }
  g_free(formids);
  g_free(s);
  _rt_end(m);
  json_builder_set_member_name(b, "added");
  json_builder_add_int_value(b, n);
  json_builder_set_member_name(b, "formids");
  json_builder_add_value(b, json_builder_get_root(ids));
  g_object_unref(ids);
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, dev->history_end);
  return TRUE;
}

static dt_masks_form_t *_rt_spot(dt_iop_module_t *m, const dt_mask_id_t formid, GList **link, gchar **err)
{
  dt_masks_form_t *grp = dt_masks_get_from_id(_cur->dev, m->blend_params->mask_id);
  for(GList *l = grp && (grp->type & DT_MASKS_GROUP) ? grp->points : NULL; l; l = g_list_next(l))
    if(((dt_masks_point_group_t *)l->data)->formid == formid)
    {
      if(link) *link = l;
      dt_masks_form_t *form = dt_masks_get_from_id(_cur->dev, formid);
      if(form) return form;
    }
  *err = g_strdup_printf("retouch has no spot %d (retouch_list)", formid);
  return NULL;
}

// moves, resizes or changes one spot: its circle, its source, its tool
// (clone and heal swap, as do blur and fill: the darkroom allows the same)
// and the tool's options
static gboolean _retouch_set(JsonObject *params, JsonBuilder *b, gchar **err)
{
  dt_iop_module_t *m = _rt_module(err);
  if(!m) return FALSE;
  const dt_mask_id_t formid = params ? json_object_get_int_member_with_default(params, "formid", 0) : 0;
  dt_masks_form_t *form = _rt_spot(m, formid, NULL, err);
  if(!form) return FALSE;
  const int k = _rt_index(m, formid);
  dt_introspection_field_t *f = NULL;
  const int *cur_algo = k >= 0 ? _rt_form_field(m, k, "algorithm", &f) : NULL;
  if(!cur_algo)
  {
    *err = g_strdup_printf("spot %d isn't listed in retouch's settings", formid);
    return FALSE;
  }
  _rt_spot_t s;
  if(!_rt_parse(m, params, &s, err)) return FALSE;
  const int algorithm = s.has[RT_ALGO] ? s.algorithm : *cur_algo;
  if(_rt_cloning(m, algorithm) != _rt_cloning(m, *cur_algo))
  {
    *err = g_strdup("a clone or heal spot can only become clone or heal, a blur or fill spot blur or fill");
    return FALSE;
  }
  if(!_rt_check(m, &s, algorithm, err)) return FALSE;
  if((s.has[RT_X] || s.has[RT_Y] || s.has[RT_R]) && !((form->type & DT_MASKS_CIRCLE) && form->points))
  {
    *err = g_strdup("only circle spots can be moved or resized here");
    return FALSE;
  }
  _rt_begin();
  if(form->type & DT_MASKS_CIRCLE && form->points)
  {
    dt_masks_point_circle_t *c = form->points->data;
    if(s.has[RT_X]) c->center[0] = s.v[RT_X];
    if(s.has[RT_Y]) c->center[1] = s.v[RT_Y];
    if(s.has[RT_R]) c->radius = s.v[RT_R];
  }
  if(s.has[RT_SX]) form->source[0] = s.v[RT_SX];
  if(s.has[RT_SY]) form->source[1] = s.v[RT_SY];
  _rt_apply(m, k, &s, FALSE);
  _rt_end(m);
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, _cur->dev->history_end);
  return TRUE;
}

// removes spots as deleting them in the darkroom does: out of retouch's
// group and the image's forms (dt_masks_form_remove), all in one step
static gboolean _retouch_remove(JsonObject *params, JsonBuilder *b, gchar **err)
{
  dt_iop_module_t *m = _rt_module(err);
  if(!m) return FALSE;
  JsonArray *a = params && json_object_has_member(params, "formids")
                 && JSON_NODE_HOLDS_ARRAY(json_object_get_member(params, "formids"))
                   ? json_object_get_array_member(params, "formids") : NULL;
  const guint n = a ? json_array_get_length(a) : 0;
  if(!n)
  {
    *err = g_strdup("needs formids [...] (retouch_list)");
    return FALSE;
  }
  for(guint i = 0; i < n; i++)
    if(!_rt_spot(m, json_array_get_int_element(a, i), NULL, err)) return FALSE;
  dt_develop_t *dev = _cur->dev;
  dt_masks_form_t *grp = dt_masks_get_from_id(dev, m->blend_params->mask_id);
  _rt_begin();
  for(guint i = 0; i < n; i++)
  {
    GList *link = NULL;
    gchar *e = NULL;
    dt_masks_form_t *form = _rt_spot(m, json_array_get_int_element(a, i), &link, &e);
    g_free(e);
    if(!form) continue; // listed twice
    free(link->data);
    grp->points = g_list_delete_link(grp->points, link);
    // not freed, as dt_masks_form_remove: a form can be the darkroom's
    // visible or selected one
    dev->forms = g_list_remove(dev->forms, form);
  }
  _rt_resync(m);
  _rt_end(m);
  json_builder_set_member_name(b, "removed");
  json_builder_add_int_value(b, n);
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, dev->history_end);
  return TRUE;
}

// ---- blending and masks -----------------------------------------------------
// a module's blending: how its output mixes with its input (blend mode,
// opacity) and where (drawn shapes, parametric ranges per channel), as the
// blending section under each module in the darkroom. settings use the
// darkroom's names and units (develop/blend_gui.c)

// the name of an enum value from darktable's tables, without a "context|"
static const char *_enum_label(const dt_introspection_type_enum_tuple_t *t, const int v)
{
  for(; t && t->name; t++)
    if(t->value == v)
    {
      const char *bar = strchr(t->name, '|');
      return bar ? bar + 1 : t->name;
    }
  return NULL;
}

static gboolean _enum_parse(const dt_introspection_type_enum_tuple_t *t, const char *name, int *v)
{
  for(; t && t->name; t++)
  {
    const char *bar = strchr(t->name, '|');
    if(!g_ascii_strcasecmp(bar ? bar + 1 : t->name, name))
    {
      *v = t->value;
      return TRUE;
    }
  }
  return FALSE;
}

// the masking choices of the blending section's tabs
static const struct { const char *name; uint32_t mode; } _mask_modes[] = {
  { "off", DEVELOP_MASK_DISABLED },
  { "uniform", DEVELOP_MASK_ENABLED },
  { "drawn", DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK },
  { "parametric", DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL },
  { "drawn & parametric", DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL },
  { "raster", DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER },
  { NULL, 0 } };

// the float settings: name, offset, the slider's range
static const struct { const char *name; size_t offset; float lo, hi; } _blend_floats[] = {
  { "opacity", G_STRUCT_OFFSET(dt_develop_blend_params_t, opacity), 0.f, 100.f },
  { "blend_parameter", G_STRUCT_OFFSET(dt_develop_blend_params_t, blend_parameter), -18.f, 18.f },
  { "feathering_radius", G_STRUCT_OFFSET(dt_develop_blend_params_t, feathering_radius), 0.f, 250.f },
  { "blur_radius", G_STRUCT_OFFSET(dt_develop_blend_params_t, blur_radius), 0.f, 100.f },
  { "brightness", G_STRUCT_OFFSET(dt_develop_blend_params_t, brightness), -1.f, 1.f },
  { "contrast", G_STRUCT_OFFSET(dt_develop_blend_params_t, contrast), -1.f, 1.f },
  { "details", G_STRUCT_OFFSET(dt_develop_blend_params_t, details), -1.f, 1.f },
  { NULL, 0, 0.f, 0.f } };

// parametric channels of each blending color space, as the blending tabs
// list them (blend_gui.c: Lab_channels, rgb_channels, rgbj_channels); kind
// is how the darkroom shows a value: percent, Lab a/b, hue in degrees.
// boost_offset: the boost slider shows the stored boost minus this
enum { _PCT, _AB, _HUE };
typedef struct _channel_t { const char *name; int index; int kind; gboolean boost; float boost_offset; } _channel_t;
static const _channel_t _lab_channels[] = {
  { "L", DEVELOP_BLENDIF_L_in, _PCT, TRUE, 0.f }, { "a", DEVELOP_BLENDIF_A_in, _AB, TRUE, 0.f },
  { "b", DEVELOP_BLENDIF_B_in, _AB, TRUE, 0.f }, { "C", DEVELOP_BLENDIF_C_in, _PCT, TRUE, 0.f },
  { "h", DEVELOP_BLENDIF_h_in, _HUE, FALSE, 0.f }, { NULL } };
static const _channel_t _rgb_channels[] = {
  { "g", DEVELOP_BLENDIF_GRAY_in, _PCT, TRUE, 0.f }, { "R", DEVELOP_BLENDIF_RED_in, _PCT, TRUE, 0.f },
  { "G", DEVELOP_BLENDIF_GREEN_in, _PCT, TRUE, 0.f }, { "B", DEVELOP_BLENDIF_BLUE_in, _PCT, TRUE, 0.f },
  { "H", DEVELOP_BLENDIF_H_in, _HUE, FALSE, 0.f }, { "S", DEVELOP_BLENDIF_S_in, _PCT, FALSE, 0.f },
  { "l", DEVELOP_BLENDIF_l_in, _PCT, FALSE, 0.f }, { NULL } };
static const _channel_t _rgbj_channels[] = {
  { "g", DEVELOP_BLENDIF_GRAY_in, _PCT, TRUE, 0.f }, { "R", DEVELOP_BLENDIF_RED_in, _PCT, TRUE, 0.f },
  { "G", DEVELOP_BLENDIF_GREEN_in, _PCT, TRUE, 0.f }, { "B", DEVELOP_BLENDIF_BLUE_in, _PCT, TRUE, 0.f },
  { "Jz", DEVELOP_BLENDIF_Jz_in, _PCT, TRUE, -6.64385619f }, { "Cz", DEVELOP_BLENDIF_Cz_in, _PCT, TRUE, -6.64385619f },
  { "hz", DEVELOP_BLENDIF_hz_in, _HUE, FALSE, 0.f }, { NULL } };

static const _channel_t *_channels_of(const dt_develop_blend_params_t *bp)
{
  switch(bp->blend_cst)
  {
    case DEVELOP_BLEND_CS_LAB: return _lab_channels;
    case DEVELOP_BLEND_CS_RGB_DISPLAY: return _rgb_channels;
    case DEVELOP_BLEND_CS_RGB_SCENE: return _rgbj_channels;
    default: return NULL;
  }
}

static double _to_display(const _channel_t *c, const double v, const double boost)
{
  return c->kind == _HUE ? v * 360.0 : c->kind == _AB ? (v * 256.0 - 128.0) * boost : v * boost * 100.0;
}

static double _from_display(const _channel_t *c, const double d, const double boost)
{
  return c->kind == _HUE ? d / 360.0 : c->kind == _AB ? (d / boost + 128.0) / 256.0 : d / boost / 100.0;
}

static dt_iop_module_t *_blend_module(JsonObject *params, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return NULL;
  }
  dt_iop_module_t *m = _find_module(params, err);
  if(!m) return NULL;
  if(!(m->flags() & IOP_FLAGS_SUPPORTS_BLENDING) || !m->blend_params || !_channels_of(m->blend_params))
  {
    *err = g_strdup_printf("module '%s' has no blending", m->op);
    return NULL;
  }
  return m;
}

static void _add_blend(JsonBuilder *b, const dt_iop_module_t *m)
{
  const dt_develop_blend_params_t *bp = m->blend_params;
  json_builder_set_member_name(b, "colorspace");
  json_builder_add_string_value(b, _enum_label(dt_develop_blend_colorspace_names, bp->blend_cst));
  json_builder_set_member_name(b, "masks");
  const char *mode = "other";
  for(int k = 0; _mask_modes[k].name; k++)
    if(_mask_modes[k].mode == (bp->mask_mode & (DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK
                                                | DEVELOP_MASK_CONDITIONAL | DEVELOP_MASK_RASTER)))
      mode = _mask_modes[k].name;
  json_builder_add_string_value(b, mode);
  json_builder_set_member_name(b, "blend_mode");
  const char *bm = _enum_label(dt_develop_blend_mode_names, bp->blend_mode & DEVELOP_BLEND_MODE_MASK);
  json_builder_add_string_value(b, bm ? bm : "other");
  json_builder_set_member_name(b, "reverse");
  json_builder_add_boolean_value(b, (bp->blend_mode & DEVELOP_BLEND_REVERSE) != 0);
  for(int k = 0; _blend_floats[k].name; k++)
  {
    json_builder_set_member_name(b, _blend_floats[k].name);
    json_builder_add_double_value(b, *(const float *)((const uint8_t *)bp + _blend_floats[k].offset));
  }
  json_builder_set_member_name(b, "combine");
  const char *cm = _enum_label(dt_develop_combine_masks_names,
                               bp->mask_combine & (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL));
  json_builder_add_string_value(b, cm ? cm : "other");
  json_builder_set_member_name(b, "feathering_guide");
  const char *fg = _enum_label(dt_develop_feathering_guide_names, bp->feathering_guide);
  json_builder_add_string_value(b, fg ? fg : "other");
  json_builder_set_member_name(b, "drawn_shapes");
  dt_masks_form_t *grp = dt_masks_get_from_id(_cur->dev, bp->mask_id);
  json_builder_add_int_value(b, grp && (grp->type & DT_MASKS_GROUP) ? g_list_length(grp->points) : 0);

  // parametric ranges: [low end, low full, high full, high end] per channel
  // in the darkroom's units, for the module's input ("_in") and output
  // ("_out"); only channels that are on
  json_builder_set_member_name(b, "parametric");
  json_builder_begin_object(b);
  for(const _channel_t *c = _channels_of(bp); c->name; c++)
    for(int out = 0; out < 2; out++)
    {
      const int idx = c->index + 4 * out;
      if(!(bp->blendif & (1u << idx))) continue;
      gchar *key = g_strdup_printf("%s_%s", c->name, out ? "out" : "in");
      const double boost = exp2(bp->blendif_boost_factors[idx]);
      json_builder_set_member_name(b, key);
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "range");
      json_builder_begin_array(b);
      for(int k = 0; k < 4; k++)
        json_builder_add_double_value(b, round(_to_display(c, bp->blendif_parameters[4 * idx + k], boost) * 1e3) / 1e3);
      json_builder_end_array(b);
      json_builder_set_member_name(b, "inverted");
      json_builder_add_boolean_value(b, (bp->blendif & (1u << (idx + 16))) != 0);
      if(c->boost)
      {
        json_builder_set_member_name(b, "boost");
        json_builder_add_double_value(b, bp->blendif_boost_factors[idx] - c->boost_offset);
      }
      json_builder_end_object(b);
      g_free(key);
    }
  json_builder_end_object(b);
  json_builder_set_member_name(b, "channels");
  json_builder_begin_array(b);
  for(const _channel_t *c = _channels_of(bp); c->name; c++) json_builder_add_string_value(b, c->name);
  json_builder_end_array(b);
  json_builder_set_member_name(b, "raster_source");
  if(bp->raster_mask_source[0])
  {
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "operation");
    json_builder_add_string_value(b, bp->raster_mask_source);
    json_builder_set_member_name(b, "instance");
    json_builder_add_int_value(b, bp->raster_mask_instance);
    json_builder_end_object(b);
  }
  else
    json_builder_add_null_value(b);
  json_builder_set_member_name(b, "raster_inverted");
  json_builder_add_boolean_value(b, bp->raster_mask_invert);
}

static gboolean _blend_get(JsonObject *params, JsonBuilder *b, gchar **err)
{
  dt_iop_module_t *m = _blend_module(params, err);
  if(!m) return FALSE;
  json_builder_set_member_name(b, "operation");
  json_builder_add_string_value(b, m->op);
  json_builder_set_member_name(b, "instance");
  json_builder_add_int_value(b, m->multi_priority);
  _add_blend(b, m);
  return TRUE;
}

// change blending settings, all or none: masks, blend_mode, reverse, the
// float settings, combine, feathering_guide, and parametric: {"g_in":
// {"range": [4 values], "inverted", "boost"} or null to switch it off}.
// parametric ranges switch parametric masking on, as the darkroom's tab does
static gboolean _blend_set(JsonObject *params, JsonBuilder *b, gchar **err)
{
  dt_iop_module_t *m = _blend_module(params, err);
  if(!m) return FALSE;
  JsonObject *values = params && json_object_has_member(params, "values")
                       && JSON_NODE_HOLDS_OBJECT(json_object_get_member(params, "values"))
                         ? json_object_get_object_member(params, "values") : NULL;
  if(!values || !json_object_get_size(values))
  {
    *err = g_strdup("blend_set needs values {setting: value}");
    return FALSE;
  }
  dt_develop_blend_params_t bp = *m->blend_params;
  gboolean parametric = FALSE, raster = FALSE, ok = TRUE;
  GList *names = json_object_get_members(values);
  for(GList *n = names; n && ok; n = g_list_next(n))
  {
    const char *name = n->data;
    JsonNode *v = json_object_get_member(values, name);
    const gboolean is_str = JSON_NODE_HOLDS_VALUE(v) && json_node_get_value_type(v) == G_TYPE_STRING;
    const gboolean is_num = JSON_NODE_HOLDS_VALUE(v) && (json_node_get_value_type(v) == G_TYPE_DOUBLE
                                                        || json_node_get_value_type(v) == G_TYPE_INT64);
    int e = 0, f = -1;
    for(int k = 0; _blend_floats[k].name; k++)
      if(!g_strcmp0(name, _blend_floats[k].name)) f = k;
    if(f >= 0)
    {
      const double x = is_num ? json_node_get_double(v) : NAN;
      if(!(x >= _blend_floats[f].lo && x <= _blend_floats[f].hi))
      {
        *err = g_strdup_printf("'%s': a number in %g..%g", name, _blend_floats[f].lo, _blend_floats[f].hi);
        ok = FALSE;
      }
      else
        *(float *)((uint8_t *)&bp + _blend_floats[f].offset) = x;
    }
    else if(!g_strcmp0(name, "masks"))
    {
      int k = 0;
      while(_mask_modes[k].name && !(is_str && !g_ascii_strcasecmp(json_node_get_string(v), _mask_modes[k].name))) k++;
      if(!_mask_modes[k].name)
      {
        *err = g_strdup("'masks': off, uniform, drawn, parametric, drawn & parametric or raster");
        ok = FALSE;
      }
      else
        bp.mask_mode = (bp.mask_mode & ~(DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL
                                         | DEVELOP_MASK_RASTER)) | _mask_modes[k].mode;
    }
    else if(!g_strcmp0(name, "blend_mode"))
    {
      if(!(is_str && _enum_parse(dt_develop_blend_mode_names, json_node_get_string(v), &e)))
      {
        *err = g_strdup("'blend_mode': a blend mode name, e.g. normal, multiply, lighten, darken");
        ok = FALSE;
      }
      else
        bp.blend_mode = (bp.blend_mode & DEVELOP_BLEND_REVERSE) | e;
    }
    else if(!g_strcmp0(name, "reverse"))
    {
      if(!(JSON_NODE_HOLDS_VALUE(v) && json_node_get_value_type(v) == G_TYPE_BOOLEAN))
      {
        *err = g_strdup("'reverse': true or false");
        ok = FALSE;
      }
      else
        bp.blend_mode = json_node_get_boolean(v) ? bp.blend_mode | DEVELOP_BLEND_REVERSE
                                                 : bp.blend_mode & ~DEVELOP_BLEND_REVERSE;
    }
    else if(!g_strcmp0(name, "combine"))
    {
      if(!(is_str && _enum_parse(dt_develop_combine_masks_names, json_node_get_string(v), &e)))
      {
        *err = g_strdup("'combine': exclusive, inclusive, exclusive & inverted or inclusive & inverted");
        ok = FALSE;
      }
      else
        bp.mask_combine = (bp.mask_combine & ~(DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL)) | e;
    }
    else if(!g_strcmp0(name, "feathering_guide"))
    {
      if(!(is_str && _enum_parse(dt_develop_feathering_guide_names, json_node_get_string(v), &e)))
      {
        *err = g_strdup("'feathering_guide': output before blur, input before blur, output after blur"
                        " or input after blur");
        ok = FALSE;
      }
      else
        bp.feathering_guide = e;
    }
    else if(!g_strcmp0(name, "parametric") && JSON_NODE_HOLDS_OBJECT(v))
    {
      JsonObject *chs = json_node_get_object(v);
      GList *keys = json_object_get_members(chs);
      for(GList *kk = keys; kk && ok; kk = g_list_next(kk))
      {
        const char *key = kk->data;
        const char *us = strrchr(key, '_');
        const _channel_t *c = NULL;
        const int out = us && !g_strcmp0(us, "_out");
        if(us && (out || !g_strcmp0(us, "_in")))
          for(const _channel_t *t = _channels_of(&bp); t->name; t++)
            if(strlen(t->name) == (size_t)(us - key) && !strncmp(t->name, key, us - key)) c = t;
        if(!c)
        {
          GString *list = g_string_new(NULL);
          for(const _channel_t *t = _channels_of(&bp); t->name; t++)
            g_string_append_printf(list, "%s%s_in, %s_out", list->len ? ", " : "", t->name, t->name);
          *err = g_strdup_printf("parametric: no channel '%s' in this module (%s)", key, list->str);
          g_string_free(list, TRUE);
          ok = FALSE;
          break;
        }
        const int idx = c->index + 4 * out;
        JsonNode *cv = json_object_get_member(chs, key);
        if(JSON_NODE_HOLDS_NULL(cv))
        {
          bp.blendif &= ~((1u << idx) | (1u << (idx + 16)));
          continue;
        }
        JsonObject *co = JSON_NODE_HOLDS_OBJECT(cv) ? json_node_get_object(cv) : NULL;
        if(co && json_object_has_member(co, "boost"))
        {
          if(!c->boost)
          {
            *err = g_strdup_printf("parametric '%s': this channel has no boost", key);
            ok = FALSE;
            break;
          }
          const double boost = json_object_get_double_member(co, "boost");
          if(!(boost >= 0.0 && boost <= 18.0))
          {
            *err = g_strdup_printf("parametric '%s': boost is 0..18 EV", key);
            ok = FALSE;
            break;
          }
          bp.blendif_boost_factors[idx] = boost + c->boost_offset;
        }
        JsonArray *range = co && json_object_has_member(co, "range")
                           && JSON_NODE_HOLDS_ARRAY(json_object_get_member(co, "range"))
                             ? json_object_get_array_member(co, "range") : NULL;
        if(co && json_object_has_member(co, "range") && (!range || json_array_get_length(range) != 4))
        {
          *err = g_strdup_printf("parametric '%s': range is 4 numbers, in the darkroom's units", key);
          ok = FALSE;
          break;
        }
        if(range)
        {
          const double boostf = exp2(bp.blendif_boost_factors[idx]);
          float r[4];
          for(int k = 0; k < 4; k++)
            r[k] = CLAMP(_from_display(c, json_array_get_double_element(range, k), boostf), 0.0, 1.0);
          if(r[0] > r[1] || r[1] > r[2] || r[2] > r[3])
          {
            *err = g_strdup_printf("parametric '%s': range must not decrease", key);
            ok = FALSE;
            break;
          }
          memcpy(&bp.blendif_parameters[4 * idx], r, sizeof(r));
        }
        if(!co)
        {
          *err = g_strdup_printf("parametric '%s': {range, inverted, boost} or null", key);
          ok = FALSE;
          break;
        }
        bp.blendif |= 1u << idx;
        if(json_object_has_member(co, "inverted"))
        {
          if(json_object_get_boolean_member(co, "inverted")) bp.blendif |= 1u << (idx + 16);
          else bp.blendif &= ~(1u << (idx + 16));
        }
        parametric = TRUE;
      }
      g_list_free(keys);
    }
    else if(!g_strcmp0(name, "raster_source"))
    {
      // another module's mask, as the raster mask menu picks it: a module
      // before this one with a mask of its own, as dt_iop_advertise_rastermask
      // decides (asked directly: the window advertises when its pipe runs)
      JsonObject *o = JSON_NODE_HOLDS_OBJECT(v) ? json_node_get_object(v) : NULL;
      const char *sop = is_str ? json_node_get_string(v)
                               : o ? json_object_get_string_member_with_default(o, "operation", NULL) : NULL;
      const int sprio = o ? json_object_get_int_member_with_default(o, "instance", 0) : 0;
      dt_iop_module_t *src = sop ? dt_iop_get_module_by_op_priority(_cur->dev->iop, sop, sprio) : NULL;
      if(JSON_NODE_HOLDS_NULL(v))
      {
        memset(bp.raster_mask_source, 0, sizeof(bp.raster_mask_source));
        bp.raster_mask_instance = 0;
        bp.raster_mask_id = INVALID_MASKID;
        bp.mask_mode &= ~DEVELOP_MASK_RASTER;
      }
      else if(!src || src == m || src->iop_order >= m->iop_order
              || !((src->blend_params->mask_mode & DEVELOP_MASK_ENABLED
                    && !(src->blend_params->mask_mode & DEVELOP_MASK_RASTER))
                   || (src->flags() & IOP_FLAGS_WRITE_RASTER)))
      {
        *err = g_strdup("'raster_source': {operation, instance} of a module before this one that has a mask,"
                        " or null");
        ok = FALSE;
      }
      else
      {
        g_strlcpy(bp.raster_mask_source, src->op, sizeof(bp.raster_mask_source));
        bp.raster_mask_instance = src->multi_priority;
        bp.raster_mask_id = BLEND_RASTER_ID;
        raster = TRUE;
      }
    }
    else if(!g_strcmp0(name, "raster_inverted"))
    {
      if(!(JSON_NODE_HOLDS_VALUE(v) && json_node_get_value_type(v) == G_TYPE_BOOLEAN))
      {
        *err = g_strdup("'raster_inverted': true or false");
        ok = FALSE;
      }
      else
        bp.raster_mask_invert = json_node_get_boolean(v);
    }
    else
    {
      *err = g_strdup_printf("blend_set: no setting '%s' (masks, blend_mode, reverse, opacity,"
                             " blend_parameter, feathering_radius, blur_radius, brightness, contrast, details,"
                             " combine, feathering_guide, parametric, raster_source, raster_inverted)", name);
      ok = FALSE;
    }
  }
  g_list_free(names);
  if(!ok) return FALSE;
  // ranges set: the parametric tab is on, keeping drawn shapes if any
  if(parametric && !(bp.mask_mode & DEVELOP_MASK_CONDITIONAL))
    bp.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL | (bp.mask_mode & DEVELOP_MASK_MASK);
  // a source set: the raster tab on (raster masks don't combine with the
  // others, develop/blend.c)
  if(raster) bp.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER;
  // the sink registered with its source, so the source keeps its mask
  // (dt_iop_commit_blend_params, as the raster mask menu's history item)
  dt_iop_commit_blend_params(m, &bp, NULL);
  _record(m, TRUE);
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, _cur->dev->history_end);
  _add_blend(b, m);
  return TRUE;
}

// a new shape on a module's mask, as drawing it in the darkroom does: in
// the module's group, joined as op (DT_MASKS_STATE_UNION, ...), drawn
// masking on, one history item; for the darkroom's image through its path
static gboolean _mask_attach(dt_iop_module_t *m, dt_masks_form_t *form, const int op, const gboolean inverted,
                             JsonBuilder *b)
{
  dt_develop_t *dev = _cur->dev;
  if(_cur->gui)
  {
    _gui_focus();
    _api_editing = TRUE;
    dt_dev_undo_start_record(dev);
  }
  dt_masks_gui_form_save_creation(dev, m, form, NULL);
  // the group entry save_creation added: how it combines, and inverted
  dt_masks_form_t *grp = dt_masks_get_from_id(dev, m->blend_params->mask_id);
  GList *last = grp ? g_list_last(grp->points) : NULL;
  if(last && ((dt_masks_point_group_t *)last->data)->formid == form->formid)
  {
    dt_masks_point_group_t *gp = last->data;
    if(last != grp->points)
      gp->state = (gp->state & ~(DT_MASKS_STATE_UNION | DT_MASKS_STATE_INTERSECTION | DT_MASKS_STATE_DIFFERENCE
                                 | DT_MASKS_STATE_EXCLUSION)) | op;
    if(inverted) gp->state |= DT_MASKS_STATE_INVERSE;
  }
  // the drawn tab on, keeping parametric ranges if on
  m->blend_params->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK
                               | (m->blend_params->mask_mode & DEVELOP_MASK_CONDITIONAL);
  if(_cur->gui)
  {
    dt_masks_iop_update(m);
    dt_dev_add_history_item(dev, m, TRUE);
    dt_dev_undo_end_record(dev);
    _api_editing = FALSE;
    _gui_changed_by_api();
  }
  else
  {
    m->enabled = TRUE;
    dt_dev_add_masks_history_item_ext(dev, m, TRUE, TRUE);
    _cur->pipe_changed = TRUE;
    _cur->dirty = TRUE;
  }
  json_builder_set_member_name(b, "formid");
  json_builder_add_int_value(b, form->formid);
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, dev->history_end);
  return TRUE;
}

// drawn shapes on a module (not retouch's, which retouch_heal makes): one
// shape per call in raw space (coords), as drawing it in the darkroom does.
// combine: how it joins the shapes before it (union, intersection,
// difference, exclusion)
static gboolean _mask_add(JsonObject *params, JsonBuilder *b, gchar **err)
{
  dt_iop_module_t *m = _blend_module(params, err);
  if(!m) return FALSE;
  if(m->flags() & IOP_FLAGS_NO_MASKS)
  {
    *err = g_strdup_printf("module '%s' takes no drawn masks (retouch: retouch_heal)", m->op);
    return FALSE;
  }
  JsonObject *sh = params && json_object_has_member(params, "shape")
                   && JSON_NODE_HOLDS_OBJECT(json_object_get_member(params, "shape"))
                     ? json_object_get_object_member(params, "shape") : NULL;
  const char *type = sh ? json_object_get_string_member_with_default(sh, "type", "") : "";
  const char *combine = params ? json_object_get_string_member_with_default(params, "combine", "union") : "union";
  const gboolean inverted = params ? json_object_get_boolean_member_with_default(params, "inverted", FALSE) : FALSE;
  static const struct { const char *name; int state; } ops[] = {
    { "union", DT_MASKS_STATE_UNION }, { "intersection", DT_MASKS_STATE_INTERSECTION },
    { "difference", DT_MASKS_STATE_DIFFERENCE }, { "exclusion", DT_MASKS_STATE_EXCLUSION }, { NULL, 0 } };
  int op = -1;
  for(int k = 0; ops[k].name; k++)
    if(!g_ascii_strcasecmp(combine, ops[k].name)) op = ops[k].state;
  if(op < 0)
  {
    *err = g_strdup("combine: union, intersection, difference or exclusion");
    return FALSE;
  }
#define _NUM(o, k, d) json_object_get_double_member_with_default((o), (k), (d))
  dt_masks_form_t *form = NULL;
  if(!g_strcmp0(type, "path") || !g_strcmp0(type, "brush"))
  {
    // corners in raw space; control points smoothed through them as the
    // path and brush tools draw them
    const gboolean brush = !g_strcmp0(type, "brush");
    JsonArray *pts = json_object_has_member(sh, "points")
                     && JSON_NODE_HOLDS_ARRAY(json_object_get_member(sh, "points"))
                       ? json_object_get_array_member(sh, "points") : NULL;
    const guint n = pts ? json_array_get_length(pts) : 0;
    const double border = _NUM(sh, brush ? "width" : "border",
                               brush ? dt_conf_get_float("plugins/darkroom/masks/brush/border")
                                     : dt_conf_get_float("plugins/darkroom/masks/path/border"));
    const double hardness = _NUM(sh, "hardness", dt_conf_get_float("plugins/darkroom/masks/brush/hardness"));
    const double density = _NUM(sh, "density", dt_conf_get_float("plugins/darkroom/masks/brush/density"));
    gboolean ok = n >= (brush ? 2 : 3) && border > 0.0 && border <= 1.0
                  && hardness >= 0.0 && hardness <= 1.0 && density >= 0.0 && density <= 1.0;
    for(guint k = 0; ok && k < n; k++)
    {
      JsonNode *e = json_array_get_element(pts, k);
      JsonArray *p = JSON_NODE_HOLDS_ARRAY(e) ? json_node_get_array(e) : NULL;
      ok = p && json_array_get_length(p) == 2;
      for(int c = 0; ok && c < 2; c++)
      {
        const double v = json_array_get_double_element(p, c);
        ok = v >= 0.0 && v <= 1.0;
      }
    }
    if(!ok)
    {
      *err = g_strdup(brush ? "brush: points [[x, y], ...] (2 or more, raw space), width 0..1 (relative to the"
                              " shorter side), hardness and density 0..1"
                            : "path: points [[x, y], ...] (3 or more, raw space; closed), border 0..1 (relative"
                              " to the shorter side)");
      return FALSE;
    }
    form = dt_masks_create(brush ? DT_MASKS_BRUSH : DT_MASKS_PATH);
    for(guint k = 0; k < n; k++)
    {
      JsonArray *p = json_array_get_array_element(pts, k);
      const float px = json_array_get_double_element(p, 0), py = json_array_get_double_element(p, 1);
      if(brush)
      {
        dt_masks_point_brush_t *pt = malloc(sizeof(dt_masks_point_brush_t));
        pt->corner[0] = px;
        pt->corner[1] = py;
        pt->ctrl1[0] = pt->ctrl1[1] = pt->ctrl2[0] = pt->ctrl2[1] = -1.0f;
        pt->border[0] = pt->border[1] = border;
        pt->hardness = hardness;
        pt->density = density;
        pt->state = DT_MASKS_POINT_STATE_NORMAL;
        form->points = g_list_append(form->points, pt);
      }
      else
      {
        dt_masks_point_path_t *pt = malloc(sizeof(dt_masks_point_path_t));
        pt->corner[0] = px;
        pt->corner[1] = py;
        pt->ctrl1[0] = pt->ctrl1[1] = pt->ctrl2[0] = pt->ctrl2[1] = -1.0f;
        pt->border[0] = pt->border[1] = MAX(0.0005f, border);
        pt->state = DT_MASKS_POINT_STATE_NORMAL;
        form->points = g_list_append(form->points, pt);
      }
    }
    if(brush)
      dt_masks_brush_init_ctrl_points(form);
    else
      dt_masks_path_init_ctrl_points(form);
    return _mask_attach(m, form, op, inverted, b);
  }
  const double x = sh ? _NUM(sh, "x", -1) : -1, y = sh ? _NUM(sh, "y", -1) : -1;
  if(!(x >= 0.0 && x <= 1.0 && y >= 0.0 && y <= 1.0))
  {
    *err = g_strdup("shape needs type (circle, ellipse, gradient, path, brush) and x, y in 0..1"
                    " (raw space, coords)");
    return FALSE;
  }
  if(!g_strcmp0(type, "circle"))
  {
    const double r = _NUM(sh, "r", 0.05);
    const double border = _NUM(sh, "border", dt_conf_get_float("plugins/darkroom/masks/circle/border"));
    if(!(r > 0.0 && r <= 1.0 && border >= 0.0 && border <= 1.0))
    {
      *err = g_strdup("circle: r in 0..1 and border in 0..1 (relative to the shorter side)");
      return FALSE;
    }
    form = dt_masks_create(DT_MASKS_CIRCLE);
    dt_masks_point_circle_t *c = malloc(sizeof(dt_masks_point_circle_t));
    c->center[0] = x;
    c->center[1] = y;
    c->radius = r;
    c->border = border;
    form->points = g_list_append(form->points, c);
  }
  else if(!g_strcmp0(type, "ellipse"))
  {
    const double ra = _NUM(sh, "ra", 0.05), rb = _NUM(sh, "rb", 0.03), rot = _NUM(sh, "rotation", 0);
    const double border = _NUM(sh, "border", dt_conf_get_float("plugins/darkroom/masks/ellipse/border"));
    if(!(ra > 0.0 && ra <= 1.0 && rb > 0.0 && rb <= 1.0 && border >= 0.0 && border <= 1.0))
    {
      *err = g_strdup("ellipse: ra, rb in 0..1, border in 0..1, rotation in degrees");
      return FALSE;
    }
    form = dt_masks_create(DT_MASKS_ELLIPSE);
    dt_masks_point_ellipse_t *e = malloc(sizeof(dt_masks_point_ellipse_t));
    e->center[0] = x;
    e->center[1] = y;
    e->radius[0] = ra;
    e->radius[1] = rb;
    e->rotation = fmod(rot, 360.0);
    e->border = border;
    e->flags = json_object_get_boolean_member_with_default(sh, "proportional", FALSE)
               ? DT_MASKS_ELLIPSE_PROPORTIONAL : DT_MASKS_ELLIPSE_EQUIDISTANT;
    form->points = g_list_append(form->points, e);
  }
  else if(!g_strcmp0(type, "gradient"))
  {
    const double rot = _NUM(sh, "rotation", 0), comp = _NUM(sh, "compression", 0.5);
    const double steep = _NUM(sh, "steepness", 0), curv = _NUM(sh, "curvature", 0);
    if(!(comp >= 0.0 && comp <= 1.0 && steep >= 0.0 && steep <= 1.0 && curv >= -2.0 && curv <= 2.0))
    {
      *err = g_strdup("gradient: rotation in degrees, compression 0..1, steepness 0..1, curvature -2..2");
      return FALSE;
    }
    form = dt_masks_create(DT_MASKS_GRADIENT);
    dt_masks_point_gradient_t *g = malloc(sizeof(dt_masks_point_gradient_t));
    g->anchor[0] = x;
    g->anchor[1] = y;
    g->rotation = fmod(rot, 360.0);
    g->compression = comp;
    g->steepness = steep;
    g->curvature = curv;
    g->state = json_object_get_boolean_member_with_default(sh, "linear", FALSE)
               ? DT_MASKS_GRADIENT_STATE_LINEAR : DT_MASKS_GRADIENT_STATE_SIGMOIDAL;
    form->points = g_list_append(form->points, g);
  }
  else
  {
    *err = g_strdup("shape type: circle, ellipse, gradient, path or brush");
    return FALSE;
  }
#undef _NUM
  return _mask_attach(m, form, op, inverted, b);
}

static gboolean _mask_list(JsonObject *params, JsonBuilder *b, gchar **err)
{
  dt_iop_module_t *m = _blend_module(params, err);
  if(!m) return FALSE;
  json_builder_set_member_name(b, "shapes");
  json_builder_begin_array(b);
  dt_masks_form_t *grp = dt_masks_get_from_id(_cur->dev, m->blend_params->mask_id);
  for(GList *l = grp && (grp->type & DT_MASKS_GROUP) ? grp->points : NULL; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *gp = l->data;
    const dt_masks_form_t *form = dt_masks_get_from_id(_cur->dev, gp->formid);
    if(!form) continue;
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "formid");
    json_builder_add_int_value(b, gp->formid);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, form->name);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, form->type & DT_MASKS_CIRCLE ? "circle" : form->type & DT_MASKS_ELLIPSE
                                     ? "ellipse" : form->type & DT_MASKS_GRADIENT ? "gradient"
                                     : form->type & DT_MASKS_PATH ? "path" : form->type & DT_MASKS_BRUSH
                                     ? "brush" : form->type & DT_MASKS_GROUP ? "group" : "other");
    json_builder_set_member_name(b, "combine");
    json_builder_add_string_value(b, gp->state & DT_MASKS_STATE_INTERSECTION ? "intersection"
                                     : gp->state & DT_MASKS_STATE_DIFFERENCE ? "difference"
                                     : gp->state & DT_MASKS_STATE_EXCLUSION ? "exclusion" : "union");
    json_builder_set_member_name(b, "inverted");
    json_builder_add_boolean_value(b, (gp->state & DT_MASKS_STATE_INVERSE) != 0);
    json_builder_set_member_name(b, "active");
    json_builder_add_boolean_value(b, (gp->state & DT_MASKS_STATE_USE) != 0);
    if(form->points && (form->type & (DT_MASKS_CIRCLE | DT_MASKS_ELLIPSE | DT_MASKS_GRADIENT)))
    {
      const float *p = form->points->data;   // center or anchor first in all three
      json_builder_set_member_name(b, "x");
      json_builder_add_double_value(b, p[0]);
      json_builder_set_member_name(b, "y");
      json_builder_add_double_value(b, p[1]);
    }
    if(form->type & (DT_MASKS_PATH | DT_MASKS_BRUSH))
    {
      // corners first in both point types
      json_builder_set_member_name(b, "points");
      json_builder_begin_array(b);
      for(GList *q = form->points; q; q = g_list_next(q))
      {
        const float *p = q->data;
        json_builder_begin_array(b);
        json_builder_add_double_value(b, p[0]);
        json_builder_add_double_value(b, p[1]);
        json_builder_end_array(b);
      }
      json_builder_end_array(b);
    }
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  return TRUE;
}

// take a shape out of a module's mask (it stays among the image's shapes,
// as the darkroom's "remove from this module" keeps it in the mask manager)
static gboolean _mask_remove(JsonObject *params, JsonBuilder *b, gchar **err)
{
  dt_iop_module_t *m = _blend_module(params, err);
  if(!m) return FALSE;
  const dt_mask_id_t formid = params ? json_object_get_int_member_with_default(params, "formid", 0) : 0;
  dt_develop_t *dev = _cur->dev;
  dt_masks_form_t *grp = dt_masks_get_from_id(dev, m->blend_params->mask_id);
  GList *hit = NULL;
  for(GList *l = grp && (grp->type & DT_MASKS_GROUP) ? grp->points : NULL; l; l = g_list_next(l))
    if(((dt_masks_point_group_t *)l->data)->formid == formid) hit = l;
  if(!hit)
  {
    *err = g_strdup_printf("module '%s' has no shape %d (mask_list)", m->op, formid);
    return FALSE;
  }
  if(_cur->gui)
  {
    _api_editing = TRUE;
    dt_masks_form_remove(m, grp, dt_masks_get_from_id(dev, formid));
    _api_editing = FALSE;
    _gui_changed_by_api();
  }
  else
  {
    free(hit->data);
    grp->points = g_list_delete_link(grp->points, hit);
    if(!grp->points)
      m->blend_params->mask_mode &= ~DEVELOP_MASK_MASK;
    dt_dev_add_masks_history_item_ext(dev, m, TRUE, TRUE);
    _cur->pipe_changed = TRUE;
    _cur->dirty = TRUE;
  }
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, dev->history_end);
  return TRUE;
}

// ---- module instances and history compression ------------------------------

// a session's pipe after modules were added or removed: new nodes for the
// new module list (the darkroom rebuilds its pipes the same way,
// dt_dev_pixelpipe_rebuild)
static void _pipe_rebuild(_session_t *s)
{
  dt_dev_pixelpipe_cleanup_nodes(&s->pipe);
  dt_dev_pixelpipe_create_nodes(&s->pipe, s->dev);
  // cache lines of the old nodes can match the new ones' hashes (an
  // instance removed, its neighbours renumbered): renders came out banded
  dt_dev_pixelpipe_cache_flush(&s->pipe);
  s->pipe_changed = TRUE;
}

static void _add_instance(JsonBuilder *b, const dt_iop_module_t *m)
{
  json_builder_set_member_name(b, "operation");
  json_builder_add_string_value(b, m->op);
  json_builder_set_member_name(b, "instance");
  json_builder_add_int_value(b, m->multi_priority);
  json_builder_set_member_name(b, "name");
  json_builder_add_string_value(b, m->multi_name);
  json_builder_set_member_name(b, "iop_order");
  json_builder_add_int_value(b, m->iop_order);
}

// a new instance of a module, after the given one in the pipe, as the
// module's "new instance" (copy: false) or "duplicate" (copy: true) menu
// entries do (imageop.c, dt_iop_gui_duplicate)
// dt_masks_iop_use_same_as for a session (darktable's works on the
// darkroom's image): the copy gets a group of its own holding the base's
// shapes, shared as the darkroom shares them. the group is new, so it can't
// contain itself (the check dt_masks_group_add_form makes)
static gboolean _masks_use_same_as(dt_develop_t *dev, dt_iop_module_t *m, const dt_iop_module_t *base)
{
  const dt_masks_form_t *src = dt_masks_get_from_id(dev, base->blend_params->mask_id);
  if(!src || src->type != DT_MASKS_GROUP) return FALSE;
  dt_masks_form_t *grp = dt_masks_create(DT_MASKS_GROUP);
  while(dt_masks_get_from_id(dev, grp->formid)) grp->formid++;
  gchar *label = dt_history_item_get_name(m);
  snprintf(grp->name, sizeof(grp->name), _("group `%s'"), label);
  g_free(label);
  for(const GList *l = src->points; l; l = g_list_next(l))
  {
    const dt_masks_point_group_t *pt = l->data;
    if(!dt_masks_get_from_id(dev, pt->formid)) continue;
    dt_masks_point_group_t *grpt = malloc(sizeof(dt_masks_point_group_t));
    *grpt = *pt;
    grpt->parentid = grp->formid;
    grp->points = g_list_append(grp->points, grpt);
  }
  dev->forms = g_list_append(dev->forms, grp);
  m->blend_params->mask_id = grp->formid;
  return TRUE;
}

static gboolean _module_add(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  dt_iop_module_t *base = _find_module(params, err);
  if(!base) return FALSE;
  if(base->flags() & IOP_FLAGS_ONE_INSTANCE)
  {
    *err = g_strdup_printf("module '%s' can only have one instance", base->op);
    return FALSE;
  }
  const gboolean copy = params ? json_object_get_boolean_member_with_default(params, "copy", FALSE) : FALSE;
  dt_develop_t *dev = _cur->dev;
  dt_iop_module_t *m = NULL;
  gboolean shapes_copied = TRUE, shapes = FALSE;
  if(_cur->gui)
  {
    _api_editing = TRUE;
    m = dt_iop_gui_duplicate(base, copy);
    _api_editing = FALSE;
    _gui_changed_by_api();
  }
  else
  {
    // the base in the history first, so the instances' order is recorded
    dt_dev_add_history_item_ext(dev, base, FALSE, TRUE);
    m = dt_dev_module_duplicate(dev, base);
    if(m)
    {
      dt_iop_reload_defaults(m);
      if(copy)
      {
        memcpy(m->params, base->params, m->params_size);
        if(m->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
        {
          dt_iop_commit_blend_params(m, base->blend_params, NULL);
          // its own group with the base's shapes, as the darkroom's copy
          if(dt_is_valid_maskid(base->blend_params->mask_id))
          {
            m->blend_params->mask_id = NO_MASKID;
            shapes = _masks_use_same_as(dev, m, base);
            if(!shapes)
            {
              m->blend_params->mask_mode &= ~DEVELOP_MASK_MASK;
              shapes_copied = FALSE;
            }
          }
        }
      }
      m->enabled = TRUE;
      if(shapes)
        dt_dev_add_masks_history_item_ext(dev, m, TRUE, TRUE);
      else
        dt_dev_add_history_item_ext(dev, m, TRUE, TRUE);
      _pipe_rebuild(_cur);
      _cur->dirty = TRUE;
    }
  }
  if(!m)
  {
    *err = g_strdup_printf("could not add an instance of '%s'", base->op);
    return FALSE;
  }
  _add_instance(b, m);
  if(!shapes_copied)
  {
    json_builder_set_member_name(b, "note");
    json_builder_add_string_value(b, "the base's drawn shapes were not copied");
  }
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, dev->history_end);
  return TRUE;
}

// delete an instance, as the module's delete menu entry does (imageop.c,
// _gui_delete_callback): its history items go, another instance of the
// module takes its place if it was the first one. not the last instance
static gboolean _module_remove(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  dt_iop_module_t *m = _find_module(params, err);
  if(!m) return FALSE;
  dt_develop_t *dev = _cur->dev;
  dt_iop_module_t *next = NULL;
  for(GList *l = dev->iop; l; l = g_list_next(l))
  {
    dt_iop_module_t *o = l->data;
    if(o != m && o->instance == m->instance && !next) next = o;
  }
  if(!next)
  {
    *err = g_strdup_printf("'%s' instance %d is the module's only instance: switch it off instead",
                           m->op, m->multi_priority);
    return FALSE;
  }
  if(_cur->gui)
  {
    _api_editing = TRUE;
    dt_iop_gui_delete(m);
    _api_editing = FALSE;
    _gui_changed_by_api();
  }
  else
  {
    const gboolean is_zero = m->multi_priority == 0;
    // what dt_dev_module_remove does for the darkroom (gui_attached)
    int k = 0;
    for(GList *l = dev->history; l;)
    {
      GList *n = g_list_next(l);
      dt_dev_history_item_t *hist = l->data;
      if(hist->module == m)
      {
        if(k < dev->history_end) dev->history_end--;
        dt_dev_free_history_item(hist);
        dev->history = g_list_delete_link(dev->history, l);
      }
      else
        k++;
      l = n;
    }
    dt_dev_module_remove(dev, m);
    // the pipe order list names instances by operation and number: drop
    // the removed one's entry, or a later instance with its number gets two
    dt_ioppr_resync_iop_list(dev);
    if(is_zero)
    {
      dt_iop_module_t *first = NULL;
      for(GList *h = dev->history; h && !first; h = g_list_next(h))
      {
        dt_dev_history_item_t *hist = h->data;
        if(hist->module->instance == m->instance) first = hist->module;
      }
      if(!first) first = next;
      // its entry in the order list follows the new number
      GList *e = dt_ioppr_get_iop_order_link(dev->iop_order_list, first->op, first->multi_priority);
      if(e) ((dt_iop_order_entry_t *)e->data)->instance = 0;
      dt_iop_update_multi_priority(first, 0);
      for(GList *h = dev->history; h; h = g_list_next(h))
      {
        dt_dev_history_item_t *hist = h->data;
        if(hist->module == first) hist->multi_priority = 0;
      }
    }
    // kept, as the darkroom keeps it: the pipe's nodes still point at it
    dev->alliop = g_list_append(dev->alliop, m);
    _pipe_rebuild(_cur);
    _cur->dirty = TRUE;
  }
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, dev->history_end);
  json_builder_set_member_name(b, "instances");
  json_builder_begin_array(b);
  for(GList *l = dev->iop; l; l = g_list_next(l))
  {
    dt_iop_module_t *o = l->data;
    if(o->instance != next->instance) continue;
    json_builder_begin_object(b);
    _add_instance(b, o);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  return TRUE;
}

// the instance's label, as renaming it in its header does (an empty name
// gives the label back to darktable)
static gboolean _module_rename(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  dt_iop_module_t *m = _find_module(params, err);
  if(!m) return FALSE;
  const char *name = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
  if(!name || strlen(name) >= sizeof(m->multi_name))
  {
    *err = g_strdup_printf("module_rename needs name (at most %zu bytes; \"\" for darktable's label)",
                           sizeof(m->multi_name) - 1);
    return FALSE;
  }
  if(_cur->gui)
  {
    _gui_focus();
    _api_editing = TRUE;
    dt_iop_update_multi_name(m, name, *name != '\0', TRUE, TRUE);
    _api_editing = FALSE;
    _gui_changed_by_api();
  }
  else
  {
    gchar *n = g_strstrip(g_strdup(name));
    g_strlcpy(m->multi_name, n, sizeof(m->multi_name));
    m->multi_name_hand_edited = *n != '\0';
    g_free(n);
    _record(m, TRUE);
  }
  _add_instance(b, m);
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, _cur->dev->history_end);
  return TRUE;
}

// compress the history stack as the history panel's button does
// (libs/history.c: _lib_history_truncate): one item per module instance,
// disabled modules dropped; truncate: only drop the items above
// history_end. it works on the library, so the edit is saved first
static gboolean _history_compress(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  const gboolean truncate = params ? json_object_get_boolean_member_with_default(params, "truncate", FALSE) : FALSE;
  const dt_imgid_t imgid = _cur->dev->image_storage.id;
  const int before = g_list_length(_cur->dev->history);
  if(_cur->gui)
  {
    dt_develop_t *dev = _cur->dev;
    _api_editing = TRUE;
    dt_dev_undo_start_record(dev);
    dt_dev_write_history(dev);
    if(truncate) dt_history_truncate_on_image(imgid, dev->history_end);
    else dt_history_compress_on_image(imgid);
    dt_dev_reload_history_items(dev);
    dt_dev_write_history(dev);
    dt_image_synch_xmp(imgid);
    dev->history_end = g_list_length(dev->history);
    dt_image_set_history_end(imgid, dev->history_end);
    dt_dev_reload_history_items(dev);
    dt_dev_undo_end_record(dev);
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_HISTORY_INVALIDATED);
    _api_editing = FALSE;
    _gui_changed_by_api();
  }
  else
  {
    _session_t *s = _cur;
    _save_session(s);
    if(truncate) dt_history_truncate_on_image(imgid, s->dev->history_end);
    else dt_history_compress_on_image(imgid);
    // reload, and write back once so the items are numbered without gaps
    _session_close(s);
    if(!(s = _session_load(imgid, err))) return FALSE;
    _cur = s;
    s->dev->history_end = g_list_length(s->dev->history);
    dt_dev_pop_history_items_ext(s->dev, s->dev->history_end);
    dt_dev_write_history(s->dev);
    dt_image_set_history_end(imgid, s->dev->history_end);
    dt_image_synch_xmp(imgid);
    s->pipe_changed = TRUE;
  }
  json_builder_set_member_name(b, "items_before");
  json_builder_add_int_value(b, before);
  json_builder_set_member_name(b, "saved");
  json_builder_add_boolean_value(b, TRUE);
  _session_info(b, _cur);
  return TRUE;
}

// ---- module order, curves, duplicates ---------------------------------------

// move a module (instance) before or after another in the pipe, as dragging
// it in the darkroom does (imageop.c, _on_drag_drop): darktable's rules for
// what may move where apply (iop_order.c)
static gboolean _module_move(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  dt_iop_module_t *m = _find_module(params, err);
  if(!m) return FALSE;
  JsonObject *ref = NULL;
  gboolean after = FALSE;
  if(params && json_object_has_member(params, "before")
     && JSON_NODE_HOLDS_OBJECT(json_object_get_member(params, "before")))
    ref = json_object_get_object_member(params, "before");
  else if(params && json_object_has_member(params, "after")
          && JSON_NODE_HOLDS_OBJECT(json_object_get_member(params, "after")))
  {
    ref = json_object_get_object_member(params, "after");
    after = TRUE;
  }
  if(!ref)
  {
    *err = g_strdup("module_move needs before or after: {operation, instance}");
    return FALSE;
  }
  dt_iop_module_t *target = _find_module(ref, err);
  if(!target) return FALSE;
  if(target == m)
  {
    *err = g_strdup("a module can't move relative to itself");
    return FALSE;
  }
  dt_develop_t *dev = _cur->dev;
  const gboolean ok = after ? dt_ioppr_check_can_move_after_iop(dev->iop, m, target)
                            : dt_ioppr_check_can_move_before_iop(dev->iop, m, target);
  if(!ok || !(after ? dt_ioppr_move_iop_after(dev, m, target) : dt_ioppr_move_iop_before(dev, m, target)))
  {
    *err = g_strdup_printf("darktable doesn't allow moving '%s' %s '%s' (fixed modules, or modules in between"
                           " that must stay in order)", m->op, after ? "after" : "before", target->op);
    return FALSE;
  }
  if(_cur->gui)
  {
    _gui_focus();
    _api_editing = TRUE;
    dt_dev_reorder_gui_module_list(dev);
    dt_dev_add_history_item(dev, m, TRUE);
    dt_dev_pixelpipe_rebuild(dev);
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_MODULE_MOVED);
    _api_editing = FALSE;
    _gui_changed_by_api();
  }
  else
  {
    dt_dev_add_history_item_ext(dev, m, TRUE, TRUE);
    _pipe_rebuild(_cur);
    _cur->dirty = TRUE;
  }
  _add_instance(b, m);
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, dev->history_end);
  return TRUE;
}

// the curve modules: nodes per channel, node count and curve type per channel,
// all lists in the params (introspection gives their layout)
typedef struct _curve_mod_t
{
  const char *op, *nodes, *count, *type;
  const char *channels[4];
} _curve_mod_t;
static const _curve_mod_t _curve_mods[] = {
  { "rgbcurve", "curve_nodes", "curve_num_nodes", "curve_type", { "R", "G", "B", NULL } },
  { "tonecurve", "tonecurve", "tonecurve_nodes", "tonecurve_type", { "L", "a", "b", NULL } },
  { "colorzones", "curve", "curve_num_nodes", "curve_type", { "lightness", "chroma", "hue", NULL } },
  { "basecurve", "basecurve", "basecurve_nodes", "basecurve_type", { "curve", NULL } },
  { NULL } };
// curve_tools.h
static const char *_curve_types[] = { "cubic spline", "centripetal spline", "monotonic spline", NULL };

static dt_introspection_field_t *_any_field(const dt_iop_module_t *m, const char *name)
{
  for(dt_introspection_field_t *f = m->so->get_introspection_linear();
      f && f->header.type != DT_INTROSPECTION_TYPE_NONE; f++)
    if(!g_strcmp0(f->header.name, name)) return f;
  return NULL;
}

// pointers into the params for channel ch: its node array (and node
// layout), its count and type
typedef struct _curve_ptr_t
{
  uint8_t *nodes;
  size_t node_size, x_off, y_off;
  int max_nodes;
  int *count, *type;
} _curve_ptr_t;

static gboolean _curve_ptr(dt_iop_module_t *m, const _curve_mod_t *c, const int ch, _curve_ptr_t *p)
{
  dt_introspection_field_t *nf = _any_field(m, c->nodes), *cf = _any_field(m, c->count), *tf = _any_field(m, c->type);
  if(!nf || !cf || !tf || nf->header.type != DT_INTROSPECTION_TYPE_ARRAY) return FALSE;
  dt_introspection_field_t *row = NULL, *node = NULL;
  void *rp = dt_introspection_access_array(nf, (uint8_t *)m->params + nf->header.offset, ch, &row);
  if(!rp || !row || row->header.type != DT_INTROSPECTION_TYPE_ARRAY) return FALSE;
  void *np = dt_introspection_access_array(row, rp, 0, &node);
  dt_introspection_field_t *xf = NULL, *yf = NULL;
  void *xp = np ? dt_introspection_get_child(node, np, "x", &xf) : NULL;
  void *yp = np ? dt_introspection_get_child(node, np, "y", &yf) : NULL;
  if(!xp || !yp) return FALSE;
  p->nodes = np;
  p->node_size = node->header.size;
  p->x_off = (uint8_t *)xp - (uint8_t *)np;
  p->y_off = (uint8_t *)yp - (uint8_t *)np;
  p->max_nodes = row->Array.count;
  p->count = dt_introspection_access_array(cf, (uint8_t *)m->params + cf->header.offset, ch, NULL);
  p->type = dt_introspection_access_array(tf, (uint8_t *)m->params + tf->header.offset, ch, NULL);
  return p->count && p->type;
}

static const _curve_mod_t *_curve_mod(const dt_iop_module_t *m, gchar **err)
{
  for(const _curve_mod_t *c = _curve_mods; c->op; c++)
    if(!g_strcmp0(c->op, m->op)) return c;
  *err = g_strdup_printf("'%s' has no curves (rgbcurve, tonecurve, colorzones, basecurve)", m->op);
  return NULL;
}

static void _add_curves(JsonBuilder *b, dt_iop_module_t *m, const _curve_mod_t *c)
{
  json_builder_set_member_name(b, "curves");
  json_builder_begin_array(b);
  for(int ch = 0; c->channels[ch]; ch++)
  {
    _curve_ptr_t p;
    if(!_curve_ptr(m, c, ch, &p)) continue;
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "channel");
    json_builder_add_string_value(b, c->channels[ch]);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, *p.type >= 0 && *p.type <= 2 ? _curve_types[*p.type] : "other");
    json_builder_set_member_name(b, "points");
    json_builder_begin_array(b);
    for(int k = 0; k < MIN(*p.count, p.max_nodes); k++)
    {
      const uint8_t *n = p.nodes + k * p.node_size;
      json_builder_begin_array(b);
      json_builder_add_double_value(b, round(*(const float *)(n + p.x_off) * 1e5) / 1e5);
      json_builder_add_double_value(b, round(*(const float *)(n + p.y_off) * 1e5) / 1e5);
      json_builder_end_array(b);
    }
    json_builder_end_array(b);
    json_builder_set_member_name(b, "max_points");
    json_builder_add_int_value(b, p.max_nodes);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
}

static gboolean _curve_get(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  dt_iop_module_t *m = _find_module(params, err);
  if(!m) return FALSE;
  const _curve_mod_t *c = _curve_mod(m, err);
  if(!c) return FALSE;
  _add_instance(b, m);
  _add_curves(b, m, c);
  return TRUE;
}

// one channel's curve: its points [[x, y], ...] (x increasing, 0..1) and
// optionally its type, as dragging nodes in the module does; one history item
static gboolean _curve_set(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  dt_iop_module_t *m = _find_module(params, err);
  if(!m) return FALSE;
  const _curve_mod_t *c = _curve_mod(m, err);
  if(!c) return FALSE;
  const char *chname = params ? json_object_get_string_member_with_default(params, "channel", c->channels[0])
                              : c->channels[0];
  int ch = -1;
  for(int k = 0; c->channels[k]; k++)
    if(!g_ascii_strcasecmp(chname, c->channels[k])) ch = k;
  if(ch < 0)
  {
    GString *list = g_string_new(NULL);
    for(int k = 0; c->channels[k]; k++) g_string_append_printf(list, "%s%s", k ? ", " : "", c->channels[k]);
    *err = g_strdup_printf("'%s' has no channel '%s' (%s)", m->op, chname, list->str);
    g_string_free(list, TRUE);
    return FALSE;
  }
  _curve_ptr_t p;
  if(!_curve_ptr(m, c, ch, &p))
  {
    *err = g_strdup_printf("'%s': this darktable's curve layout isn't the expected one", m->op);
    return FALSE;
  }
  int type = *p.type;
  const char *tname = params ? json_object_get_string_member_with_default(params, "type", NULL) : NULL;
  if(tname)
  {
    type = -1;
    for(int k = 0; _curve_types[k]; k++)
      if(!g_ascii_strcasecmp(tname, _curve_types[k])) type = k;
    if(type < 0)
    {
      *err = g_strdup("type: cubic spline, centripetal spline or monotonic spline");
      return FALSE;
    }
  }
  JsonArray *pts = params && json_object_has_member(params, "points")
                   && JSON_NODE_HOLDS_ARRAY(json_object_get_member(params, "points"))
                     ? json_object_get_array_member(params, "points") : NULL;
  const guint n = pts ? json_array_get_length(pts) : 0;
  if(n < 2 || n > (guint)p.max_nodes)
  {
    *err = g_strdup_printf("points: 2..%d points [[x, y], ...]", p.max_nodes);
    return FALSE;
  }
  float (*xy)[2] = g_malloc0_n(n, sizeof(*xy));
  for(guint k = 0; k < n; k++)
  {
    JsonArray *q = JSON_NODE_HOLDS_ARRAY(json_array_get_element(pts, k)) ? json_array_get_array_element(pts, k) : NULL;
    const gboolean ok = q && json_array_get_length(q) == 2;
    xy[k][0] = ok ? json_array_get_double_element(q, 0) : -1;
    xy[k][1] = ok ? json_array_get_double_element(q, 1) : -1;
    if(!ok || xy[k][0] < 0 || xy[k][0] > 1 || xy[k][1] < 0 || xy[k][1] > 1 || (k && xy[k][0] <= xy[k - 1][0]))
    {
      g_free(xy);
      *err = g_strdup_printf("point %u: [x, y] in 0..1, x increasing", k);
      return FALSE;
    }
  }
  for(guint k = 0; k < n; k++)
  {
    uint8_t *node = p.nodes + k * p.node_size;
    *(float *)(node + p.x_off) = xy[k][0];
    *(float *)(node + p.y_off) = xy[k][1];
  }
  g_free(xy);
  *p.count = n;
  *p.type = type;
  _record(m, TRUE);
  _add_instance(b, m);
  json_builder_set_member_name(b, "history_end");
  json_builder_add_int_value(b, _cur->dev->history_end);
  _add_curves(b, m, c);
  return TRUE;
}

static gboolean _image_info(JsonObject *params, JsonBuilder *b, gchar **err);

// a duplicate (virtual copy) of a photo, as the lighttable's duplicate does
// (control_jobs.c): with the saved edit, or none (virgin); in the photo's
// group. unsaved changes of an open photo are refused unless saved first
static gboolean _image_duplicate(JsonObject *params, const dt_imgid_t current, gboolean *saved,
                                 JsonBuilder *b, gchar **err)
{
  const dt_imgid_t imgid = params && json_object_has_member(params, "imgid")
    ? json_object_get_int_member(params, "imgid") : current;
  if(!dt_is_valid_imgid(imgid) || !_image_exists(imgid))
  {
    *err = g_strdup_printf("no image with id %d", imgid);
    return FALSE;
  }
  const gboolean virgin = params ? json_object_get_boolean_member_with_default(params, "virgin", FALSE) : FALSE;
  const gboolean save = params ? json_object_get_boolean_member_with_default(params, "save", FALSE) : FALSE;
  _session_t *s = _session_find(imgid);
  *saved = FALSE;
  if(s && s->gui)
    dt_dev_write_history(s->dev);
  else if(s && s->dirty && !virgin)
  {
    if(!save)
    {
      *err = g_strdup("the photo has unsaved changes, and the duplicate gets the saved edit: save first,"
                      " or pass save: true");
      return FALSE;
    }
    _save_session(s);
    *saved = TRUE;
  }
  const dt_imgid_t newid = dt_image_duplicate(imgid);
  if(!dt_is_valid_imgid(newid))
  {
    *err = g_strdup("darktable couldn't duplicate the photo");
    return FALSE;
  }
  if(virgin)
    dt_history_delete_on_image(newid);
  else
    dt_history_copy_and_paste_on_image(imgid, newid, FALSE, NULL, TRUE, TRUE, TRUE);
  dt_image_cache_set_change_timestamp_from_image(newid, imgid);
  if(_in_gui) DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_FILMROLLS_CHANGED);
  JsonObject *q = json_object_new();
  json_object_set_int_member(q, "imgid", newid);
  const gboolean ok = _image_info(q, b, err);
  json_object_unref(q);
  json_builder_set_member_name(b, "saved");
  json_builder_add_boolean_value(b, *saved);
  return ok;
}

// ---- styles and copy/paste ---------------------------------------------------
// these change an image's saved edit in the library with darktable's own
// code (common/styles.c, common/history.c), as the lighttable does; an image
// open here is saved first and reopened after, as the darkroom writes its
// history before applying a style or pasting (dt_styles_apply_to_dev)

// the history items of an image's saved edit that hold each module
// instance's state (the last below history_end), or those of the modules
// asked for: names, or {operation, instance}
static GList *_history_nums(const dt_imgid_t imgid, JsonArray *modules, const gboolean styles_only,
                            gchar **err)
{
  typedef struct { int num, prio; gchar *op; gboolean used; } _item_t;
  GArray *items = g_array_new(FALSE, TRUE, sizeof(_item_t));
  sqlite3_stmt *st;
  sqlite3_prepare_v2(dt_database_get(darktable.db),
                     "SELECT MAX(h.num), h.operation, h.multi_priority"
                     " FROM main.history AS h"
                     " JOIN main.images AS i ON i.id = h.imgid"
                     " WHERE h.imgid = ?1 AND h.num < i.history_end"
                     "   AND h.operation != 'mask_manager'"
                     " GROUP BY h.operation, h.multi_priority"
                     " ORDER BY 1",
                     -1, &st, NULL);
  sqlite3_bind_int(st, 1, imgid);
  while(sqlite3_step(st) == SQLITE_ROW)
  {
    _item_t it = { sqlite3_column_int(st, 0), sqlite3_column_int(st, 2),
                   g_strdup((const char *)sqlite3_column_text(st, 1)), FALSE };
    g_array_append_val(items, it);
  }
  sqlite3_finalize(st);

  const guint n = modules ? json_array_get_length(modules) : 0;
  for(guint i = 0; i < n && !*err; i++)
  {
    JsonNode *e = json_array_get_element(modules, i);
    const char *op = NULL;
    int prio = -1;
    if(JSON_NODE_HOLDS_VALUE(e) && json_node_get_value_type(e) == G_TYPE_STRING)
      op = json_node_get_string(e);
    else if(JSON_NODE_HOLDS_OBJECT(e))
    {
      JsonObject *o = json_node_get_object(e);
      op = json_object_get_string_member_with_default(o, "operation", NULL);
      prio = json_object_get_int_member_with_default(o, "instance", -1);
    }
    gboolean found = FALSE;
    for(guint k = 0; op && k < items->len; k++)
    {
      _item_t *it = &g_array_index(items, _item_t, k);
      if(!g_strcmp0(it->op, op) && (prio < 0 || prio == it->prio)) found = it->used = TRUE;
    }
    if(!found)
      *err = op ? g_strdup_printf("the saved edit of %d has no module '%s'%s", imgid, op,
                                  prio >= 0 ? " with that instance" : "")
                : g_strdup("modules are names or {operation, instance}");
  }
  GList *nums = NULL;
  for(guint k = 0; k < items->len; k++)
  {
    _item_t *it = &g_array_index(items, _item_t, k);
    const gboolean take = n ? it->used
                            : !styles_only || (dt_iop_get_module_flags(it->op) & IOP_FLAGS_INCLUDE_IN_STYLES);
    if(take && !*err) nums = g_list_append(nums, GINT_TO_POINTER(it->num));
    g_free(it->op);
  }
  g_array_free(items, TRUE);
  if(*err)
  {
    g_list_free(nums);
    return NULL;
  }
  if(!nums) *err = g_strdup_printf("the saved edit of %d has no modules to take", imgid);
  return nums;
}

// the images a call changes: imgids, else the client's image. each must
// exist, and one open here with unsaved changes needs save: true
static GArray *_targets(JsonObject *params, const dt_imgid_t current, const dt_imgid_t source, gchar **err)
{
  GArray *ids = g_array_new(FALSE, FALSE, sizeof(dt_imgid_t));
  JsonArray *a = params && json_object_has_member(params, "imgids")
                 && JSON_NODE_HOLDS_ARRAY(json_object_get_member(params, "imgids"))
                   ? json_object_get_array_member(params, "imgids") : NULL;
  if(a)
    for(guint i = 0; i < json_array_get_length(a); i++)
    {
      const dt_imgid_t id = json_array_get_int_element(a, i);
      g_array_append_val(ids, id);
    }
  else if(dt_is_valid_imgid(current))
    g_array_append_val(ids, current);
  const gboolean save = params ? json_object_get_boolean_member_with_default(params, "save", FALSE) : FALSE;
  if(!ids->len) *err = g_strdup("no image: pass imgids, or open one first");
  for(guint i = 0; i < ids->len && !*err; i++)
  {
    const dt_imgid_t id = g_array_index(ids, dt_imgid_t, i);
    const _session_t *s = _session_find(id);
    if(!dt_is_valid_imgid(id) || !_image_exists(id))
      *err = g_strdup_printf("no image with id %d", id);
    else if(id == source)
      *err = g_strdup_printf("image %d is the source", id);
    else if(s && !s->gui && s->dirty && !save)
      *err = g_strdup_printf("image %d has unsaved changes, which this saves: save first, or pass save: true", id);
  }
  if(*err)
  {
    g_array_free(ids, TRUE);
    return NULL;
  }
  return ids;
}

typedef gboolean (*_library_edit_t)(const dt_imgid_t imgid, const gboolean darkroom, gpointer data);

// runs a change of saved edits on each target: the darkroom's image through
// the darkroom (edit is told so), a session here saved before and reopened
// after. changed lists the images done, for their events
static gboolean _edit_saved(GArray *ids, _library_edit_t edit, gpointer data, GArray *changed,
                            JsonBuilder *b, gchar **err)
{
  json_builder_set_member_name(b, "images");
  json_builder_begin_array(b);
  for(guint i = 0; i < ids->len; i++)
  {
    const dt_imgid_t id = g_array_index(ids, dt_imgid_t, i);
    _session_t *s = _session_find(id);
    gboolean saved = FALSE;
    if(s && !s->gui && s->dirty)
    {
      _save_session(s);
      saved = TRUE;
    }
    if(s && s->gui)
    {
      _api_editing = TRUE;
      edit(id, TRUE, data);
      _api_editing = FALSE;
      _gui_changed_by_api();
    }
    else
      edit(id, FALSE, data);
    gboolean reopened = FALSE;
    if(s && !s->gui)
    {
      _session_close(s);
      reopened = _session_load(id, err) != NULL;
    }
    g_array_append_val(changed, id);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "imgid");
    json_builder_add_int_value(b, id);
    json_builder_set_member_name(b, "saved_first");
    json_builder_add_boolean_value(b, saved);
    json_builder_set_member_name(b, "reopened");
    json_builder_add_boolean_value(b, reopened || (s && s->gui));
    json_builder_end_object(b);
    if(*err) break;
  }
  json_builder_end_array(b);
  return *err == NULL;
}

static gboolean _style_list(JsonObject *params, JsonBuilder *b, gchar **err)
{
  const char *filter = params ? json_object_get_string_member_with_default(params, "filter", "") : "";
  GList *styles = dt_styles_get_list(filter);
  json_builder_set_member_name(b, "styles");
  json_builder_begin_array(b);
  for(GList *l = styles; l; l = g_list_next(l))
  {
    const dt_style_t *style = l->data;
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, style->name);
    // as the styles module shows it: darktable's own styles carry
    // translation markers ("_l10n_darktable|_l10n_examples|...")
    gchar *label = dt_util_localize_segmented_name(style->name, TRUE);
    json_builder_set_member_name(b, "label");
    json_builder_add_string_value(b, label);
    g_free(label);
    gchar *desc = dt_util_localize_segmented_name(style->description ? style->description : "", FALSE);
    json_builder_set_member_name(b, "description");
    json_builder_add_string_value(b, desc);
    g_free(desc);
    json_builder_set_member_name(b, "module_order");
    json_builder_add_boolean_value(b, dt_styles_has_module_order(style->name));
    json_builder_set_member_name(b, "items");
    json_builder_begin_array(b);
    GList *items = dt_styles_get_item_list(style->name, FALSE, NO_IMGID, FALSE);
    for(GList *i = items; i; i = g_list_next(i))
    {
      const dt_style_item_t *it = i->data;
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "operation");
      json_builder_add_string_value(b, it->operation);
      json_builder_set_member_name(b, "instance");
      json_builder_add_int_value(b, it->multi_priority);
      json_builder_set_member_name(b, "name");
      json_builder_add_string_value(b, it->multi_name ? it->multi_name : "");
      json_builder_set_member_name(b, "enabled");
      json_builder_add_boolean_value(b, it->enabled);
      json_builder_end_object(b);
    }
    g_list_free_full(items, dt_style_item_free);
    json_builder_end_array(b);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  g_list_free_full(styles, dt_style_free);
  return TRUE;
}

static gboolean _style_apply_one(const dt_imgid_t imgid, const gboolean darkroom, gpointer data)
{
  if(darkroom)
    dt_styles_apply_to_dev(data, imgid);
  else
    dt_styles_apply_to_image(data, FALSE, FALSE, imgid);
  return TRUE;
}

// a style onto images: its modules added to each edit as new history items,
// as applying it in the lighttable or darkroom does
static gboolean _style_apply(JsonObject *params, const dt_imgid_t current, GArray *changed, JsonBuilder *b,
                             gchar **err)
{
  const char *name = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
  if(!name || !dt_styles_exists(name))
  {
    *err = g_strdup_printf("no style named '%s' (style_list)", name ? name : "");
    return FALSE;
  }
  GArray *ids = _targets(params, current, NO_IMGID, err);
  if(!ids) return FALSE;
  const gboolean ok = _edit_saved(ids, _style_apply_one, (gpointer)name, changed, b, err);
  g_array_free(ids, TRUE);
  return ok;
}

// a style from an image's saved edit: the modules darktable's create style
// dialog ticks (those meant for styles), or the ones asked for
static gboolean _style_create(JsonObject *params, const dt_imgid_t current, JsonBuilder *b, gchar **err)
{
  const char *name = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
  const dt_imgid_t imgid = params && json_object_has_member(params, "imgid")
    ? json_object_get_int_member(params, "imgid") : current;
  if(!name || !*name)
  {
    *err = g_strdup("needs a name");
    return FALSE;
  }
  if(dt_styles_exists(name))
  {
    *err = g_strdup_printf("a style named '%s' exists", name);
    return FALSE;
  }
  if(!dt_is_valid_imgid(imgid) || !_image_exists(imgid))
  {
    *err = g_strdup_printf("no image with id %d", imgid);
    return FALSE;
  }
  const _session_t *s = _session_find(imgid);
  if(s && s->gui)
    dt_dev_write_history(s->dev);
  else if(s && s->dirty)
  {
    *err = g_strdup_printf("image %d has unsaved changes and a style takes the saved edit: save first", imgid);
    return FALSE;
  }
  JsonArray *modules = params && json_object_has_member(params, "modules")
                       && JSON_NODE_HOLDS_ARRAY(json_object_get_member(params, "modules"))
                         ? json_object_get_array_member(params, "modules") : NULL;
  GList *nums = _history_nums(imgid, modules, TRUE, err);
  if(!nums) return FALSE;
  const char *desc = json_object_get_string_member_with_default(params, "description", "");
  const gboolean order = json_object_get_boolean_member_with_default(params, "module_order", FALSE);
  const gboolean ok = dt_styles_create_from_image(name, desc, imgid, nums, order);
  g_list_free(nums);
  if(!ok)
  {
    *err = g_strdup("darktable couldn't create the style");
    return FALSE;
  }
  json_builder_set_member_name(b, "name");
  json_builder_add_string_value(b, name);
  JsonObject *q = json_object_new();
  json_object_set_string_member(q, "filter", name);
  // the new style's items, as style_list shows them
  JsonBuilder *lb = json_builder_new();
  json_builder_begin_object(lb);
  _style_list(q, lb, err);
  json_builder_end_object(lb);
  JsonNode *root = json_builder_get_root(lb);
  JsonArray *all = json_object_get_array_member(json_node_get_object(root), "styles");
  for(guint i = 0; i < json_array_get_length(all); i++)
  {
    JsonObject *o = json_array_get_object_element(all, i);
    if(g_strcmp0(json_object_get_string_member(o, "name"), name)) continue;
    json_builder_set_member_name(b, "items");
    json_builder_add_value(b, json_node_copy(json_object_get_member(o, "items")));
  }
  json_node_unref(root);
  g_object_unref(lb);
  json_object_unref(q);
  return TRUE;
}

static gboolean _style_delete(JsonObject *params, JsonBuilder *b, gchar **err)
{
  const char *name = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
  if(!name || !dt_styles_exists(name))
  {
    *err = g_strdup_printf("no style named '%s' (style_list)", name ? name : "");
    return FALSE;
  }
  dt_styles_delete_by_name(name);
  json_builder_set_member_name(b, "deleted");
  json_builder_add_string_value(b, name);
  return TRUE;
}

typedef struct _paste_t
{
  dt_imgid_t from;
  gboolean merge, order, full;
  GList *nums;
} _paste_t;

static gboolean _paste_one(const dt_imgid_t imgid, const gboolean darkroom, gpointer data)
{
  const _paste_t *p = data;
  // writes the darkroom's history first and reloads it after when imgid
  // is the darkroom's image
  return dt_history_copy_and_paste_on_image(p->from, imgid, p->merge, p->nums, p->order, p->full, TRUE);
}

// one image's saved edit onto others, as the lighttable's copy and paste:
// all of it (as "copy"), or the modules asked for (as "selective copy"),
// appended to each edit or replacing it
static gboolean _history_paste(JsonObject *params, const dt_imgid_t current, GArray *changed, JsonBuilder *b,
                               gchar **err)
{
  const dt_imgid_t from = params ? json_object_get_int_member_with_default(params, "from", NO_IMGID) : NO_IMGID;
  if(!dt_is_valid_imgid(from) || !_image_exists(from))
  {
    *err = g_strdup_printf("no source image %d (from)", from);
    return FALSE;
  }
  const _session_t *s = _session_find(from);
  if(s && !s->gui && s->dirty)
  {
    *err = g_strdup_printf("image %d has unsaved changes and pasting takes the saved edit: save first", from);
    return FALSE;
  }
  const char *mode = json_object_get_string_member_with_default(params, "mode", "append");
  if(g_strcmp0(mode, "append") && g_strcmp0(mode, "overwrite"))
  {
    *err = g_strdup("mode is append or overwrite");
    return FALSE;
  }
  _paste_t p = { from, !g_strcmp0(mode, "append"),
                 json_object_get_boolean_member_with_default(params, "module_order", FALSE), FALSE, NULL };
  JsonArray *modules = json_object_has_member(params, "modules")
                       && JSON_NODE_HOLDS_ARRAY(json_object_get_member(params, "modules"))
                         ? json_object_get_array_member(params, "modules") : NULL;
  if(modules)
  {
    p.nums = _history_nums(from, modules, FALSE, err);
    if(!p.nums) return FALSE;
    // modules picked by hand go even when darktable's copy would skip them
    // (dt_history_copy_parts)
    p.full = TRUE;
  }
  GArray *ids = _targets(params, current, from, err);
  if(!ids)
  {
    g_list_free(p.nums);
    return FALSE;
  }
  if(s && s->gui) dt_dev_write_history(s->dev);
  const gboolean ok = _edit_saved(ids, _paste_one, &p, changed, b, err);
  g_array_free(ids, TRUE);
  g_list_free(p.nums);
  return ok;
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

// an export's settings and target, decided on the server's thread (the
// format modules read darktablerc, which the request's quality overrides
// for the moment); the writing can run on another (_export_write)
typedef struct _export_t
{
  dt_imgid_t imgid;
  dt_imageio_module_format_t *format;
  dt_imageio_module_storage_t *storage;
  dt_imageio_module_data_t *fdata, *sdata;
  gboolean hq, upscale, skipped, saved, failed;
  dt_export_metadata_t metadata;
  dt_colorspaces_color_profile_type_t icc_type;
  gchar *icc_filename;
  dt_iop_color_intent_t icc_intent;
  gchar *file;
  gint64 t0;
} _export_t;

static void _export_free(_export_t *e)
{
  if(!e) return;
  g_list_free_full(e->metadata.list, g_free);
  g_free(e->icc_filename);
  if(e->sdata) e->storage->free_params(e->storage, e->sdata);
  if(e->fdata) e->format->free_params(e->format, e->fdata);
  g_free(e->file);
  g_free(e);
}

static _export_t *_export_prepare(JsonObject *params, const dt_imgid_t current, gchar **err)
{
  const dt_imgid_t imgid = params && json_object_has_member(params, "imgid")
    ? json_object_get_int_member(params, "imgid") : current;
  if(!dt_is_valid_imgid(imgid) || !_image_exists(imgid))
  {
    *err = g_strdup_printf("no image with id %d", imgid);
    return NULL;
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
    return NULL;
  }
  if(max_w < -1 || max_h < -1)
  {
    *err = g_strdup("max_width/max_height: pixels, 0 for no limit");
    return NULL;
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
    return NULL;
  }
  if(style && *style && !dt_styles_exists(style))
  {
    *err = g_strdup_printf("no style '%s'", style);
    return NULL;
  }

  // the export reads the saved history: unsaved changes are saved first on
  // request, else refused. the darkroom's edit is saved as its autosave does
  _session_t *s = _session_find(imgid);
  gboolean saved = FALSE;
  if(s && s->gui)
    dt_dev_write_history(s->dev);
  else if(s && s->dirty)
  {
    if(!save)
    {
      *err = g_strdup("the image has unsaved changes, and export uses the saved edit: save first,"
                      " or pass save: true");
      return NULL;
    }
    _save_session(s);
    saved = TRUE;
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
    return NULL;
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
  _export_t *e = g_new0(_export_t, 1);
  e->imgid = imgid;
  e->format = format;
  e->storage = storage;
  e->fdata = fdata;
  e->sdata = sdata;
  e->hq = hq;
  e->upscale = upscale;
  e->skipped = skipped;
  e->saved = saved;
  e->metadata = metadata;
  e->icc_type = icc_type;
  e->icc_filename = icc_filename;
  e->icc_intent = icc_intent;
  e->file = file;
  e->t0 = t0;
  if(!file && !skipped)
  {
    _export_free(e);
    return NULL;
  }
  return e;
}

// as the disk storage's store() does
static void _export_write(_export_t *e)
{
  e->failed = e->file && dt_imageio_export(e->imgid, e->file, e->format, e->fdata, e->hq, e->upscale, FALSE, 1.0,
                                           TRUE, FALSE, e->icc_type, e->icc_filename, e->icc_intent,
                                           e->storage, e->sdata, 1, 1, &e->metadata);
}

static gboolean _export_finish(_export_t *e, JsonBuilder *b, gchar **err)
{
  if(e->failed)
  {
    *err = g_strdup_printf("could not export to '%s'", e->file);
    return FALSE;
  }
  if(e->file)
  {
    // as the export job: tagged exported, no longer changed
    guint tagid = 0, etagid = 0;
    dt_tag_new("darktable|changed", &tagid);
    dt_tag_new("darktable|exported", &etagid);
    dt_tag_detach(tagid, e->imgid, FALSE, FALSE);
    dt_tag_attach(etagid, e->imgid, FALSE, FALSE);
    dt_image_cache_set_export_timestamp(e->imgid);
  }
  json_builder_set_member_name(b, "imgid");
  json_builder_add_int_value(b, e->imgid);
  json_builder_set_member_name(b, "file");
  if(e->file) json_builder_add_string_value(b, e->file);
  else json_builder_add_null_value(b);
  // the conflict setting can leave an existing file alone
  json_builder_set_member_name(b, "skipped");
  json_builder_add_boolean_value(b, e->skipped);
  json_builder_set_member_name(b, "format");
  json_builder_add_string_value(b, e->format->plugin_name);
  json_builder_set_member_name(b, "saved");
  json_builder_add_boolean_value(b, e->saved);
  json_builder_set_member_name(b, "ms");
  json_builder_add_int_value(b, (g_get_monotonic_time() - e->t0) / 1000);
  return TRUE;
}

static gboolean _export(JsonObject *params, const dt_imgid_t current, gboolean *saved,
                        JsonBuilder *b, gchar **err)
{
  _export_t *e = _export_prepare(params, current, err);
  if(!e) return FALSE;
  *saved = e->saved;
  _export_write(e);
  const gboolean ok = _export_finish(e, b, err);
  _export_free(e);
  return ok;
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
  // the cached level is the one at or above size: down to size, as the
  // mipmap cache makes its smaller levels (mipmap_cache.c)
  uint32_t w = ok ? buf.width : 0, h = ok ? buf.height : 0;
  uint8_t *scaled = NULL;
  if(ok && (w > size || h > size))
  {
    scaled = dt_alloc_aligned((size_t)size * size * 4);
    if(scaled)
      dt_iop_flip_and_zoom_8(buf.buf, buf.width, buf.height, scaled, size, size, ORIENTATION_NONE, &w, &h);
    else
      w = buf.width, h = buf.height;
  }
  // the mipmap cache keeps 8-bit levels in the byte order its own jpeg
  // writer takes (mipmap_cache.c, disk cache)
  if(ok && dt_imageio_jpeg_write(path, scaled ? scaled : buf.buf, w, h, quality, NULL, 0))
    ok = FALSE;
  dt_free_align(scaled);
  if(ok)
  {
    json_builder_set_member_name(b, "width");
    json_builder_add_int_value(b, w);
    json_builder_set_member_name(b, "height");
    json_builder_add_int_value(b, h);
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
// ---- tags, metadata, location -----------------------------------------------
// with darktable's own code, as its tagging, metadata editor and geotagging
// modules: written to the library and the sidecar; the window's panels are
// told (they listen for these signals)

static GList *_imgids(JsonObject *params, const dt_imgid_t current, gchar **err)
{
  GList *imgs = NULL;
  JsonArray *a = params && json_object_has_member(params, "imgids")
                 && JSON_NODE_HOLDS_ARRAY(json_object_get_member(params, "imgids"))
                   ? json_object_get_array_member(params, "imgids") : NULL;
  if(a)
    for(guint i = 0; i < json_array_get_length(a); i++)
      imgs = g_list_append(imgs, GINT_TO_POINTER(json_array_get_int_element(a, i)));
  else if(dt_is_valid_imgid(current))
    imgs = g_list_append(imgs, GINT_TO_POINTER(current));
  if(!imgs) *err = g_strdup("no image: pass imgids, or open one first");
  for(GList *l = imgs; l && !*err; l = g_list_next(l))
    if(!_image_exists(GPOINTER_TO_INT(l->data)))
      *err = g_strdup_printf("no image with id %d", GPOINTER_TO_INT(l->data));
  if(*err)
  {
    g_list_free(imgs);
    return NULL;
  }
  return imgs;
}

static void _imgs_changed(GList *imgs, JsonBuilder *b)
{
  for(GList *l = imgs; l; l = g_list_next(l)) dt_image_synch_xmp(GPOINTER_TO_INT(l->data));
  json_builder_set_member_name(b, "imgids");
  json_builder_begin_array(b);
  for(GList *l = imgs; l; l = g_list_next(l)) json_builder_add_int_value(b, GPOINTER_TO_INT(l->data));
  json_builder_end_array(b);
}

// a metadata field by its name in the metadata editor ("title") or its xmp
// key ("Xmp.dc.title"); darktable's internal ones are left out
static const dt_metadata_t *_metadata_field(const char *name)
{
  for(GList *l = dt_metadata_get_list(); l; l = g_list_next(l))
  {
    const dt_metadata_t *md = l->data;
    if(!md->internal && (!g_strcmp0(md->name, name) || !g_strcmp0(md->tagname, name))) return md;
  }
  return NULL;
}

// an image's tags (not darktable's own "darktable|..."), metadata and
// location
static gboolean _image_metadata(JsonObject *params, const dt_imgid_t current, JsonBuilder *b, gchar **err)
{
  const dt_imgid_t imgid = params && json_object_has_member(params, "imgid")
    ? json_object_get_int_member(params, "imgid") : current;
  if(!dt_is_valid_imgid(imgid) || !_image_exists(imgid))
  {
    *err = g_strdup_printf("no image with id %d", imgid);
    return FALSE;
  }
  json_builder_set_member_name(b, "imgid");
  json_builder_add_int_value(b, imgid);
  GList *tags = NULL;
  dt_tag_get_attached(imgid, &tags, TRUE);
  json_builder_set_member_name(b, "tags");
  json_builder_begin_array(b);
  for(GList *l = tags; l; l = g_list_next(l))
    json_builder_add_string_value(b, ((dt_tag_t *)l->data)->tag);
  json_builder_end_array(b);
  dt_tag_free_result(&tags);
  json_builder_set_member_name(b, "metadata");
  json_builder_begin_object(b);
  for(GList *l = dt_metadata_get_list(); l; l = g_list_next(l))
  {
    const dt_metadata_t *md = l->data;
    if(md->internal) continue;
    uint32_t count = 0;
    GList *v = dt_metadata_get(imgid, md->tagname, &count);
    json_builder_set_member_name(b, md->name);
    json_builder_add_string_value(b, v ? (const char *)v->data : "");
    g_list_free_full(v, g_free);
  }
  json_builder_end_object(b);
  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  json_builder_set_member_name(b, "location");
  if(img && !isnan(img->geoloc.latitude) && !isnan(img->geoloc.longitude))
  {
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "latitude");
    json_builder_add_double_value(b, img->geoloc.latitude);
    json_builder_set_member_name(b, "longitude");
    json_builder_add_double_value(b, img->geoloc.longitude);
    json_builder_set_member_name(b, "elevation");
    if(isnan(img->geoloc.elevation)) json_builder_add_null_value(b);
    else json_builder_add_double_value(b, img->geoloc.elevation);
    json_builder_end_object(b);
  }
  else
    json_builder_add_null_value(b);
  dt_image_cache_read_release(img);
  return TRUE;
}

// attach and detach tags by name ("places|france|paris": the hierarchy
// darktable's tagging module shows), creating new ones
static gboolean _set_tags(JsonObject *params, const dt_imgid_t current, JsonBuilder *b, gchar **err)
{
  JsonArray *attach = params && json_object_has_member(params, "attach")
                      && JSON_NODE_HOLDS_ARRAY(json_object_get_member(params, "attach"))
                        ? json_object_get_array_member(params, "attach") : NULL;
  JsonArray *detach = params && json_object_has_member(params, "detach")
                      && JSON_NODE_HOLDS_ARRAY(json_object_get_member(params, "detach"))
                        ? json_object_get_array_member(params, "detach") : NULL;
  if(!attach && !detach)
  {
    *err = g_strdup("needs attach and/or detach: lists of tag names");
    return FALSE;
  }
  for(int pass = 0; pass < 2; pass++)
  {
    JsonArray *a = pass ? detach : attach;
    for(guint i = 0; a && i < json_array_get_length(a); i++)
    {
      JsonNode *n = json_array_get_element(a, i);
      const char *name = JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING
                         ? json_node_get_string(n) : NULL;
      if(!name || !*name || g_str_has_prefix(name, "darktable|"))
      {
        *err = g_strdup("tag names are non-empty and not darktable's own (darktable|...)");
        return FALSE;
      }
    }
  }
  GList *imgs = _imgids(params, current, err);
  if(!imgs) return FALSE;
  for(guint i = 0; attach && i < json_array_get_length(attach); i++)
  {
    guint tagid = 0;
    dt_tag_new(json_array_get_string_element(attach, i), &tagid);
    dt_tag_attach_images(tagid, imgs, FALSE);
  }
  for(guint i = 0; detach && i < json_array_get_length(detach); i++)
  {
    guint tagid = 0;
    if(dt_tag_exists(json_array_get_string_element(detach, i), &tagid))
      dt_tag_detach_images(tagid, imgs, FALSE);
  }
  if(_in_gui) DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);
  _imgs_changed(imgs, b);
  g_list_free(imgs);
  return TRUE;
}

// metadata fields by name; "" clears one
static gboolean _set_metadata(JsonObject *params, const dt_imgid_t current, JsonBuilder *b, gchar **err)
{
  JsonObject *values = params && json_object_has_member(params, "values")
                       && JSON_NODE_HOLDS_OBJECT(json_object_get_member(params, "values"))
                         ? json_object_get_object_member(params, "values") : NULL;
  GList *names = values ? json_object_get_members(values) : NULL;
  if(!names) *err = g_strdup("needs values: {field: text}");
  for(GList *l = names; l && !*err; l = g_list_next(l))
  {
    JsonNode *v = json_object_get_member(values, l->data);
    if(!_metadata_field(l->data))
      *err = g_strdup_printf("no metadata field '%s' (image_metadata lists them)", (const char *)l->data);
    else if(!JSON_NODE_HOLDS_VALUE(v) || json_node_get_value_type(v) != G_TYPE_STRING)
      *err = g_strdup_printf("'%s' takes text", (const char *)l->data);
  }
  GList *imgs = *err ? NULL : _imgids(params, current, err);
  if(!imgs)
  {
    g_list_free(names);
    return FALSE;
  }
  for(GList *i = imgs; i; i = g_list_next(i))
    for(GList *l = names; l; l = g_list_next(l))
      dt_metadata_set(GPOINTER_TO_INT(i->data), _metadata_field(l->data)->tagname,
                      json_object_get_string_member(values, l->data), FALSE);
  g_list_free(names);
  if(_in_gui) DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_METADATA_CHANGED, DT_METADATA_SIGNAL_NEW_VALUE);
  _imgs_changed(imgs, b);
  g_list_free(imgs);
  return TRUE;
}

// the location, as the geotagging module sets it; clear removes it
static gboolean _set_location(JsonObject *params, const dt_imgid_t current, JsonBuilder *b, gchar **err)
{
  const gboolean clear = params ? json_object_get_boolean_member_with_default(params, "clear", FALSE) : FALSE;
  dt_image_geoloc_t loc = { NAN, NAN, NAN };
  if(!clear)
  {
    if(!params || !json_object_has_member(params, "latitude") || !json_object_has_member(params, "longitude"))
    {
      *err = g_strdup("needs latitude and longitude (degrees; elevation in m optional), or clear: true");
      return FALSE;
    }
    loc.latitude = json_object_get_double_member(params, "latitude");
    loc.longitude = json_object_get_double_member(params, "longitude");
    if(json_object_has_member(params, "elevation") && !json_object_get_null_member(params, "elevation"))
      loc.elevation = json_object_get_double_member(params, "elevation");
    if(fabs(loc.latitude) > 90.0 || fabs(loc.longitude) > 180.0)
    {
      *err = g_strdup("latitude is -90..90 and longitude -180..180");
      return FALSE;
    }
  }
  GList *imgs = _imgids(params, current, err);
  if(!imgs) return FALSE;
  dt_image_set_locations(imgs, &loc, FALSE);
  if(_in_gui) DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_GEOTAG_CHANGED, g_list_copy(imgs), 0);
  _imgs_changed(imgs, b);
  g_list_free(imgs);
  return TRUE;
}

// the library's tags with how many images carry each (not darktable's own)
static gboolean _tag_list(JsonObject *params, JsonBuilder *b, gchar **err)
{
  const char *filter = params ? json_object_get_string_member_with_default(params, "filter", "") : "";
  gchar *like = g_strdup_printf("%%%s%%", filter);
  sqlite3_stmt *st;
  sqlite3_prepare_v2(dt_database_get(darktable.db),
                     "SELECT t.name, COUNT(ti.imgid)"
                     " FROM data.tags AS t"
                     " LEFT JOIN main.tagged_images AS ti ON ti.tagid = t.id"
                     " WHERE t.name NOT LIKE 'darktable|%' AND t.name LIKE ?1"
                     " GROUP BY t.id"
                     " ORDER BY t.name",
                     -1, &st, NULL);
  sqlite3_bind_text(st, 1, like, -1, SQLITE_TRANSIENT);
  json_builder_set_member_name(b, "tags");
  json_builder_begin_array(b);
  while(sqlite3_step(st) == SQLITE_ROW)
  {
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, (const char *)sqlite3_column_text(st, 0));
    json_builder_set_member_name(b, "images");
    json_builder_add_int_value(b, sqlite3_column_int(st, 1));
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  sqlite3_finalize(st);
  g_free(like);
  return TRUE;
}

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

// what a render shows: the region of the (scaled) image it processed
typedef struct _view_t
{
  int x, y, w, h;
  double scale, pw, ph;              // pw, ph: full-size image as rendered
  gboolean uncropped, zoomed;
  gint64 ms;
} _view_t;

// run the pipe as render asks (fitted inside max_w x max_h, or zoomed on a
// region, uncropped or not), leaving the result in the pipe's backbuf (BGRA)
static gboolean _process_view(JsonObject *params, const int max_w, const int max_h, _view_t *v, gchar **err)
{
  const gint64 t0 = g_get_monotonic_time();
  // history_end: the image as it was at that step (0 = original), for
  // before/after, without moving the edit's own history_end (popping keeps
  // the later items, as undo does until the next edit)
  const int end_now = _cur->dev->history_end;
  const int end_view = params ? json_object_get_int_member_with_default(params, "history_end", end_now) : end_now;
  if(end_view < 0 || end_view > (int)g_list_length(_cur->dev->history))
  {
    *err = g_strdup_printf("history_end must be 0..%d", g_list_length(_cur->dev->history));
    return FALSE;
  }
  if(end_view != end_now)
  {
    dt_dev_pop_history_items_ext(_cur->dev, end_view);
    _cur->pipe_changed = TRUE;
  }
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
  v->pw = _cur->pipe.processed_width;
  v->ph = _cur->pipe.processed_height;
  v->x = v->y = 0;
  v->zoomed = zoom > 0.0;
  v->uncropped = crop != NULL;
  if(v->zoomed)
  {
    v->scale = CLAMP(zoom, 0.01, 2.0);
    const double cx = params ? json_object_get_double_member_with_default(params, "center_x", 0.5) : 0.5;
    const double cy = params ? json_object_get_double_member_with_default(params, "center_y", 0.5) : 0.5;
    v->w = MIN(max_w, (int)floor(v->scale * v->pw));
    v->h = MIN(max_h, (int)floor(v->scale * v->ph));
    v->x = CLAMP((int)round(cx * v->scale * v->pw - v->w / 2.0), 0, (int)floor(v->scale * v->pw) - v->w);
    v->y = CLAMP((int)round(cy * v->scale * v->ph - v->h / 2.0), 0, (int)floor(v->scale * v->ph) - v->h);
  }
  else
  {
    v->scale = fmin(1.0, fmin(max_w / v->pw, max_h / v->ph));
    v->w = floor(v->scale * v->pw);
    v->h = floor(v->scale * v->ph);
  }

  // as a non-hq export does: downscale right after demosaic, not in finalscale
  dt_dev_pixelpipe_iop_t *finalscale = NULL;
  for(GList *n = g_list_last(_cur->pipe.nodes); n; n = g_list_previous(n))
  {
    dt_dev_pixelpipe_iop_t *piece = n->data;
    if(dt_iop_module_is_finalscale(piece->module)) { finalscale = piece; break; }
  }
  if(finalscale) finalscale->enabled = FALSE;
  dt_dev_pixelpipe_process(&_cur->pipe, _cur->dev, v->x, v->y, v->w, v->h, v->scale, DT_DEVICE_NONE);
  if(finalscale) finalscale->enabled = TRUE;
  if(crop)
  {
    crop->enabled = TRUE;
    dt_dev_pixelpipe_get_dimensions(&_cur->pipe, _cur->dev, _cur->pipe.iwidth, _cur->pipe.iheight,
                                    &_cur->pipe.processed_width, &_cur->pipe.processed_height);
  }
  if(end_view != end_now)
  {
    dt_dev_pop_history_items_ext(_cur->dev, end_now);
    _cur->pipe_changed = TRUE;
  }
  v->ms = (g_get_monotonic_time() - t0) / 1000;
  if(!_cur->pipe.backbuf || _cur->pipe.backbuf_width != v->w || _cur->pipe.backbuf_height != v->h)
  {
    *err = g_strdup("the pipe produced no output");
    return FALSE;
  }
  return TRUE;
}

static void _add_view(JsonBuilder *b, const _view_t *v)
{
  json_builder_set_member_name(b, "width");
  json_builder_add_int_value(b, v->w);
  json_builder_set_member_name(b, "height");
  json_builder_add_int_value(b, v->h);
  json_builder_set_member_name(b, "uncropped");
  json_builder_add_boolean_value(b, v->uncropped);
  if(v->zoomed)
  {
    // the region shown, in fractions of the whole image
    json_builder_set_member_name(b, "zoom");
    json_builder_add_double_value(b, v->scale);
    json_builder_set_member_name(b, "region");
    _add_box(b, (float[4]){ v->x / (v->scale * v->pw), v->y / (v->scale * v->ph),
                            (v->x + v->w) / (v->scale * v->pw), (v->y + v->h) / (v->scale * v->ph) });
  }
  json_builder_set_member_name(b, "process_ms");
  json_builder_add_int_value(b, v->ms);
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
  _view_t v;
  if(!_process_view(params, max_w, max_h, &v, err)) return FALSE;
  const gint64 t1 = g_get_monotonic_time();

  // backbuf is a cache line in display byte order (BGRA): convert a copy
  const size_t npix = (size_t)v.w * v.h;
  uint8_t *rgba = g_malloc(npix * 4);
  const uint8_t *src = _cur->pipe.backbuf;
  for(size_t k = 0; k < npix; k++)
  {
    rgba[4 * k + 0] = src[4 * k + 2];
    rgba[4 * k + 1] = src[4 * k + 1];
    rgba[4 * k + 2] = src[4 * k + 0];
    rgba[4 * k + 3] = 255;
  }
  const int jerr = dt_imageio_jpeg_write(path, rgba, v.w, v.h, quality, NULL, 0);
  g_free(rgba);
  if(jerr)
  {
    *err = g_strdup_printf("cannot write %s", path);
    return FALSE;
  }
  _add_view(b, &v);
  json_builder_set_member_name(b, "jpeg_ms");
  json_builder_add_int_value(b, (g_get_monotonic_time() - t1) / 1000);
  return TRUE;
}

// ---- readouts ---------------------------------------------------------------
// what the rendered image holds: values at points and in boxes, histograms
// and clipping. these are the output (display-referred sRGB) as the
// darkroom shows it, not values inside the pipe

static double _srgb_to_linear(const double c)
{
  return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static void _rgb_to_lab(const double rgb[3], double lab[3])
{
  const double r = _srgb_to_linear(rgb[0]), g = _srgb_to_linear(rgb[1]), b = _srgb_to_linear(rgb[2]);
  // sRGB (D65) to XYZ, relative to D65 white
  const double xyz[3] = { (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047,
                          0.2126 * r + 0.7152 * g + 0.0722 * b,
                          (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883 };
  double f[3];
  for(int k = 0; k < 3; k++)
    f[k] = xyz[k] > 216.0 / 24389.0 ? cbrt(xyz[k]) : (24389.0 / 27.0 * xyz[k] + 16.0) / 116.0;
  lab[0] = 116.0 * f[1] - 16.0;
  lab[1] = 500.0 * (f[0] - f[1]);
  lab[2] = 200.0 * (f[1] - f[2]);
}

// the mean over pixels [x0, x1) x [y0, y1) of the BGRA view
static void _add_area(JsonBuilder *b, const uint8_t *buf, const int w, int x0, int y0, int x1, int y1)
{
  double sum[3] = { 0 }, lmin = 1.0, lmax = 0.0;
  size_t n = 0;
  for(int y = y0; y < y1; y++)
    for(int x = x0; x < x1; x++)
    {
      const uint8_t *p = buf + 4 * ((size_t)y * w + x);
      const double rgb[3] = { p[2] / 255.0, p[1] / 255.0, p[0] / 255.0 };
      for(int k = 0; k < 3; k++) sum[k] += rgb[k];
      const double l = 0.2126 * _srgb_to_linear(rgb[0]) + 0.7152 * _srgb_to_linear(rgb[1])
                       + 0.0722 * _srgb_to_linear(rgb[2]);
      lmin = MIN(lmin, l);
      lmax = MAX(lmax, l);
      n++;
    }
  if(!n)
  {
    json_builder_add_null_value(b);
    return;
  }
  const double rgb[3] = { sum[0] / n, sum[1] / n, sum[2] / n };
  double lab[3];
  _rgb_to_lab(rgb, lab);
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "rgb");
  json_builder_begin_array(b);
  for(int k = 0; k < 3; k++) json_builder_add_double_value(b, round(rgb[k] * 1e4) / 1e4);
  json_builder_end_array(b);
  json_builder_set_member_name(b, "lab");
  json_builder_begin_array(b);
  for(int k = 0; k < 3; k++) json_builder_add_double_value(b, round(lab[k] * 100) / 100);
  json_builder_end_array(b);
  json_builder_set_member_name(b, "luminance_min");
  json_builder_add_double_value(b, round(lmin * 1e4) / 1e4);
  json_builder_set_member_name(b, "luminance_max");
  json_builder_add_double_value(b, round(lmax * 1e4) / 1e4);
  json_builder_set_member_name(b, "pixels");
  json_builder_add_int_value(b, n);
  json_builder_end_object(b);
}

static gboolean _sample_session(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  const int max_w = params ? json_object_get_int_member_with_default(params, "width", 1024) : 1024;
  const int max_h = params ? json_object_get_int_member_with_default(params, "height", 1024) : 1024;
  const int radius = params ? CLAMP(json_object_get_int_member_with_default(params, "radius", 2), 0, 200) : 2;
  const int bins = params ? CLAMP(json_object_get_int_member_with_default(params, "bins", 64), 2, 256) : 64;
  if(max_w <= 0 || max_h <= 0)
  {
    *err = g_strdup("sample needs width > 0 and height > 0");
    return FALSE;
  }
  _view_t v;
  if(!_process_view(params, max_w, max_h, &v, err)) return FALSE;
  const uint8_t *buf = _cur->pipe.backbuf;
  // image fractions to view pixels
  const double kx = v.scale * v.pw, ky = v.scale * v.ph;
  _add_view(b, &v);

  JsonArray *points = params && json_object_has_member(params, "points")
                      && JSON_NODE_HOLDS_ARRAY(json_object_get_member(params, "points"))
                        ? json_object_get_array_member(params, "points") : NULL;
  json_builder_set_member_name(b, "points");
  json_builder_begin_array(b);
  for(guint k = 0; points && k < json_array_get_length(points); k++)
  {
    JsonArray *p = JSON_NODE_HOLDS_ARRAY(json_array_get_element(points, k))
                   ? json_array_get_array_element(points, k) : NULL;
    if(!p || json_array_get_length(p) != 2)
    {
      json_builder_add_null_value(b);
      continue;
    }
    const int px = (int)floor(json_array_get_double_element(p, 0) * kx) - v.x;
    const int py = (int)floor(json_array_get_double_element(p, 1) * ky) - v.y;
    if(px < 0 || py < 0 || px >= v.w || py >= v.h)
      json_builder_add_null_value(b);   // outside what was rendered
    else
      _add_area(b, buf, v.w, MAX(0, px - radius), MAX(0, py - radius),
                MIN(v.w, px + radius + 1), MIN(v.h, py + radius + 1));
  }
  json_builder_end_array(b);

  JsonArray *boxes = params && json_object_has_member(params, "boxes")
                     && JSON_NODE_HOLDS_ARRAY(json_object_get_member(params, "boxes"))
                       ? json_object_get_array_member(params, "boxes") : NULL;
  json_builder_set_member_name(b, "boxes");
  json_builder_begin_array(b);
  for(guint k = 0; boxes && k < json_array_get_length(boxes); k++)
  {
    JsonObject *o = JSON_NODE_HOLDS_OBJECT(json_array_get_element(boxes, k))
                    ? json_array_get_object_element(boxes, k) : NULL;
    if(!o)
    {
      json_builder_add_null_value(b);
      continue;
    }
    const int x0 = CLAMP((int)floor(json_object_get_double_member_with_default(o, "left", 0) * kx) - v.x, 0, v.w);
    const int y0 = CLAMP((int)floor(json_object_get_double_member_with_default(o, "top", 0) * ky) - v.y, 0, v.h);
    const int x1 = CLAMP((int)ceil(json_object_get_double_member_with_default(o, "right", 1) * kx) - v.x, 0, v.w);
    const int y1 = CLAMP((int)ceil(json_object_get_double_member_with_default(o, "bottom", 1) * ky) - v.y, 0, v.h);
    _add_area(b, buf, v.w, x0, y0, x1, y1);
  }
  json_builder_end_array(b);

  // histograms of R, G, B and luminance (8-bit output, bins), and how much
  // is clipped: any channel at 255, or all at 0
  guint64 *hist = g_new0(guint64, 4 * bins);
  guint64 high = 0, low = 0;
  const size_t npix = (size_t)v.w * v.h;
  for(size_t k = 0; k < npix; k++)
  {
    const uint8_t *p = buf + 4 * k;
    const int r = p[2], g = p[1], bl = p[0];
    const double l = 0.2126 * _srgb_to_linear(r / 255.0) + 0.7152 * _srgb_to_linear(g / 255.0)
                     + 0.0722 * _srgb_to_linear(bl / 255.0);
    // luminance binned as an sRGB-encoded value, like the channels
    const int lv = (int)round(255.0 * (l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1 / 2.4) - 0.055));
    hist[0 * bins + r * bins / 256]++;
    hist[1 * bins + g * bins / 256]++;
    hist[2 * bins + bl * bins / 256]++;
    hist[3 * bins + CLAMP(lv, 0, 255) * bins / 256]++;
    if(r == 255 || g == 255 || bl == 255) high++;
    if(r == 0 && g == 0 && bl == 0) low++;
  }
  static const char *names[] = { "red", "green", "blue", "luminance" };
  json_builder_set_member_name(b, "histogram");
  json_builder_begin_object(b);
  for(int c = 0; c < 4; c++)
  {
    json_builder_set_member_name(b, names[c]);
    json_builder_begin_array(b);
    for(int k = 0; k < bins; k++) json_builder_add_int_value(b, hist[c * bins + k]);
    json_builder_end_array(b);
  }
  json_builder_end_object(b);
  g_free(hist);
  json_builder_set_member_name(b, "clipped_highlights");
  json_builder_add_double_value(b, npix ? round(1e6 * high / (double)npix) / 1e6 : 0.0);
  json_builder_set_member_name(b, "clipped_shadows");
  json_builder_add_double_value(b, npix ? round(1e6 * low / (double)npix) / 1e6 : 0.0);
  return TRUE;
}

static gboolean _sample(JsonObject *params, JsonBuilder *b, gchar **err)
{
  if(!_cur || !_cur->gui) return _sample_session(params, b, err);
  _session_t *gui = _cur;
  _session_t *mirror = _pipe_session(err);
  if(!mirror) return FALSE;
  _cur = mirror;
  const gboolean ok = _sample_session(params, b, err);
  _cur = gui;
  return ok;
}

// ---- AI object masks --------------------------------------------------------
// what the darkroom's AI object mask does (develop/masks/object.c): SAM
// segments the photo around points the user clicks, and the mask is traced
// into path shapes in a group on the module's mask (holes subtracted). the
// darkroom's version works on its own image and preview pipe; this one on
// the session's pipe, so it runs headless too

#ifdef HAVE_AI
static dt_ai_environment_t *_seg_env = NULL;
static dt_seg_context_t *_seg = NULL;
static gchar *_seg_key = NULL;        // which photo and geometry _seg has encoded
static int _seg_w = 0, _seg_h = 0;
static int _seg_job = 0;              // the mask_ai_encode job encoding, 0 if none

// keep only the part of the mask connected to (sx, sy): what the darkroom
// does so stray blobs don't become shapes (object.c: _keep_seed_component)
static void _keep_component(float *mask, const int w, const int h, const float thr, const int sx, const int sy)
{
  if(sx < 0 || sy < 0 || sx >= w || sy >= h || mask[(size_t)sy * w + sx] < thr) return;
  uint8_t *keep = g_malloc0((size_t)w * h);
  int *stack = g_malloc(sizeof(int) * 2 * (size_t)w * h);
  size_t top = 0;
  stack[top++] = sx;
  stack[top++] = sy;
  keep[(size_t)sy * w + sx] = 1;
  while(top)
  {
    const int y = stack[--top], x = stack[--top];
    static const int d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    for(int k = 0; k < 4; k++)
    {
      const int nx = x + d[k][0], ny = y + d[k][1];
      if(nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
      const size_t i = (size_t)ny * w + nx;
      if(keep[i] || mask[i] < thr) continue;
      keep[i] = 1;
      stack[top++] = nx;
      stack[top++] = ny;
    }
  }
  for(size_t i = 0; i < (size_t)w * h; i++)
    if(!keep[i]) mask[i] = 0.0f;
  g_free(stack);
  g_free(keep);
}
#endif

#ifdef HAVE_AI
// the AI mask model, loaded once
static gboolean _seg_model(gchar **err)
{
  if(!_seg)
  {
    if(!_seg_env) _seg_env = dt_ai_env_init(NULL);
    gchar *model = dt_ai_models_get_active_for_task("mask");
    _seg = model && _seg_env ? dt_seg_load(_seg_env, model) : NULL;
    g_free(model);
  }
  if(!_seg) *err = g_strdup("no AI mask model could be loaded (darktable's preferences: AI models)");
  return _seg != NULL;
}

// the photo as rendered at darktable's size for AI masks (1536 by
// default), RGB, and the key its encoding is kept under (the photo and its
// geometry); rgb stays NULL when that encoding is the current one. leaves
// _cur at the session whose pipe rendered it
static gboolean _seg_input(_session_t **sp, uint8_t **rgb, int *w, int *h, gchar **key, gchar **err)
{
  _session_t *s = _pipe_session(err);
  if(!s) return FALSE;
  _cur = s;
  _pipe_sync(s);
  const dt_hash_t dh = dt_dev_hash_distort_plus(s->dev, &s->pipe, 0, DT_DEV_TRANSFORM_DIR_ALL);
  *key = g_strdup_printf("%d:%" PRIu64 ":%dx%d", s->dev->image_storage.id, (uint64_t)dh,
                         s->pipe.processed_width, s->pipe.processed_height);
  *sp = s;
  *rgb = NULL;
  if(_seg_key && !g_strcmp0(*key, _seg_key) && dt_seg_is_encoded(_seg)) return TRUE;
  const int size = MAX(dt_conf_key_exists("plugins/darkroom/masks/object/render_size")
                       ? dt_conf_get_int("plugins/darkroom/masks/object/render_size") : 1536, 1024);
  _view_t v;
  if(!_process_view(NULL, size, size, &v, err))
  {
    g_free(*key);
    *key = NULL;
    return FALSE;
  }
  *rgb = g_malloc((size_t)v.w * v.h * 3);
  const uint8_t *src = s->pipe.backbuf;
  for(size_t k = 0; k < (size_t)v.w * v.h; k++)
  {
    (*rgb)[3 * k + 0] = src[4 * k + 2];
    (*rgb)[3 * k + 1] = src[4 * k + 1];
    (*rgb)[3 * k + 2] = src[4 * k + 0];
  }
  *w = v.w;
  *h = v.h;
  return TRUE;
}
#endif

static gboolean _mask_ai(JsonObject *params, JsonBuilder *b, gchar **err)
{
#ifndef HAVE_AI
  *err = g_strdup("this darktable was built without AI (USE_AI)");
  return FALSE;
#else
  dt_iop_module_t *m = _blend_module(params, err);
  if(!m) return FALSE;
  if(m->flags() & IOP_FLAGS_NO_MASKS)
  {
    *err = g_strdup_printf("module '%s' takes no drawn masks", m->op);
    return FALSE;
  }
  if(!dt_ai_registry_is_enabled())
  {
    *err = g_strdup("AI features are off in darktable's preferences (plugins/ai/enabled)");
    return FALSE;
  }
  JsonArray *pts = params && json_object_has_member(params, "points")
                   && JSON_NODE_HOLDS_ARRAY(json_object_get_member(params, "points"))
                     ? json_object_get_array_member(params, "points") : NULL;
  const guint n = pts ? json_array_get_length(pts) : 0;
  float (*p)[3] = g_malloc0_n(MAX(n, 1), sizeof(*p));
  int seed = -1;
  for(guint k = 0; k < n; k++)
  {
    JsonObject *o = JSON_NODE_HOLDS_OBJECT(json_array_get_element(pts, k))
                    ? json_array_get_object_element(pts, k) : NULL;
    p[k][0] = o ? json_object_get_double_member_with_default(o, "x", -1) : -1;
    p[k][1] = o ? json_object_get_double_member_with_default(o, "y", -1) : -1;
    p[k][2] = o && json_object_get_boolean_member_with_default(o, "include", TRUE) ? 1 : 0;
    if(!(p[k][0] >= 0 && p[k][0] <= 1 && p[k][1] >= 0 && p[k][1] <= 1))
    {
      g_free(p);
      *err = g_strdup_printf("point %u: x, y as fractions 0..1 of the photo as rendered", k);
      return FALSE;
    }
    if(p[k][2] > 0 && seed < 0) seed = k;
  }
  if(seed < 0)
  {
    g_free(p);
    *err = g_strdup("mask_ai needs points [{x, y, include}] with at least one include point on the object");
    return FALSE;
  }
  const char *combine = params ? json_object_get_string_member_with_default(params, "combine", "union") : "union";
  static const struct { const char *name; int state; } ops[] = {
    { "union", DT_MASKS_STATE_UNION }, { "intersection", DT_MASKS_STATE_INTERSECTION },
    { "difference", DT_MASKS_STATE_DIFFERENCE }, { "exclusion", DT_MASKS_STATE_EXCLUSION }, { NULL, 0 } };
  int op = -1;
  for(int k = 0; ops[k].name; k++)
    if(!g_ascii_strcasecmp(combine, ops[k].name)) op = ops[k].state;
  if(op < 0)
  {
    g_free(p);
    *err = g_strdup("combine: union, intersection, difference or exclusion");
    return FALSE;
  }
  const gboolean inverted = params ? json_object_get_boolean_member_with_default(params, "inverted", FALSE) : FALSE;
  const gint64 t0 = g_get_monotonic_time();

  if(_seg_job)
  {
    g_free(p);
    *err = g_strdup_printf("the photo is being encoded for AI masks (job %d): wait for it", _seg_job);
    return FALSE;
  }
  if(!_seg_model(err))
  {
    g_free(p);
    return FALSE;
  }

  // the photo as rendered, encoded at darktable's size (1536 by default);
  // kept while the photo and its geometry stay the same
  _session_t *gui = _cur;
  _session_t *s = NULL;
  uint8_t *rgb = NULL;
  int rw = 0, rh = 0;
  gchar *key = NULL;
  if(!_seg_input(&s, &rgb, &rw, &rh, &key, err))
  {
    _cur = gui;
    g_free(p);
    return FALSE;
  }
  gboolean encoded = rgb == NULL;
  if(rgb)
  {
    dt_seg_reset_encoding(_seg);
    encoded = dt_seg_encode_image(_seg, rgb, rw, rh);
    g_free(rgb);
    _seg_w = rw;
    _seg_h = rh;
    g_free(_seg_key);
    _seg_key = encoded ? g_strdup(key) : NULL;
  }
  g_free(key);
  if(!encoded)
  {
    _cur = gui;
    g_free(p);
    *err = g_strdup("the AI mask model could not encode the photo");
    return FALSE;
  }
  const gint64 t1 = g_get_monotonic_time();

  // the points in encoding pixels, then the mask
  dt_seg_point_t *sp = g_new(dt_seg_point_t, n);
  for(guint k = 0; k < n; k++)
  {
    sp[k].x = p[k][0] * _seg_w;
    sp[k].y = p[k][1] * _seg_h;
    sp[k].label = (int)p[k][2];
  }
  dt_seg_reset_prev_mask(_seg);
  int mw = 0, mh = 0;
  float *mask = dt_seg_compute_mask(_seg, sp, n, &mw, &mh);
  g_free(sp);
  const float thr = CLAMP(dt_conf_key_exists("plugins/darkroom/masks/object/threshold")
                          ? dt_conf_get_float("plugins/darkroom/masks/object/threshold") : 0.5f, 0.3f, 0.9f);
  if(!mask || !mw || !mh)
  {
    _cur = gui;
    g_free(mask);
    g_free(p);
    *err = g_strdup("the AI mask model found nothing");
    return FALSE;
  }
  _keep_component(mask, mw, mh, thr, (int)(p[seed][0] * mw), (int)(p[seed][1] * mh));
  g_free(p);
  size_t area = 0;
  for(size_t k = 0; k < (size_t)mw * mh; k++)
  {
    if(mask[k] >= thr) area++;
    mask[k] = 1.0f - mask[k];          // ras2forms: below threshold is inside
  }
  GList *signs = NULL;
  GList *forms = ras2forms(mask, mw, mh, NULL, 1.0f - thr,
                           dt_conf_key_exists("plugins/darkroom/masks/object/cleanup")
                             ? dt_conf_get_int("plugins/darkroom/masks/object/cleanup") : 10,
                           dt_conf_key_exists("plugins/darkroom/masks/object/smoothing")
                             ? dt_conf_get_float("plugins/darkroom/masks/object/smoothing") : 0.0,
                           &signs);
  g_free(mask);
  if(!forms)
  {
    _cur = gui;
    g_list_free(signs);
    *err = g_strdup("the AI mask had no outline to trace");
    return FALSE;
  }

  // mask pixels to the pipe's input, normalized (raw space), as the
  // darkroom's _register_vectorized_forms does through its preview pipe
  const double kx = (double)s->pipe.processed_width / mw, ky = (double)s->pipe.processed_height / mh;
  for(GList *l = forms; l; l = g_list_next(l))
  {
    dt_masks_form_t *f = l->data;
    const int np = g_list_length(f->points);
    float *xy = g_new(float, 6 * MAX(np, 1));
    int i = 0;
    for(GList *q = f->points; q; q = g_list_next(q))
    {
      dt_masks_point_path_t *pt = q->data;
      xy[i++] = pt->corner[0] * kx; xy[i++] = pt->corner[1] * ky;
      xy[i++] = pt->ctrl1[0] * kx;  xy[i++] = pt->ctrl1[1] * ky;
      xy[i++] = pt->ctrl2[0] * kx;  xy[i++] = pt->ctrl2[1] * ky;
    }
    dt_dev_distort_backtransform_plus(s->dev, &s->pipe, 0, DT_DEV_TRANSFORM_DIR_ALL, xy, 3 * np);
    i = 0;
    for(GList *q = f->points; q; q = g_list_next(q))
    {
      dt_masks_point_path_t *pt = q->data;
      pt->corner[0] = xy[i++] / s->pipe.iwidth; pt->corner[1] = xy[i++] / s->pipe.iheight;
      pt->ctrl1[0] = xy[i++] / s->pipe.iwidth;  pt->ctrl1[1] = xy[i++] / s->pipe.iheight;
      pt->ctrl2[0] = xy[i++] / s->pipe.iwidth;  pt->ctrl2[1] = xy[i++] / s->pipe.iheight;
    }
    g_free(xy);
  }
  _cur = gui;

  // the paths in a group, holes subtracted, named as the darkroom names them
  dt_develop_t *dev = _cur->dev;
  int paths = 0, groups = 0;
  for(GList *l = dev->forms; l; l = g_list_next(l))
  {
    const dt_masks_form_t *f = l->data;
    if(g_str_has_prefix(f->name, _("ai object group"))) groups++;
    else if(g_str_has_prefix(f->name, _("ai object"))) paths++;
  }
  dt_masks_form_t *grp = dt_masks_create(DT_MASKS_GROUP);
  int npaths = 0;
  GList *sg = signs;
  for(GList *l = forms; l; l = g_list_next(l), sg = sg ? g_list_next(sg) : NULL)
  {
    dt_masks_form_t *f = l->data;
    snprintf(f->name, sizeof(f->name), "%s #%d", _("ai object"), ++paths);
    dev->forms = g_list_append(dev->forms, f);
    dt_masks_point_group_t *gp = dt_masks_group_add_form(grp, f);
    if(gp && sg && GPOINTER_TO_INT(sg->data) == '-')
      gp->state = (gp->state & ~DT_MASKS_STATE_UNION) | DT_MASKS_STATE_DIFFERENCE;
    npaths++;
  }
  g_list_free(forms);
  g_list_free(signs);
  if(!_mask_attach(m, grp, op, inverted, b)) return FALSE;
  // save_creation numbered it as a plain group
  snprintf(grp->name, sizeof(grp->name), "%s #%d", _("ai object group"), groups + 1);
  json_builder_set_member_name(b, "paths");
  json_builder_add_int_value(b, npaths);
  json_builder_set_member_name(b, "area");
  json_builder_add_double_value(b, round(1e4 * area / ((double)mw * mh)) / 1e4);
  json_builder_set_member_name(b, "encode_ms");
  json_builder_add_int_value(b, (t1 - t0) / 1000);
  json_builder_set_member_name(b, "ms");
  json_builder_add_int_value(b, (g_get_monotonic_time() - t0) / 1000);
  return TRUE;
#endif
}

// ---- background jobs ----------------------------------------------------------
// work that takes minutes (AI denoise) runs on a thread of its own, so the
// server keeps answering: the request replies a job id at once; job_status,
// job_list and job_cancel follow it, and every client gets a "job" event
// when it ends. the thread does the computing; reading the result into the
// library happens back on the server's thread (the main loop: the engine's,
// or darktable's window). in darktable's window a job also has a darktable
// job handle: its progress shows in darktable's progress bar, and cancel
// stops the computation; headless (darktable's control isn't running, so
// there are no handles) progress is unknown and cancel discards the result

typedef enum _job_state_t { _JOB_RUNNING, _JOB_DONE, _JOB_FAILED, _JOB_CANCELLED } _job_state_t;
static const char *_job_states[] = { "running", "done", "failed", "cancelled" };

typedef struct _job_t
{
  int id;
  const char *type;
  dt_imgid_t imgid;
  _job_state_t state;
  gint cancel;                       // set by job_cancel, read by the thread
  gint64 started, finished;
  dt_job_t *handle;                  // darktable's window only
  GThread *thread;
  gchar *error;
  dt_imgid_t new_imgid;
  gchar *file;
  // the task's own data
  gchar *src;
  float strength;
  int status;                        // the thread's result: 0 = ok
  // a picker or auto button in the darkroom (no thread)
  gchar *op;
  int prio;
  gchar *control;
  gboolean picker;
  int phase, presses, phase_presses, arms;
  float area[4];                     // a picker's box, or point in area[0..1]
  gboolean point;
  guint64 preview_mark, full_mark;   // pipe runs counted at the last step
  void *before;                      // the module's settings when it started
  guint watch;
  JsonNode *result;
  // an export, or the photo to encode for AI masks
  struct _export_t *export;
  uint8_t *rgb;
  int rw, rh;
  gchar *seg_key;
  gboolean ok;
} _job_t;

static GList *_jobs = NULL;
static int _next_job = 1;

static void _notify_job(const _job_t *j);

static gboolean _jobs_running(void)
{
  for(GList *l = _jobs; l; l = g_list_next(l))
    if(((_job_t *)l->data)->state == _JOB_RUNNING) return TRUE;
  return FALSE;
}

static void _add_job(JsonBuilder *b, const _job_t *j)
{
  json_builder_set_member_name(b, "job");
  json_builder_add_int_value(b, j->id);
  // "task", not "type": the "job" event's own type is "job"
  json_builder_set_member_name(b, "task");
  json_builder_add_string_value(b, j->type);
  json_builder_set_member_name(b, "imgid");
  json_builder_add_int_value(b, j->imgid);
  json_builder_set_member_name(b, "state");
  json_builder_add_string_value(b, _job_states[j->state]);
  const double progress = j->handle && j->state == _JOB_RUNNING ? dt_control_job_get_progress(j->handle) : -1.0;
  json_builder_set_member_name(b, "progress");
  if(j->state != _JOB_RUNNING) json_builder_add_double_value(b, 1.0);
  else if(progress >= 0.0) json_builder_add_double_value(b, round(progress * 1e3) / 1e3);
  else json_builder_add_null_value(b);
  json_builder_set_member_name(b, "elapsed_ms");
  json_builder_add_int_value(b, ((j->finished ? j->finished : g_get_monotonic_time()) - j->started) / 1000);
  if(j->result)
  {
    json_builder_set_member_name(b, "result");
    json_builder_add_value(b, json_node_copy(j->result));
  }
  if(j->state == _JOB_DONE && dt_is_valid_imgid(j->new_imgid))
  {
    json_builder_set_member_name(b, "new_imgid");
    json_builder_add_int_value(b, j->new_imgid);
    json_builder_set_member_name(b, "file");
    json_builder_add_string_value(b, j->file ? j->file : "");
  }
  if(j->error)
  {
    json_builder_set_member_name(b, "error");
    json_builder_add_string_value(b, j->error);
  }
}

static _job_t *_job_find(JsonObject *params, gchar **err)
{
  const int id = params ? json_object_get_int_member_with_default(params, "job", 0) : 0;
  for(GList *l = _jobs; l; l = g_list_next(l))
    if(((_job_t *)l->data)->id == id) return l->data;
  *err = g_strdup_printf("no job %d (job_list)", id);
  return NULL;
}

static gboolean _job_status(JsonObject *params, JsonBuilder *b, gchar **err)
{
  _job_t *j = _job_find(params, err);
  if(!j) return FALSE;
  _add_job(b, j);
  return TRUE;
}

static gboolean _job_list(JsonBuilder *b, gchar **err)
{
  json_builder_set_member_name(b, "jobs");
  json_builder_begin_array(b);
  for(GList *l = _jobs; l; l = g_list_next(l))
  {
    json_builder_begin_object(b);
    _add_job(b, l->data);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  return TRUE;
}

static gboolean _job_cancel(JsonObject *params, JsonBuilder *b, gchar **err)
{
  _job_t *j = _job_find(params, err);
  if(!j) return FALSE;
  if(j->state != _JOB_RUNNING)
  {
    *err = g_strdup_printf("job %d has already ended (%s)", j->id, _job_states[j->state]);
    return FALSE;
  }
  if(j->export)
  {
    *err = g_strdup_printf("job %d is an export, which runs to its end", j->id);
    return FALSE;
  }
  g_atomic_int_set(&j->cancel, 1);
  if(j->handle) dt_control_job_cancel(j->handle);
  _add_job(b, j);
  json_builder_set_member_name(b, "note");
  json_builder_add_string_value(b, j->handle ? "the computation stops at its next check"
                                             : "headless the computation runs to its end; its result is discarded");
  return TRUE;
}

#ifdef HAVE_AI
// the source raw's embedded JPEG, as the DNG's thumbnail (neural_restore.c:
// _extract_source_jpeg_preview)
static gboolean _source_preview(const char *src, dt_imageio_dng_preview_t *pv)
{
  uint8_t *buf = NULL;
  size_t size = 0;
  char *mime = NULL;
  memset(pv, 0, sizeof(*pv));
  if(dt_exif_get_thumbnail(src, &buf, &size, &mime) || !buf)
  {
    free(mime);
    return FALSE;
  }
  dt_imageio_jpeg_t jpg;
  const gboolean jpeg = mime && !g_strcmp0(mime, "image/jpeg")
                        && !dt_imageio_jpeg_decompress_header(buf, size, &jpg);
  free(mime);
  if(!jpeg)
  {
    free(buf);
    return FALSE;
  }
  pv->data = buf;                    // malloc'd by dt_exif_get_thumbnail: free()
  pv->len = size;
  pv->width = jpg.width;
  pv->height = jpg.height;
  return TRUE;
}

// AI denoise of one raw, as the neural restore panel's raw denoise does
// (neural_restore.c: _process_raw_denoise_one): the model runs on the
// sensor data and a DNG is written; runs on the job's thread
static gpointer _ai_denoise_thread(gpointer data)
{
  _job_t *j = data;
  dt_restore_env_t *env = dt_restore_env_init();
  const dt_image_t *cached = dt_image_cache_get(j->imgid, 'r');
  dt_image_t img = *cached;
  dt_image_cache_read_release(cached);
  const dt_restore_sensor_class_t cls = dt_restore_classify_sensor(&img);
  dt_restore_context_t *ctx = !env ? NULL
    : cls == DT_RESTORE_SENSOR_CLASS_BAYER ? dt_restore_load_rawdenoise_bayer(env)
    : cls == DT_RESTORE_SENSOR_CLASS_XTRANS ? dt_restore_load_rawdenoise_xtrans(env)
    : cls == DT_RESTORE_SENSOR_CLASS_LINEAR ? dt_restore_load_rawdenoise_linear(env) : NULL;
  j->status = 1;
  if(!ctx)
    j->error = g_strdup("the AI raw denoise model could not be loaded for this sensor");
  else if(cls == DT_RESTORE_SENSOR_CLASS_BAYER)
  {
    dt_mipmap_buffer_t mb;
    dt_mipmap_cache_get(&mb, j->imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
    const size_t npix = (size_t)img.width * img.height;
    float *cfa = mb.buf ? g_try_malloc(npix * sizeof(float)) : NULL;
    if(cfa && img.buf_dsc.datatype == TYPE_UINT16)
      for(size_t i = 0; i < npix; i++) cfa[i] = ((const uint16_t *)mb.buf)[i];
    else if(cfa && img.buf_dsc.datatype == TYPE_FLOAT)
      memcpy(cfa, mb.buf, npix * sizeof(float));
    else
    {
      g_free(cfa);
      cfa = NULL;
    }
    dt_mipmap_cache_release(&mb);
    uint16_t *out = cfa ? g_try_malloc(npix * sizeof(uint16_t)) : NULL;
    if(!cfa || !out)
      j->error = g_strdup("could not read the raw's sensor data");
    else if((j->status = dt_restore_raw_bayer(ctx, &img, cfa, img.width, img.height, out, j->strength, j->handle)))
      j->error = g_strdup("the AI raw denoise failed or was cancelled");
    else
    {
      uint8_t *exif = NULL;
      const int exif_len = dt_exif_read_blob(&exif, j->src, j->imgid, FALSE, img.width, img.height, TRUE);
      dt_imageio_dng_preview_t pv;
      const gboolean has_pv = _source_preview(j->src, &pv);
      j->status = dt_imageio_dng_write_cfa_bayer(j->file, out, img.width, img.height, &img, exif, exif_len,
                                                 has_pv ? &pv : NULL);
      free((void *)pv.data);
      free(exif);
      if(j->status) j->error = g_strdup_printf("could not write %s", j->file);
    }
    g_free(cfa);
    g_free(out);
  }
  else
  {
    // X-Trans and linear raws: darktable demosaics, the model denoises RGB
    float *rgb = NULL;
    int w = 0, h = 0;
    if((j->status = dt_restore_raw_linear(ctx, j->imgid, &rgb, &w, &h, j->strength, j->handle)) || !rgb)
    {
      j->status = 1;
      j->error = g_strdup("the AI raw denoise failed or was cancelled");
    }
    else
    {
      uint8_t *exif = NULL;
      const int exif_len = dt_exif_read_blob(&exif, j->src, j->imgid, FALSE, w, h, TRUE);
      dt_imageio_dng_preview_t pv;
      const gboolean has_pv = _source_preview(j->src, &pv);
      j->status = dt_imageio_dng_write_linear(j->file, rgb, w, h, &img, exif, exif_len, has_pv ? &pv : NULL);
      free((void *)pv.data);
      free(exif);
      if(j->status) j->error = g_strdup_printf("could not write %s", j->file);
    }
    dt_free_align(rgb);
  }
  if(!j->status && dt_conf_get_bool("plugins/lighttable/neural_restore/mark_output"))
    dt_exif_xmp_write_neural_restore(j->file, "raw-denoise");
  if(ctx) dt_restore_unref(ctx);
  if(env) dt_restore_env_destroy(env);
  return NULL;
}

// back on the server's thread: the DNG into the library beside the source
// (same film roll, its rating, labels, metadata, tags and group), as the
// panel's _import_image does
static gboolean _ai_denoise_finish(gpointer data)
{
  _job_t *j = data;
  g_thread_join(j->thread);
  j->thread = NULL;
  j->finished = g_get_monotonic_time();
  if(j->status || g_atomic_int_get(&j->cancel))
  {
    if(j->file) g_unlink(j->file);
    j->state = g_atomic_int_get(&j->cancel) ? _JOB_CANCELLED : _JOB_FAILED;
  }
  else
  {
    dt_film_t film;
    dt_film_init(&film);
    gchar *dir = g_path_get_dirname(j->file);
    const dt_filmid_t filmid = dt_film_new(&film, dir);
    g_free(dir);
    j->new_imgid = dt_image_import(filmid, j->file, TRUE, FALSE);
    dt_film_cleanup(&film);
    if(!dt_is_valid_imgid(j->new_imgid))
    {
      j->state = _JOB_FAILED;
      j->error = g_strdup_printf("%s was written but couldn't be imported", j->file);
    }
    else
    {
      const dt_imgid_t src = j->imgid, dst = j->new_imgid;
      const dt_image_t *si = dt_image_cache_get(src, 'r');
      const int rating = dt_image_get_xmp_rating(si);
      const dt_imgid_t grpid = si && dt_is_valid_imgid(si->group_id) ? si->group_id : src;
      dt_image_cache_read_release(si);
      dt_image_t *di = dt_image_cache_get(dst, 'w');
      dt_image_set_xmp_rating(di, rating);
      dt_image_cache_write_release(di, DT_IMAGE_CACHE_SAFE);
      const int labels = dt_colorlabels_get_labels(src);
      for(int c = 0; c < DT_COLORLABELS_LAST; c++)
        if(labels & (1 << c)) dt_colorlabels_set_label(dst, c);
      GList *meta = dt_metadata_get_list_id(src);
      if(meta)
      {
        GList *imgs = g_list_prepend(NULL, GINT_TO_POINTER(dst));
        dt_metadata_set_list_id(imgs, meta, FALSE, FALSE);
        g_list_free(imgs);
        g_list_free_full(meta, g_free);
      }
      GList *tags = NULL;
      if(dt_tag_get_attached(src, &tags, TRUE))
      {
        GList *imgs = g_list_prepend(NULL, GINT_TO_POINTER(dst));
        for(GList *t = tags; t; t = g_list_next(t)) dt_tag_attach_images(((dt_tag_t *)t->data)->id, imgs, FALSE);
        g_list_free(imgs);
      }
      dt_tag_free_result(&tags);
      dt_grouping_add_to_group(grpid, dst);
      if(grpid == src) dt_grouping_change_representative(dst);
      dt_image_synch_xmp(dst);
      if(_in_gui)
        dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                                   DT_COLLECTION_PROP_UNDEF, NULL);
      j->state = _JOB_DONE;
    }
  }
  if(j->handle)
  {
    dt_control_job_dispose(j->handle);
    j->handle = NULL;
  }
  _notify_job(j);
  return G_SOURCE_REMOVE;
}

static gpointer _ai_denoise_run(gpointer data)
{
  _ai_denoise_thread(data);
  g_idle_add(_ai_denoise_finish, data);
  return NULL;
}
#endif

// AI raw denoise of a photo as a job: a new DNG, imported beside it
static gboolean _ai_denoise(JsonObject *params, const dt_imgid_t current, JsonBuilder *b, gchar **err)
{
#ifndef HAVE_AI
  *err = g_strdup("this darktable was built without AI (USE_AI)");
  return FALSE;
#else
  const dt_imgid_t imgid = params && json_object_has_member(params, "imgid")
    ? json_object_get_int_member(params, "imgid") : current;
  if(!dt_is_valid_imgid(imgid) || !_image_exists(imgid))
  {
    *err = g_strdup_printf("no image with id %d", imgid);
    return FALSE;
  }
  if(!dt_ai_registry_is_enabled())
  {
    *err = g_strdup("AI features are off in darktable's preferences (plugins/ai/enabled)");
    return FALSE;
  }
  const double strength = params && json_object_has_member(params, "strength")
    ? json_object_get_double_member(params, "strength")
    : (dt_conf_key_exists("plugins/lighttable/neural_restore/raw_strength")
         ? dt_conf_get_float("plugins/lighttable/neural_restore/raw_strength") : 1.0);
  if(!(strength >= 0.0 && strength <= 1.0))
  {
    *err = g_strdup("strength: 0..1 (blend of the original and the denoised raw)");
    return FALSE;
  }
  // the raw loaded, so its sensor layout is known (neural_restore.c does the same)
  dt_mipmap_buffer_t warm;
  dt_mipmap_cache_get(&warm, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
  const gboolean loaded = warm.buf != NULL;
  dt_mipmap_cache_release(&warm);
  const dt_image_t *ci = dt_image_cache_get(imgid, 'r');
  const dt_restore_sensor_class_t cls = ci ? dt_restore_classify_sensor(ci) : DT_RESTORE_SENSOR_CLASS_UNSUPPORTED;
  dt_image_cache_read_release(ci);
  if(!loaded || cls == DT_RESTORE_SENSOR_CLASS_UNSUPPORTED)
  {
    *err = g_strdup("AI raw denoise needs a raw photo darktable can read (Bayer, X-Trans or linear)");
    return FALSE;
  }
  dt_restore_env_t *env = dt_restore_env_init();
  const gboolean available = env && dt_restore_rawdenoise_available(env);
  if(env) dt_restore_env_destroy(env);
  if(!available)
  {
    *err = g_strdup("no AI raw denoise model is installed (darktable's preferences: AI models)");
    return FALSE;
  }

  // the output: the panel's pattern, unique, folder created
  char src[PATH_MAX] = { 0 };
  gboolean from_cache = FALSE;
  dt_image_full_path(imgid, src, sizeof(src), &from_cache);
  const char *pat = params ? json_object_get_string_member_with_default(params, "path", NULL) : NULL;
  gchar *conf = pat ? NULL : dt_conf_get_string("plugins/lighttable/neural_restore/output_pattern/raw_denoise");
  const char *pattern = pat ? pat : (conf && *conf ? conf : "$(FILE_FOLDER)/$(FILE.NAME)_restore");
  dt_variables_params_t *vp = NULL;
  dt_variables_params_init(&vp);
  vp->filename = src;
  vp->jobcode = "neural_restore";
  vp->imgid = imgid;
  vp->sequence = 1;
  gchar *base = dt_variables_expand_path(vp, (gchar *)pattern, TRUE);
  dt_variables_params_destroy(vp);
  g_free(conf);
  if(!base || !*base)
  {
    g_free(base);
    *err = g_strdup("the output pattern expands to nothing");
    return FALSE;
  }
  gchar *dir = g_path_get_dirname(base);
  if(g_mkdir_with_parents(dir, 0755))
  {
    *err = g_strdup_printf("cannot create %s", dir);
    g_free(dir);
    g_free(base);
    return FALSE;
  }
  g_free(dir);
  gchar *file = g_strdup_printf("%s.dng", base);
  for(int k = 1; g_file_test(file, G_FILE_TEST_EXISTS) && k < 1000; k++)
  {
    g_free(file);
    file = g_strdup_printf("%s_%d.dng", base, k);
  }
  g_free(base);

  _job_t *j = g_new0(_job_t, 1);
  j->id = _next_job++;
  j->type = "ai_denoise";
  j->imgid = imgid;
  j->state = _JOB_RUNNING;
  j->started = g_get_monotonic_time();
  j->src = g_strdup(src);
  j->file = file;
  j->strength = strength;
  // darktable's job handle, where darktable's control runs (its window):
  // progress in its progress bar, and cancel
  j->handle = dt_control_job_create(NULL, "api: AI denoise %d", imgid);
  if(j->handle) dt_control_job_add_progress(j->handle, _("AI denoise"), TRUE);
  _jobs = g_list_append(_jobs, j);
  j->thread = g_thread_new("api-ai-denoise", _ai_denoise_run, j);
  _add_job(b, j);
  json_builder_set_member_name(b, "note");
  json_builder_add_string_value(b, "runs in the background: job_status, or wait for the \"job\" event");
  return TRUE;
#endif
}

// ---- exports and AI mask encoding in the background ---------------------
// as AI denoise: the slow part on a thread of its own, the rest on the
// server's thread

static gboolean _export_job_finish(gpointer data)
{
  _job_t *j = data;
  g_thread_join(j->thread);
  j->thread = NULL;
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  gchar *err = NULL;
  const gboolean ok = _export_finish(j->export, b, &err);
  json_builder_end_object(b);
  if(ok) j->result = json_builder_get_root(b);
  g_object_unref(b);
  j->error = err;
  j->state = ok ? _JOB_DONE : _JOB_FAILED;
  j->finished = g_get_monotonic_time();
  _export_free(j->export);
  j->export = NULL;
  _notify_job(j);
  return G_SOURCE_REMOVE;
}

static gpointer _export_job_run(gpointer data)
{
  _job_t *j = data;
  _export_write(j->export);
  g_idle_add(_export_job_finish, j);
  return NULL;
}

// export {background: true}: replies the job at once; its result is what
// export replies
static gboolean _export_job(JsonObject *params, const dt_imgid_t current, gboolean *saved, JsonBuilder *b,
                            gchar **err)
{
  _export_t *e = _export_prepare(params, current, err);
  if(!e) return FALSE;
  *saved = e->saved;
  _job_t *j = g_new0(_job_t, 1);
  j->id = _next_job++;
  j->type = "export";
  j->imgid = e->imgid;
  j->started = g_get_monotonic_time();
  j->export = e;
  _jobs = g_list_append(_jobs, j);
  j->thread = g_thread_new("api-export", _export_job_run, j);
  _add_job(b, j);
  return TRUE;
}

#ifdef HAVE_AI
static gboolean _seg_job_finish(gpointer data)
{
  _job_t *j = data;
  g_thread_join(j->thread);
  j->thread = NULL;
  _seg_job = 0;
  g_free(j->rgb);
  j->rgb = NULL;
  if(g_atomic_int_get(&j->cancel) || !j->ok)
  {
    dt_seg_reset_encoding(_seg);
    g_free(_seg_key);
    _seg_key = NULL;
    j->state = g_atomic_int_get(&j->cancel) ? _JOB_CANCELLED : _JOB_FAILED;
    if(!j->ok) j->error = g_strdup("the AI mask model could not encode the photo");
  }
  else
  {
    _seg_w = j->rw;
    _seg_h = j->rh;
    g_free(_seg_key);
    _seg_key = j->seg_key;
    j->seg_key = NULL;
    j->state = _JOB_DONE;
  }
  g_free(j->seg_key);
  j->seg_key = NULL;
  j->finished = g_get_monotonic_time();
  _notify_job(j);
  return G_SOURCE_REMOVE;
}

static gpointer _seg_job_run(gpointer data)
{
  _job_t *j = data;
  dt_seg_reset_encoding(_seg);
  j->ok = dt_seg_encode_image(_seg, j->rgb, j->rw, j->rh);
  g_idle_add(_seg_job_finish, j);
  return NULL;
}
#endif

// encodes the photo for AI masks in the background (the slow part of the
// first mask_ai on a photo: seconds), so mask_ai is quick after. done at
// once when it is encoded already
static gboolean _mask_ai_encode(JsonObject *params, JsonBuilder *b, gchar **err)
{
#ifndef HAVE_AI
  *err = g_strdup("this darktable was built without AI (USE_AI)");
  return FALSE;
#else
  if(!_cur)
  {
    *err = g_strdup("no open session");
    return FALSE;
  }
  if(!dt_ai_registry_is_enabled())
  {
    *err = g_strdup("AI features are off in darktable's preferences (plugins/ai/enabled)");
    return FALSE;
  }
  if(_seg_job)
  {
    *err = g_strdup_printf("a photo is being encoded already (job %d)", _seg_job);
    return FALSE;
  }
  if(!_seg_model(err)) return FALSE;
  _session_t *gui = _cur, *s = NULL;
  _job_t *j = g_new0(_job_t, 1);
  const gboolean ok = _seg_input(&s, &j->rgb, &j->rw, &j->rh, &j->seg_key, err);
  _cur = gui;
  if(!ok)
  {
    g_free(j);
    return FALSE;
  }
  j->id = _next_job++;
  j->type = "mask_ai_encode";
  j->imgid = gui->dev->image_storage.id;
  j->started = g_get_monotonic_time();
  _jobs = g_list_append(_jobs, j);
  if(!j->rgb)
  {
    // encoded already
    g_free(j->seg_key);
    j->seg_key = NULL;
    j->state = _JOB_DONE;
    j->finished = j->started;
  }
  else
  {
    _seg_job = j->id;
    j->thread = g_thread_new("api-ai-encode", _seg_job_run, j);
  }
  _add_job(b, j);
  return TRUE;
#endif
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

// ---- pickers and auto buttons -----------------------------------------------
// a module's pickers and auto buttons compute in its window code
// (color_picker_apply reads its widgets), so they work on the photo in
// darktable's darkroom only. the API presses them through darktable's
// actions, as a shortcut would, sets the picker's area, and finishes when
// the preview pipe's sample has been applied (a job: the pipe runs on
// another thread)

typedef struct _control_t
{
  dt_action_t *action;
  GtkWidget *widget;
  gboolean picker;
  gchar *name;
} _control_t;

static void _control_free(gpointer p)
{
  _control_t *c = p;
  g_free(c->name);
  g_free(c);
}

// the module's pickers (bauhaus quads or buttons darktable made with
// dt_color_picker_new) and auto buttons (tone equalizer's wands, plain
// buttons such as rgb levels' "auto levels"), named by their action's path
// below the module, in english: "exposure range/auto tune levels"
static GList *_controls(dt_iop_module_t *m)
{
  GList *list = NULL;
  for(GSList *l = m->widget_list; l; l = g_slist_next(l))
  {
    const dt_action_target_t *ref = l->data;
    GtkWidget *w = ref ? ref->target : NULL;
    if(!ref || !ref->action || !GTK_IS_WIDGET(w)) continue;
    gboolean picker = FALSE;
    if(DT_IS_BAUHAUS_WIDGET(w))
    {
      const dt_bauhaus_quad_paint_f paint = dt_bauhaus_widget_get_quad_paint(w);
      if(paint == dtgtk_cairo_paint_colorpicker) picker = TRUE;
      else if(paint != dtgtk_cairo_paint_wand) continue;
    }
    else if(g_object_get_data(G_OBJECT(w), DT_COLOR_PICKER_INSTANCE_KEY))
      picker = TRUE;
    else if(!GTK_IS_BUTTON(w) || GTK_IS_TOGGLE_BUTTON(w))
      continue;
    // the module's own controls; blending's are shared by all modules
    dt_action_t *owner = ref->action;
    GSList *labels = NULL;
    for(; owner && owner->type >= DT_ACTION_TYPE_SECTION; owner = owner->owner)
      labels = g_slist_prepend(labels, (gpointer)owner->label);
    if(owner != &m->so->actions)
    {
      g_slist_free(labels);
      continue;
    }
    GString *name = g_string_new(NULL);
    for(GSList *k = labels; k; k = g_slist_next(k))
      g_string_append_printf(name, "%s%s", name->len ? "/" : "", (const char *)k->data);
    g_slist_free(labels);
    _control_t *c = g_new0(_control_t, 1);
    c->action = ref->action;
    c->widget = w;
    c->picker = picker;
    c->name = g_string_free(name, FALSE);
    list = g_list_append(list, c);
  }
  return list;
}

static dt_iop_module_t *_darkroom_module(JsonObject *params, gchar **err)
{
  if(!_cur || !_cur->gui)
  {
    *err = g_strdup("pickers and auto buttons work on the photo in darktable's darkroom, with darktable's window"
                    " serving the API (library_status: darkroom_imgid)");
    return NULL;
  }
  return _find_module(params, err);
}

static gboolean _picker_list(JsonObject *params, JsonBuilder *b, gchar **err)
{
  dt_iop_module_t *m = _darkroom_module(params, err);
  if(!m) return FALSE;
  GList *controls = _controls(m);
  json_builder_set_member_name(b, "controls");
  json_builder_begin_array(b);
  for(GList *l = controls; l; l = g_list_next(l))
  {
    const _control_t *c = l->data;
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, c->name);
    json_builder_set_member_name(b, "kind");
    json_builder_add_string_value(b, c->picker ? "picker" : "button");
    if(c->picker)
    {
      json_builder_set_member_name(b, "active");
      json_builder_add_boolean_value(b, dt_iop_color_picker_is_active(c->widget));
    }
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  g_list_free_full(controls, _control_free);
  return TRUE;
}

// darktable's path of an action ("iop/agx/exposure range/auto tune levels"),
// its ids escaped as _action_find reads them
static gchar *_action_path(const dt_action_t *ac)
{
  GSList *ids = NULL;
  for(; ac; ac = ac->owner) ids = g_slist_prepend(ids, (gpointer)ac->id);
  GString *path = g_string_new(NULL);
  for(GSList *k = ids; k; k = g_slist_next(k))
  {
    if(path->len) g_string_append_c(path, '/');
    for(const char *c = k->data; *c; c++)
      if(*c == '/') g_string_append(path, "@<");
      else if(*c == '@') g_string_append(path, "@@");
      else g_string_append_c(path, *c);
  }
  g_slist_free(ids);
  return g_string_free(path, FALSE);
}

static gboolean _press(dt_iop_module_t *m, const _control_t *c)
{
  // a plain button clicked through its action is activated, which GTK
  // turns into a click a quarter second later (gtk_real_button_activate):
  // click it now, so the pipe runs that follow see the press
  if(GTK_IS_BUTTON(c->widget) && !c->picker && !DT_IS_BAUHAUS_WIDGET(c->widget)
     && !g_object_get_data(G_OBJECT(c->widget), DT_ACTION_GESTURE_KEY))
  {
    if(!gtk_widget_is_sensitive(c->widget)) return FALSE;
    gtk_button_clicked(GTK_BUTTON(c->widget));
    return TRUE;
  }
  // the instance as darktable's actions count them: by position among the
  // module's instances in the pipe (accelerators.c, _process_action)
  int instance = 0;
  for(GList *l = darktable.develop->iop; l; l = g_list_next(l))
  {
    const dt_iop_module_t *o = l->data;
    if(o->so == m->so && o->iop_order != INT_MAX) instance++;
    if(o == m) break;
  }
  gchar *path = _action_path(c->action);
  // a bauhaus quad presses on any effect but on and off (bauhaus.c,
  // _action_process_button)
  const float r = DT_IS_BAUHAUS_WIDGET(c->widget)
    ? dt_action_process(path, instance, "button", "toggle", 1.0f)
    : dt_action_process(path, instance, NULL, c->picker ? "toggle" : "activate", 1.0f);
  g_free(path);
  // a button's action reports nothing (accelerators.c,
  // _action_process_button): it presses when the button can be used
  if(!DT_IS_BAUHAUS_WIDGET(c->widget) && !c->picker) return gtk_widget_is_sensitive(c->widget);
  return !DT_ACTION_IS_INVALID(r);
}

static _control_t *_control_find(dt_iop_module_t *m, const char *name, GList **list)
{
  *list = _controls(m);
  for(GList *l = *list; l; l = g_list_next(l))
    if(!g_strcmp0(((_control_t *)l->data)->name, name)) return l->data;
  return NULL;
}

// the darkroom's pipe runs, for auto buttons that need one before they can
// work (tone equalizer's wands read its histogram) or after (rgb levels'
// auto levels computes in the pipe)
static guint64 _preview_runs = 0, _full_runs = 0;

static void _gui_preview_finished_cb(gpointer instance, gpointer user_data)
{
  _preview_runs++;
}

static void _gui_full_finished_cb(gpointer instance, gpointer user_data)
{
  _full_runs++;
}

static _job_t *_pick_job(const dt_iop_module_t *m)
{
  for(GList *l = _jobs; l; l = g_list_next(l))
  {
    _job_t *j = l->data;
    if(j->state == _JOB_RUNNING && j->control && !g_strcmp0(j->op, m->op) && j->prio == m->multi_priority)
      return j;
  }
  return NULL;
}

static dt_iop_module_t *_job_module(const _job_t *j)
{
  return darktable.develop ? dt_iop_get_module_by_op_priority(darktable.develop->iop, j->op, j->prio) : NULL;
}

// the settings a pick or press changed, as module_get names them
static JsonNode *_changed_settings(const dt_iop_module_t *m, const void *before)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "enabled");
  json_builder_add_boolean_value(b, m->enabled);
  json_builder_set_member_name(b, "changed");
  json_builder_begin_object(b);
  for(dt_introspection_field_t *f = m->so->get_introspection_linear();
      before && f && f->header.type != DT_INTROSPECTION_TYPE_NONE; f++)
  {
    const gboolean list = _settable_list(f);
    if(!list && !_settable(f)) continue;
    const size_t off = f->header.offset;
    if(!memcmp((const uint8_t *)before + off, (const uint8_t *)m->params + off, f->header.size)) continue;
    json_builder_set_member_name(b, f->header.name);
    if(list)
      _add_list_value(b, f, (const uint8_t *)m->params + off);
    else
      _add_field_value(b, f, m->params);
  }
  json_builder_end_object(b);
  json_builder_end_object(b);
  JsonNode *n = json_builder_get_root(b);
  g_object_unref(b);
  return n;
}

static void _pick_end(_job_t *j, const _job_state_t state, const char *error)
{
  dt_iop_module_t *m = _job_module(j);
  if(m && j->picker) dt_iop_color_picker_reset(m, FALSE);
  if(m && state == _JOB_DONE) j->result = _changed_settings(m, j->before);
  if(j->watch) g_source_remove(j->watch);
  j->watch = 0;
  if(error) j->error = g_strdup(error);
  j->state = state;
  j->finished = g_get_monotonic_time();
  g_free(j->before);
  j->before = NULL;
  _notify_job(j);
}

// the window runs the darkroom's pipes when it draws, which it doesn't
// while the screen sleeps: start them too. the preview pipe waits for the
// full pipe while that is still loading the image (develop.c,
// dt_dev_process_image_job)
static void _run_preview(dt_develop_t *dev)
{
  if(dev->full.pipe->loading || dev->full.pipe->status != DT_DEV_PIXELPIPE_VALID)
    dt_dev_process_image(dev);
  dev->preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY;
  dt_dev_process_preview(dev);
  dt_control_queue_redraw_center();
}

static void _run_pipes(dt_develop_t *dev)
{
  dt_dev_invalidate_all(dev);
  dt_dev_process_preview(dev);
  dt_dev_process_image(dev);
  dt_control_queue_redraw_center();
}

// switches a picker on (unless it is) and sets its area: in darktable's
// picker space (the pipe's input, so it stays on the same spot when the
// crop changes), as dragging it does (darkroom.c, mouse_moved). an error,
// or NULL
static const char *_pick_arm(_job_t *j, dt_iop_module_t *m, const _control_t *c)
{
  dt_develop_t *dev = darktable.develop;
  if(!dt_iop_color_picker_is_active(c->widget) && !_press(m, c))
    return "darktable didn't accept the press";
  const dt_iop_color_picker_t *proxy = darktable.lib->proxy.colorpicker.picker_proxy;
  if(!proxy || proxy->module != m || !dt_iop_color_picker_is_active(c->widget))
    return "darktable didn't switch the picker on";
  if(j->point)
  {
    dt_pickerpoint_t pos;
    dt_color_picker_backtransform_box(dev, 1, j->area, pos);
    dt_lib_colorpicker_set_point(darktable.lib, pos);
  }
  else
  {
    dt_pickerbox_t box;
    dt_color_picker_backtransform_box(dev, 2, j->area, box);
    dt_lib_colorpicker_set_box_area(darktable.lib, box);
  }
  // a run already under way samples the old area: then the first sample
  // is not this one (_gui_pickerdata_cb)
  j->phase = dt_pipe_processing(dev->preview_pipe) ? 0 : 1;
  _run_preview(dev);
  return NULL;
}

// the preview pipe sampled a picker's area and darktable applied it
// (color_picker_proxy.c connected first, so it has run)
static void _gui_pickerdata_cb(gpointer instance, dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe,
                               gpointer user_data)
{
  _job_t *j = module ? _pick_job(module) : NULL;
  if(!j || !j->picker) return;
  if(j->phase == 0)
  {
    // the preview pipe was running when the area was set: this sample may
    // be of the area before. move the area by a hair, which makes
    // darktable apply the next sample (color_picker_proxy.c,
    // _record_point_area)
    dt_colorpicker_sample_t *sample = darktable.lib->proxy.colorpicker.primary_sample;
    for(int k = 0; k < 2; k++) sample->point[k] += 1e-6f;
    for(int k = 0; k < 8; k++) sample->box[k] += 1e-6f;
    j->phase = 1;
    _run_preview(darktable.develop);
    return;
  }
  _pick_end(j, _JOB_DONE, NULL);
}

// pickers: cancel and time out. buttons: pressed once the pipes have run
// with the module focused, read once they ran after the press
static gboolean _pick_watch(gpointer data)
{
  _job_t *j = data;
  dt_iop_module_t *m = _job_module(j);
  const gint64 age = g_get_monotonic_time() - j->started;
  if(!m || _darkroom_image() != j->imgid)
  {
    j->watch = 0;
    _pick_end(j, _JOB_FAILED, "the darkroom left the photo or the module");
    return G_SOURCE_REMOVE;
  }
  if(g_atomic_int_get(&j->cancel))
  {
    j->watch = 0;
    _pick_end(j, _JOB_CANCELLED, NULL);
    return G_SOURCE_REMOVE;
  }
  if(age > 30 * G_USEC_PER_SEC)
  {
    j->watch = 0;
    _pick_end(j, _JOB_FAILED, "no result within 30 s: is darktable's window showing the darkroom?");
    return G_SOURCE_REMOVE;
  }
  dt_develop_t *dev = darktable.develop;
  const dt_iop_color_picker_t *proxy = darktable.lib->proxy.colorpicker.picker_proxy;
  if(j->picker && (!proxy || proxy->module != m))
  {
    // the modules' controls refreshed after the darkroom reloaded (a style
    // applied) switch pickers off (dt_iop_color_picker_reset): switch it on
    // again, twice at most. not when another picker took over (another
    // job, a click in the window)
    GList *list = NULL;
    const _control_t *c = !proxy && ++j->arms <= 2 ? _control_find(m, j->control, &list) : NULL;
    const char *e = c ? _pick_arm(j, m, c) : "the picker was switched off before darktable applied it";
    g_list_free_full(list, _control_free);
    if(!e) return G_SOURCE_CONTINUE;
    j->watch = 0;
    _pick_end(j, _JOB_FAILED, e);
    return G_SOURCE_REMOVE;
  }
  if(j->picker)
  {
    // the preview pipe may not have run (see _run_pipes): ask again now
    // and then
    if(++j->presses % 20 == 0 && !dt_pipe_processing(dev->preview_pipe)) _run_preview(dev);
    return G_SOURCE_CONTINUE;
  }
  // buttons: once both pipes have run since the last step and are idle,
  // press (phase 0) or read what the press did (phase 1)
  // (a run cut short by a newer change ends too, but leaves its pipe dirty)
  const gboolean idle = !dt_pipe_processing(dev->full.pipe) && !dt_pipe_processing(dev->preview_pipe)
                        && dev->full.pipe->status == DT_DEV_PIXELPIPE_VALID
                        && dev->preview_pipe->status == DT_DEV_PIXELPIPE_VALID
                        && _preview_runs > j->preview_mark && _full_runs > j->full_mark;
  if(!idle)
  {
    if(++j->presses % 20 == 0) _run_pipes(dev);
    return G_SOURCE_CONTINUE;
  }
  j->preview_mark = _preview_runs;
  j->full_mark = _full_runs;
  // a press may do nothing yet (a wand whose histogram isn't ready): up to
  // three
  if(j->phase == 1 && (memcmp(j->before, m->params, m->params_size) || j->phase_presses >= 3))
  {
    j->watch = 0;
    _pick_end(j, _JOB_DONE, NULL);
    return G_SOURCE_REMOVE;
  }
  GList *list = NULL;
  const _control_t *c = _control_find(m, j->control, &list);
  const gboolean pressed = c && _press(m, c);
  g_list_free_full(list, _control_free);
  if(!pressed)
  {
    j->watch = 0;
    _pick_end(j, _JOB_FAILED, c ? "darktable didn't accept the press" : "the button is gone");
    return G_SOURCE_REMOVE;
  }
  j->phase_presses++;
  j->phase = 1;
  // the press may change nothing the pipes see: run them anyway
  _run_pipes(dev);
  return G_SOURCE_CONTINUE;
}

static gboolean _parse_area(JsonObject *params, float box[4], gboolean *point, gchar **err)
{
  *point = json_object_has_member(params, "point");
  JsonNode *n = json_object_get_member(params, *point ? "point" : "box");
  if(!n)
  {
    // darktable's own area for a picker used the first time
    // (dt_lib_colorpicker_reset_box_area)
    box[0] = box[1] = 0.02f;
    box[2] = box[3] = 0.98f;
    return TRUE;
  }
  gboolean ok = FALSE;
  if(*point && JSON_NODE_HOLDS_ARRAY(n) && json_array_get_length(json_node_get_array(n)) == 2)
  {
    JsonArray *a = json_node_get_array(n);
    box[0] = json_array_get_double_element(a, 0);
    box[1] = json_array_get_double_element(a, 1);
    ok = box[0] >= 0.f && box[0] <= 1.f && box[1] >= 0.f && box[1] <= 1.f;
  }
  else if(!*point && JSON_NODE_HOLDS_OBJECT(n))
  {
    JsonObject *o = json_node_get_object(n);
    static const char *keys[4] = { "left", "top", "right", "bottom" };
    ok = TRUE;
    for(int k = 0; k < 4 && ok; k++)
    {
      ok = json_object_has_member(o, keys[k]);
      if(ok) box[k] = json_object_get_double_member(o, keys[k]);
    }
    ok = ok && box[0] >= 0.f && box[2] <= 1.f && box[1] >= 0.f && box[3] <= 1.f
            && box[0] < box[2] && box[1] < box[3];
  }
  if(!ok)
    *err = g_strdup("point is [x, y] and box {left, top, right, bottom}, in 0..1 of the image as rendered");
  return ok;
}

// presses a picker or auto button of a module of the darkroom's photo. a
// picker samples box (or point) in image space; without one, darktable's
// default area for a fresh picker (nearly the whole image)
static gboolean _picker_apply(JsonObject *params, JsonBuilder *b, gchar **err)
{
  dt_iop_module_t *m = _darkroom_module(params, err);
  if(!m) return FALSE;
  const char *name = json_object_get_string_member_with_default(params, "control", NULL);
  GList *list = NULL;
  const _control_t *c = _control_find(m, name, &list);
  if(!c)
  {
    g_list_free_full(list, _control_free);
    *err = g_strdup_printf("module '%s' has no picker or button '%s' (picker_list)", m->op, name ? name : "");
    return FALSE;
  }
  float area[4] = { 0.f };
  gboolean point = FALSE;
  if(c->picker && !_parse_area(params, area, &point, err))
  {
    g_list_free_full(list, _control_free);
    return FALSE;
  }
  if(_pick_job(m))
  {
    g_list_free_full(list, _control_free);
    *err = g_strdup_printf("a picker of '%s' is already at work (job_list)", m->op);
    return FALSE;
  }
  _job_t *j = g_new0(_job_t, 1);
  j->id = _next_job++;
  j->type = c->picker ? "picker" : "button";
  j->imgid = _cur->dev->image_storage.id;
  j->started = g_get_monotonic_time();
  j->op = g_strdup(m->op);
  j->prio = m->multi_priority;
  j->control = g_strdup(c->name);
  j->picker = c->picker;
  memcpy(j->area, area, sizeof(area));
  j->point = point;
  j->before = g_malloc(m->params_size);
  memcpy(j->before, m->params, m->params_size);
  _jobs = g_list_append(_jobs, j);

  dt_develop_t *dev = darktable.develop;
  if(c->picker)
  {
    const char *e = _pick_arm(j, m, c);
    if(e)
    {
      _pick_end(j, _JOB_FAILED, e);
      g_list_free_full(list, _control_free);
      _add_job(b, j);
      return TRUE;
    }
  }
  else
  {
    dt_iop_request_focus(m);
    // on first, as its power button would: some buttons only switch the
    // module on when it's off (tone equalizer's wands), others compute in
    // a pipe run the switch cuts short (rgb levels' auto levels)
    if(!m->enabled) _record(m, TRUE);
    j->preview_mark = _preview_runs;
    j->full_mark = _full_runs;
    _run_pipes(dev);
  }
  j->watch = g_timeout_add(100, _pick_watch, j);
  g_list_free_full(list, _control_free);
  _add_job(b, j);
  return TRUE;
}

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

// a job ended: every client hears it (the one that started it too)
static void _notify_job(const _job_t *j)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "jsonrpc");
  json_builder_add_string_value(b, "2.0");
  json_builder_set_member_name(b, "method");
  json_builder_add_string_value(b, "event");
  json_builder_set_member_name(b, "params");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "type");
  json_builder_add_string_value(b, "job");
  _add_job(b, j);
  json_builder_end_object(b);
  json_builder_end_object(b);
  JsonNode *root = json_builder_get_root(b);
  for(GList *l = _clients; l; l = g_list_next(l)) _send_node(l->data, root);
  json_node_unref(root);
  g_object_unref(b);
}

static const char *_methods[] = {
  "ping", "film_rolls", "images_list", "image_info", "thumbnail", "set_rating", "set_label",
  "session_open", "session_close", "module_list", "module_get", "module_set", "module_set.lists",
  "module_enable", "preset_list", "preset_apply", "history_list", "history_end", "geometry_get",
  "geometry_set", "coords", "retouch_list", "retouch_heal", "retouch_add", "retouch_set",
  "retouch_remove", "blend_get", "blend_set", "mask_add", "mask_list",
  "mask_remove", "mask_add.path", "mask_ai", "mask_ai_encode", "export.background", "sample", "ai_denoise", "job_status", "job_list", "job_cancel", "module_add", "module_move", "curve_get", "curve_set", "image_duplicate", "style_list", "style_create", "style_delete", "style_apply",
  "history_paste", "image_metadata", "tag_list", "set_tags",
  "set_metadata", "set_location", "picker_list", "picker_apply", "module_remove", "module_rename", "history_compress",
  "render.history_end", "save", "reset", "render", "render.uncropped", "render.zoom", "export",
  "library_status", "library_release", "library_acquire", "handover", "shutdown", NULL };

// methods that work on an edit session, and the event each one sends
static const struct { const char *method; const char *event; } _session_methods[] = {
  { "module_list", NULL }, { "module_get", NULL }, { "history_list", NULL }, { "render", NULL },
  { "geometry_get", NULL }, { "preset_list", NULL }, { "preset_apply", "edit" },
  { "coords", NULL }, { "retouch_list", NULL }, { "retouch_heal", "edit" },
  { "retouch_add", "edit" }, { "retouch_set", "edit" }, { "retouch_remove", "edit" },
  { "blend_get", NULL }, { "blend_set", "edit" }, { "mask_add", "edit" }, { "mask_list", NULL },
  { "mask_remove", "edit" }, { "mask_ai", "edit" }, { "mask_ai_encode", NULL }, { "sample", NULL },
  { "module_move", "edit" }, { "picker_list", NULL }, { "picker_apply", NULL }, { "curve_get", NULL }, { "curve_set", "edit" },
  { "module_add", "edit" }, { "module_remove", "edit" }, { "module_rename", "edit" }, { "history_compress", "saved" },
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
  else if(_jobs_running() && (!g_strcmp0(method, "library_release") || !g_strcmp0(method, "handover")
                               || !g_strcmp0(method, "shutdown")))
    err = g_strdup("a background job is running (job_list): wait for it, or job_cancel it, first");
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
    const gboolean bg = params && json_object_get_boolean_member_with_default(params, "background", FALSE);
    if((bg ? _export_job(params, c->current, &saved, b, &err) : _export(params, c->current, &saved, b, &err))
       && saved)
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
  else if(!g_strcmp0(method, "job_status"))
    _job_status(params, b, &err);
  else if(!g_strcmp0(method, "job_list"))
    _job_list(b, &err);
  else if(!g_strcmp0(method, "job_cancel"))
    _job_cancel(params, b, &err);
  else if(!g_strcmp0(method, "ai_denoise"))
    _ai_denoise(params, c->current, b, &err);
  else if(!g_strcmp0(method, "module_move"))
    _module_move(params, b, &err);
  else if(!g_strcmp0(method, "curve_get"))
    _curve_get(params, b, &err);
  else if(!g_strcmp0(method, "curve_set"))
    _curve_set(params, b, &err);
  else if(!g_strcmp0(method, "image_duplicate"))
  {
    gboolean saved = FALSE;
    if(_image_duplicate(params, c->current, &saved, b, &err))
    {
      event = saved ? "saved" : "image";
      event_img = params && json_object_has_member(params, "imgid")
        ? json_object_get_int_member(params, "imgid") : c->current;
    }
  }
  else if(!g_strcmp0(method, "image_metadata"))
    _image_metadata(params, c->current, b, &err);
  else if(!g_strcmp0(method, "tag_list"))
    _tag_list(params, b, &err);
  else if(!g_strcmp0(method, "set_tags") || !g_strcmp0(method, "set_metadata")
          || !g_strcmp0(method, "set_location"))
  {
    const gboolean ok = !g_strcmp0(method, "set_tags") ? _set_tags(params, c->current, b, &err)
                      : !g_strcmp0(method, "set_metadata") ? _set_metadata(params, c->current, b, &err)
                      : _set_location(params, c->current, b, &err);
    if(ok)
    {
      GList *imgs = _imgids(params, c->current, &err);
      for(GList *l = imgs; l; l = g_list_next(l)) _notify(c, "image", GPOINTER_TO_INT(l->data));
      g_list_free(imgs);
    }
  }
  else if(!g_strcmp0(method, "style_list"))
    _style_list(params, b, &err);
  else if(!g_strcmp0(method, "style_create"))
    _style_create(params, c->current, b, &err);
  else if(!g_strcmp0(method, "style_delete"))
    _style_delete(params, b, &err);
  else if(!g_strcmp0(method, "style_apply") || !g_strcmp0(method, "history_paste"))
  {
    GArray *changed = g_array_new(FALSE, FALSE, sizeof(dt_imgid_t));
    if(!g_strcmp0(method, "style_apply"))
      _style_apply(params, c->current, changed, b, &err);
    else
      _history_paste(params, c->current, changed, b, &err);
    // images done before an error changed too
    for(guint i = 0; i < changed->len; i++)
    {
      _notify(c, _session_find(g_array_index(changed, dt_imgid_t, i)) ? "reopened" : "image",
              g_array_index(changed, dt_imgid_t, i));
    }
    g_array_free(changed, TRUE);
  }
  else if(!g_strcmp0(method, "picker_list"))
    _picker_list(params, b, &err);
  else if(!g_strcmp0(method, "picker_apply"))
    _picker_apply(params, b, &err);
  else if(!g_strcmp0(method, "module_add"))
    _module_add(params, b, &err);
  else if(!g_strcmp0(method, "module_remove"))
    _module_remove(params, b, &err);
  else if(!g_strcmp0(method, "module_rename"))
    _module_rename(params, b, &err);
  else if(!g_strcmp0(method, "history_compress"))
    _history_compress(params, b, &err);
  else if(!g_strcmp0(method, "blend_get"))
    _blend_get(params, b, &err);
  else if(!g_strcmp0(method, "blend_set"))
    _blend_set(params, b, &err);
  else if(!g_strcmp0(method, "mask_add"))
    _mask_add(params, b, &err);
  else if(!g_strcmp0(method, "mask_ai"))
    _mask_ai(params, b, &err);
  else if(!g_strcmp0(method, "mask_ai_encode"))
    _mask_ai_encode(params, b, &err);
  else if(!g_strcmp0(method, "mask_list"))
    _mask_list(params, b, &err);
  else if(!g_strcmp0(method, "mask_remove"))
    _mask_remove(params, b, &err);
  else if(!g_strcmp0(method, "sample"))
    _sample(params, b, &err);
  else if(!g_strcmp0(method, "coords"))
    _coords(params, b, &err);
  else if(!g_strcmp0(method, "retouch_list"))
    _retouch_list(params, b, &err);
  else if(!g_strcmp0(method, "retouch_heal"))
    _retouch_add(params, "heal", b, &err);
  else if(!g_strcmp0(method, "retouch_add"))
    _retouch_add(params, "heal", b, &err);
  else if(!g_strcmp0(method, "retouch_set"))
    _retouch_set(params, b, &err);
  else if(!g_strcmp0(method, "retouch_remove"))
    _retouch_remove(params, b, &err);
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
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_CONTROL_PICKERDATA_READY, _gui_pickerdata_cb, NULL);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED, _gui_preview_finished_cb, NULL);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED, _gui_full_finished_cb, NULL);
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
    DT_CONTROL_SIGNAL_DISCONNECT(_gui_pickerdata_cb, NULL);
    DT_CONTROL_SIGNAL_DISCONNECT(_gui_preview_finished_cb, NULL);
    DT_CONTROL_SIGNAL_DISCONNECT(_gui_full_finished_cb, NULL);
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
