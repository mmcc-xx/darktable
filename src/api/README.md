# darktable-api (proof of concept)

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

This code was written with AI assistance (Claude), directed and reviewed by
the branch owner.

## Build

Build darktable as usual with the MCP server enabled (`-DUSE_MCP=ON`, or
`./build.sh --enable-mcp`); `darktable-api` is built with it:

    cmake --build build --target darktable-api

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
  `image` (rating or label), `library_released`, `library_acquired`.
- `--idle-exit` stops the engine that many seconds after the last client
  disconnected, unless an image has unsaved changes.

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
- `library_release` and `shutdown` are refused while the window serves.

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
the client last opened. Errors are JSON-RPC errors (code -32000) with a
message.

| method | does |
|---|---|
| `ping` | version |
| `film_rolls` | film rolls with image counts |
| `images_list {film_id, rating, label, offset, limit}` | images in folder/filename order with rating, reject, color labels; `rating` is `visible` (default), `all`, `rejected` or `1`..`5` (at least) |
| `image_info {imgid}` | one image, as in `images_list` |
| `thumbnail {imgid, size, path, quality}` | JPEG from darktable's thumbnail (mipmap) cache, rendered with the current edit if not cached |
| `set_rating {imgid, rating}` / `set_label {imgid, label, on}` | 0..5 or `"reject"` (as the lighttable); color label 0..4 (red, yellow, green, blue, purple) |
| `session_open {imgid, fresh}` / `session_close` | open a photo for editing: load it and replay its history, as the darkroom does, or join the session another client has open (`joined`, `unsaved` in the reply); `fresh` reloads it as saved, dropping unsaved changes for everyone. `session_close` closes it for everyone |
| `module_list` | the photo's modules in pipe order: enabled, in history |
| `module_get {operation, instance}` | settings by name through introspection: value, default, declared range, enum names and labels |
| `module_set {operation, instance, values}` | change settings by name, all or none; range-checked; enums by name, label or number; switches the module on and adds a history item (consecutive edits of one module merge, as in the darkroom) |
| `module_enable {operation, instance, enabled}` | module on/off |
| `history_list`, `history_end {end}` | history items; undo/redo to a step (0 = original) |
| `geometry_get` | orientation (`rotation` clockwise from the raw file, `mirrored`), straightening `angle` and `autocrop` (rotate and perspective), `crop` box (`left`, `top`, `right`, `bottom`, fractions of the uncropped image), `aspect`, and the uncropped (`frame_width`/`frame_height`) and final sizes |
| `geometry_set {rotate, flip, angle, autocrop, crop, aspect}` | as the darkroom does, all or nothing: `rotate` turns by 90° steps (clockwise; -90 left) and `flip` (`horizontal`/`vertical`) mirrors in the flip module, and an existing crop box follows; `angle` straightens (degrees, positive turns counter-clockwise) and the automatic crop is refitted as ashift's GUI does; `crop` sets the box (`null` removes it); `aspect` (`free`, `original`, `square`, `W:H`) fits the largest box of that ratio inside the given or current one |
| `save` | write the history to the library (and the sidecar, if `write_sidecar_files` asks for it), as leaving the darkroom does |
| `reset` | discard the history and reload, so darktable applies the workflow defaults and auto-apply presets again |
| `export {imgid, format, quality, max_width, max_height, high_quality, upscale, style, path, on_conflict, save}` | darktable's export of the image's saved edit through a format module, as the export module does: its settings (format, size, quality, output pattern `$(FILE_FOLDER)/darktable_exported/$(FILE_NAME)`, conflict handling, metadata, ICC profile) unless given; tags the image `darktable|exported`. Unsaved changes are refused unless `save` is true. Replies `file`, or `skipped` when `on_conflict` (`unique`, `overwrite`, `overwrite_if_changed`, `skip`) leaves an existing file alone. Runs on the request thread (darktable's window pauses while it exports) |
| `render {width, height, path, quality, uncropped}` | sRGB JPEG fitted inside width x height; `uncropped` leaves crop's box out (as the darkroom shows the image while crop has the focus), to draw a box on |
| `library_status` / `library_release` / `library_acquire` | hand the library to darktable's GUI and take it back while running; unsaved edits of every open photo are kept and restored if the photo wasn't changed meanwhile |
| `shutdown` | close and exit (SIGTERM does the same) |

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

- settings the GUI adjusts alongside a change (`gui_changed`), pickers and
  other operations whose logic lives in GUI code
- array fields (e.g. channel mixer coefficients), masks, module instances
  and order, styles, tags and metadata, import; export to storages other
  than disk
- perspective correction (ashift's line detection and fitting); the
  geometry calls cover orientation, straightening and crop only
- `shutdown` stops the engine for every client
