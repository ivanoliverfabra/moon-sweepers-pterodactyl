# Moon Sweepers dedicated server

An unofficial dedicated server for Moon Sweepers. It runs the real Windows game headless under Wine, on a CPU-only
Linux host, with a genuine anonymous Steam GameServer identity. Players join with their own licensed Steam accounts
and a join password; nobody needs a third account or a Steam desktop on the server.

**Supported game:** Moon Sweepers 1.0.5, Steam build 26317497. Every player needs the matching client module (the
[launcher](https://github.com/ivanoliverfabra/moon-sweepers-launcher) installs it for them).

This is an unofficial, experimental adapter, not affiliated with or supported by the game's developers. You supply
your own game files; none are included.

## Ways to run it

| | Guide | Good for |
| --- | --- | --- |
| **Pterodactyl / Calagopus egg** | [PTERODACTYL.md](PTERODACTYL.md) | Game panels. Downloads the game from the Moon Sweepers API, BepInEx mods, modpacks by token, a server console with mod commands, an ID API the launcher reads (status, players, icon, banner, mods, required versions). |
| **Ubuntu with Docker Compose** | below, and [SERVER.md](SERVER.md) | A box you run yourself, with your own copy of the game folder. |

What both do:

- Run the world with `-batchmode`/`-nographics` and keep saves, backups and logs in a persistent volume. A clean stop
  saves first and archives the save (ten kept by default).
- Check Steam tickets and game ownership before anyone joins, then the server's join password. An optional SteamID
  allowlist restricts it further.
- Connect over Steam P2P/relay: no inbound ports to open. Players join by the server's current Steam ID, which can
  change when it restarts.

The Pterodactyl image adds, among other things:

- **Mods:** BepInEx 6, built with the fixes this game needs (see [modding/README.md](modding/README.md)). Mods live in
  `mods/` and are described in `mods/mods.json`; the server reads each mod's DLLs for its name, version, plugins and
  dependencies, and tells launchers exactly which files players need.
- **Modpacks:** set **Modpack Token** to a pack from the Moon Sweepers registry and the server installs it on every
  start, including the game version it pins.
- **Required versions:** the server reports its game build and the client release players need (**Client Version**),
  and launchers only allow Play with exactly those.

## Players

The easy way is the [Moon Sweepers Launcher](https://github.com/ivanoliverfabra/moon-sweepers-launcher): add the
server's address, and it installs the right game and client module, gets the server's mods, and joins with the
current Steam ID. Without the launcher, install the client module by hand as described under
[Connect each Windows player](#connect-each-windows-player).

## Documentation

| File | What's in it |
| --- | --- |
| [PTERODACTYL.md](PTERODACTYL.md) | The egg: setup, variables, folders, console commands, the ID API, mods and modpacks, SpaceWar mode |
| [SERVER.md](SERVER.md) | The full Ubuntu guide: copying the game, settings, backups, networking, troubleshooting |
| [modding/README.md](modding/README.md) | How BepInEx is built for this game, and the bundle the launcher installs |
| [native/README.md](native/README.md) | The native server and client module, and how to build them |
| [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) | Third-party attribution |

## Status

**Tested:** a real licensed Steam client joined the CPU-only Ubuntu container,
loaded its world, moved, and swept dust. The server saved the changed terrain.
Fresh installation, clean Docker shutdown, automatic backup, and restoration after
restart also passed. Password checks pass automated tests; the new password prompt
still needs a live client connection test. Two simultaneous playing clients have
not yet been tested.

## Install on Ubuntu with Docker Compose

Copy this project and your owned Windows game folder to your server. Then, from
the project directory:

```sh
git clone https://github.com/hexonMD/moon-sweepers-dedicated-server.git
cd moon-sweepers-dedicated-server
python3 scripts/server_setup.py
docker compose --env-file .server.env -f compose.server.yaml up -d --build
docker compose --env-file .server.env -f compose.server.yaml exec -T server /opt/moon/bin/entrypoint status
```

Setup asks for the game folder, generates a join password, and prints it plainly.
Status prints the current anonymous `server_steam_id`. Read the complete [Ubuntu installation guide](SERVER.md)
for the Windows-to-Ubuntu copy command, settings, backups, and troubleshooting.

No Steam username or password is used. The image contains Wine and our module;
it imports your game files from a read-only mount and downloads only Valve's
official server redistributables anonymously. Steam tickets and game ownership are
checked before a player can join. Any licensed player with the join password can
connect. An optional SteamID allowlist can restrict access further.

Set the password with Docker's `MOON_SERVER_PASSWORD` environment variable (8-128
printable ASCII characters). Compose reads it from `.server.env`:

```dotenv
MOON_SERVER_PASSWORD='your-chosen-server-password'
MOON_ALLOWED_STEAM_IDS=
```

Run `docker compose --env-file .server.env -f compose.server.yaml up -d` after
changing it so Compose recreates the container with the new environment. This is
the game's shared join password;
it is separate from every player's Steam password.

## Ports and joining

```dotenv
MOON_GAME_PORT=27025
MOON_QUERY_PORT=27026
```

These configure the Steam GameServer API inside the container. Both must be
different numbers from 1024 to 65535. The status command reports their values.

**Players connect using the current server Steam ID and join password. Direct
`IP:port` joining is not implemented.** Changing these settings does not create an
IP-based join endpoint. The default Compose file publishes no inbound ports;
the adapter uses Steam P2P/relay networking. Forwarding a port on your router
does not change the connection method. See [networking details](SERVER.md#ports-and-connection-method).

## Connect each Windows player

Download the client/server ZIP from [GitHub Releases](https://github.com/hexonMD/moon-sweepers-dedicated-server/releases).
Close Moon Sweepers, open PowerShell in the unpacked release, and run:

```powershell
.\scripts\install-client.ps1 -GamePath "D:\SteamLibrary\steamapps\common\Moon Sweepers" -ServerId YOUR_SERVER_STEAM_ID -Launch
```

Use that player's actual game folder and the server ID from Docker status.
Alternatively, install the DLL with the script and set this game's Steam launch
options to `-moon-connect YOUR_SERVER_STEAM_ID`. Each player remains signed into
their own normal Steam client. Enter the server password in the connection prompt.
Complete the game's first-time new-hire intro when
it appears. The server ID can change on restart; use the current one.

The installer creates **Join Moon Sweepers** on your Windows desktop, using the
game's icon. After that, double-click it and enter the server password to join.
Its launcher is installed beside the game and reads the saved connection settings;
you can remove the extracted download folder after installing. The password is
requested by the game each time and is not stored in the shortcut.

If the client module is already installed, create the shortcut without reinstalling:

```powershell
.\scripts\install-client.ps1 -Action Shortcut -GamePath "D:\SteamLibrary\steamapps\common\Moon Sweepers"
```

To update a changed server Steam ID, add `-ServerId NEW_SERVER_STEAM_ID` to that
command. The existing shortcut will use the updated setting. Pass
`-NoDesktopShortcut` during installation if you do not want a desktop icon.

You can also launch again from the release folder with saved connection settings:

```powershell
.\scripts\install-client.ps1 -Action Launch -GamePath "D:\SteamLibrary\steamapps\common\Moon Sweepers"
```

Pass a new `-ServerId` when the server's ID changes. To remove the client module,
use `-Action Uninstall`; this removes the owned shortcut and installed launcher,
and preserves game saves and Steam settings.

## Save and stop

```sh
docker compose --env-file .server.env -f compose.server.yaml exec -T server /opt/moon/bin/entrypoint save
docker compose --env-file .server.env -f compose.server.yaml stop
```

Docker stop asks the game to finish saving and quit. A confirmed clean stop creates
a save archive in `/data/backups`, with ten retained by default. The `server-data`
volume stores saves, backups, logs, imported game files, and the Wine prefix.
Export backups to another disk. Avoid `down -v` when keeping the world.

See [SERVER.md](SERVER.md) for backups, restoring and troubleshooting.
