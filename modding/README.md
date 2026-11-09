# Modding (BepInEx 6)

Moon Sweepers is built with Unity 6000.6 (IL2CPP metadata v108), and stock BepInEx can't load plugins on it yet:

- **Cpp2IL:** the copy bundled with BepInEx be.793 reads metadata only up to v106, so the interop assemblies are never generated.
- **Il2CppInterop:** 1.5.3, its newest release, uses an older layout of IL2CPP's class structure. Unity 6000.5 moved the fields; 6000.6 also merged the class's two interface arrays into one table. As a result, BepInEx fails at startup with `Base class Il2CppSystem.Object is value type`.

`build-bepinex.sh` builds an install that works around both problems.

## Build

```sh
modding/build-bepinex.sh "<game folder>" out/bepinex
```

Then copy the contents of `out/bepinex` into the game folder. The script needs bash, curl, git, unzip, sha256sum and the .NET 8 SDK. It works from Git Bash on Windows and in Linux CI.

The work also splits into two steps, which the server image uses:

- `build-bepinex.sh prepare <dir>` builds everything that doesn't need the game files: `<dir>/base` (BepInEx with the patched core), `<dir>/tools` (both generators) and `<dir>/unity-libs`. `Dockerfile.pterodactyl` runs this in its `bepinex` stage.
- `build-bepinex.sh interop <dir> <game folder> <output>` generates the interop assemblies for one `GameAssembly.dll`. It needs only the .NET 8 runtime. `container/server/bepinex-setup` runs it on the server's first start with `MOON_BEPINEX=1` and caches the result per `GameAssembly.dll` hash (see `PTERODACTYL.md`).

The script does the following:

1. Downloads these files, each pinned by SHA-256:
   - BepInEx 6.0.0-be.793 (IL2CPP, win-x64).
   - Unity's 6000.6.0 base libraries.
   - TerraFX.Interop.Windows, which the newer Il2CppInterop needs.
2. Builds Il2CppInterop from [BepInEx/Il2CppInterop#292](https://github.com/BepInEx/Il2CppInterop/pull/292), which adds Unity 6000.5 support. On top of it, the script applies `patches/il2cppinterop-unity-6000.6.patch`, which does three things:
   - Adds `NativeClassStructHandler_6000_6` with the 6000.6 class layout.
   - Teaches class injection to build the merged interface table.
   - Updates the debug dump.

   The script then swaps the result into `BepInEx/core`.
3. Generates the interop assemblies offline with two tools:
   - `dummygen` runs Cpp2IL 2022.1.0-development.1743, which reads v108.
   - `interopgen` runs the patched generator and writes the `assembly-hash.txt` that BepInEx checks.

   They are separate programs because each needs a different AsmResolver version.
4. Sets `UpdateInteropAssemblies = false`. A BepInEx regeneration attempt deletes `BepInEx/interop` before it runs its own Cpp2IL, and that Cpp2IL fails on this game, which would leave an install with no interop assemblies.

When the game updates, rerun the script. `GameAssembly.dll` changes, so the interop assemblies must be regenerated.

## How the 6000.6 layout was measured

The offsets come from the `il2cpp_class_*` exports and from `Class::SetupInterfaces` in the game's `GameAssembly.dll`. The header of `Class_6000_6.cs` in the patch lists them.

In the interface table, each entry is `{Il2CppClass* interface; int32 vtable offset; int32 depth}`:

- The first `interface_offsets_count` entries carry offsets. These are the only entries interface dispatch searches.
- `interfaces_count` is the total number of entries.
- `il2cpp_class_get_interfaces` returns the entries with depth 0, the interfaces a class implements directly.

## Test plugin

`test-plugin` checks every part of the loader:

- It injects a `MonoBehaviour` subclass and checks that `Start` and `Update` run.
- It runs a managed coroutine through `WrapToIl2Cpp`. That injects an `IEnumerator` implementation that Unity calls through the interface table.
- It applies a Harmony prefix to `MoonSteam.Update`.

Build and install it:

```sh
dotnet build modding/test-plugin -c Release -p:GameDir="<game folder with BepInEx installed>"
cp modding/test-plugin/bin/Release/net6.0/MoonModTest.dll "<game folder>/BepInEx/plugins/"
```

`BepInEx/LogOutput.log` should then end with `Coroutine finished (interface dispatch works)` and `Harmony prefix on MoonSteam.Update ran`.

When writing plugins, don't patch auto-property getters. IL2CPP inlines them into their callers, so a Harmony patch on one never runs.

## Status

- **Windows client:** works (batch mode tested).
- **Dedicated server under Wine:** works in the Pterodactyl image with `MOON_BEPINEX=1`. Tested with WineHQ stable and .NET 6.0.7 (BepInEx's bundled runtime). On the first start, interop generation takes about 30 s.
- **Upstream:** the 6000.6 patch is not submitted yet.
