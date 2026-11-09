# Moon Sweepers — Pterodactyl / Calagopus server

An unofficial community-hosted Moon Sweepers server.

This project uses an experimental server adapter based on the upstream Moon Sweepers project. It is not affiliated with or supported by the game developers.

## Requirements

- Either a licensed retail copy of Moon Sweepers, or an online-fix patched copy for SpaceWar mode. Online-fix files are **not** provided by this repository (see the disclaimer below) — you supply your own game files.
- Windows PC with Steam installed.
- Moon Sweepers version **1.0.5** (Steam build `26317497`).
- The matching client module.

## Server setup (Pterodactyl / Calagopus)

1. Build and publish the image from the repository root (the
   `publish-pterodactyl.yml` workflow does this on push to `main`):

   ```bash
   docker build -f Dockerfile.pterodactyl -t ghcr.io/ivanoliverfabra/moon-sweepers-pterodactyl:latest .
   docker push ghcr.io/ivanoliverfabra/moon-sweepers-pterodactyl:latest
   ```

   The image bundles the Wine runtime, the native server adapter
   (`native/`) and the Pterodactyl supervisor (`ptero/`). If you publish under
   a different image name, change it in `egg-moon-sweepers.json`
   (`docker_images` and the installation `container`).
2. Import `egg-moon-sweepers.json` into the panel (Admin > Nests > Import Egg).
3. Create a server from the egg. Set **Server Password** (8-128 printable
   ASCII characters, required). Allocate one port: the ID API listens on the
   TCP side of the primary allocation.
4. After the install step finishes, upload the **contents** of your Moon
   Sweepers 1.0.5 game folder to `game-source/` over SFTP (not the parent
   folder), then start the server. The first start imports and validates the
   game; the egg marks the server running when the host reports `world-bound`.
5. Console commands: `status`, `save`, `stop`, `help`. Stopping (or the panel's
   stop signal, `^C`) saves the world gracefully and keeps backups
   (`MOON_BACKUP_KEEP`).

> **Compatibility:** the server accepts both the previous client module (protocol v2) and
> the current one (v3). Existing clients keep joining without changes; only clients on the
> new module (`scripts/package.py`, then `client/install-moon-client.ps1 -ZipPath <zip>`)
> share their face with other players.

## Console commands

Type these in the panel console (names are matched case-insensitively; use the
SteamID64 from `players` if two names are alike):

| Command | Effect |
| --- | --- |
| `players` | Lists connected players with their SteamID64. |
| `kick <player>` | Disconnects a player; they may rejoin. |
| `ban <player or SteamID64>` | Disconnects and blocks a player. Bans persist in `control/bans.txt` (one `SteamID64 name` per line). |
| `unban <player or SteamID64>` | Removes a ban. |
| `addmoney <dollars>` | Adds to the crew's shared wallet (negative removes). Up to 2 decimal places. |
| `setmoney <dollars>` | Sets the crew's shared wallet. |
| `stop` | Runs a final world save, then disconnects every player ("Server is shutting down"), then exits. The panel's stop button does the same. |
| `status`, `save` | Show server state / save the world now. |

Money is one wallet shared by the whole crew. The commands use the game's own
developer cheat path, so the host marks the amount as not earned for
achievements, and the result is logged (`Crew money set: 100 -> 5000`).

## Logs

The console shows player-facing events only, for example
`Fabra (76561198074424621) is connecting`, `Fabra joined the server (2 online)`
and `Fabra left the server`. Set **Network Debug Log** (`MOON_NET_DEBUG=1`) to
add Steam transport and packet detail. The Unity player log is written to
`/dev/null` because it grows without bound; set `MOON_UNITY_LOG` to a file path
to capture it while debugging.

## Mods (BepInEx)

The image includes BepInEx 6. Stock BepInEx does not start on this game, so the image builds it with the fixes
described in `modding/README.md`. Mods are off by default. To turn them on:

