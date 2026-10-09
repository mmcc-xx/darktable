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

This code was written with AI assistance (Claude), directed and reviewed by
the branch owner.

## Build

Build darktable as usual with the MCP server enabled (`-DUSE_MCP=ON`, or
`./build.sh --enable-mcp`); `darktable-api` is built with it:

    cmake --build build --target darktable-api

## Run

    darktable-api --core --configdir <dir> [--cachedir <dir>] [darktable options]

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

| method | does |
|---|---|
| `ping` | version |
| `film_rolls` | film rolls with image counts |
| `images_list {film_id, rating, label, offset, limit}` | images in folder/filename order with rating, reject, color labels; `rating` is `visible` (default), `all`, `rejected` or `1`..`5` (at least) |
| `image_info {imgid}` | one image, as in `images_list` |
| `thumbnail {imgid, size, path, quality}` | JPEG from darktable's thumbnail (mipmap) cache, rendered with the current edit if not cached |
| `set_rating {imgid, rating}` / `set_label {imgid, label, on}` | 0..5 or `"reject"` (as the lighttable); color label 0..4 (red, yellow, green, blue, purple) |
| `session_open {imgid}` / `session_close` | load a photo and replay its history, as the darkroom does (one open photo at a time) |
| `module_list` | the photo's modules in pipe order: enabled, in history |
| `module_get {operation, instance}` | settings by name through introspection: value, default, declared range, enum names and labels |
| `module_set {operation, instance, values}` | change settings by name, all or none; range-checked; enums by name, label or number; switches the module on and adds a history item (consecutive edits of one module merge, as in the darkroom) |
| `module_enable {operation, instance, enabled}` | module on/off |
| `history_list`, `history_end {end}` | history items; undo/redo to a step (0 = original) |
| `save` | write the history to the library (and the sidecar, if `write_sidecar_files` asks for it), as leaving the darkroom does |
| `reset` | discard the history and reload, so darktable applies the workflow defaults and auto-apply presets again |
| `render {width, height, path, quality}` | sRGB JPEG fitted inside width x height |
| `library_status` / `library_release` / `library_acquire` | hand the library to darktable's GUI and take it back while running; unsaved edits are kept and restored if the photo wasn't changed meanwhile |
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
  and order, styles, export, tags and metadata, import
- one open photo per process
