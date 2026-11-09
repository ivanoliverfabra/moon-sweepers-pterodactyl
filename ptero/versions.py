"""What players must run to join this server, modded or not: the game build and the client module release.

Served in GET /mods as "requires", so launchers allow Play only with exactly these:

    "requires": {
      "game": {"name": "Moon.Sweepers.v1.0.5.zip", "build": "14f711a6a3b1b7cd"},
      "client": "v0.2.2"
    }

- game.build is the first 16 hex digits of GameAssembly.dll's SHA-256: the same build check the launcher uses, so it
  holds however the game got here. game.name is the API download it came from (when it did), which launchers install.
- client is the modpack's pinned release, else the egg's Client Version (MOON_CLIENT_VERSION), else null for any.
"""
import hashlib
import os
import re
from pathlib import Path

import game_download

GAME_SOURCE = Path("/home/container/game-source")
CLIENT_TAG = re.compile(r"^v\d{1,4}\.\d{1,4}\.\d{1,4}(?:-[A-Za-z0-9.]{1,32})?$")

_build = {}  # path -> (size, mtime_ns, build)


def game_build(source=None):
    """The game's build id (16 hex digits of GameAssembly.dll's SHA-256), or None when the game isn't there."""
    path = (source or GAME_SOURCE) / "GameAssembly.dll"
    try:
        stat = path.stat()
    except OSError:
        return None
    cached = _build.get(path)
    if cached and cached[0] == stat.st_size and cached[1] == stat.st_mtime_ns:
        return cached[2]
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    build = digest.hexdigest()[:16]
    _build[path] = (stat.st_size, stat.st_mtime_ns, build)
    return build


def client_setting():
    """The egg's Client Version, when it's a release tag; warns once about anything else."""
    value = os.environ.get("MOON_CLIENT_VERSION", "").strip()
    if value and not CLIENT_TAG.match(value):
        if not getattr(client_setting, "warned", False):
            print(f"Warning: Client Version {value!r} should look like v0.2.2; players may use any client.", flush=True)
            client_setting.warned = True
        return None
    return value or None


def requires(pack=None):
    """`pack` is the modpack record in mods.json, when the server installs one."""
    pack = pack if isinstance(pack, dict) else {}
    record = game_download.installed()
    build = game_build()
    game = {"name": record["name"] if record else None, "build": build} if build else None
    pinned = pack.get("client")
    client = pinned if isinstance(pinned, str) and CLIENT_TAG.match(pinned) else client_setting()
    return {"game": game, "client": client}
