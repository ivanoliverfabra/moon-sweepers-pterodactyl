"""`mod` console commands: a small package manager for the server's BepInEx mods.

It installs a mod from a download link (a .zip or a .dll), sorts its files into mods/plugins and mods/patchers, and
keeps mods/mods.json in step, so players' launchers get exactly the same files from the same link. See `mod help`.
"""
import json
import os
import re
import shutil
import tempfile
import threading
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

import mods

MAX_DOWNLOAD = 256 * 1024 * 1024
MAX_UNPACKED = 1024 * 1024 * 1024
MAX_ENTRIES = 5000
# Package metadata that isn't part of the mod (Thunderstore and most GitHub releases ship these at the top).
METADATA = {"manifest.json", "icon.png", "readme.md", "changelog.md", "license", "license.md", "license.txt"}
# Folders BepInEx owns; a mod archive that carries them is not installed over ours.
SKIPPED_FOLDERS = {"core", "interop", "unity-libs", "cache"}

_lock = threading.Lock()

HELP = """Mod commands (restart the server after changes so BepInEx loads them):
  mod list                          what is installed, and what players need
  mod install <url> [name]          download a .zip or .dll and install it for the server and players
  mod update <name>                 install the newest file from the mod's link again
  mod remove <name>                 delete the mod's files and its mods.json entry
  mod adopt                         add mods.json entries for files already in mods/ that no entry lists
  mod require <name> | mod optional <name>
  mod link <name> <page url>        where players get the mod (shown in the launcher)
  mod url <name> <download url>     direct download the launcher uses
  mod nexus <name> <mod id> [file id]
  mod version <name> <version>
Names match case-insensitively; a unique part of a name is enough."""


def say(text):
    print(text, flush=True)


# --- mods.json ------------------------------------------------------------------------------------------------

def _load():
    if not mods.MANIFEST_PATH.is_file():
        return {"mods": []}
    data = json.loads(mods.MANIFEST_PATH.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or not isinstance(data.get("mods"), list):
        raise ValueError('mods.json must be an object with a "mods" list')
    return data


def _save(data):
    mods.MODS_DIR.mkdir(parents=True, exist_ok=True)
    temporary = mods.MANIFEST_PATH.with_name(".mods.json.tmp")
    temporary.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, mods.MANIFEST_PATH)


def _find(data, query):
    """The entry whose name matches `query`: exactly, or as the only one containing it."""
    entries = [e for e in data["mods"] if isinstance(e, dict) and isinstance(e.get("name"), str)]
    lowered = query.strip().lower()
    exact = [e for e in entries if e["name"].lower() == lowered]
    if exact:
        return exact[0]
    partial = [e for e in entries if lowered in e["name"].lower()]
    if len(partial) == 1:
        return partial[0]
    if partial:
        raise ValueError(f"'{query}' matches {', '.join(e['name'] for e in partial)}; be more specific")
    raise ValueError(f"No mod called '{query}'. Type 'mod list' to see them.")


def _claimed(data, skip=None):
    taken = {}
    for entry in data["mods"]:
        if entry is skip or not isinstance(entry, dict):
            continue
        for key, label, _folder in mods._folders():
            for path in entry.get(key) or []:
                if isinstance(path, str):
                    taken[f"{label}/{path}"] = entry.get("name", "?")
    return taken


# --- downloads and archives -----------------------------------------------------------------------------------

def _download(url, target):
    if not url.lower().startswith("https://"):
        raise ValueError("Only https:// links can be installed.")
    request = urllib.request.Request(url, headers={"User-Agent": "moon-sweepers-server"})
    with urllib.request.urlopen(request, timeout=60) as response, target.open("wb") as handle:
        total = 0
        while True:
            block = response.read(1 << 20)
            if not block:
                break
            total += len(block)
            if total > MAX_DOWNLOAD:
                raise ValueError("The download is larger than 256 MB, which is too big for a mod.")
            handle.write(block)
        name = Path(urllib.request.url2pathname(response.geturl().split("?", 1)[0])).name
    return name


def _safe_parts(name):
    path = PurePosixPath(name.replace("\\", "/"))
    if path.is_absolute() or any(part in ("", ".", "..") or ":" in part for part in path.parts):
        return None
    return path.parts


