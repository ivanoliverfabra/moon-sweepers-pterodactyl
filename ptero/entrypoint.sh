#!/bin/bash
# Pterodactyl / Calagopus entrypoint.
#
# Wings starts the container with an arbitrary uid:gid (for example 988:988)
# that has no entry in /etc/passwd or /etc/group, on a read-only root
# filesystem with a writable /tmp. This script:
#   1. tidies /home/container into game-source, mods, logs, backups and .runtime (see layout.sh), moving the
#      folders of servers set up before that layout,
#   2. gives that uid/gid a name through nss_wrapper (no /etc writes needed),
#   3. substitutes {{VARIABLES}} in the egg's STARTUP string,
#   4. execs the resulting command so signals and stdin reach it directly.

cd /home/container || exit 1
bash /opt/moon/ptero/layout.sh || exit 1

# Programs write caches and settings into HOME; keep them out of the folder the panel shows.
export HOME=/home/container/.runtime/home
export USER=container
export TZ="${TZ:-UTC}"

uid="$(id -u)"
gid="$(id -g)"

if [ "$uid" != "0" ] && ! getent passwd "$uid" >/dev/null 2>&1; then
    nss_lib=/usr/lib/x86_64-linux-gnu/libnss_wrapper.so
    nss_dir="$(mktemp -d /tmp/nss.XXXXXX 2>/dev/null || true)"
    if [ -f "$nss_lib" ] && [ -n "$nss_dir" ]; then
        printf 'root:x:0:0:root:/root:/bin/bash\ncontainer:x:%s:%s:container:/home/container/.runtime/home:/bin/bash\n' \
            "$uid" "$gid" > "$nss_dir/passwd"
        printf 'root:x:0:\ncontainer:x:%s:\n' "$gid" > "$nss_dir/group"
        export NSS_WRAPPER_PASSWD="$nss_dir/passwd"
        export NSS_WRAPPER_GROUP="$nss_dir/group"
        export LD_PRELOAD="${LD_PRELOAD:+$LD_PRELOAD:}$nss_lib"
    else
        echo "[entrypoint] Warning: could not register uid $uid; continuing without a passwd entry." >&2
    fi
fi

if [ -z "${STARTUP:-}" ]; then
    echo "[entrypoint] STARTUP is not set; the panel should provide it from the egg." >&2
    exit 1
fi

# Turn Pterodactyl's {{VAR}} placeholders into ${VAR} and let a fresh shell
# expand them. Variable values are expanded, not re-parsed, so a value cannot
# inject commands. The unexpanded template is printed so secrets stay out of
# the console.
PARSED="$(printf '%s' "$STARTUP" | sed -e 's/{{/${/g' -e 's/}}/}/g')"
printf 'container@moon-sweepers~ %s\n' "$PARSED"
exec bash -c "exec $PARSED"
