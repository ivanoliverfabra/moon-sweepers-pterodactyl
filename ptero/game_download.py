"""Installs the game from the Moon Sweepers API when game-source is empty.

Set the egg's Game API URL (MOON_GAME_API_URL) and Server Key (MOON_SERVER_KEY, the API's SERVER_KEY). The server then
asks the API for its file list, downloads the game archive, checks it, and unpacks it into game-source, so nothing has to
be uploaded by hand. The key is only ever sent to the API itself, never to the download host it redirects to.
"""
import hashlib
import json
import os
import shutil
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

REQUIRED = ("MoonSweepers.exe", "GameAssembly.dll", "UnityPlayer.dll", "MoonSweepers_Data")
WORK = Path("/data")
MAX_ARCHIVE = 20 * 1024 * 1024 * 1024


def say(text):
    print(text, flush=True)


def present(source):
    return all((source / name).exists() for name in REQUIRED)


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def _api(base, path, key):
    """GET on the API with the key. Redirects are returned, not followed, so the key never leaves the API."""
    request = urllib.request.Request(
        base + path, headers={"Authorization": f"Bearer {key}", "User-Agent": "moon-sweepers-server"})
    opener = urllib.request.build_opener(_NoRedirect)
    try:
        return opener.open(request, timeout=30)
    except urllib.error.HTTPError as error:
        if error.code in (301, 302, 303, 307, 308):
            return error
        if error.code == 401:
            raise RuntimeError("the API didn't accept the Server Key (MOON_SERVER_KEY)") from None
        if error.code == 404:
            raise RuntimeError("the API has no server downloads; set SERVER_KEY on the API") from None
        raise RuntimeError(f"the API answered HTTP {error.code}") from None


def _pick(files):
    wanted = os.environ.get("MOON_GAME_FILE", "").strip()
    if wanted:
        for entry in files:
            if entry.get("name") == wanted:
                return entry
        raise RuntimeError(f"the API doesn't list {wanted} (MOON_GAME_FILE)")
    zips = [e for e in files if str(e.get("name", "")).lower().endswith(".zip")]
    if not zips:
        raise RuntimeError("the API lists no .zip game archive")
    return zips[0]


def _download(url, target, size, sha256):
    request = urllib.request.Request(url, headers={"User-Agent": "moon-sweepers-server"})
    digest = hashlib.sha256()
    received, next_report, started = 0, 0.1, time.time()
    with urllib.request.urlopen(request, timeout=60) as response, target.open("wb") as handle:
        total = size or int(response.headers.get("Content-Length") or 0)
        while True:
            block = response.read(1 << 20)
            if not block:
                break
            received += len(block)
            if received > MAX_ARCHIVE:
                raise RuntimeError("the download is larger than 20 GB")
            digest.update(block)
            handle.write(block)
            if total and received / total >= next_report:
                rate = received / max(time.time() - started, 0.1) / 1048576
                say(f"Downloading the game: {received * 100 // total}% ({received // 1048576} of "
                    f"{total // 1048576} MB, {rate:.1f} MB/s)")
                next_report += 0.1
    if size and received != size:
        raise RuntimeError(f"the download ended early ({received} of {size} bytes)")
    if sha256 and digest.hexdigest() != sha256.lower():
        raise RuntimeError("the download doesn't match its SHA-256; try again")


def _unpack(archive, source):
    """Unpacks into a staging folder next to game-source, then moves the game's own folder contents in."""
    staging = WORK / "game-download-staging"
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    with zipfile.ZipFile(archive) as zipped:
        for member in zipped.infolist():
            parts = PurePosixPath(member.filename.replace("\\", "/")).parts
            if member.filename.startswith("/") or ".." in parts or any(":" in p for p in parts):
                raise RuntimeError("the archive contains unsafe paths")
        zipped.extractall(staging)
    # The game may sit in a folder inside the archive ("Moon Sweepers/MoonSweepers.exe").
    roots = [path.parent for path in staging.rglob("MoonSweepers.exe") if path.is_file()]
    if not roots:
        raise RuntimeError("the archive doesn't contain MoonSweepers.exe")
    root = min(roots, key=lambda path: len(path.parts))
    source.mkdir(parents=True, exist_ok=True)
    for item in root.iterdir():
        target = source / item.name
        if target.is_dir() and not target.is_symlink():
            shutil.rmtree(target)
        elif target.exists() or target.is_symlink():
            target.unlink()
        shutil.move(str(item), str(target))
    shutil.rmtree(staging, ignore_errors=True)


def install(source):
    """Downloads and unpacks the game into `source` when it's missing. Returns True when the game is there after."""
    if present(source):
        return True
    base = os.environ.get("MOON_GAME_API_URL", "").strip().rstrip("/")
    key = os.environ.get("MOON_SERVER_KEY", "").strip()
    if not base or not key:
        return False
    parsed = urllib.parse.urlparse(base)
    local = parsed.hostname in ("localhost", "127.0.0.1", "::1")
    if parsed.scheme != "https" and not (parsed.scheme == "http" and local):
        say("Game API URL must start with https://; not downloading the game.")
        return False

    archive = WORK / "game-download.zip"
    try:
        say(f"The game isn't in game-source yet; getting it from {base}.")
        with _api(base, "/server/manifest.json", key) as response:
            files = json.loads(response.read().decode("utf-8")).get("files") or []
        entry = _pick(files)
        name, size, sha256 = entry["name"], entry.get("size"), entry.get("sha256")
        link = urllib.parse.urlparse(str(entry.get("url", "")))
        if (link.scheme, link.netloc) != (parsed.scheme, parsed.netloc):
            raise RuntimeError("the API's download link points somewhere else; not sending the key there")
        free = shutil.disk_usage(WORK).free
        if size and free < size * 2.2:
            raise RuntimeError(f"not enough disk space ({free // 1048576} MB free, about "
                               f"{int(size * 2.2) // 1048576} MB needed to download and unpack)")

        with _api(base, link.path, key) as response:
            location = response.headers.get("Location")
        if not location:
            raise RuntimeError("the API gave no download link")
        say(f"Downloading {name}{f' ({size // 1048576} MB)' if size else ''}...")
        WORK.mkdir(parents=True, exist_ok=True)
        _download(location, archive, size, sha256)
        say("Unpacking the game into game-source...")
        _unpack(archive, source)
        (WORK / "game-download.json").write_text(json.dumps(
            {"name": name, "sha256": sha256, "installed_at": int(time.time())}, indent=2) + "\n")
        say(f"Installed {name} into game-source.")
        return present(source)
    except (RuntimeError, OSError, ValueError, zipfile.BadZipFile, urllib.error.URLError) as error:
        say(f"Couldn't install the game from the API: {error}")
        return False
    finally:
        archive.unlink(missing_ok=True)
