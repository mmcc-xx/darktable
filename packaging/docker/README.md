# darktable-api in Docker

A headless darktable-api engine, the MCP server (`darktable-api-mcp`) and the
web app (port 8080) in one Ubuntu 24.04 image. It edits a darktable library
you mount into it; there is no window.

## Build

From the source root, with the MCP and web app repos next to it:

    docker build -f packaging/docker/Dockerfile \
      --build-context mcp=../darktable-api-mcp \
      --build-context web=../darktable-api-web \
      -t darktable-api .

(amd64 only so far; the build compiles darktable, about half an hour on 4 cores.)

## Run

    docker run -d --name dtapi -p 8080:8080 \
      -v $HOME/dt-config-copy:/library/config \
      -v dtapi-cache:/cache \
      -v /home/me/photos:/home/me/photos:ro \
      -v $HOME/.local/share/darktable/models:/home/dt/.local/share/darktable/models:ro \
      darktable-api

- `/library/config`: a COPY of your darktable config dir (`library.db`,
  `data.db`, `darktablerc`), read-write and owned by uid 1000 (the
  container's user). The container refuses to start without `library.db`.
  In the copied `darktablerc`, set
  `plugins/ai/models_path=/home/dt/.local/share/darktable/models` and
  delete any `plugins/ai/ort_library_path` line (the image has its own
  onnxruntime; a host path there breaks the AI features).
- `/cache`: a volume for thumbnails and the like.
- Photos: mount them at the SAME absolute path the library has for them (the
  library stores absolute paths). `:ro` by default; the engine then cannot
  write sidecar files, so set `write_sidecar_files=never` in the copied
  `darktablerc`. Mount read-write if you want sidecars or `ai_fill` files
  (written beside the photo, in `darktable_fills/`).
- Models: darktable's models folder, read-only, for the AI features
  (the AI object mask, denoise, local `ai_fill`).
- Open http://localhost:8080/. The engine starts on the first request.

One container per library, and darktable on the host must not open that
library meanwhile.

## Claude Code

    claude mcp add darktable-docker -- docker exec -i dtapi darktable-api-mcp

It reaches the same engine as the web app (same socket inside the container).

## Limits

- Headless only: pickers that need darktable's window do not work.
- The web app has no AI chat.
- Online fills (`ai_fill(model=...)`): pass the key with `-e BFL_API_KEY=...`
  at `docker run`; it is never in the image.
- amd64 only so far; arm64 (Mac) and a registry image come later.