def _layout(archive, fallback_name):
    """Where each archive member goes: (member, "plugins"|"patchers", relative path). Also returns name and version
    from a Thunderstore manifest.json when there is one."""
    members = [m for m in archive.infolist() if not m.is_dir()]
    if len(members) > MAX_ENTRIES:
        raise ValueError("The archive has too many files to be a mod.")
    if sum(m.file_size for m in members) > MAX_UNPACKED:
        raise ValueError("The archive unpacks to more than 1 GB, which is too big for a mod.")

    name, version = fallback_name, None
    for member in members:
        if member.filename.lower() in ("manifest.json",):
            try:
                info = json.loads(archive.read(member).decode("utf-8-sig"))
                name = str(info.get("name") or name).replace("_", " ")[:64]
                version = str(info.get("version_number") or "")[:32] or None
            except (ValueError, UnicodeDecodeError, AttributeError):
                pass

    parts_of = {m: _safe_parts(m.filename) for m in members}
    if any(parts is None for parts in parts_of.values()):
        raise ValueError("The archive contains unsafe paths, so it was not installed.")

    placed, loose = [], []
    for member, parts in parts_of.items():
        lowered = [p.lower() for p in parts]
        if "bepinex" in lowered:  # ...BepInEx/plugins/... style
            parts = parts[lowered.index("bepinex") + 1:]
            lowered = lowered[lowered.index("bepinex") + 1:]
        if lowered and lowered[0] in ("plugins", "patchers") and len(parts) > 1:
            placed.append((member, lowered[0], "/".join(parts[1:])))
        elif lowered and (lowered[0] in SKIPPED_FOLDERS or lowered[0] == "config"):
            continue
        elif len(parts) == 1 and lowered[0] in METADATA:
            continue
        else:
            loose.append((member, parts))

    if loose:
        # Everything else is the plugin itself; keep it together in a folder named after the mod.
        roots = {parts[0] for _member, parts in loose if len(parts) > 1}
        strip = 1 if len(roots) == 1 and all(len(parts) > 1 for _m, parts in loose) else 0
        folder = re.sub(r"[^A-Za-z0-9._-]+", "", name.replace(" ", "")) or "Mod"
        for member, parts in loose:
            placed.append((member, "plugins", "/".join((folder,) + tuple(parts[strip:]))))
    if not placed:
        raise ValueError("The archive doesn't contain any mod files.")
    return placed, name, version


def _guess_name(url, filename):
    stem = re.sub(r"\.(zip|dll)$", "", filename or url.rstrip("/").split("/")[-1], flags=re.I)
    stem = re.sub(r"[-_ ]?v?\d+(\.\d+)+.*$", "", stem)  # drop a trailing version
    return (stem.replace("_", " ").replace("-", " ").strip() or "Mod")[:64]


# --- commands -------------------------------------------------------------------------------------------------

def install(url, name=None):
    """Installs (or, for a mod already listed under this name, replaces) a mod from a download link."""
    mods.MODS_DIR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=str(mods.MODS_DIR), prefix=".install-") as work:
        work = Path(work)
        download = work / "download"
        say(f"Downloading {url} ...")
        filename = _download(url, download)
        guessed = name or _guess_name(url, filename)
        version = None

        if zipfile.is_zipfile(download):
            with zipfile.ZipFile(download) as archive:
                placed, found_name, version = _layout(archive, guessed)
                if not name:
                    guessed = found_name
                staged = []
                for member, folder, relative in placed:
                    target = work / "staged" / folder / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with archive.open(member) as source, target.open("wb") as handle:
                        shutil.copyfileobj(source, handle)
                    staged.append((folder, relative, target))
        elif filename.lower().endswith(".dll") or download.read_bytes()[:2] == b"MZ":
            dll = filename if filename.lower().endswith(".dll") else f"{guessed.replace(' ', '')}.dll"
            staged = [("plugins", dll, download)]
        else:
            raise ValueError("The link must lead to a .zip or a .dll (.7z and .rar can't be installed here).")

        with _lock:
            data = _load()
            existing = next(
                (e for e in data["mods"] if isinstance(e, dict) and str(e.get("name", "")).lower() == guessed.lower()),
                None,
            )
            taken = _claimed(data, skip=existing)
            for folder, relative, _source in staged:
                if f"{folder}/{relative}" in taken:
                    raise ValueError(f"{folder}/{relative} belongs to '{taken[f'{folder}/{relative}']}'. Remove that mod first.")

            if existing is not None:
                _delete_files(existing)
            for folder, relative, source in staged:
                target = (mods.PLUGINS_DIR if folder == "plugins" else mods.PATCHERS_DIR).joinpath(*relative.split("/"))
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)

            entry = existing if existing is not None else {"name": guessed, "required": True}
            if existing is None:
                data["mods"].append(entry)
            entry["files"] = sorted(r for f, r, _s in staged if f == "plugins")
            entry["patchers"] = sorted(r for f, r, _s in staged if f == "patchers")
            if not entry["patchers"]:
                entry.pop("patchers")
            if not entry["files"]:
                entry.pop("files")
            entry["url"] = url
            if version:
                entry["version"] = version
            _save(data)

    count = len(staged)
    say(f"Installed {entry['name']} ({count} file(s)). Restart the server to load it; players get it from the launcher.")


def _delete_files(entry):
    for key, _label, folder in mods._folders():
        for relative in entry.get(key) or []:
            target = mods._mod_file(folder, relative)
            if target is not None and target.is_file():
                target.unlink()
                parent = target.parent
                while parent != folder and parent.is_dir() and not any(parent.iterdir()):
                    parent.rmdir()
                    parent = parent.parent


