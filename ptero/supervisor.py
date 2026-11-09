#!/usr/bin/env python3
"""Pterodactyl console/signal bridge for the upstream Moon Sweepers runner."""
import json
import os
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import modpm
import mods

ENTRYPOINT = "/opt/moon/bin/entrypoint"
IMPORTER = "/opt/moon/bin/server-import"
SYNC_GAME = "/opt/moon/ptero/sync-game.py"
STATE_PATH = Path("/data/logs/moon-host-state.json")
COMMAND_PATH = Path("/data/control/command")
# Commands the game host handles itself (see native/moon_host.c control_tick).
HOST_COMMANDS = {"addmoney", "setmoney", "players", "kick", "ban", "unban"}
# The runner rewrites the state file on every game heartbeat, so a stale
# heartbeat means the host process is wedged even though the file still exists.
HEARTBEAT_FRESH_SECONDS = 15
child = None
stopping = False

def server_metadata():
    """Host-configured identity: icon/banner URLs and the message of the day.

    The icon should be a 1:1 (square) image and the banner a 16:9 image; both
    must already be hosted somewhere public, so the API only hands out their
    URLs instead of fetching them itself.
    """
    return {
        "icon": os.environ.get("MOON_ICON", "").strip() or None,
        "banner": os.environ.get("MOON_BANNER", "").strip() or None,
        "motd": os.environ.get("MOON_MOTD", "").strip() or None,
    }

def host_state():
    """Read the runner's state file, flagging heartbeat staleness."""
    try:
        state = json.loads(STATE_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError, ValueError):
        state = {"ready": False, "phase": "no-state"}
    heartbeat_ms = state.get("heartbeat_unix_ms")
    if isinstance(heartbeat_ms, (int, float)):
        age = max(0.0, time.time() * 1000 - heartbeat_ms) / 1000
        state["heartbeat_age_seconds"] = round(age, 1)
        state["heartbeat_fresh"] = age <= HEARTBEAT_FRESH_SECONDS
    else:
        state["heartbeat_age_seconds"] = None
        state["heartbeat_fresh"] = False
    state.update(server_metadata())
    return state

def player_entries(state):
    """Connected players as {"name", "steam_id"} records."""
    players = state.get("players")
    if not isinstance(players, list):
        return []
    return [
        {"name": p["name"], "steam_id": str(p.get("id", ""))}
        for p in players
        if isinstance(p, dict) and isinstance(p.get("name"), str)
    ]

def game_port():
    try:
        return int(os.environ.get("MOON_GAME_PORT", "27025"))
    except ValueError:
        return 27025

def player_names(state):
    return [entry["name"] for entry in player_entries(state)]

