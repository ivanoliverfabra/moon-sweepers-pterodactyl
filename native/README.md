# Moon Sweepers native server and client module

This Windows x64 `version.dll` forwards the system VERSION API and, when enabled,
adapts the owned game to an anonymous Steam GameServer host or direct-connect
client. It contains our adapter code and MinHook, without game binaries, accounts,
or copied game source. Runtime methods and fields are resolved through IL2CPP.

Supported game: Moon Sweepers 1.0.5, Steam build 26317497, Unity 6000.6.0f1.
The import and client installation tools reject a different GameAssembly hash.
Both players still need their own Steam accounts and game licenses. The server
uses Valve's anonymous GameServer login and consumes no playing account.

Build from the repository root with Linux Docker:

```sh
docker build -f native/Dockerfile.build --output native/out native
```

MinHook is pinned to commit `c3fcafdc10146beb5919319d0683e44e3c30d537`.
Its license is exported beside the DLL. The image build compiles the same sources.
Never replace a different mod's `version.dll`; the client installer checks this.

Server opt-in is `MOON_DEDICATED=1` or `-moon-dedicated`. The Docker runtime also
sets `MOON_HOST=1` and uses `-batchmode -nographics -moon-headless`.
CPU progress, world simulation, and saves remain active. GPU-only progress texture
operations and menu rendering are suppressed. No GPU device is mounted.

Client opt-in is `-moon-connect SERVER_STEAM_ID` (or `MOON_CONNECT`). Launch through
Steam normally. Client graphics and normal Steam authentication remain enabled.
The adapter obtains a genuine session ticket, and the server requires successful
Steam ticket validation and a Moon Sweepers license before admitting the player.
`MOON_SERVER_PASSWORD` supplies the required shared join password; clients prompt
for it before requesting admission. Passwords stay out of logs and launch arguments.
An empty `MOON_ALLOWED_STEAM_IDS` allows licensed players with the password; a
nonempty value adds a player filter. Server and clients must use the same release
(protocol v3: clients relay their astronaut look through the server). The server still
accepts protocol v2 clients, which keep working but do not share faces.

`SteamAppId`/`SteamGameId` select the Steam AppID the server runs under (default
5335510). With any other value (for example 480, SpaceWar) the Moon Sweepers
license check is skipped; the password and allowlist still apply.

The adapter tunes Steam networking globally at start-up: the server gets the same
4 MB send buffer and 1-8 MB/s send rate the game sets on clients, Nagle disabled,
and both sides allow 20 s (instead of 10 s) for the initial relay handshake.

The dedicated headless server runs its own tick (`server_tick.c`). The game budgets its work per frame for a client
that must keep drawing (48 cleaning strokes and `MoonSettings.cleaningWorkBudgetMs` per frame, 3 far robots per frame,
1 ms of dust serving per frame), and the server used to run those budgets at a fixed 30 frames/s. Now, while
`MoonWorld`'s stroke queue holds more than a frame's work, the server raises `Application.targetFrameRate` from
`MOON_TICK_RATE` (default 30) to `MOON_TICK_RATE_MAX` (default 120) and calls the game's `ProcessToolStrokes` again
within the frame until the queue is empty, 40% of the frame's time is spent, or a pass makes no progress. It returns
to the base rate after 5 quiet seconds. `Graphics.CopyTexture` and `Material.SetPass` become no-ops, since without a
GPU they only log errors with stack traces. The state JSON's `tick` object and the `perf` command report ticks per
second, frame and `MoonWorld.Update` times, and the backlog; the log says when the server falls behind and when it
catches up. `MOON_CATCHUP=0` keeps the fixed rate.

The host defaults to save slot 1; `MOON_SLOT` or `-moon-slot` selects 1-5.
The ordinary game save directory is retained within the persistent Wine prefix.
The module reports a world ready only after CPU initialization has completed and
the crew session is bound to that same world.

`MOON_HOST_LOG`, `MOON_HOST_STATE`, and `MOON_HOST_COMMAND` select the log, atomic
JSON heartbeat, and command-file paths. Default filenames beside the game are
`moon-host.log`, `moon-host-state.json`, and `moon-host-command.txt`.
A command file containing `save` invokes the game's complete-strokes save and
waits for its task. `stop` saves first, shuts down networking, and requests the
game's normal quit. Save failure/cancellation/90-second timeout is reported.
Docker's runner allows additional time for process exit before forced cleanup.

The normal game autosave remains enabled. Back up the persistent volume while
stopped. A health check verifies both ready state and a recent heartbeat; it does
not establish that a remote player has joined. See the root installation guide
for the current multiplayer verification status.

With no opt-in, or in processes other than MoonSweepers.exe, the DLL only forwards
VERSION exports. Removing an owned installation disables the module. This is an
unofficial experimental adapter and must be revalidated after a game update.