def remove(query):
    with _lock:
        data = _load()
        entry = _find(data, query)
        _delete_files(entry)
        data["mods"].remove(entry)
        _save(data)
    say(f"Removed {entry['name']}. Restart the server to unload it.")


def adopt():
    """Entries for unlisted files: each top-level folder is a mod, and loose files are grouped by name."""
    with _lock:
        data = _load()
        taken = _claimed(data)
        groups = {}
        for _key, label, folder in mods._folders():
            if not folder.is_dir():
                continue
            for full in sorted(folder.rglob("*")):
                if not full.is_file():
                    continue
                relative = full.relative_to(folder).as_posix()
                if f"{label}/{relative}" in taken:
                    continue
                first = relative.split("/", 1)[0]
                group = first if "/" in relative else re.sub(r"\.[^.]+$", "", first)
                groups.setdefault(group, {"files": [], "patchers": []})[
                    "files" if label == "plugins" else "patchers"].append(relative)
        names = {e.get("name", "").lower() for e in data["mods"] if isinstance(e, dict)}
        added = []
        for group, files in sorted(groups.items()):
            name = group[:64]
            if name.lower() in names:
                name = f"{name} ({len(added) + 2})"
            entry = {"name": name, "required": True}
            entry.update({k: v for k, v in files.items() if v})
            data["mods"].append(entry)
            added.append(name)
        if added:
            _save(data)
    say(f"Added {', '.join(added)} to mods.json, all required." if added else "Every file in mods/ is already listed.")


def _set(query, key, value):
    with _lock:
        data = _load()
        entry = _find(data, query)
        if value is None:
            entry.pop(key, None)
        else:
            entry[key] = value
        _save(data)
    return entry["name"]


def _https(value, what):
    if not re.match(r"^https://\S+$", value or ""):
        raise ValueError(f"The {what} must be an https:// link.")
    return value[:1024]


def run(line):
    """Handles one `mod ...` command line. Never raises; problems are printed."""
    words = line.split()
    sub = words[0].lower() if words else "help"
    args = words[1:]
    try:
        if sub in ("help", "?"):
            say(HELP)
        elif sub == "list":
            say(mods.describe(_port()))
        elif sub == "install" and args:
            url = _https(args[0], "download link")
            if mods.NEXUS_PAGE.match(url):
                raise ValueError(
                    "Nexus Mods only gives files to a signed-in browser. Download the zip there, unzip it into "
                    "mods/plugins (and mods/patchers), then run: mod adopt, and mod link <name> " + url)
            install(url, " ".join(args[1:]) or None)
        elif sub == "update" and args:
            with _lock:
                entry = _find(_load(), " ".join(args))
            if not entry.get("url"):
                raise ValueError(f"{entry['name']} has no download link. Set one with: mod url {entry['name']} <link>")
            install(entry["url"], entry["name"])
        elif sub == "remove" and args:
            remove(" ".join(args))
        elif sub == "adopt":
            adopt()
        elif sub in ("require", "optional") and args:
            name = _set(" ".join(args), "required", sub == "require")
            say(f"{name} is now {'required' if sub == 'require' else 'optional'} for players.")
        elif sub == "link" and len(args) >= 2:
            name = _set(" ".join(args[:-1]), "link", _https(args[-1], "page link"))
            say(f"Players are sent to {args[-1]} to get {name}.")
        elif sub == "url" and len(args) >= 2:
            target = _https(args[-1], "download link")
            if mods.NEXUS_PAGE.match(target):
                name = _set(" ".join(args[:-1]), "link", target)
                say(f"That's a Nexus Mods page, so players are sent there to download {name} (saved as its link).")
            else:
                name = _set(" ".join(args[:-1]), "url", target)
                say(f"The launcher downloads {name} from {target}.")
        elif sub == "nexus" and len(args) >= 2:
            numbers = [a for a in args if a.isdigit()]
            words_only = [a for a in args if not a.isdigit()]
            if not words_only or not numbers:
                raise ValueError("Usage: mod nexus <name> <mod id> [file id]")
            value = {"mod_id": int(numbers[0])}
            if len(numbers) > 1:
                value["file_id"] = int(numbers[1])
            name = _set(" ".join(words_only), "nexus", value)
            say(f"{name} now points at nexusmods.com/{mods.NEXUS_GAME}/mods/{numbers[0]}.")
        elif sub == "version" and len(args) >= 2:
            name = _set(" ".join(args[:-1]), "version", args[-1][:32])
            say(f"{name} is version {args[-1][:32]}.")
        else:
            say(HELP)
    except Exception as error:  # a console command must never take the supervisor down
        say(f"mod {sub}: {error}")


def _port():
    try:
        return int(os.environ.get("MOON_GAME_PORT", "27025"))
    except ValueError:
        return 27025


def run_in_background(line):
    """Downloads can take a while; keep the console responsive."""
    threading.Thread(target=run, args=(line,), name="mod-command", daemon=True).start()