1. Set **Enable BepInEx Mods** (`MOON_BEPINEX=1`) and restart.
2. Wait for the first start after enabling. It prints `Preparing BepInEx for this game build`, and takes about a
   minute while it generates interop assemblies for the imported `GameAssembly.dll`. The result is cached in
   `/home/container/bepinex-cache` and only rebuilt when the game changes.
3. Upload plugin `.dll` files to `/home/container/mods/plugins` and restart. Use `mods/patchers` for preloader
   patchers. Plugin configs and `BepInEx.cfg` are in `mods/config`.

These folders are kept outside the game folder, so a re-import does not remove them.

The console shows BepInEx's plugin loading, warnings and errors, prefixed with `BepInEx:`. The full log is
`/home/container/game/BepInEx/LogOutput.log`.

Leave `UpdateInteropAssemblies = false` in `BepInEx.cfg`. If you change it, BepInEx tries to rebuild the interop
assemblies with its own Cpp2IL, which cannot read this game. The startup script sets it back.

Setting `MOON_BEPINEX=0` removes BepInEx's loader (`winhttp.dll`) from the game folder and leaves your mods in
place.

### Managing mods from the console

The `mod` command installs mods and keeps `mods.json` up to date for you:

| Command | What it does |
| --- | --- |
| `mod install <url> [name]` | Downloads a `.zip` or `.dll` (https only), puts plugins in `mods/plugins` and patchers in `mods/patchers`, and adds the mod to `mods.json` as required. The same link becomes the mod's download, so players' launchers fetch exactly the same files. Thunderstore packages and `BepInEx/plugins/...` archives are understood; anything else is kept together in a folder named after the mod. |
| `mod update <name>` | Installs the mod again from its link, replacing its old files. |
| `mod remove <name>` | Deletes the mod's files and its `mods.json` entry. |
| `mod adopt` | Adds `mods.json` entries for files you uploaded yourself: each folder in `mods/plugins` becomes one mod, loose files are grouped by name (`MoonFramework.dll` and `MoonFramework.xml` together), and each patcher becomes its own mod. All are marked required. |
| `mod require <name>`, `mod optional <name>` | Whether players must have it. |
| `mod link <name> <page url>` | The page players are sent to for the download (a GitHub release, Thunderstore, ...). |
| `mod url <name> <download url>`, `mod nexus <name> <mod id> [file id]`, `mod version <name> <version>` | Set the other fields. |
| `mod list` | Same as `mods`: what's installed and what players need. |

A unique part of a name is enough (`mod remove dust`). Restart the server after installing or removing mods so
BepInEx loads the change.

### Telling players which mods they need

`mod install` and `mod adopt` write this file for you; you can also edit it by hand. List the mods players must also
have in `/home/container/mods/mods.json`:

```json
{
  "mods": [
    {
      "name": "Bigger Vacuum",
      "version": "1.2.0",
      "required": true,
      "files": ["BiggerVacuum.dll"],
      "patchers": ["BiggerVacuum.Patcher.dll"],
      "nexus": { "mod_id": 12, "file_id": 34 },
      "url": "https://example.com/BiggerVacuum-1.2.0.zip"
    }
  ]
}
```

Each entry takes these fields:

- **`files`:** paths inside `mods/plugins` that belong to the mod. The server adds each file's size and SHA-256, so
  the launcher installs exactly those files.
- **`patchers`:** optional paths inside `mods/patchers` for BepInEx preloader patchers. The launcher installs them
  into the game's `BepInEx/patchers`. A mod can have only patchers and no `files`.
- **`required`:** defaults to `true`. Set it to `false` for mods players may skip.
- **`nexus`:** the numbers from the mod's Nexus Mods page,
  `nexusmods.com/moonsweepers/mods/<mod_id>?tab=files&file_id=<file_id>`. The launcher uses them to download the
  mod.
- **`url`:** an optional direct download, either a `.zip` or a `.dll`. The launcher downloads it with one click.
- **`link`:** an optional page where players get the mod. The launcher opens it and installs the file as soon as it
  lands in their Downloads folder.

