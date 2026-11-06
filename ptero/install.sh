#!/usr/bin/env bash
set -euo pipefail

ROOT=/home/container
mkdir -p \
  "$ROOT/game-source" \
  "$ROOT/game" \
  "$ROOT/backups" \
  "$ROOT/logs" \
  "$ROOT/control" \
  "$ROOT/probe"

echo "Moon Sweepers Pterodactyl files initialized."
echo "Upload the CONTENTS of your owned Moon Sweepers 1.0.5 installation to:"
echo "  /home/container/game-source/"
echo "Do not upload the parent folder as an extra nested directory."
echo "Required entries: MoonSweepers.exe, GameAssembly.dll, UnityPlayer.dll, MoonSweepers_Data/"
echo "The first server start validates the exact supported game build and imports it."
