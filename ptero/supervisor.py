#!/usr/bin/env python3
"""Pterodactyl console/signal bridge for the upstream Moon Sweepers runner."""
import json
import os
import signal
import subprocess
import sys
import threading
from pathlib import Path

ENTRYPOINT = "/opt/moon/bin/entrypoint"
child = None
stopping = False

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

def request_stop(signum, _frame):
    global stopping
    if stopping:
        return
    stopping = True
    print(f"Received {signal.Signals(signum).name}; forwarding graceful stop to game runner.", flush=True)
    if child is not None and child.poll() is None:
        try:
            child.send_signal(signum)
        except ProcessLookupError:
            pass

def console():
    for raw in sys.stdin:
        command = raw.strip().lower()
        if command in ("", "help"):
            print("Console commands: status, save, stop, help", flush=True)
        elif command in ("status", "save"):
            run_control(command)
        elif command == "stop":
            request_stop(signal.SIGTERM, None)
        else:
            print(f"Unknown command: {command}. Available: status, save, stop, help", flush=True)

def main():
    global child
    game = Path("/home/container/game-source")
    required = ("MoonSweepers.exe", "GameAssembly.dll", "UnityPlayer.dll", "MoonSweepers_Data")
    missing = [name for name in required if not (game / name).exists()]
    if missing:
        print("Upload the CONTENTS of your owned Moon Sweepers 1.0.5 game folder to /home/container/game-source/ using SFTP.", flush=True)
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

    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)

    print("Moon Sweepers server starting. Console commands: status, save, stop, help", flush=True)
    child = subprocess.Popen([ENTRYPOINT, "run"], cwd="/home/container", env=os.environ.copy())
    threading.Thread(target=console, daemon=True).start()
    code = child.wait()
    print(f"Moon Sweepers runner exited with code {code}.", flush=True)
    return code

if __name__ == "__main__":
    raise SystemExit(main())
