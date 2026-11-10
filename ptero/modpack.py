"""Installs the modpack named by the egg's Modpack Token (MOON_MODPACK) from the Moon Sweepers API's registry.

On every start (and on `mod pack`), the server asks the API for the pack, downloads each package it doesn't already
have at that version, checks it against the registry's SHA-256, and installs it like `mod install` does. Entries it
installs are marked with where they came from ("registry"), so the /mods manifest tells players' launchers to get
them from the registry, and mods the pack no longer lists are removed. Mods you installed yourself are left alone.

    MOON_MODPACK=K7QX3MP      the newest revision of that pack
    MOON_MODPACK=K7QX3MP@3    exactly revision 3

Uses Game API URL (MOON_GAME_API_URL) and Server Key (MOON_SERVER_KEY), like the game download.
"""
import hashlib
import json
import os
import re
import tempfile
import urllib.error
import urllib.request
from pathlib import Path

import game_download
import modpm
import mods

TOKEN = re.compile(r"^[A-Za-z0-9]{6,8}(@\d{1,6})?$")
MAX_PACKAGE = 64 * 1024 * 1024


def say(text):
    print(f"[modpack] {text}", flush=True)


def _token():
    return os.environ.get("MOON_MODPACK", "").strip()


def _api():
    base = os.environ.get("MOON_GAME_API_URL", "").strip().rstrip("/")
    key = os.environ.get("MOON_SERVER_KEY", "").strip()
    if not base or not key:
        raise RuntimeError("set Game API URL (MOON_GAME_API_URL) and Server Key (MOON_SERVER_KEY) to use a modpack")
    if not base.lower().startswith("https://") and not base.lower().startswith("http://localhost"):
        raise RuntimeError("Game API URL must be an https:// address")
    return base, key


def _get(base, path, key):
    """GET on the API with the key, returning (status, headers, body). Redirects are returned, not followed, so the
    key never goes to the storage host."""
    request = urllib.request.Request(
        base + path, headers={"Authorization": f"Bearer {key}", "User-Agent": "moon-sweepers-server"})
    opener = urllib.request.build_opener(game_download._NoRedirect)
    try:
        with opener.open(request, timeout=30) as response:
            return response.status, response.headers, response.read()
    except urllib.error.HTTPError as error:
        return error.code, error.headers, error.read()


def _fetch_pack(base, key, token):
    status, _headers, body = _get(base, f"/server/packs/{token}", key)
    if status == 401:
        raise RuntimeError("the API didn't accept the Server Key (MOON_SERVER_KEY)")
    # 404: no such pack or revision. 410: a revision that lists a mod its author has since deleted.
    if status in (404, 410):
        try:
            reason = json.loads(body).get("message")
        except ValueError:
            reason = None
        raise RuntimeError(reason or "the API doesn't offer modpacks (is it up to date, with SERVER_KEY set?)")
    if status != 200:
        raise RuntimeError(f"the API answered HTTP {status} for modpack {token}")
    return json.loads(body.decode("utf-8"))


def _download(base, key, mod, target):
    """Follows the API's redirect to storage (without the key) and checks size and SHA-256 against the pack."""
    path = mod["download"][len(base):] if mod["download"].startswith(base) else None
    if not path or not path.startswith("/server/packages/"):
        raise RuntimeError(f"{mod['package']}: unexpected download link")
    status, headers, _body = _get(base, path, key)
    location = headers.get("Location") if status in (301, 302, 303, 307, 308) else None
    if not location:
        raise RuntimeError(f"{mod['package']}: the API answered HTTP {status} instead of a download link")
    request = urllib.request.Request(location, headers={"User-Agent": "moon-sweepers-server"})
    digest, total = hashlib.sha256(), 0
    with urllib.request.urlopen(request, timeout=120) as body, target.open("wb") as handle:
        for block in iter(lambda: body.read(1 << 20), b""):
            total += len(block)
            if total > MAX_PACKAGE:
                raise RuntimeError(f"{mod['package']}: the download is larger than the registry allows")
            digest.update(block)
            handle.write(block)
    if digest.hexdigest() != mod["sha256"]:
        raise RuntimeError(f"{mod['package']} {mod['version']}: the download doesn't match the registry's checksum")


def _display_name(mod):
    return mod["name"].replace("_", " ")[:64]


def _installed(entry, mod):
    """True when this entry is the pack's mod at the same version with all its files still in place."""
    source = entry.get("registry") or {}
    if source.get("package") != mod["package"] or source.get("version") != mod["version"]:
        return False
    if (entry.get("side") == "client") != (mod["side"] == "client"):
        return False  # moving to or from client-only means installing or deleting the files
    for key, _label, folder in mods._folders():
        for relative in entry.get(key) or []:
            target = mods._mod_file(folder, relative)
            if target is None or not target.is_file():
                return False
    return True


def _record(base, mod):
    """What the manifest needs to send players to the registry for this mod."""
    return {
        "package": mod["package"],
        "version": mod["version"],
        "sha256": mod["sha256"],
        "side": mod["side"],
        # Players download through their own launcher sign-in, on the player route.
        "download": f"{base}/packages/{mod['author']}/{mod['name']}/{mod['version']}/download",
        "page": mod.get("website") or None,
    }


def _remove_entry(data, entry):
    modpm._delete_files(entry)
    icon = mods._mod_file(mods.MODS_DIR, entry.get("icon")) if entry.get("icon") else None
    if icon is not None and icon.is_file() and icon.parent.name == "icons":
        icon.unlink()
    data["mods"].remove(entry)


_FETCH = object()


