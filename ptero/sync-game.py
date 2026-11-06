#!/usr/bin/env python3
"""Keep the imported game compatible with the uploaded game variant.

The upstream importer copies the retail file set only. An online-fix patched
install also ships a replacement Steam API DLL and a few root-level support
files. This script mirrors those variant-specific files into the imported game
after the import and before the game starts. It is safe to run on every start
and when switching between a retail and an online-fix upload.
"""
from pathlib import Path
import os
import shutil
import sys

SOURCE = Path("/import/game")
TARGET = Path("/data/game")
PLUGIN_STEAM_API = Path("MoonSweepers_Data/Plugins/x86_64/steam_api64.dll")

# Root-level files installed by the online-fix release. The retail game does
# not contain them, so their presence identifies an online-fix upload.
CRACK_FILES = {
    "dlllist.txt",
    "onlinefix.ini",
    "onlinefix.url",
    "onlinefix64.dll",
    "steam_api64.dll",
    "steamoverlay64.dll",
    "winmm.dll",
}


def read_onlinefix_appids() -> tuple[str, str]:
    """Return RealAppId/FakeAppId from the online-fix config, if present."""
    candidates = [entry for entry in SOURCE.iterdir() if entry.name.lower() == "onlinefix.ini"]
    if not candidates:
        return "", ""
    real = fake = ""
    try:
        for line in candidates[0].read_text(encoding="utf-8", errors="replace").splitlines():
            key, _, value = line.partition("=")
            key = key.strip().lower()
            if key == "realappid":
                real = value.strip()
            elif key == "fakeappid":
                fake = value.strip()
    except OSError:
        return "", ""
    return real, fake


def main() -> int:
    if not (SOURCE / "MoonSweepers.exe").is_file():
        print("Game source is missing MoonSweepers.exe; nothing to synchronize.", flush=True)
        return 1
    if not (TARGET / "MoonSweepers.exe").is_file():
        print("Imported game is missing MoonSweepers.exe; run the importer first.", flush=True)
        return 1

    plugin_source = SOURCE / PLUGIN_STEAM_API
    if plugin_source.is_file() and not plugin_source.is_symlink():
        plugin_target = TARGET / PLUGIN_STEAM_API
        plugin_target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(plugin_source, plugin_target)
        print(f"Synchronized {PLUGIN_STEAM_API.as_posix()} from the uploaded game.", flush=True)

    # Wine runs on a case-sensitive filesystem. Match the online-fix files by
    # name while preserving the casing used by the upload.
    wanted = {}
    for entry in SOURCE.iterdir():
        if entry.is_file() and not entry.is_symlink() and entry.name.lower() in CRACK_FILES:
            wanted[entry.name.lower()] = entry.name

    for entry in TARGET.iterdir():
        if not entry.is_file() or entry.is_symlink():
            continue
        lower = entry.name.lower()
        if lower in CRACK_FILES and wanted.get(lower) != entry.name:
            entry.unlink()
            print(f"Removed stale online-fix file: {entry.name}", flush=True)

    for lower, name in sorted(wanted.items()):
        shutil.copy2(SOURCE / name, TARGET / name)
        print(f"Copied online-fix file: {name}", flush=True)

    if wanted:
        real, fake = read_onlinefix_appids()
        configured = os.environ.get("SteamAppId") or os.environ.get("SteamGameId") or ""
        if real and fake and configured not in (real, fake):
            print(
                f"Warning: SteamAppId/SteamGameId is {configured or 'unset'}, but OnlineFix.ini "
                f"expects {real} (retail) or {fake} (SpaceWar).",
                flush=True,
            )
        elif real and fake and configured == real:
            print(
                f"Warning: online-fix files are loaded with the retail AppId {real}. "
                f"Set SteamAppId and SteamGameId to {fake} to use SpaceWar.",
                flush=True,
            )
        print("Online-fix game files detected.", flush=True)
    else:
        configured = os.environ.get("SteamAppId") or os.environ.get("SteamGameId") or ""
        if configured and configured != "5335510":
            print(
                f"Warning: normal retail game files are loaded with AppId {configured}. "
                "Set SteamAppId and SteamGameId to 5335510 for retail clients.",
                flush=True,
            )
        print("Normal retail game files detected.", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