class IdApiHandler(BaseHTTPRequestHandler):
    """Quiet HTTP endpoint exposing the host state and changing server ID.

    The game talks Steam P2P over UDP only, so the TCP side of the primary
    game port stays free; Wings already publishes it externally.
    """

    def do_GET(self):
        path = self.path.split("?", 1)[0].rstrip("/") or "/"
        if path == "/":
            state = host_state()
            entries = player_entries(state)
            state["players"] = [entry["name"] for entry in entries]
            state["player_entries"] = entries
            state["mods"] = mods.summary(game_port())
            self._send(200, "application/json",
                       json.dumps(state, indent=2).encode())
        elif path == "/players":
            state = host_state()
            entries = player_entries(state)
            self._send(200, "application/json",
                       json.dumps({
                           "count": len(entries),
                           "players": [entry["name"] for entry in entries],
                           "entries": entries,
                       }, indent=2).encode())
        elif path == "/mods":
            self._send(200, "application/json",
                       json.dumps(mods.manifest(game_port()), indent=2).encode())
        elif path == "/id":
            state = host_state()
            steam_id = state.get("server_steam_id")
            if steam_id:
                self._send(200, "text/plain", str(steam_id).encode())
            else:
                state["error"] = "server_steam_id not available yet"
                self._send(503, "application/json",
                           json.dumps(state, indent=2).encode())
        elif path in ("/icon", "/banner"):
            url = host_state().get(path[1:])
            if url:
                self.send_response(302)
                self.send_header("Location", url)
                self.send_header("Content-Length", "0")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
            else:
                self._send(
                    404, "application/json",
                    json.dumps({"error": f"{path[1:]} not configured"}).encode(),
                )
        elif path == "/motd":
            motd = host_state().get("motd")
            if motd:
                self._send(200, "text/plain", motd.encode())
            else:
                self._send(404, "application/json",
                           json.dumps({"error": "motd not configured"}).encode())
        elif path == "/health":
            self._send(200, "text/plain", b"ok")
        else:
            self._send(404, "text/plain", b"not found")

    def _send(self, code, content_type, body):
        self.send_response(code)
        self.send_header("Content-Type", content_type + "; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        pass

def start_id_api():
    """Serve the ID API on the primary game port (TCP) when enabled."""
    if os.environ.get("MOON_ID_API_ENABLED", "1").strip().lower() in ("0", "false", "no"):
        return None
    port = int(os.environ.get("MOON_GAME_PORT", "27025"))
    for name, variable in (("icon", "MOON_ICON"), ("banner", "MOON_BANNER")):
        url = server_metadata()[name]
        if url and not url.lower().startswith(("http://", "https://")):
            print(f"Warning: {variable} should point to a hosted http(s) image URL; ignoring {url!r}.", flush=True)
            os.environ[variable] = ""
    try:
        server = ThreadingHTTPServer(("0.0.0.0", port), IdApiHandler)
    except OSError as error:
        print(f"ID API disabled: could not bind TCP {port}: {error}", flush=True)
        return None
    server.daemon_threads = True
    thread = threading.Thread(
        target=server.serve_forever,
        kwargs={"poll_interval": 0.5},
        name="moon-id-api",
        daemon=True,
    )
    thread.start()
    print(
        f"ID API listening on TCP {port}: GET / (state JSON), GET /id "
        "(server_steam_id), GET /players, GET /mods, GET /health, GET /icon, GET /banner, GET /motd",
        flush=True,
    )
    return server

def send_host_command(line):
    """Hand a command to the game host through its command file.

    The host consumes one command per file, so wait briefly for the previous
    one to be picked up instead of overwriting it.
    """
    COMMAND_PATH.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.time() + 3
    while COMMAND_PATH.exists() and time.time() < deadline:
        time.sleep(0.1)
    if COMMAND_PATH.exists():
        print("The server has not read the previous command yet; try again.", flush=True)
        return
    temporary = COMMAND_PATH.with_name(f".command-{os.getpid()}")
    temporary.write_text(line + "\n", encoding="utf-8")
    os.replace(temporary, COMMAND_PATH)

def run_control(action):
    result = subprocess.run(
        [ENTRYPOINT, action],
        cwd="/home/container",
        env=os.environ.copy(),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    output = result.stdout.strip()
    if action == "status" and output:
        try:
            state = json.loads(output)
            steam_id = state.get("server_steam_id")
            if steam_id:
                print(f"Server Steam ID: {steam_id}", flush=True)
            print(json.dumps(state, indent=2), flush=True)
        except json.JSONDecodeError:
            print(output, flush=True)
    elif output:
        print(output, flush=True)
    if result.returncode:
        print(f"Command '{action}' exited with code {result.returncode}.", flush=True)

def prepare_game():
    """Import the uploaded game once, then sync retail/online-fix files.

    The upstream runner validates and imports the retail file set. Running the
    importer before the runner lets us mirror the online-fix support files into
    the imported tree while the game is not running.
    """
    environment = os.environ.copy()
    target = Path("/data/game")
    if target.is_dir() and not any(target.iterdir()):
        # The egg install step historically created an empty game directory.
        # The upstream importer refuses to run when /data/game already exists.
        target.rmdir()
        print("Removed the empty imported-game directory left by the install step.", flush=True)
    if (target / "MoonSweepers.exe").is_file():
        print("Using the existing imported game files.", flush=True)
    else:
        result = subprocess.run(
            [IMPORTER],
            cwd="/home/container",
            env=environment,
            check=False,
        )
        if result.returncode:
            print("Game import failed; see the messages above.", flush=True)
            return result.returncode
    result = subprocess.run(
        [sys.executable, SYNC_GAME],
        cwd="/home/container",
        env=environment,
        check=False,
    )
    if result.returncode:
        print("Game file synchronization failed; see the messages above.", flush=True)
        return result.returncode
    return 0

def request_stop(signum, _frame):
    global stopping
    if stopping:
        return
    stopping = True
    if child is not None and child.poll() is None:
        print(f"Received {signal.Signals(signum).name}; forwarding graceful stop to game runner.", flush=True)
        try:
            child.send_signal(signum)
        except ProcessLookupError:
            pass

def console():
    for raw in sys.stdin:
        line = " ".join(raw.split())
        command = line.lower()
        verb, _, argument = line.partition(" ")
        verb = verb.lower()
        if verb in HOST_COMMANDS:
            if len(line) > 200 or not line.isprintable():
                print("Command rejected: too long or contains control characters.", flush=True)
            else:
                send_host_command(f"{verb} {argument}".strip())
        elif command in ("", "help"):
            print("Console commands: status, save, stop, players, kick <player>, ban <player>, "
                  "unban <player>, addmoney <amount>, setmoney <amount>, mods, mod help, help", flush=True)
        elif command in ("status", "save"):
            run_control(command)
        elif command == "mods":
            print(mods.describe(game_port()), flush=True)
        elif verb == "mod":
            if len(line) > 1200 or not line.isprintable():
                print("Command rejected: too long or contains control characters.", flush=True)
            else:
                modpm.run_in_background(argument)
        elif command == "stop":
            request_stop(signal.SIGTERM, None)
        else:
            print(f"Unknown command: {command}. Type help for the list.", flush=True)

def main():
    global child
    game = Path("/home/container/game-source")
    required = ("MoonSweepers.exe", "GameAssembly.dll", "UnityPlayer.dll", "MoonSweepers_Data")
    missing = [name for name in required if not (game / name).exists()]
    if missing:
        print("Upload the CONTENTS of your Moon Sweepers 1.0.5 game folder to /home/container/game-source/ using SFTP.", flush=True)
        print("Missing: " + ", ".join(missing), flush=True)
        return 1

    os.environ.setdefault("WINEARCH", "win64")
    os.environ.setdefault("WINEPREFIX", "/home/container/wineprefix")
    os.environ.setdefault("WINEDEBUG", "-all")
    os.environ.setdefault("DISPLAY", ":99")
    os.environ.setdefault("SteamAppId", "5335510")
    os.environ.setdefault("SteamGameId", "5335510")
    os.environ.setdefault("MOON_HOST", "1")
    os.environ.setdefault("MOON_DEDICATED", "1")
    os.environ.setdefault("MOON_HEADLESS", "1")

    app_id = os.environ.get("SteamAppId", "")
    game_id = os.environ.get("SteamGameId", "")
    if app_id and game_id and app_id != game_id:
        print(
            f"Warning: SteamAppId ({app_id}) and SteamGameId ({game_id}) differ. "
            "Set both to 5335510 for the retail game or both to 480 for SpaceWar.",
            flush=True,
        )

    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)

    id_api = start_id_api()

    code = prepare_game()
    if stopping:
        print("Stop requested during game preparation.", flush=True)
        return 0
    if code:
        return code

    print("Moon Sweepers server starting. Console commands: status, save, stop, players, kick, ban, unban, addmoney, setmoney, mods, mod, help", flush=True)
    child = subprocess.Popen([ENTRYPOINT, "run"], cwd="/home/container", env=os.environ.copy())
    threading.Thread(target=console, daemon=True).start()
    code = child.wait()
    print(f"Moon Sweepers runner exited with code {code}.", flush=True)
    return code

if __name__ == "__main__":
    raise SystemExit(main())
