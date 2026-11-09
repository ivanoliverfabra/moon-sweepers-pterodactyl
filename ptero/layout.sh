#!/bin/bash
# Keeps /home/container (what the panel's file manager shows) down to the folders people use:
#
#   game-source/   upload the game here (or let the server download it, see PTERODACTYL.md)
#   mods/          BepInEx plugins, patchers, configs and mods.json
#   logs/          moon-host.log and the other logs
#   backups/       world backups made when the server stops
#   .runtime/      everything the server manages itself: the imported game, the Wine prefix (with the saves),
#                  Steam files, caches and HOME
#
# The server's own paths all live under /data, which the image points at .runtime/. The four folders above are linked
# into .runtime/ so /data/mods, /data/logs, /data/backups and /data/game-source still reach them.
#
# Servers set up before this layout had all of these at the top. They are moved into .runtime/ once, here, before
# anything else starts. Nothing is overwritten: if both the old and the new place exist, the old one is left alone.
set -u

root=/home/container
runtime=$root/.runtime
mkdir -p "$runtime/home" "$root/game-source" "$root/mods" "$root/logs" "$root/backups" || exit 1

moved=()
move() { # old new
    local old=$1 new=$2
    [[ -e "$old" && ! -L "$old" ]] || return 0
    if [[ -e "$new" || -L "$new" ]]; then
        echo "Both ${old#"$root"/} and ${new#"$root"/} exist; leaving ${old#"$root"/} where it is."
        return 0
    fi
    mkdir -p "$(dirname "$new")"
    if mv "$old" "$new"; then
        moved+=("${old#"$root"/}")
    else
        echo "Couldn't move ${old#"$root"/} into .runtime/." >&2
    fi
}

# The server's own folders and files.
for name in game wineprefix control probe bepinex-cache steam-redist steamcmd import-manifest.json \
            game-download.json; do
    move "$root/$name" "$runtime/$name"
done
# Things programs wrote into HOME, which used to be /home/container.
for name in .cache .config .local .steam Steam steam .wine .pki .bash_history; do
    move "$root/$name" "$runtime/home/$name"
done

for name in game-source mods logs backups; do
    link=$runtime/$name
    if [[ -L "$link" ]]; then
        [[ "$(readlink "$link")" == "$root/$name" ]] && continue
        rm -f "$link"
    elif [[ -e "$link" ]]; then
        echo "Expected .runtime/$name to be a link to $name/; leaving it." >&2
        continue
    fi
    ln -s "$root/$name" "$link"
done

if ((${#moved[@]})); then
    echo "Tidied the server folder: moved ${moved[*]} into .runtime/ (files the server manages itself)."
fi
exit 0
