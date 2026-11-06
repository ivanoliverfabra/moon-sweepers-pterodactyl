#!/usr/bin/env bash
set -euo pipefail

ROOT=/mnt/server
mkdir -p \
  "$ROOT/game-source" \
  "$ROOT/backups" \
  "$ROOT/logs" \
  "$ROOT/control" \
  "$ROOT/probe"

echo "Moon Sweepers Pterodactyl files initialized."
echo "Upload the CONTENTS of your Moon Sweepers 1.0.5 installation to:"
echo "  /mnt/server/game-source/"
echo "Do not upload the parent folder as an extra nested directory."
echo "Required entries: MoonSweepers.exe, GameAssembly.dll, UnityPlayer.dll, MoonSweepers_Data/"
echo "You may upload a clean retail install or a game with the online-fix crack applied."
echo "The host detects online-fix files automatically; set SteamAppId and SteamGameId to 480 for SpaceWar."
echo "The first server start validates the exact supported game build and imports it."
