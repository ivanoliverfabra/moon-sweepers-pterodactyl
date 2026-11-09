#!/usr/bin/env bash
set -euo pipefail

# The folders the panel shows. Everything the server manages itself goes in .runtime/ (see layout.sh, which also
# moves the folders of servers set up before this layout when they next start).
ROOT=/mnt/server
mkdir -p \
  "$ROOT/game-source" \
  "$ROOT/mods" \
  "$ROOT/logs" \
  "$ROOT/backups" \
  "$ROOT/.runtime/home"

echo "Moon Sweepers Pterodactyl files initialized."
echo "Either set Game API URL and Server Key so the server downloads the game itself on its first start,"
echo "or upload the CONTENTS of your Moon Sweepers 1.0.5 installation to:"
echo "  /mnt/server/game-source/"
echo "Do not upload the parent folder as an extra nested directory."
echo "Required entries: MoonSweepers.exe, GameAssembly.dll, UnityPlayer.dll, MoonSweepers_Data/"
echo "You may upload a clean retail install or a game with the online-fix crack applied."
echo "The host detects online-fix files automatically; set SteamAppId and SteamGameId to 480 for SpaceWar."
echo "The first server start validates the exact supported game build and imports it."
