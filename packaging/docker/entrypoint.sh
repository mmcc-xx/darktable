#!/bin/sh
# darktable-api container: web app on :8080; it starts the engine on the
# first request.
if [ ! -f /library/config/library.db ]; then
    echo "darktable-api: /library/config/library.db not found." >&2
    echo "Mount a copy of your darktable config dir (library.db, data.db," >&2
    echo "darktablerc) at /library/config, e.g. -v \$HOME/dt-config-copy:/library/config" >&2
    exit 1
fi
exec uvicorn dtweb.main:app --host 0.0.0.0 --port 8080