Files in `mods/plugins` or `mods/patchers` that no entry lists count as server-only: players don't need them.

The ID API serves the manifest at `GET /mods`, and `GET /` includes a short `mods` summary. Type `mods` in the
console to check the manifest; problems such as a missing file are listed there as warnings.

## Connecting to the Server

### 1. Install the Client Module

From a clone of this repository, open PowerShell and run:

```powershell
.\client\install-moon-client.ps1 -ServerId SERVER_ID -Launch
```

Alternatively, run the installer directly from GitHub:

```powershell
& ([scriptblock]::Create((irm "https://raw.githubusercontent.com/ivanoliverfabra/moon-sweepers-pterodactyl/main/client/install-moon-client.ps1"))) -ServerId SERVER_ID -Launch
```

The installer downloads the client module from this repository's latest GitHub release (built by the `release-client` workflow when a `v*` tag is pushed; pass `-Tag vX.Y.Z` to pin one):

- `install-client.ps1`
- `launch-client.ps1`
- `version.dll`

It verifies the downloads against the release's `SHA256SUMS`, runs the upstream installer, and removes the downloaded files afterward.

**Security note:** Review the script before executing it remotely.

### 2. Obtain the Server ID

The server uses Steam P2P/relay networking rather than direct IP:port connections.

Obtain the current `server_steam_id` from the server administrator.

