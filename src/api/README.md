# darktable-api

A long-running, headless darktable that edits photos in a darktable library
on request: open a photo, change module settings by name, render previews,
undo/redo, save, start over from the defaults. It is meant as a back end for
web apps and AI agents. It is built from darktable's own code: processing,
history, presets, library and sidecars all go through libdarktable, as in the
darkroom. It started from `src/mcp` (darktable-mcp), but stays running and
keeps an edit session with a live pixelpipe between requests.

**Experimental.** Run it on a copy of your library (see below), never on the
library your darktable uses. Saving writes the history in this build's
module versions; an older darktable can't read some of them.

A web front end using it (library browsing, editing with live previews,
handing the library to darktable's GUI and back):
https://github.com/mmcc-xx/darktable-api-web
An MCP server using it, for AI assistants:
https://github.com/mmcc-xx/darktable-api-mcp

A demo video (the web app, darktable's window and an AI assistant on one
photo): https://www.youtube.com/watch?v=VDfZbxvvniY

This code was written with AI assistance (Claude), directed and reviewed by
the branch owner.

## Build

Build darktable as usual with the API enabled (`-DUSE_API=ON`, or
`./build.sh --enable-api`; off by default). That builds `darktable-api`
and lets darktable's window serve the API (`--api-socket`); without it
neither exists (a build folder configured before the option existed
needs `cmake -B build -DUSE_API=ON` once). The AI methods (`mask_ai`,
`ai_denoise`) need `-DUSE_AI=ON`:

    cmake --build build

Build all targets, not just `--target darktable-api`: the engine also
needs darktable's data files (rawspeed's `cameras.xml`, the default
`darktablerc`), which only the full build puts in `build/share`.
Without them a fresh build hangs on its first raw import.

`src/api` builds against darktable master and against releases from 5.6.2
on: branch `darktable-api` is darktable master, `darktable-api-5.6` is
darktable 5.6.2 (the same `src/api`; use the one matching the darktable
that edits the library, as newer module versions can't be read by an
older darktable). `darktable-api` merges darktable master weekly once the
tests pass, and release branches merge darktable's point releases; nothing
is rebased or force-pushed, so `git pull` and a rebuild update a clone. What differs between them is in `compat.h`, chosen by the feature
checks in `compat.cmake` (what a darktable has, not its version number).
A few features use functions only this branch adds to darktable, looked up
at runtime: without them, sliders' pickers and wands aren't listed,
`darkroom_snapshot` refuses, and straightening doesn't fit the crop.

Tested on macOS (Apple M1) and Linux (Ubuntu 24.04, gcc 13), headless and
with darktable's window. On Linux, ONNX Runtime is loaded by file name, so
darktable run from the build tree needs `plugins/ai/ort_library_path`
(an install puts the library next to darktable).

## Run

    darktable-api [--listen <socket>] [--max-sessions <n>] [--idle-exit <seconds>] \
                  --core --configdir <dir> [--cachedir <dir>] [darktable options]

Without `--listen` it serves one client on stdin/stdout (the client starts
it as a child process). With `--listen` it serves every client connecting to
the unix socket (created with mode 0700, so only your user can connect), and
several front ends share one library: a web app and an MCP server, for
example. The example clients below start it this way when nobody has.

- **Shared edit sessions:** sessions belong to the engine, one per open image
  (at most `--max-sessions`, default 3; each holds a full-size raw and a
  pipe cache, about 0.4-0.7 GB). Two clients that open the same photo edit
  the same session, unsaved changes included. To make room the engine closes
  the least recently used session *without* unsaved changes; it refuses to
  open another image rather than drop someone's unsaved edit.
- **Notifications:** every change is announced to the other clients:
  `{"jsonrpc":"2.0","method":"event","params":{"type","imgid","history_end","unsaved","client"}}`
  with `type` one of `edit`, `saved`, `reset`, `reopened`, `closed`,
  `image` (rating or label), `markers` (marker_set or marker_remove), `removed`, `library_released`, `library_acquired`,
  `clients` (a client said `client_hello` or, having said it, disconnected).
- `--idle-exit` stops the engine that many seconds after the last client
  disconnected, unless an image has unsaved changes or a job runs. Once its
  executable was rebuilt (changed on disk), it stops that many seconds after
  the last request even with clients connected (not while the library is
  released), so a client that stays connected for events, like the web app,
  doesn't keep an old build running: clients reconnect and start the new
  one on their next request.

