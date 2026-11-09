"""The server's mod manifest, served by the ID API as GET /mods.

Operators describe their mods in /data/mods/mods.json (see PTERODACTYL.md):

    {
      "mods": [
        {
          "name": "Bigger Vacuum",
          "version": "1.2.0",
          "required": true,
          "files": ["BiggerVacuum.dll"],
          "patchers": ["BiggerVacuum.Patcher.dll"],
          "nexus": {"mod_id": 12, "file_id": 34},
          "url": "https://example.com/BiggerVacuum-1.2.0.zip",
          "link": "https://github.com/example/BiggerVacuum/releases"
        }
      ]
    }

`files` are paths inside /data/mods/plugins, and the optional `patchers` are BepInEx preloader patchers inside
/data/mods/patchers. The server adds each file's size and SHA-256, so a launcher can tell exactly which files a player
needs. Files in either folder that no entry lists are reported as server-only: they run on the server but players
don't need them.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
from pathlib import Path, PurePosixPath

import versions

MODS_DIR = Path("/data/mods")
PLUGINS_DIR = MODS_DIR / "plugins"
PATCHERS_DIR = MODS_DIR / "patchers"
MANIFEST_PATH = MODS_DIR / "mods.json"
# Nexus Mods game domain: https://www.nexusmods.com/games/moonsweepers
NEXUS_GAME = "moonsweepers"
NEXUS_PAGE = re.compile(r"^https?://(www\.)?nexusmods\.com/", re.I)
NEXUS_MOD_PAGE = re.compile(rf"^https?://(?:www\.)?nexusmods\.com/{NEXUS_GAME}/mods/(\d+)", re.I)
MAX_MODS = 64
MAX_FILES = 256

_hashes = {}  # path -> (size, mtime_ns, sha256)
# Reads [BepInPlugin] and assembly details from mod DLLs (modding/modinfo); built into the image.
MODINFO = Path(os.environ.get("MOON_MODINFO", "/opt/moon/bepinex/modinfo/modinfo.dll"))
_modinfo = {}  # sha256 -> what modinfo said about that file ({} when it isn't a .NET assembly)
_icons = {}  # mod id -> icon file, for GET /mods/<id>/icon
ICON_TYPES = {".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".webp": "image/webp"}


def enabled():
    return os.environ.get("MOON_BEPINEX", "0").strip() == "1"


def _folders():
    """(mods.json key, folder name, path). Looked up on each call so tests can point the paths elsewhere."""
    return (("files", "plugins", PLUGINS_DIR), ("patchers", "patchers", PATCHERS_DIR))


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


def _mod_file(folder, relative):
    """A file inside `folder` named by a manifest entry, or None if the path is unsafe."""
    if not isinstance(relative, str) or not relative or len(relative) > 260 or "\\" in relative:
        return None
    path = PurePosixPath(relative)
    if path.is_absolute() or ".." in path.parts or not path.parts:
        return None
    full = folder.joinpath(*path.parts)
    root = folder.resolve()
    try:
        resolved = full.resolve()
    except OSError:
        return None
    if resolved != root and root not in resolved.parents:
        return None
    return full


def _file_record(relative, full, folder=None):
    record = {"path": relative, "size": full.stat().st_size, "sha256": _sha256(full)}
    if folder:
        record["folder"] = folder
    return record


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
    # The page players are sent to for the download (a release page, Thunderstore, ...).
    link = _text(raw.get("link"), 1024)
    if link and not link.lower().startswith("https://"):
        errors.append(f"{where}: link must start with https://")
        link = None
    # A Nexus Mods page is not a download (Nexus only hands files to a signed-in browser): send players there.
    if url and NEXUS_PAGE.match(url):
        link = link or url
        page = NEXUS_MOD_PAGE.match(url)
        if nexus is None and page:
            mod_id = int(page.group(1))
            nexus = {"game": NEXUS_GAME, "mod_id": mod_id,
                     "page": f"https://www.nexusmods.com/{NEXUS_GAME}/mods/{mod_id}"}
        url = None

    found, missing = {"files": [], "patchers": []}, []
    # A modpack's client-only mod: not installed here, so its files are the registry's list (paths and hashes).
    client_files = raw.get("client_files") if raw.get("side") == "client" else None
    for record in client_files if isinstance(client_files, list) else []:
        if not isinstance(record, dict) or record.get("folder") not in ("plugins", "patchers"):
            continue
        relative, sha, size = record.get("path"), record.get("sha256"), record.get("size")
        if (isinstance(relative, str) and _mod_file(PLUGINS_DIR, relative) is not None and isinstance(sha, str)
                and re.fullmatch(r"[0-9a-f]{64}", sha) and isinstance(size, int) and size >= 0):
            found["files" if record["folder"] == "plugins" else "patchers"].append(
                {"path": relative, "size": size, "sha256": sha})
    for key, label, folder in _folders():
        if client_files is not None:
            break
        listed = raw.get(key)
        if listed is None:
            continue
        if not isinstance(listed, list):
            errors.append(f"{where}: {key} must be a list of paths")
            continue
        for relative in listed[:MAX_FILES]:
            full = _mod_file(folder, relative)
            if full is None:
                errors.append(f"{where}: {relative!r} is not a path inside mods/{label}")
                continue
            claim = f"{label}/{relative}"
            if claim in claimed:
                errors.append(f"{where}: {claim} is already listed by '{claimed[claim]}'")
                continue
            claimed[claim] = name
            if full.is_file():
                found[key].append(_file_record(relative, full))
            else:
                missing.append(claim)
    if not found["files"] and not found["patchers"] and not missing:
        errors.append(f"{where} lists no files")
    if missing:
        errors.append(f"{where}: missing from mods/: {', '.join(missing)}")

    icon = None
    if raw.get("icon") is not None:
        icon = _mod_file(MODS_DIR, raw.get("icon"))
        if icon is None or not icon.is_file() or icon.suffix.lower() not in ICON_TYPES:
            errors.append(f"{where}: icon must be a .png, .jpg or .webp inside mods/")
            icon = None

    registry = raw.get("registry") if isinstance(raw.get("registry"), dict) else None
    if registry is not None:
        download = _text(registry.get("download"), 1024)
        registry = {
            "package": _text(registry.get("package"), 130),
            "version": _text(registry.get("version"), 32),
            "sha256": registry.get("sha256") if re.fullmatch(r"[0-9a-f]{64}", str(registry.get("sha256"))) else None,
            # Where the launcher downloads the package zip, with the player's own sign-in.
            "download": download if download and download.lower().startswith(("https://", "http://localhost")) else None,
        }
        if not all(registry.values()):
            errors.append(f"{where}: its registry details are incomplete; run mod pack")
            registry = None
        else:
            link = link or _text(raw.get("registry", {}).get("page"), 1024)

    required = raw.get("required", True)
    return {
        "id": f"nexus-{nexus['mod_id']}" if nexus else _slug(name),
        "name": name,
        "version": _text(raw.get("version"), 32),
        "description": _text(raw.get("description"), 400),
        "author": _text(raw.get("author"), 64),
        "required": required if isinstance(required, bool) else True,
        "nexus": nexus,
        "registry": registry,
        "url": url,
        "link": link,
        "files": found["files"],
        # Separate from `files` so launchers that predate patchers skip them instead of putting them in plugins.
        "patchers": found["patchers"],
        "missing": missing,
        "_icon": icon,
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
        if isinstance(item, dict) and item.get("side") == "server":
            continue  # a modpack's server-only mod: its files are reported as server-only below
        entry = _entry(item, index, claimed, errors)
        if entry is None or not (entry["files"] or entry["patchers"]):
            continue  # nothing a player could match; the reason is in errors
        base, suffix = entry["id"], 2
        while entry["id"] in seen_ids:
            entry["id"] = f"{base}-{suffix}"
            suffix += 1
        seen_ids.add(entry["id"])
        mods.append(entry)

    _describe_plugins(mods, errors)

    _icons.clear()
    for mod in mods:
        icon = mod.pop("_icon") or _folder_icon(mod)
        mod["icon"] = f"/mods/{mod['id']}/icon" if icon else None
        if icon:
            _icons[mod["id"]] = icon

    server_only = []
    for _key, label, folder in _folders():
        if not folder.is_dir():
            continue
        for full in sorted(folder.rglob("*")):
            if len(server_only) >= MAX_FILES:
                break
            if not full.is_file():
                continue
            relative = full.relative_to(folder).as_posix()
            if f"{label}/{relative}" not in claimed:
                server_only.append(_file_record(relative, full, label))

    # One value that changes whenever the set of files players must have changes.
    lines = sorted(
        f"{label}/{record['path']}:{record['sha256']}"
        for mod in mods
        if mod["required"]
        for key, label, _folder in _folders()
        for record in mod[key]
    )
    digest = hashlib.sha256("\n".join(lines).encode())

    return {
        "schema": 1,
        "game_port": game_port,
        "enabled": enabled(),
        "game": NEXUS_GAME,
        "hash": digest.hexdigest(),
        # What players must run to join, with or without mods: {game: {name, build}, client}. See versions.py.
        "requires": versions.requires(raw.get("pack") if isinstance(raw, dict) else None),
        # The modpack this server installs (MOON_MODPACK), when it uses one: {token, name, revision, game, client},
        # where game ({name, size, sha256}) and client (a release tag) are what players must run, or null for any.
        "modpack": raw.get("pack") if isinstance(raw, dict) and isinstance(raw.get("pack"), dict) else None,
        "mods": mods,
        "server_only": server_only,
        "errors": errors,
    }


def _read_modinfo(files):
    """{sha256: details} for these DLLs ({sha256: Path}), asking modinfo only about files it hasn't seen."""
    unseen = {sha: path for sha, path in files.items() if sha not in _modinfo}
    dotnet = shutil.which("dotnet")
    if unseen and dotnet and MODINFO.is_file():
        try:
            # Without a heap cap the .NET runtime reserves more than containers allow and gets killed.
            env = {**os.environ, "DOTNET_GCHeapHardLimit": "0x20000000", "DOTNET_CLI_TELEMETRY_OPTOUT": "1"}
            result = subprocess.run([dotnet, str(MODINFO), *map(str, unseen.values())],
                                    capture_output=True, text=True, timeout=60, check=False, env=env)
            for sha, details in zip(unseen, json.loads(result.stdout or "[]")):
                _modinfo[sha] = details if isinstance(details, dict) and "error" not in details else {}
        except (OSError, ValueError, subprocess.SubprocessError):
            pass
    return {sha: _modinfo.get(sha, {}) for sha in files}


