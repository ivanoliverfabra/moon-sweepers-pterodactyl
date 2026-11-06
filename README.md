# Moon Sweepers — Pterodactyl / Calagopus

Unofficial experimental adapter. This is **not** an official Moon Sweepers dedicated server. The upstream project currently targets Moon Sweepers 1.0.5 / Steam build 26317497 and checks the exact GameAssembly hash and IL2CPP metadata version. Check the upstream README before updating or deploying.

## What is included

- `egg-moon-sweepers.json`: import into the Pterodactyl/Calagopus panel.
- `Dockerfile`: builds a GHCR image with the upstream Wine runtime and native client/server module.
- `ptero/install.sh`: egg install script; creates persistent directories.
- `ptero/entrypoint.sh`: image entrypoint Wings/Calagopus runs; maps the panel's unmapped uid/gid to a name (nss_wrapper) and expands the egg's `STARTUP`.
- `ptero/supervisor.py`: console command bridge and signal forwarding.
- `.github/workflows/publish.yml`: publishes `ghcr.io/<owner>/<repo>:latest`.

The Docker build fetches upstream source at commit `5132f8a5e755da84b3eb62279f32c951badf972d`, and pins MinHook. It does not bundle Moon Sweepers game files.

## 1. Publish the image

1. Create a GitHub repository and upload this bundle's contents.
2. Ensure GitHub Actions is enabled and the workflow has package write permission.
3. Push to `main` or run the workflow manually.
4. In GitHub Packages, make the resulting container package public (or configure private-registry credentials on the Calagopus node).
5. The egg JSON uses `ghcr.io/OWNER/REPOSITORY:latest`. Replace that placeholder with your actual lowercase GHCR image path before importing.

The image is `linux/amd64`. The build compiles the upstream `version.dll` and Steam probe and installs Wine/Xvfb/runtime dependencies.

## 2. Import the egg

In the panel's Admin area, import `egg-moon-sweepers.json`. Edit its Docker image entry to your published GHCR path if needed. Create a server using this egg.

No game ports need to be allocated. This adapter joins through Steam P2P/relay and does not implement direct IP:port joining. The game/query port variables configure the Steam GameServer API internally only.

## 3. Upload the owned game files

After the server is created, use SFTP or the panel file manager to upload the **contents** of your own Windows Steam installation into:

`/home/container/game-source/`

The folder should directly contain at least:

- `MoonSweepers.exe`
- `GameAssembly.dll`
- `UnityPlayer.dll`
- `MoonSweepers_Data/`

Do not place the files one directory deeper (for example `game-source/Moon Sweepers/MoonSweepers.exe`). The upstream importer also checks other required game files and exact build metadata.

Supported upstream build: Moon Sweepers 1.0.5 / Steam build 26317497. The importer rejects incompatible builds rather than silently running an incompatible native module.

## 4. Configure variables

- `MOON_SERVER_PASSWORD`: required, 8–128 printable ASCII characters.
- `MOON_ALLOWED_STEAM_IDS`: optional comma-separated player SteamID64 allowlist (1–15 distinct IDs).
- `MOON_SLOT`: save slot 1–5.
- `MOON_GAME_PORT`, `MOON_QUERY_PORT`: distinct values 1024–65535; internal Steam API settings, not public allocations.
- `MOON_BACKUP_KEEP`: 1–100 confirmed shutdown backups; default 10.
- `TZ`: timezone for logs/backup timestamps.

Players still need their own licensed Steam accounts and the matching client module. The server ID is anonymous and may change after a restart.

## 5. Console and shutdown

The Pterodactyl console supports:

- `status` — prints native state and current `server_steam_id`.
- `save` — queues an in-game save; inspect status/logs for completion.
- `stop` — requests graceful shutdown.
- `help`

Use the panel Stop button. The egg sends Ctrl-C (`SIGINT`) to the supervisor, which forwards it to the upstream runner. The runner requests a native save/quit, waits up to 110 seconds, and creates a backup only if the final save is confirmed. Configure the Wings/Pterodactyl stop grace period to **at least 150 seconds** (180 seconds recommended) so Wings does not force-kill the container before the upstream save/backup finishes.

Backups are stored in `/home/container/backups/`. Download them with SFTP or the panel file manager. The complete persistent state (imported game, Wine prefix, logs, control files and backups) lives under `/home/container`. Do not reinstall with a wipe/delete-data option if you need the world.

## Important limitations

- Experimental, unofficial and not developer-supported.
- The upstream project documents a successful single licensed client join and world save, but two simultaneous players have not been verified.
- The server uses Steam relay networking; no inbound allocation or IP:port join is provided.
- The SteamID can change after restart. Run `status` again and update each client's connection setting.
- Revalidate after every game update. Do not overwrite an existing unrelated `version.dll` on client PCs.

## Troubleshooting

- `groups: cannot find name for group ID 988` followed by nothing: the image was started without a Pterodactyl-style entrypoint, so the daemon's unmapped uid/gid dropped into an interactive `bash`. Rebuild and pull the current image (`ENTRYPOINT ["/usr/bin/tini", "--"]`, `CMD ["/entrypoint.sh"]`), then reinstall/restart. If the Calagopus node caches images, pull `:latest` again.
- The console should print `container@moon-sweepers~ python3 /opt/moon/ptero/supervisor.py` right after "Server marked as Starting...". If it does not, the node is running an old image.