## darktable's window serves the same API

Started with `--api` (socket next to the config dir, the same rule as the
example clients) or `--api-socket <path>`, darktable itself serves the
library on that socket while its window is open, on GTK's main thread:

- clients don't need to know who serves them (`ping` and `library_status`
  say `"server": "gui"` or `"engine"`);
- the image shown in the darkroom is edited through the darkroom's own path:
  an API change moves darktable's sliders and adds to its history panel;
  undo is the history panel's undo; a change made in the window reaches the
  clients as an `edit` event (`client` 0). Renders of that image go through
  a copy reloaded from the darkroom's history when it changed;
- ratings, rejects and color labels changed in the window (stars clicked,
  shortcuts, undo, the filmstrip) reach the clients as `image` events; the
  API's `set_rating` / `set_label` redraw the window's thumbnails, as
  darktable's own rating and color label shortcuts do;
- `library_status` reports `darkroom_imgid`, the photo the darkroom shows
  (0 if none), and clients get a `darkroom` event when that changes, so they
  can follow the user;
- other images get sessions as in the engine;
- **automatic hand-off:** at startup, darktable asks an engine serving the
  same socket to hand over (`handover`: the engine releases the library,
  sends its unsaved edits, tells its clients and exits) before opening the
  library itself. When darktable quits, the clients start an engine again.
  Unsaved edits of images other than the darkroom's are kept in
  `<socket>.drafts.json` when a server stops and restored by the next one;
- `library_release` is refused while the window serves; `shutdown` too,
  unless `quit_window` (darktable quits as from its quit menu).

Measured: hand-off ~0.2 s plus darktable's startup; clients back on an
engine ~3 s after darktable quit.