def _dlls(mod):
    for key, _label, folder in _folders():
        for record in mod[key]:
            path = folder.joinpath(*record["path"].split("/"))
            # A client-only modpack mod has no files here to read.
            if record["path"].lower().endswith(".dll") and path.is_file():
                yield record["sha256"], path


def _describe_plugins(mods, errors):
    """Fills in what the mods' own DLLs say: plugins (guid, name, version, dependencies), and the version, description
    and author when mods.json leaves them out. Then works out which mods need which."""
    info = _read_modinfo({sha: path for mod in mods for sha, path in _dlls(mod)})
    provided = {}
    for mod in mods:
        plugins, assemblies = [], []
        for sha, _path in _dlls(mod):
            details = info.get(sha) or {}
            if details:
                assemblies.append(details)
            for plugin in details.get("plugins") or []:
                guid = _text(plugin.get("guid"), 128)
                if not guid:
                    continue
                plugins.append({
                    "kind": "patcher" if plugin.get("kind") == "patcher" else "plugin",
                    "guid": guid,
                    "name": _text(plugin.get("name"), 64),
                    "version": _text(plugin.get("version"), 32),
                    "dependencies": [
                        {"guid": _text(d.get("guid"), 128), "soft": bool(d.get("soft"))}
                        for d in plugin.get("dependencies") or [] if isinstance(d, dict) and d.get("guid")
                    ][:32],
                })
                provided.setdefault(guid, mod["id"])
        mod["plugins"] = plugins[:32]
        if not mod["version"] and plugins:
            mod["version"] = plugins[0]["version"]
        if not mod["description"]:
            mod["description"] = next((_text(a.get("description"), 400) for a in assemblies if a.get("description")), None)
        if not mod["author"]:
            for details in assemblies:
                company = _text(details.get("company"), 64)
                # SDK projects default the company to the assembly name; that says nothing about who made it.
                if company and company not in (details.get("assembly"), details.get("title")):
                    mod["author"] = company
                    break

    for mod in mods:
        requires, unmet = set(), set()
        for plugin in mod["plugins"]:
            for dependency in plugin["dependencies"]:
                owner = provided.get(dependency["guid"])
                if owner and owner != mod["id"]:
                    requires.add(owner)
                elif not owner and not dependency["soft"]:
                    unmet.add(dependency["guid"])
        mod["requires"] = sorted(requires)
        mod["missing_dependencies"] = sorted(unmet)
        for guid in sorted(unmet):
            errors.append(f"mods.json mod '{mod['name']}' needs plugin {guid}, which no listed mod provides")
        if mod["required"]:
            for owner in sorted(requires):
                other = next(m for m in mods if m["id"] == owner)
                if not other["required"]:
                    errors.append(f"mods.json mod '{mod['name']}' needs '{other['name']}', which is only optional")


