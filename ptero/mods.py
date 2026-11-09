"""The server's mod manifest, served by the ID API as GET /mods.

Operators describe their mods in /data/mods/mods.json (see PTERODACTYL.md):

    {
      "mods": [
        {
          "name": "Bigger Vacuum",
          "version": "1.2.0",
          "required": true,
          "files": ["BiggerVacuum.dll"],
          "nexus": {"mod_id": 12, "file_id": 34},
          "url": "https://example.com/BiggerVacuum-1.2.0.zip"
        }
      ]
    }

`files` are paths inside /data/mods/plugins. The server adds each file's size and SHA-256, so a launcher can tell
exactly which files a player needs. Plugin files that no entry lists are reported as server-only: they run on the
server but players don't need them.
"""
import hashlib
import json
import os
import re
from pathlib import Path, PurePosixPath

MODS_DIR = Path("/data/mods")
PLUGINS_DIR = MODS_DIR / "plugins"
MANIFEST_PATH = MODS_DIR / "mods.json"
# Nexus Mods game domain: https://www.nexusmods.com/games/moonsweepers
NEXUS_GAME = "moonsweepers"
MAX_MODS = 64
MAX_FILES = 256

_hashes = {}  # path -> (size, mtime_ns, sha256)


def enabled():
    return os.environ.get("MOON_BEPINEX", "0").strip() == "1"


def _sha256(path):
    stat = path.stat()
    cached = _hashes.get(path)
    if cached and cached[0] == stat.st_size and cached[1] == stat.st_mtime_ns:
        return cached[2]
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    _hashes[path] = (stat.st_size, stat.st_mtime_ns, digest.hexdigest())
    return digest.hexdigest()


def _text(value, limit):
    if not isinstance(value, str):
        return None
    value = "".join(c for c in value if c.isprintable()).strip()
    return value[:limit] or None


def _plugin_file(relative):
    """A plugins/ file named by a manifest entry, or None if the path is unsafe."""
    if not isinstance(relative, str) or not relative or len(relative) > 260 or "\\" in relative:
        return None
    path = PurePosixPath(relative)
    if path.is_absolute() or ".." in path.parts or not path.parts:
        return None
    full = PLUGINS_DIR.joinpath(*path.parts)
    root = PLUGINS_DIR.resolve()
    try:
        resolved = full.resolve()
    except OSError:
        return None
    if resolved != root and root not in resolved.parents:
        return None
    return full


def _file_record(relative, full):
    return {"path": relative, "size": full.stat().st_size, "sha256": _sha256(full)}


def _slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")[:48] or "mod"


def _entry(raw, index, claimed, errors):
    where = f"mods.json mod {index + 1}"
    if not isinstance(raw, dict):
        errors.append(f"{where} is not an object")
        return None
    name = _text(raw.get("name"), 64)
    if not name:
        errors.append(f"{where} has no name")
        return None
    where = f"mods.json mod '{name}'"

    nexus = None
    if raw.get("nexus") is not None:
        source = raw["nexus"] if isinstance(raw["nexus"], dict) else {}
        mod_id, file_id = source.get("mod_id"), source.get("file_id")
        if isinstance(mod_id, int) and not isinstance(mod_id, bool) and mod_id > 0:
            nexus = {
                "game": NEXUS_GAME,
                "mod_id": mod_id,
                "page": f"https://www.nexusmods.com/{NEXUS_GAME}/mods/{mod_id}",
            }
            if isinstance(file_id, int) and not isinstance(file_id, bool) and file_id > 0:
                nexus["file_id"] = file_id
        else:
            errors.append(f"{where}: nexus.mod_id must be a positive number")

    url = _text(raw.get("url"), 1024)
    if url and not url.lower().startswith(("https://", "http://")):
        errors.append(f"{where}: url must start with https://")
        url = None

    files, missing = [], []
    listed = raw.get("files")
    if not isinstance(listed, list) or not listed:
        errors.append(f"{where} lists no files")
        listed = []
    for relative in listed[:MAX_FILES]:
        full = _plugin_file(relative)
        if full is None:
            errors.append(f"{where}: {relative!r} is not a path inside mods/plugins")
            continue
        if relative in claimed:
            errors.append(f"{where}: {relative} is already listed by '{claimed[relative]}'")
            continue
        claimed[relative] = name
        if full.is_file():
            files.append(_file_record(relative, full))
        else:
            missing.append(relative)
    if missing:
        errors.append(f"{where}: missing from mods/plugins: {', '.join(missing)}")

    required = raw.get("required", True)
    return {
        "id": f"nexus-{nexus['mod_id']}" if nexus else _slug(name),
        "name": name,
        "version": _text(raw.get("version"), 32),
        "description": _text(raw.get("description"), 200),
        "required": required if isinstance(required, bool) else True,
        "nexus": nexus,
        "url": url,
        "files": files,
        "missing": missing,
    }


