# syntax=docker/dockerfile:1
# darktable-api: headless engine + MCP server + web app, Ubuntu 24.04 amd64.
# Build (from the source root):
#   docker build -f packaging/docker/Dockerfile \
#     --build-context mcp=../darktable-api-mcp --build-context web=../darktable-api-web \
#     -t darktable-api .

FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git perl gettext intltool po4a xsltproc \
    libsaxon-java libxml2-utils appstream-util desktop-file-utils python3-jsonschema \
    ca-certificates curl pkg-config \
    libarchive-dev libatk1.0-dev libavif-dev libcairo2-dev libcolord-dev \
    libcolord-gtk-dev libcups2-dev libcurl4-gnutls-dev libexiv2-dev \
    libgdk-pixbuf-2.0-dev libglib2.0-dev libgmic-dev libgphoto2-dev \
    libgraphicsmagick1-dev libgtk-3-dev libheif-dev libjpeg-dev libjson-glib-dev \
    liblcms2-dev liblensfun-dev liblua5.4-dev \
    libopencv-calib3d-dev libopencv-core-dev libopencv-features2d-dev \
    libopencv-flann-dev libopencv-imgproc-dev libopenexr-dev libopenjp2-7-dev \
    libosmgpsmap-1.0-dev libpango1.0-dev libpng-dev libportmidi-dev libpotrace-dev \
    libpugixml-dev libraw-dev librsvg2-dev libsdl2-dev libsecret-1-dev \
    libsqlite3-dev libtiff-dev libwebp-dev libx11-dev libxml2-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
COPY . /src
WORKDIR /src
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBINARY_PACKAGE_BUILD=ON \
      -DUSE_AI=ON -DUSE_API=ON -DCMAKE_INSTALL_PREFIX=/opt/darktable \
 && cmake --build build -j4 \
 && cmake --install build
# the installed tree must have the engine and onnxruntime
RUN test -x /opt/darktable/bin/darktable-api \
 && { find /opt/darktable -name 'libonnxruntime.so*' | grep -q . \
      || { f=$(find /src/build -name 'libonnxruntime.so*' | head -n1); test -n "$f" \
           && cp -a $(dirname "$f")/libonnxruntime.so* /opt/darktable/lib/; }; }
# runtime packages: the Ubuntu package of every library the engine and its
# plugins link to (outside /opt/darktable)
RUN { ldd /opt/darktable/bin/darktable-api; \
      find /opt/darktable -name '*.so*' -type f -exec ldd {} \; ; } 2>/dev/null \
    | awk '/=> \//{print $3}' | grep -v '^/opt/darktable' | sort -u \
    | xargs -r realpath | sort -u | xargs -r dpkg -S 2>/dev/null | grep -E '^[a-z0-9][a-z0-9.+-]*(:amd64)?: ' | cut -d: -f1 | sort -u > /runtime-pkgs.txt \
 && wc -l /runtime-pkgs.txt

FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
COPY --from=build /runtime-pkgs.txt /tmp/runtime-pkgs.txt
RUN apt-get update && apt-get install -y --no-install-recommends \
      $(cat /tmp/runtime-pkgs.txt) ca-certificates liblensfun-data-v1 \
      python3 python3-venv python3-pip \
 && rm -rf /var/lib/apt/lists/* /tmp/runtime-pkgs.txt
COPY --from=build /opt/darktable /opt/darktable
# every library must resolve
RUN bad=$( { ldd /opt/darktable/bin/darktable-api; \
             find /opt/darktable -name '*.so*' -type f -exec ldd {} \; ; } 2>&1 | grep 'not found' | sort -u ); \
    if [ -n "$bad" ]; then echo "missing libraries:"; echo "$bad"; exit 1; fi

RUN python3 -m venv /opt/venv
COPY --from=mcp . /src/mcp
COPY --from=web requirements.txt /tmp/web-requirements.txt
RUN /opt/venv/bin/pip install --no-cache-dir "/src/mcp[dust]" -r /tmp/web-requirements.txt \
 && rm -rf /src /tmp/web-requirements.txt
COPY --from=web . /opt/darktable-api-web

# uid 1000 for dt (the image's own "ubuntu" user has it)
RUN userdel -r ubuntu 2>/dev/null; useradd -m -u 1000 -s /bin/bash dt \
 && mkdir -p /run/dtapi /cache /library/config /home/dt/.local/share/darktable/models \
 && chown -R dt:dt /run/dtapi /cache /library /home/dt
COPY packaging/docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod 755 /usr/local/bin/entrypoint.sh

ENV HOME=/home/dt USER=dt \
    PATH=/opt/venv/bin:/opt/darktable/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    DTAPI_BIN=/opt/darktable/bin/darktable-api \
    DTAPI_CONFIGDIR=/library/config \
    DTAPI_CACHEDIR=/cache \
    DTAPI_SOCKET=/run/dtapi/engine.sock \
    DTAPI_MCP=/opt/venv/bin/darktable-api-mcp \
    DTAPI_MCP_PYTHON=/opt/venv/bin/python
USER dt
WORKDIR /opt/darktable-api-web
VOLUME ["/cache", "/home/dt/.local/share/darktable/models"]
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