def _folder_icon(mod):
    """An icon.png (or .jpg/.webp) next to the mod's files, e.g. plugins/DustSilo/icon.png."""
    for key, _label, folder in _folders():
        for record in mod[key]:
            parent = folder.joinpath(*record["path"].split("/")).parent
            if parent == folder:
                continue
            for suffix in ICON_TYPES:
                candidate = parent / f"icon{suffix}"
                if candidate.is_file():
                    return candidate
    return None


def icon(mod_id):
    """(bytes, content type) of a mod's icon, or None."""
    path = _icons.get(mod_id)
    if path is None or not path.is_file() or path.stat().st_size > 2 * 1024 * 1024:
        return None
    return path.read_bytes(), ICON_TYPES[path.suffix.lower()]


def plugin_names(paths):
    """{path: plugin name} for DLLs that declare one, used to name mods `mod adopt` creates."""
    files = {_sha256(path): path for path in paths if path.suffix.lower() == ".dll" and path.is_file()}
    names = {}
    for sha, details in _read_modinfo(files).items():
        for plugin in details.get("plugins") or []:
            if plugin.get("name"):
                names[files[sha]] = {"name": plugin["name"], "version": plugin.get("version")}
                break
    return names


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
    needed = full["requires"]
    game = needed["game"] or {}
    lines.append(f"Players need game {game.get('name') or 'build ' + str(game.get('build') or 'unknown')} "
                 f"and client {needed['client'] or 'any (set Client Version)'}.")
    if full["modpack"]:
        pack = full["modpack"]
        lines.append(f"Modpack: {pack.get('name')} ({pack.get('token')}) revision {pack.get('revision')}.")
    if not full["mods"]:
        lines.append("No mods are listed in /home/container/mods/mods.json.")
    for mod in full["mods"]:
        version = f" {mod['version']}" if mod["version"] else ""
        version += f" by {mod['author']}" if mod.get("author") else ""
        need = "required" if mod["required"] else "optional"
        patchers = f", {len(mod['patchers'])} patcher(s)" if mod["patchers"] else ""
        lines.append(f"  {mod['name']}{version} ({need}, {len(mod['files'])} plugin file(s){patchers})")
    if full["server_only"]:
        lines.append(f"  {len(full['server_only'])} server-only file(s) that players don't need:")
        lines.extend(f"    {f['folder']}/{f['path']}" for f in full["server_only"][:20])
    lines.extend(f"Warning: {error}" for error in full["errors"])
    return "\n".join(lines)