def manifest(game_port):
    """Everything a client needs to match this server's mods. Never raises for a bad mods.json."""
    errors, mods, claimed = [], [], {}
    raw = {"mods": []}
    if MANIFEST_PATH.is_file():
        try:
            raw = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            errors.append(f"mods.json could not be read: {error}")
            raw = {"mods": []}
    entries = raw.get("mods") if isinstance(raw, dict) else None
    if not isinstance(entries, list):
        errors.append('mods.json must be an object with a "mods" list')
        entries = []
    seen_ids = set()
    for index, item in enumerate(entries[:MAX_MODS]):
        entry = _entry(item, index, claimed, errors)
        if entry is None or not entry["files"]:
            continue  # nothing a player could match; the reason is in errors
        base, suffix = entry["id"], 2
        while entry["id"] in seen_ids:
            entry["id"] = f"{base}-{suffix}"
            suffix += 1
        seen_ids.add(entry["id"])
        mods.append(entry)

    server_only = []
    if PLUGINS_DIR.is_dir():
        for full in sorted(PLUGINS_DIR.rglob("*")):
            if len(server_only) >= MAX_FILES:
                break
            if not full.is_file():
                continue
            relative = full.relative_to(PLUGINS_DIR).as_posix()
            if relative not in claimed:
                server_only.append(_file_record(relative, full))

    # One value that changes whenever the set of files players must have changes.
    digest = hashlib.sha256()
    for line in sorted(f"{f['path']}\t{f['sha256']}" for m in mods if m["required"] for f in m["files"]):
        digest.update(line.encode() + b"\n")

    return {
        "schema": 1,
        "game_port": game_port,
        "enabled": enabled(),
        "game": NEXUS_GAME,
        "hash": digest.hexdigest(),
        "mods": mods,
        "server_only": server_only,
        "errors": errors,
    }


def summary(game_port):
    """The short form included in GET /."""
    full = manifest(game_port)
    return {
        "enabled": full["enabled"],
        "count": len(full["mods"]),
        "required": sum(1 for mod in full["mods"] if mod["required"]),
        "hash": full["hash"],
    }


def describe(game_port):
    """Console output for the `mods` command."""
    full = manifest(game_port)
    lines = [f"BepInEx mods are {'enabled' if full['enabled'] else 'disabled (MOON_BEPINEX=0)'}."]
    if not full["mods"]:
        lines.append("No mods are listed in /home/container/mods/mods.json.")
    for mod in full["mods"]:
        version = f" {mod['version']}" if mod["version"] else ""
        need = "required" if mod["required"] else "optional"
        lines.append(f"  {mod['name']}{version} ({need}, {len(mod['files'])} file(s))")
    if full["server_only"]:
        lines.append(f"  {len(full['server_only'])} server-only file(s) in mods/plugins that players don't need")
    lines.extend(f"Warning: {error}" for error in full["errors"])
    return "\n".join(lines)