It refuses to start without `--configdir` (or `--library`), so it can't open
your default library by accident. For a copy: create a directory, copy
`library.db`, `data.db` and `darktablerc` from your darktable config dir into
it (with darktable closed, or with SQLite's backup command), and set
`write_sidecar_files=never` in the copied darktablerc so the engine never
writes XMP files next to your photos.

The engine holds the library lock while it runs, like darktable. A darktable
GUI built from the same tree can open the library after `library_release`.

## Protocol

JSON-RPC 2.0, one request per line on stdin, one reply per line on stdout:

    {"jsonrpc":"2.0","id":1,"method":"session_open","params":{"imgid":42}}
    {"jsonrpc":"2.0","id":2,"method":"module_set","params":{"operation":"exposure","values":{"exposure":0.5}}}
    {"jsonrpc":"2.0","id":3,"method":"render","params":{"width":1200,"height":1200,"path":"/tmp/p.jpg"}}

Session methods (`module_*`, `history_*`, `save`, `reset`, `render`,
`session_close`) take an optional `imgid`; without it they work on the image
the client last opened. Errors are JSON-RPC errors with a readable
`message`, a `code` and `data` {`name`, `retry`, `param`}: `retry` says the
same request can work later, `param` names the parameter concerned when
there is one (capability `errors.codes`; older servers send -32000 and a
message only):

| code | name | means |
|---|---|---|
| -32700 | `parse_error` | the request isn't JSON |
| -32601 | `unknown_method` | no such method |
| -32602 | `invalid_params` | a parameter is missing, of the wrong type, out of range, or an unknown setting or value |
| -32000 | `failed` | the operation failed (rendering, writing a file, the database, ...) |
| -32001 | `not_found` | no such image, module, instance, shape, spot, preset, style, picker or job |
| -32002 | `not_open` | a session method on an image no session has open: `session_open` first |
| -32003 | `released` | the library is released or held by another process (`retry`: after `library_acquire`) |
| -32004 | `busy` | a job, a dialog in darktable's window or another task is in the way (`retry`) |
| -32005 | `unavailable` | not possible on this server: headless vs darktable's window, a build without AI, a module without that feature |
| -32006 | `unsaved_changes` | the image has unsaved changes the method would lose or ignore: save first (or the method's `save` option) |

| method | does |
|---|---|
| `ping` | version, `server` (`engine` or `gui`) and `methods`: the methods this server has, and options added to a method later (`render.zoom`), so a client can check before calling |
| `client_hello {kind, name}` | a client says what it is: `kind` a short word (at most 32 bytes: `mcp` for darktable-api-mcp, `web` for the web app, `web-worker` for its helper processes), `name` free text (at most 128 bytes, optional); replaces earlier values; replies `client`; the other clients get a `clients` event. Works while the library is released |
| `clients` | `{clients: [{id, kind, name, imgid, self}]}` in connection order: `kind` and `name` are null until the client said hello, `imgid` is the image it last opened (0 if none), `self` marks the caller. Works while the library is released |
| `film_rolls` | film rolls with image counts |
| `images_list {film_id, rating, label, tag, offset, limit}` | images in folder/filename order with rating, reject, color labels; `rating` is `visible` (default), `all`, `rejected` or `1`..`5` (at least); `tag` (`images_list.tag`) only those with that tag or one under it (`places` takes `places\|paris`; case ignored) |
| `image_info {imgid}` | one image, as in `images_list`, and its camera data (`exif`: maker, model, lens, aperture, exposure time, bias, ISO, focal length, focus distance, crop factor, when taken, raw / HDR / monochrome) |
| `collection {offset, limit}` | darktable's current collection (the lighttable's and filmstrip's photos, in their order), its rules, and the selected images |
| `darkroom_open {imgid}` | darktable's window only: shows the photo in the darkroom, as clicking it in the filmstrip (in the darkroom) or double-clicking it in the lighttable does; the switch completes on the main loop (a `darkroom` event, `library_status` `darkroom_imgid`) |
| `thumbnail {imgid, size, path, quality}` | JPEG from darktable's thumbnail (mipmap) cache, rendered with the current edit if not cached, scaled to fit inside size x size (never enlarged) |
| `thumbnails {imgids \| film_id, size, outdated_only}` | a background job that makes thumbnails in bulk, as `darktable-generate-cache` does: for each image (the `imgids`, a film roll, or with neither the whole library) the thumbnail levels up to the one holding `size` (default 720) are rendered with the saved edit and written to darktable's disk cache, where darktable's lighttable and `thumbnail` find them. With `outdated_only` (default) an image is skipped when every level is on disk and its history hasn't changed since. One at a time; cancellable. The job's result: `images`, `generated`, `skipped`, `failed`, `max_level`, `disk_cache` (darktable's preference; off, they stay in memory only) |
| `import {paths, recursive, copy, jobcode, skip_existing}` | a background job that imports photos as darktable's import does (`control_jobs.c`): `paths` are files or folders (a folder's photos; `recursive` also its subfolders). Without `copy` they are added where they are ("add to library"); with `copy` they are copied into the folders darktable's import settings name (`session/base_directory_pattern`, `session/sub_directory_pattern`, `session/filename_pattern`; `jobcode` fills `$(JOBCODE)`, default darktable's last one) and added there ("copy & import": a raw's JPEG twin keeps its name, the copy its modification time, the original name in `Xmp.xmpMM.PreservedFileName`). With `skip_existing` (default) a photo already in the library is skipped: the same file, the id copy & import stores (name and modification time, which darktable's import dialog checks), or the same file name and capture time (a modification time lost in an upload). darktable's "ignore non-raws" preference is followed. One at a time; cancellable. The job's result: `files`, `copy`, `film_ids`, `imported` (`source`, `imgid`, `file`), `skipped` (`source`, `imgid` of the photo already there, `reason`), `failed` (`source`, `error`) |
| `image_metadata {imgid}` | an image's tags (not darktable's own `darktable\|...`), metadata fields as the metadata editor names them (title, description, creator, publisher, rights, notes, version name, ...) and location (`latitude`, `longitude`, `elevation`, or null) |
| `set_tags {imgids, attach, detach}` / `tag_list {filter}` | attach and detach tags by name (`\|` for the hierarchy; new ones created) as the tagging module does; the library's tags with how many images carry each |
| `set_metadata {imgids, values}` | metadata fields by name or xmp key (`""` clears one), as the metadata editor |
| `set_location {imgids, latitude, longitude, elevation, clear}` | the location, as geotagging sets it; `clear` removes it. These three write the sidecar and tell the window's panels; each image gets an `image` event |
| `set_rating {imgid, rating}` / `set_label {imgid, label, on}` | 0..5 or `"reject"` (as the lighttable); color label 0..4 (red, yellow, green, blue, purple) |
| `marker_set {imgid, name, kind, points, note}` / `marker_list {imgid}` / `marker_remove {imgid, name}` or `{imgid, all: true}` | named markers: points and areas a client puts on a photo, so others can refer to them ("left tree"). Per image, shared by every client, kept in `<library>.markers.json` (in memory for a `:memory:` library), not in the edit history. `kind`: `point` (1 point), `rect` (4 corners), `ellipse` (8-128 outline points), `outline` (3-500); points are `[x, y]` in raw space (as `mask_add` shapes); `name` 1-64 chars, unique per image ignoring case (setting an existing one replaces it in place); `note` <= 500 chars; at most 100 per image. A marker is `{name, kind, points, note, by, updated}` (`by`: the setting client's `client_hello` kind). `marker_set` and `marker_remove` send a `markers` event |
| `session_open {imgid, fresh}` / `session_close` | open a photo for editing: load it and replay its history, as the darkroom does, or join the session another client has open (`joined`, `unsaved` in the reply); `fresh` reloads it as saved, dropping unsaved changes for everyone (refused for the photo in darktable's darkroom, which writes its edit to the library every `autosave_interval` seconds itself: undo there with `history_end`). `session_close` closes it for everyone |
| `module_list` | the photo's modules in pipe order: enabled, in history, and `label` as darktable shows the instance ("local contrast", "exposure • sky") |
| `module_get {operation, instance, raw}` | settings by name through introspection: value, default, declared range, enum names and labels; lists with their `shape` and nested values; `raw` adds `params_hex`, the settings as darktable stores them (every field, also those not listed) |
| `module_set {operation, instance, values}` | change settings by name, all or none; range-checked (`strict: false` lets values outside a field's declared range through, as darktable stores some there itself, e.g. borders' aspect -1); enums by name, label or number; lists of floats, bools, enums or levels (rawprepare's black levels) whole (nested arrays of their `shape`) or by element (`grey[1]`, `x[0][3]`); switches the module on and adds a history item (consecutive edits of one module merge, as in the darkroom). On the darkroom's photo in darktable's window the module's own window code then runs for each changed setting, as after moving its control, and may adjust others (exposure's black point, agx's pivot, filmic's range, ...): the reply lists them in `darktable_also_changed`. The other settings sent in the same call keep their values, lists sent whole included (color harmonizer's `custom_hue` sent with its rule; a setting's own adjustment, such as borders snapping its aspect, stays). Headless the engine makes the same adjustments for the modules ported so far (`also_changed.c`: 21 modules, see darktable-api's `gui_changed_inventory.json` for the rest), listed the same way |
| `coords {points, from, to}` | map `[x, y]` points between `raw` (drawn-mask space: the pipe's input, normalized; circle radii are relative to its shorter side), `image` (the rendered image, fractions) and `uncropped` (crop's input), through the pipe's distortions (lens, rotation, crop) as the darkroom maps the mouse |
| `retouch_list` | retouch's shapes: type, active, circle center, radius and feather, source (raw space), algorithm and its options (blur type and radius; fill mode, color and brightness) |
| `retouch_add {spots: [{x, y, r, algorithm, sx, sy, blur_type, blur_radius, fill_mode, fill_color, fill_brightness}]}` | add circles in raw space as one history item, as clicking with retouch's circle tool (feather from the user's circle border setting): `clone` or `heal` (default) from a source `sx, sy`, `blur` (gaussian or bilateral), `fill` (erase, or a color in the module's working RGB); options not given come from the module's current ones, as in the darkroom. `retouch_heal` is the same with heal as the default; in darktable's window the darkroom's photo goes through the darkroom's own path |
| `retouch_set {formid, x, y, r, sx, sy, algorithm, ...}` | move, resize or change one spot: its circle, source, tool (clone and heal swap, as do blur and fill, as the darkroom allows) and the tool's options |
| `retouch_remove {formids}` | delete spots as one history item |
| `blend_get` / `blend_set {operation, instance, values}` | a module's blending in the darkroom's names and units: `masks` (off, uniform, drawn, parametric, drawn & parametric, raster), `blend_mode`, `reverse`, `opacity`, `blend_parameter`, `feathering_radius`, `blur_radius`, `brightness`, `contrast`, `details`, `combine`, `feathering_guide`, and `parametric` ranges per channel and direction (`"g_in": {"range": [4 values], "inverted", "boost"}`, percent / Lab a,b / hue degrees as the sliders show them), `raster_source` (`{operation, instance}` of an earlier module with a mask, or null: its mask as this module's, as the raster mask menu) and `raster_inverted`; all or none, one history item |
| `mask_add {operation, instance, shape, combine, inverted}` / `mask_list` / `mask_remove {formid}` | drawn shapes on a module's mask: circle (`x, y, r, border`), ellipse (`ra, rb, rotation, border`), gradient (`rotation, compression, steepness, curvature`), path (`points` (3 or more, closed), `border`) and brush (`points` (2 or more), `width`, `hardness`, `density`): a point is `[x, y]` (control points smoothed through its neighbours, as the tools draw them) or `{corner, ctrl1, ctrl2, border, hardness, density, smooth}` as `mask_list` gives it (kept as given; `smooth: false` without control points: a sharp corner); `corners: "sharp"` (`mask_add.corners`) puts the control points of `[x, y]` points on them, as the path tool's ctrl+click, for a polygon or polyline (default `"smooth"`), and a group (`members`, each a shape with its `combine`, `inverted`, `opacity`, `name`), in raw space (`coords`); combined by union, intersection, difference or exclusion (with flexi masks also screen, product and sum), with an `opacity`. `mask_list` gives each shape's full geometry, and a group's members, so an edit's shapes can be copied. With darktable's flexi masks (master, 5.8 on) a group folds its shapes with one operator: a shape whose `combine` differs from its group's makes a new group, the shapes before it its first member (classic's left fold); `mask_list` gives each shape its group's operator (also screen, product or sum, from darktable's masks panel) |
| `mask_ai {operation, instance, points: [{x, y, include}], combine, inverted}` | darktable's AI object mask (SAM, `src/common/ai/segmentation.c`): the photo as rendered is encoded (kept while its geometry is unchanged; ~4-13 s the first time on an M1, then ~50 ms per mask), the mask around the include points (fractions of the image) is traced into path shapes (`ras2forms`) in a group on the module's mask, holes subtracted. Needs a build with `USE_AI` and `plugins/ai/enabled` |
| `mask_ai_encode` | the slow part of a first `mask_ai` on a photo (encoding it) as a background job; `mask_ai` is refused while it runs, and quick after |
| `ai_denoise {imgid, strength, path}` | darktable's AI raw denoise (neural restore: RawNIND on the sensor data, as its raw denoise does) as a background job: a DNG (by default darktable's output pattern beside the photo), imported into the photo's film roll and group with its rating, labels, metadata and tags. Replies the job at once |
| `ai_fill {shape, context, model, name, path, size}` | fill a drawn shape with an AI inpainting model (`inpaint-lama` by default, else `inpaint-migan`: a folder with `config.json` and `model.onnx` in darktable's models folder; `model` another): the photo where the composite module (overlay) gets it, around the shape (`context`: the margin in shape sizes, 0.25..4, default 0.5), is filled at 512 x 512 (`size`: the model's working size, 256..4096, a multiple of 8, for a model without fixed sizes; larger does not fill large shapes better) in linear sRGB (from camera RGB through the camera's matrix), resampled back with the photo's grain and written as a float TIFF (`path`: a pattern, default `$(FILE_FOLDER)/darktable_fills/$(FILE.NAME)_fill`), imported into the photo's group and placed 1:1 by a new composite instance (`name`, default "fill"; compositing "native", the image at its own size, where the build has it, else high precision). Only in builds whose composite has high-precision compositing (master, 5.8 on; `ping` lists it then) with `shape` (as `mask_add`'s) as its mask. Before exposure and color calibration, so later edits change the fill as the rest. Replies the instance, the patch's `imgid`, `file` and box (`patch`: pixels and fractions of the composite's input, `model_size`), `shape_px` (the shape's longest side at full size) and a `warning` to pass on: over 1280 px, large shapes fill as a soft smear rather than texture; for a fill made elsewhere (`filled`) enlarged more than 1.25x to the patch (`patch.size` / `model_size`, not upscaled), that it is softer than the photo. Runs on the request thread (about a second) |
| `ai_fill {shape, crop_to, context, name, size}`, then `ai_fill {instance, filled, model, context, path}` (`ai_fill.external`) | a fill made elsewhere (an online model, for one), in two calls. `crop_to` (a path without extension): instead of running a model, writes the crop the model would get as `<crop_to>.png` (8-bit RGB at `size` x `size`, encoded as the model sees it: linear sRGB normalized and gamma 2.2) and its hole as `<crop_to>_mask.png` (white: fill); the composite instance stays set up with its mask and no patch. Replies the instance, `image`, `mask`, `size`, the patch box, `shape_px`. `filled`: a PNG of that crop filled (any size, stretched to the crop), for that `instance`; finishes as `ai_fill` does (grain, the TIFF, placed 1:1), `model` naming what made it; `upscale: true` runs a fill much smaller than the crop (over 1.25x) through darktable's AI upscaler first (the active upscale model, 2x or 4x; `patch.upscaled` in the reply gives the factor, missing when none is installed; `background: true` (`ai_fill.background`) replies a job at once (`task` "ai_fill", `patch.size`): the PNG is read then, the upscale and resample run on a thread, and the job's `result` is what ai_fill replies (an `edit` event when it is placed); a second one on the photo while it runs is refused; cancelled, the instance stays without a patch. Without it, on the request thread: minutes on a CPU). Unless `color_match: false`, the fill is first matched to the photo: per channel, the gain and offset that take the returned crop's known pixels (near the hole) to ours are applied to the fill, undoing a model that shifted the whole crop (FLUX Erase does); `patch.color_match` gives them. The same `context` both times. To drop a fill that didn't come, remove the instance (`module_remove`; instance 0: `module_enable` off) |
| `job_status {job}` / `job_list` / `job_cancel {job}` | background jobs: state (running, done, failed, cancelled), progress where known, result (`new_imgid`, `file`) or error. Every client gets `{"type": "job", ...}` when one ends. In darktable's window a job has a darktable job handle (progress in its progress bar; cancel stops the computation); headless, progress is unknown (`thumbnails` counts its images) and a cancelled job's result is discarded. `library_release`, `handover` and `shutdown` are refused while a job runs |
| `sample {width, height, zoom, center_x, center_y, uncropped, points, radius, boxes, bins}` | render as `render` does and read the output (display sRGB): color (sRGB, Lab, luminance range) at points and in boxes, R/G/B/luminance histograms, clipped highlight/shadow fractions |
| `preset_list {operation, instance}` / `preset_apply {operation, instance, name}` | the module's presets for its version (`name`, `label` as the menu shows it, `builtin`, `autoapply`); apply one by name or label as the presets menu does: settings, on/off, blending and the module's label, one history item |
| `module_enable {operation, instance, enabled}` | module on/off |
| `capture_autotune` | measure darktable's capture sharpening radius and noise threshold on the open photo (the full-size raw, about 1 s for 16 MP, as the reset button by the radius does) and store `cs_enabled` true, `cs_radius`, `cs_thrs` as one history item, through `module_set`; works on edited and unedited photos. Replies as `module_set` (`values`, `enabled`, `history_end`, maybe `darktable_also_changed`) plus `ms`; sends an `edit` event. Refused (-32602) for a non-raw or monochrome DNG, passthrough demosaic or a 4-color sensor ("capture sharpening needs a raw photo demosaiced in color ..."), and without a session. In darktable's window it measures on the render mirror and stores through the darkroom |
| `history_list`, `history_end {end}` | history items, each with its `label` as darktable's history panel shows it (`history_list.label`); undo/redo to a step (0 = original) |
| `history_compress {truncate}` | compress the history stack as the history panel does (`truncate`: only drop the steps above `history_end`); it works on the library, so the edit is saved first |
| `module_move {operation, instance, before \| after: {operation, instance}}` | move a module in the pipe, as dragging it in the darkroom (darktable's rules for what may move apply) |
| `curve_get` / `curve_set {operation, instance, channel, points, type}` | curves of rgbcurve (R, G, B), tonecurve (L, a, b), colorzones (lightness, chroma, hue) and basecurve: points `[[x, y], ...]` (x increasing, 0..1) and spline type per channel |
| `style_list {filter}` | darktable's styles: `name`, `label` as the styles module shows it, description, items (operation, instance, name, enabled), whether it sets the module order |
| `style_create {name, description, imgid, modules, module_order}` | a style from an image's saved edit: the modules darktable's create style dialog ticks by default (those meant for styles), or those listed (names or `{operation, instance}`) |
| `style_apply {name, imgids, save}` / `style_delete {name}` | apply a style to images (default the client's), as the lighttable and darkroom do: its modules added to each edit as history items, written to the library. A session here is saved first (refused while it has unsaved changes unless `save`) and reopened after; the darkroom's photo goes through the darkroom (`dt_styles_apply_to_dev`). Each image gets a `reopened` (open here) or `image` event |
| `history_paste {from, imgids, mode, modules, module_order, save}` | the source's saved edit onto images, as the lighttable's copy and paste: all of it (as copy), or the modules listed (as selective copy); `mode` `append` (default) or `overwrite`. Sessions as for `style_apply` |
| `picker_list {operation, instance}` / `picker_apply {operation, instance, control, box \| point}` | in darktable's window, the darkroom's photo: a module's pickers and auto buttons (`picker` or `button`, named by their action path in the module, e.g. `exposure range/auto tune levels`), and pressing one as a click or shortcut does. A picker samples `box` (`left, top, right, bottom`) or `point` (`[x, y]`, fractions of the image as rendered; default darktable's area for a fresh picker) and is switched off when darktable has applied it. A background job: its result lists the settings that changed. The job starts darktable's pipes itself, as the window draws only while the screen shows it. Tone equalizer's wands need its graph drawn (its histogram is computed there), so they do nothing while the screen is locked. The result has the `sample` darktable applied (`colorspace`, `mean`, `min`, `max`, the module's input in the picker's color space), and `output_sample` (the module's output) for a picker that reads it (retouch's fill color). A control darktable hides for this image (hidden, or on a stack page not shown: white balance on a monochrome raw, color calibration's picker without adaptation) is refused at once (`unavailable`). Headless (`picker_apply.headless`): the pickers ported so far (`src/api/pickers.c`: 69 pickers of 26 modules, every one that sets a module, among them exposure, white balance, color calibration, agx, filmic rgb, rgb levels, levels, color balance rgb, color balance, negadoctor, invert, spektrafilm, color harmonizer, retouch's fill color and the curves' "create curve"), listed by `picker_list`; a preview pipe of the session's edit, on the same downscaled image darktable's preview renders (the mipmap), takes the sample inside the pipe as darktable's preview pipe does for the darkroom's picker (`dt_dev_pixelpipe_t.picker`: the module's input, white balance's being the raw mosaic, and its output when the picker reads it), and the module's picker math applies it (exposure's and color calibration's area mode "correction": exposure to the lightness darktable keeps for it, the illuminant that maps the area to the mapping target darktable keeps, neutral grey by default). A picker darktable hides is refused (`unavailable`: color calibration's while its adaptation is "none (bypass)", filmic rgb's middle gray without custom middle gray values, spektrafilm's without its data pack, which its simulation needs). Color balance keeps its picks per module while the session is open, as its controls do in the window: its optimizers fit to the patches its factor and hue pickers took before, their own area filling only the patches not picked. The job has ended when the reply comes; one history step, switching the module on as the window's picker does. Others: `unavailable` |
| `image_duplicate {imgid, virgin, save}` | a duplicate (virtual copy, next version) as the lighttable's duplicate: with the saved edit (unsaved changes refused unless `save`) or none (`virgin`) |
| `image_remove {imgids}` | remove images from the library as the lighttable's remove does (`control_jobs.c`): library entries and edits deleted, files and other versions kept, sidecars of the remaining versions updated; all or nothing, refused for the darkroom's photo, unsaved changes, a local copy, an image used as an overlay, or while a job runs. Other clients get a `removed` event |
| `module_add {operation, instance, copy}` / `module_remove` / `module_rename {name}` | module instances: a new one after the given one (`copy`: duplicate its settings, blending and drawn shapes, the shapes shared as in the darkroom), delete one (not a module's only instance; its history steps go), label one |
| `geometry_get` | orientation (`rotation` clockwise from the raw file, `mirrored`), straightening `angle` and `autocrop` (rotate and perspective), `crop` box (`left`, `top`, `right`, `bottom`, fractions of the uncropped image), `aspect`, and the uncropped (`frame_width`/`frame_height`) and final sizes |
| `geometry_set {rotate, flip, angle, autocrop, crop, aspect}` | as the darkroom does, all or nothing: `rotate` turns by 90° steps (clockwise; -90 left) and `flip` (`horizontal`/`vertical`) mirrors in the flip module, and an existing crop box follows; `angle` straightens (degrees, positive turns counter-clockwise) and the automatic crop is refitted as ashift's GUI does; `crop` sets the box (`null` removes it); `aspect` (`free`, `original`, `square`, `W:H`) fits the largest box of that ratio inside the given or current one |
| `save` | write the history to the library (and the sidecar, if `write_sidecar_files` asks for it), as leaving the darkroom does. A session without darktable's window that saves a photo unedited in the database (after `reset`, or a fresh import with only auto-applied presets) whose demosaic has `cs_enabled` on and `cs_radius` exactly 0 first measures as `capture_autotune` does and stores radius and threshold as a history item (darktable's window does the same on the first render) |
| `reset` | discard the history and reload, so darktable applies the workflow defaults and auto-apply presets again |
| `export {imgid, format, quality, max_width, max_height, high_quality, upscale, style, path, on_conflict, save}` | darktable's export of the image's saved edit through a format module, as the export module does: its settings (format, size, quality, output pattern `$(FILE_FOLDER)/darktable_exported/$(FILE_NAME)`, conflict handling, metadata, ICC profile) unless given; tags the image `darktable|exported`. Unsaved changes are refused unless `save` is true. Replies `file`, or `skipped` when `on_conflict` (`unique`, `overwrite`, `overwrite_if_changed`, `skip`) leaves an existing file alone. Runs on the request thread (darktable's window pauses while it exports) unless `background`: then it replies a job at once, whose result is this reply (an export runs to its end; it can't be cancelled) |
| `render {width, height, path, quality, uncropped, zoom, center_x, center_y, history_end, show_mask}` | sRGB JPEG fitted inside width x height; `uncropped` leaves crop's box out (as the darkroom shows the image while crop has the focus), to draw a box on; `zoom` (1 = 100%, up to 2) renders the width x height region around `center_x`/`center_y` (fractions) at that scale, as the darkroom zoomed in, and replies its `region`; `history_end` renders an earlier step (before/after) without undoing (also for `sample`); `show_mask {operation, instance}` (capability `render.show_mask`) shows that module's mask as the darkroom's "display mask" does, yellow over a gray image, without the module having the focus; refused if the module is off or has no mask; replies `mask_shown` (false: the mask is the same everywhere, nothing drawn) |
| `render_variants {variants: [{label, path, steps: [{method, params}]}], width, height, quality, uncropped}` | alternatives for the user to choose from (crops, B&W filters, looks): each variant's steps (`module_set`, `module_enable`, `preset_apply`, `blend_set`, `curve_set`, `geometry_set`; nothing that adds or moves an instance) applied to the edit, rendered to its path, and taken back, so the history is as before; a failing step fails only its variant (`error`). On the darkroom's photo they run on the render mirror, not the darkroom's edit |
| `darkroom_snapshot {history_end, show}` | darktable's window only: a snapshot in the darkroom's snapshots module of its photo at a history step (0 = the original), shown split against the edit as clicking it does (`show`, default true); the edit stays where it is. Refused when all 10 slots are used |
| `share {kind, imgid, data}` | relay something to the other clients as a `shared` event with its `kind` and `data`, e.g. a comparison an AI prepared for the web app to show (`compare`: `{history_end}`, `variants`: `{variants}`); the engine doesn't interpret it |
| `library_status` / `library_release` / `library_acquire` | hand the library to darktable's GUI and take it back while running; unsaved edits of every open photo are kept and restored if the photo wasn't changed meanwhile |
| `shutdown {quit_window}` | close and exit (SIGTERM does the same). Served by darktable's window, only with `quit_window: true`: darktable quits as from its quit menu (the darkroom's history saved), after replying; refused while a modal dialog is open. For a client that needs the library; on macOS this needs no Automation permission, unlike a quit request from another application |

## Measured (Apple M1, CPU only, 20 MP raw with a 22-step edit)

| | |
|---|---|
| start | 3.2 s |
| first render 600 / 1200 / 1920 px | 0.29 / 0.37-0.49 / 0.9 s |
| render after a change, 1200 px | 0.14-0.39 s (a repeat: ~3 ms) |
| memory with the photo open | 1.7 GB |
| release / acquire | ~1 ms / 30-90 ms |
| list 770 images / a page | < 1 ms |
| thumbnail ~600 px: cached / rendered | 1 ms / 0.2-0.9 s |

The engine's render matches darktable's own export of the same edit within
about 1/255 (mean absolute difference).

## Not done

- headless, settings the module's GUI code adjusts alongside a change
  (`gui_changed`) are left as they are; in darktable's window they follow
- auto buttons headless (rgb levels' and levels' "auto levels", tone
  equalizer's wands): they need darktable's window
- integer arrays and arrays of structs other than curves, path and brush
  shapes
- export to storages other than disk
- perspective correction (ashift's line detection and fitting); the
  geometry calls cover orientation, straightening and crop only
- highlight reconstruction switching itself off on a fresh raw with nothing
  clipped (the window does it after its first render; headless it stays on,
  which changes nothing visible)
- `shutdown` stops the engine for every client