If the host keeps the **ID API** enabled (on by default), the current ID is also
available at `http://SERVER_ADDRESS:GAME_PORT/id` (plain text), where
`SERVER_ADDRESS:GAME_PORT` is the Pterodactyl allocation your client normally
connects through. See [ID API](#id-api) for all endpoints.

Use that ID when running the client installer.

### 3. Launch the Game

The installer can launch the game automatically using `-Launch`.

For the retail game, the desktop shortcut starts the game through Steam with the
matching AppId. For an online-fix client, launch `MoonSweepers.exe` directly with
`-moon-connect SERVER_ID` instead; keep `OnlineFix64.dll`, `winmm.dll`,
`SteamOverlay64.dll` and `OnlineFix.ini` beside the executable. The retail Steam
`-applaunch` shortcut is not used in SpaceWar mode.

You can also update an existing desktop shortcut without reinstalling the client module:

```powershell
.\client\install-moon-client.ps1 -Action Shortcut -ServerId SERVER_ID
```

### Updating the Server ID

The server ID may change whenever the server restarts.

If you can no longer connect:

1. Obtain the latest server ID from the administrator.
2. Update your desktop shortcut using `-Action Shortcut`.
3. Launch the game again.

## Retail vs online-fix (SpaceWar) mode

The server container supports both game variants. The host detects the uploaded
files on every start and keeps the imported game in sync with them.

| Uploaded server game | `SteamAppId` | `SteamGameId` | Players |
| --- | --- | --- | --- |
| Clean retail install | `5335510` | `5335510` | Licensed retail game |
| Online-fix patched install | `480` | `480` | Online-fix client using SpaceWar |

For an online-fix server, upload the whole patched game folder to
`game-source/`, including `OnlineFix.ini`, `OnlineFix64.dll`,
`SteamOverlay64.dll`, `winmm.dll`, `dlllist.txt` and the patched
`MoonSweepers_Data/Plugins/x86_64/steam_api64.dll`. Set both `SteamAppId` and
`SteamGameId` to `480` in the panel and restart. The host copies those files into
the imported game automatically. When switching back to a clean retail upload,
the host removes the stale online-fix files and restores the retail Steam API DLL.

In SpaceWar mode the server skips the Moon Sweepers ownership check because
SpaceWar is free and online-fix clients do not own the retail game. The join
password and optional SteamID allowlist still apply.

> **Disclaimer (educational purposes only):** The online-fix / SpaceWar mode in
> this repository exists for educational and research purposes only — it
> demonstrates how this server adapter interacts with the Steamworks API and
> how the host detects and syncs an alternative game file set. It is not
> intended to enable piracy, and it must not be used to play Moon Sweepers
> without owning it. **This repository does not provide, host, or link
> directly to any crack or online-fix files.** The file names are referenced
> for compatibility only; you must supply your own legally obtained game
> files and assume full responsibility for how you use them. If you do not
> own Moon Sweepers, buy a licensed copy and use retail mode instead.

## ID API

The game itself only listens on the UDP side of the game port, so the host also
serves a small HTTP API on the **TCP side of the primary game port**
(`http://SERVER_ADDRESS:GAME_PORT/`). Disable it by setting the `ID API`
variable to `0`.

| Endpoint | Response |
| --- | --- |
| `GET /` | Full server state JSON (same data as the console `status` command) plus heartbeat freshness. |
| `GET /players` | `{"count": 2, "players": ["Fabra", "Joonas"], "entries": [{"name": "Fabra", "steam_id": "76561198074424621"}, ...]}`: `players` is the display names, `entries` pairs each name with its SteamID64 (a string). `GET /` carries both: `players` (names) and `player_entries` (the same records as `entries`), so a launcher needs one request. |
| `GET /mods` | The mod manifest: `enabled` (`MOON_BEPINEX`), `hash` (changes whenever the files players need change), `mods` (each with `id`, `name`, `version`, `required`, `nexus`, `url`, and `files` and `patchers` with `path`, `size` and `sha256`), `server_only` files (each with its `folder`) and `errors` in `mods.json`. `GET /` carries a short `mods` summary (`enabled`, `count`, `required`, `hash`). See [Telling players which mods they need](#telling-players-which-mods-they-need). |
| `GET /id` | The current `server_steam_id` as plain text. Returns `503` with an error JSON until the server finishes booting. |
| `GET /health` | `ok`, for uptime checks. |
| `GET /icon` | `302` redirect to the hosted icon URL, when the **Server Icon URL** variable is set. |
| `GET /banner` | `302` redirect to the hosted banner URL, when the **Server Banner URL** variable is set. |
| `GET /motd` | The message of the day as plain text, when the **Server MOTD** variable is set. |

Server identity variables (all optional, all exposed in `GET /`):

| Variable | Purpose |
| --- | --- |
| `MOON_ICON` | URL of a hosted **1:1 (square) icon** image, e.g. `https://example.com/icon.png`. Served as `icon` in `GET /` and via `GET /icon`. |
| `MOON_BANNER` | URL of a hosted **16:9 banner** image, e.g. `https://example.com/banner.png`. Served as `banner` in `GET /` and via `GET /banner`. |
| `MOON_MOTD` | Free-text message of the day, e.g. `Welcome to our moon base!`. Served as `motd` in `GET /` and as plain text on `GET /motd`. |

The API never fetches the images itself — point the variables at URLs already
hosted somewhere public (an image host, GitHub, a CDN, or another Pterodactyl
allocation). Unconfigured variables come back as `null` in `GET /`, and their
dedicated endpoints return `404`. The icon/banner URLs are validated to be
`http(s)://` links at startup and are ignored (with a console warning)
otherwise.

Example `GET /` response:

```json
{
  "ready": true,
  "phase": "world-bound",
  "server_steam_id": "90296903580028958",
  "heartbeat_age_seconds": 0.2,
  "heartbeat_fresh": true,
  "icon": "https://example.com/icon.png",
  "banner": "https://example.com/banner.png",
  "motd": "Welcome to Fabra's moon base!"
}
```

## Important Information

- Retail players must own a licensed Steam copy of Moon Sweepers. Online-fix/SpaceWar players use their patched client instead.
- The client must use the supported game version: **1.0.5**.
- The matching client module is required.
- Direct IP:port connections are not supported.
- Server availability depends on the community-hosted server.
- The host runs the game under Wine **10 or newer** (WineHQ, bundled in the image); Unity 6000.6 does not boot under Wine 9.
- This is an unofficial, experimental project and is not endorsed by the game developers.