def resolve():
    """The modpack Modpack Token names, as the API resolves it, or None when there's no token or it couldn't be
    fetched (the reason is printed). Fetched before the game is installed, since the pack can name the game version."""
    token = _token()
    if not token:
        return None
    try:
        if not TOKEN.match(token):
            raise RuntimeError(f"Modpack Token {token!r} should look like K7QX3MP or K7QX3MP@3")
        base, key = _api()
        return _fetch_pack(base, key, token)
    except Exception as error:  # the server should still start with what it had
        say(f"Couldn't get the modpack: {str(error).rstrip('.')}. Starting with what's already installed.")
        return None


def required_game(pack):
    """The game archive the pack needs, or None for any."""
    game = (pack or {}).get("game")
    return game.get("name") if isinstance(game, dict) and isinstance(game.get("name"), str) else None


def sync(pack=_FETCH):
    """Brings mods/ in line with the modpack (`pack` from resolve(), fetched now if not given). Never raises: a failed
    sync leaves the last good install in place."""
    token = _token()
    try:
        if not token:
            _forget_pack()
            return
        if pack is _FETCH:
            pack = resolve()
        if pack is None:
            return
        if not mods.enabled():
            say("Modpack Token is set but BepInEx mods are off (Enable BepInEx Mods = 0); not installing its mods.")
            return
        base, key = _api()
        _apply(base, key, pack)
    except Exception as error:  # the server should still start with what it had
        say(f"Couldn't install the modpack: {str(error).rstrip('.')}. Starting with the mods already installed.")


def _apply(base, key, pack):
    say(f"{pack['name']} revision {pack['revision']} ({len(pack['mods'])} mods)")
    wanted = {mod["package"]: mod for mod in pack["mods"]}
    with modpm._lock:
        data = modpm._load()
    by_package = {
        (entry.get("registry") or {}).get("package"): entry
        for entry in data["mods"]
        if isinstance(entry, dict) and entry.get("registry")
    }

    changed = 0
    for mod in pack["mods"]:
        entry = by_package.get(mod["package"])
        extra = {
            "registry": _record(base, mod),
            "required": bool(mod["required"]),
            "side": mod["side"],
            "author": mod["author"],
        }
        if mod["side"] == "client":
            # Players' launchers install it; the server doesn't run it, so only the registry's file list is kept.
            with modpm._lock:
                data = modpm._load()
                old = next((e for e in data["mods"] if (e.get("registry") or {}).get("package") == mod["package"]), None)
                if old is not None and _installed(old, mod) and all(old.get(k) == v for k, v in extra.items()):
                    continue
                if old is not None:
                    _remove_entry(data, old)
                data["mods"].append({
                    "name": _display_name(mod),
                    "version": mod["version"],
                    "description": (mod.get("description") or "")[:400] or None,
                    "link": mod.get("website") or None,
                    "client_files": mod["files"],
                    **extra,
                })
                modpm._save(data)
            changed += 1
            continue
        if entry is not None and _installed(entry, mod):
            # Same files: only the pack's settings (required, side) can have changed.
            if any(entry.get(k) != v for k, v in extra.items()):
                with modpm._lock:
                    data = modpm._load()
                    current = next(e for e in data["mods"] if (e.get("registry") or {}).get("package") == mod["package"])
                    current.update(extra)
                    modpm._save(data)
            continue
        say(f"Installing {mod['package']} {mod['version']} ...")
        with tempfile.TemporaryDirectory(dir=str(mods.MODS_DIR), prefix=".pack-") as work:
            archive = Path(work) / f"{mod['package']}-{mod['version']}.zip"
            _download(base, key, mod, archive)
            if entry is not None and entry.get("name") != _display_name(mod):
                with modpm._lock:
                    data = modpm._load()
                    old = next(e for e in data["mods"] if (e.get("registry") or {}).get("package") == mod["package"])
                    _remove_entry(data, old)
                    modpm._save(data)
            modpm.install(None, _display_name(mod), archive=archive, extra=extra, quiet=True)
        changed += 1

    with modpm._lock:
        data = modpm._load()
        gone = [e for e in data["mods"] if isinstance(e, dict) and e.get("registry") and e["registry"].get("package") not in wanted]
        for entry in gone:
            _remove_entry(data, entry)
            say(f"Removed {entry['name']}: the modpack no longer lists it.")
        data["pack"] = {
            "token": pack["token"],
            "name": pack["name"],
            "revision": pack["revision"],
            # What players must run; launchers install exactly these before joining.
            "game": pack.get("game"),
            "client": pack.get("client"),
        }
        modpm._save(data)

    if changed or gone:
        say(f"Installed {changed} and removed {len(gone)} mod(s). Players' launchers get the same mods from the registry.")
    else:
        say("Everything is up to date.")


def _forget_pack():
    """With no Modpack Token, mods a previous pack installed are removed, and the manifest stops naming a pack."""
    with modpm._lock:
        if not mods.MANIFEST_PATH.is_file():
            return
        data = modpm._load()
        gone = [e for e in data["mods"] if isinstance(e, dict) and e.get("registry")]
        if not gone and "pack" not in data:
            return
        for entry in gone:
            _remove_entry(data, entry)
        data.pop("pack", None)
        modpm._save(data)
    say(f"Modpack Token is empty: removed the {len(gone)} mod(s) the previous modpack installed.")


def status():
    token = _token()
    try:
        pack = modpm._load().get("pack")
    except (OSError, ValueError):
        pack = None
    if not token:
        return "No modpack (Modpack Token is empty)."
    if not pack:
        return f"Modpack Token is {token}, but it hasn't been installed yet. Run: mod pack"
    game = (pack.get("game") or {}).get("name") or "any"
    return (f"Modpack {pack['name']} ({pack['token']}) revision {pack['revision']}. Token setting: {token}. "
            f"Game: {game}. Client: {pack.get('client') or 'any'}.")
