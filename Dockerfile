# syntax=docker/dockerfile:1.7
ARG UPSTREAM_REF=5132f8a5e755da84b3eb62279f32c951badf972d

FROM ubuntu:24.04 AS build
ARG UPSTREAM_REF
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl git tar gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64 \
    && rm -rf /var/lib/apt/lists/*
RUN git init /src \
    && git -C /src remote add origin https://github.com/hexonMD/moon-sweepers-dedicated-server.git \
    && git -C /src fetch --depth 1 origin "${UPSTREAM_REF}" \
    && git -C /src checkout --detach FETCH_HEAD
# Make the Steam AppId/GameId runtime-configurable (5335510 retail, 480
# SpaceWar for online-fix clients) instead of hardcoding the retail AppId.
COPY ptero/patches/spacewar.patch /tmp/spacewar.patch
RUN git -C /src apply --check /tmp/spacewar.patch \
    && git -C /src apply /tmp/spacewar.patch
RUN mkdir -p /out \
    && x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -Werror \
       -Wno-cast-function-type -static-libgcc \
       /src/container/server/steam-probe.c -o /out/steam-probe.exe
ARG MINHOOK_COMMIT=c3fcafdc10146beb5919319d0683e44e3c30d537
RUN mkdir -p /native \
    && curl --fail --location --retry 3 \
       "https://github.com/TsudaKageyu/minhook/archive/${MINHOOK_COMMIT}.tar.gz" \
       | tar -xz --strip-components=1 -C /native \
    && cp -a /src/native/. /native/host/ \
    && cd /native \
    && x86_64-w64-mingw32-gcc -shared -O2 -Wall -Wextra -Werror \
       -Iinclude -Isrc -Ihost host/*.c host/version_proxy.S host/version.def \
       src/buffer.c src/hook.c src/trampoline.c src/hde/hde64.c \
       -static-libgcc -lshell32 -luser32 -lgdi32 \
       -Wl,--no-insert-timestamp -o /out/version.dll \
    && cp LICENSE.txt /out/MinHook-LICENSE.txt

FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl python3 tini procps tar gzip tzdata libnss-wrapper \
    lib32gcc-s1 lib32stdc++6 lib32z1 wine64 wine xvfb xauth x11-utils \
    && rm -rf /var/lib/apt/lists/*
# Unity 6000.6 host does not boot under Ubuntu's Wine 9.0 (the 64-bit host
# either access-violates during import resolution or hangs before opening a
# window). Use the current WineHQ stable build, which boots the game and was
# verified end to end, including an anonymous SpaceWar login and a client join.
RUN mkdir -pm755 /etc/apt/keyrings /etc/apt/sources.list.d \
    && curl -fsSL https://dl.winehq.org/wine-builds/winehq.key \
        -o /etc/apt/keyrings/winehq-archive.key \
    && curl -fsSL \
        https://dl.winehq.org/wine-builds/ubuntu/dists/noble/winehq-noble.sources \
        -o /etc/apt/sources.list.d/winehq.sources \
    && dpkg --add-architecture i386 \
    && apt-get update \
    && apt-get install -y --no-install-recommends winehq-stable \
    && rm -rf /var/lib/apt/lists/*
RUN mkdir -p /home/container/game-source /opt/moon/bin /opt/moon/mod \
             /opt/moon/ptero /import \
    && ln -s /home/container /data \
    && ln -s /home/container/game-source /import/game
COPY --from=build /src/container/server/entrypoint /opt/moon/bin/entrypoint
COPY --from=build /src/container/server/steam-redist /opt/moon/bin/steam-redist
COPY --from=build /src/container/server/server-import /opt/moon/bin/server-import
COPY --from=build /src/container/server/health.py /opt/moon/bin/health.py
COPY --from=build /src/container/server/redist-lock.json /opt/moon/bin/redist-lock.json
COPY --from=build /out/steam-probe.exe /opt/moon/bin/steam-probe.exe
COPY --from=build /out/version.dll /opt/moon/mod/version.dll
COPY --from=build /out/MinHook-LICENSE.txt /opt/moon/mod/MinHook-LICENSE.txt
COPY ptero/supervisor.py /opt/moon/ptero/supervisor.py
COPY ptero/sync-game.py /opt/moon/ptero/sync-game.py
COPY ptero/install.sh /opt/moon/ptero/install.sh
COPY ptero/entrypoint.sh /entrypoint.sh
RUN sed -i 's/\r$//' /entrypoint.sh /opt/moon/ptero/install.sh \
              /opt/moon/ptero/supervisor.py /opt/moon/ptero/sync-game.py \
    && chmod 755 /opt/moon/bin/entrypoint /opt/moon/bin/steam-redist \
              /opt/moon/bin/server-import /opt/moon/ptero/install.sh \
              /entrypoint.sh \
    && chmod 644 /opt/moon/ptero/supervisor.py /opt/moon/ptero/sync-game.py
# No USER directive: Wings starts the server as its own uid:gid, and the
# install container must stay root to write into /mnt/server.
ENV HOME=/home/container \
    USER=container \
    WINEARCH=win64 \
    WINEPREFIX=/data/wineprefix \
    WINEDEBUG=-all \
    DISPLAY=:99 \
    SteamAppId=5335510 \
    SteamGameId=5335510 \
    MOON_HOST=1 \
    MOON_DEDICATED=1 \
    MOON_HEADLESS=1 \
    MOON_SLOT=1 \
    MOON_GAME_PORT=27025 \
    MOON_QUERY_PORT=27026 \
    MOON_BACKUP_KEEP=10 \
    WINEDLLOVERRIDES="mscoree,mshtml=;version=n,b;winmm=b;winealsa.drv=d"
WORKDIR /home/container
# Wings does not pass a command: it relies on the image's ENTRYPOINT/CMD to run
# the egg's STARTUP. tini (without -g) reaps Wine's child processes and forwards
# signals only to the supervisor, which handles the graceful save/stop itself.
ENTRYPOINT ["/usr/bin/tini", "--"]
CMD ["/entrypoint.sh"]
LABEL org.opencontainers.image.title="Moon Sweepers Pterodactyl runtime" \
      org.opencontainers.image.description="Unofficial Moon Sweepers CPU-only server runtime for Pterodactyl"
