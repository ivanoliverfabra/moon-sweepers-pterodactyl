#!/usr/bin/env bash
# Builds a BepInEx 6 install that works on Moon Sweepers (Unity 6000.6, IL2CPP metadata v108).
#
# Stock BepInEx be.793 cannot start on this game: its Cpp2IL stops at metadata v106, and Il2CppInterop 1.5.3
# does not know the Unity 6000.5+/6000.6 Il2CppClass layout. This script:
#   1. builds Il2CppInterop from the Unity 6000.5 pull request (BepInEx/Il2CppInterop#292) plus our 6000.6
#      patch (patches/il2cppinterop-unity-6000.6.patch) and swaps it into BepInEx's core folder,
#   2. generates the interop assemblies offline (dummygen: Cpp2IL development.1743, which reads v108;
#      interopgen: the patched generator), so BepInEx never tries to run its own Cpp2IL,
#   3. pins UpdateInteropAssemblies = false, because a regeneration attempt deletes the interop folder first and
#      then fails with the bundled Cpp2IL.
#
# Usage:
#   build-bepinex.sh <game folder> <output folder>    everything; copy the output's contents over the game
#   build-bepinex.sh prepare <dir>                    the parts that need no game files: <dir>/base (BepInEx
#                                                     with the patched core), <dir>/tools, <dir>/unity-libs
#   build-bepinex.sh interop <dir> <game> <output>    interop assemblies for one GameAssembly.dll, using a
#                                                     prepared <dir>; needs only the .NET 8 runtime
#   build-bepinex.sh bundle <zip>                     the launcher's download: a prepared folder whose generators
#                                                     are self-contained Windows programs (tools/*/<name>.exe),
#                                                     plus bundle.json; the launcher runs the interop step itself
# Needs: bash, curl, git, unzip, sha256sum, .NET 8 SDK (the interop step alone needs only the runtime).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
work="$here/.work"

bepinex_url="https://builds.bepinex.dev/projects/bepinex_be/793/BepInEx-Unity.IL2CPP-win-x64-6.0.0-be.793%2Bdbc82f0.zip"
bepinex_sha=5b63d7d0e92a2448ccd13b738ff95ff1cc1026c6d718555d4df8e6c824953ac4
unity_version=6000.6.0f1
unity_libs_url="https://unity.bepinex.dev/libraries/6000.6.0.zip"
unity_libs_sha=080addbfee6513c7a4b54dc8de9d83e34ca372e498ddff7e7d38d1e13dfe0e70
terrafx_url="https://api.nuget.org/v3-flatcontainer/terrafx.interop.windows/10.0.22621.2/terrafx.interop.windows.10.0.22621.2.nupkg"
terrafx_sha=34bd80810e9ea4728830d0565c9603ac62912c97f4401b9ca6945233974cfe9b
interop_repo="https://github.com/RTBRuhan/Il2CppInterop.git"
interop_commit=72a5fd3cefbc580e1311d2e8242673e23e432023

fetch() { # url sha256 destination
  if [ ! -f "$3" ] || ! echo "$2  $3" | sha256sum -c --status; then
    echo "Downloading $1"
    curl -fsSL --retry 3 -o "$3.part" "$1"
    echo "$2  $3.part" | sha256sum -c --status || { echo "Checksum mismatch for $1" >&2; exit 1; }
    mv "$3.part" "$3"
  fi
}

prepare() { # dir
  local dir=$1
  mkdir -p "$work" "$dir"
  fetch "$bepinex_url" "$bepinex_sha" "$work/bepinex.zip"
  fetch "$unity_libs_url" "$unity_libs_sha" "$work/unity-libs.zip"
  fetch "$terrafx_url" "$terrafx_sha" "$work/terrafx.nupkg"

  echo "Building Il2CppInterop with the Unity 6000.6 patch"
  if [ ! -d "$work/Il2CppInterop/.git" ]; then
    git clone --quiet "$interop_repo" "$work/Il2CppInterop"
  fi
  git -C "$work/Il2CppInterop" checkout --quiet --force "$interop_commit"
  git -C "$work/Il2CppInterop" clean --quiet -fdx -e bin -e obj
  git -C "$work/Il2CppInterop" apply "$here/patches/il2cppinterop-unity-6000.6.patch"
  for p in Il2CppInterop.Runtime Il2CppInterop.HarmonySupport Il2CppInterop.Generator; do
    dotnet build "$work/Il2CppInterop/$p/$p.csproj" -c Release -v quiet -nologo
  done

  echo "Publishing the interop generators"
  rm -rf "$dir/tools"
  # TOOLS_RID (e.g. win-x64) publishes them self-contained, for machines without .NET.
  local publish=(-c Release -v quiet -nologo)
  if [ -n "${TOOLS_RID:-}" ]; then publish+=(-r "$TOOLS_RID" --self-contained true); fi
  dotnet publish "$here/dummygen" "${publish[@]}" -o "$dir/tools/dummygen"
  dotnet publish "$here/interopgen" "${publish[@]}" -p:Il2CppInteropDir="$work/Il2CppInterop" \
    -o "$dir/tools/interopgen"
  rm -rf "$dir/unity-libs" && mkdir -p "$dir/unity-libs" && unzip -q -o "$work/unity-libs.zip" -d "$dir/unity-libs"

  echo "Assembling BepInEx"
  rm -rf "$dir/base" && mkdir -p "$dir/base"
  unzip -q -o "$work/bepinex.zip" -d "$dir/base"
  local bin="$work/Il2CppInterop/bin"
  cp "$bin/Il2CppInterop.Runtime/net6.0/Il2CppInterop.Runtime.dll" \
     "$bin/Il2CppInterop.Runtime/net6.0/Il2CppInterop.Common.dll" \
     "$bin/Il2CppInterop.HarmonySupport/net6.0/Il2CppInterop.HarmonySupport.dll" \
     "$bin/Il2CppInterop.Generator/netstandard2.1/Il2CppInterop.Generator.dll" "$dir/base/BepInEx/core/"
  rm -rf "$work/terrafx" && unzip -q -o "$work/terrafx.nupkg" -d "$work/terrafx"
  cp "$work/terrafx/lib/net6.0/TerraFX.Interop.Windows.dll" "$dir/base/BepInEx/core/"
  mkdir -p "$dir/base/BepInEx/plugins" "$dir/base/BepInEx/patchers" "$dir/base/BepInEx/config"
  pin_config "$dir/base/BepInEx/config/BepInEx.cfg"
}

pin_config() { # BepInEx.cfg
  if [ -f "$1" ] && grep -q '^UpdateInteropAssemblies' "$1"; then
    sed -i 's/^UpdateInteropAssemblies = .*/UpdateInteropAssemblies = false/' "$1"
  else
    printf '[IL2CPP]\n\n## Regenerating needs a newer Cpp2IL than BepInEx bundles; rebuild with modding/build-bepinex.sh instead.\nUpdateInteropAssemblies = false\n' >> "$1"
  fi
}

interop() { # prepared dir, game folder, output folder
  local dir=$1 game=$2 out=$3
  for f in GameAssembly.dll MoonSweepers_Data/il2cpp_data/Metadata/global-metadata.dat; do
    [ -f "$game/$f" ] || { echo "Not a Moon Sweepers folder: $game/$f is missing" >&2; exit 1; }
  done
  local scratch
  scratch=$(mktemp -d)
  trap 'rm -rf "$scratch"' RETURN
  dotnet "$dir/tools/dummygen/dummygen.dll" "$game/GameAssembly.dll" \
    "$game/MoonSweepers_Data/il2cpp_data/Metadata/global-metadata.dat" "$unity_version" "$scratch/dummy"
  dotnet "$dir/tools/interopgen/interopgen.dll" "$scratch/dummy" "$game/GameAssembly.dll" "$scratch/interop" \
    "$dir/unity-libs"
  rm -rf "$out" && mkdir -p "$(dirname "$out")" && mv "$scratch/interop" "$out"
}

bundle() { # zip
  local zip=$1 dir="$work/bundle"
  TOOLS_RID=win-x64 prepare "$dir"
  cat >"$dir/bundle.json" <<JSON
{
  "format": 1,
  "bepinex": "6.0.0-be.793",
  "unity_version": "$unity_version",
  "tools": "win-x64"
}
JSON
  mkdir -p "$(dirname "$zip")"
  "$(command -v python3 || command -v python)" - "$dir" "$zip" <<'PY'
import os, sys, zipfile
root, target = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
    for folder, _dirs, files in os.walk(root):
        for name in files:
            full = os.path.join(folder, name)
            archive.write(full, os.path.relpath(full, root).replace(os.sep, "/"))
PY
  echo "Wrote $zip"
}

case "${1:-}" in
  prepare) prepare "$2" ;;
  bundle) bundle "$2" ;;
  interop) interop "$2" "$3" "$4" ;;
  "") echo "Usage: $0 <game folder> <output folder> | prepare <dir> | interop <dir> <game> <output> | bundle <zip>" >&2; exit 2 ;;
  *)
    game=$(cd "$1" && pwd)
    out=$2
    prepare "$work/prepared"
    interop "$work/prepared" "$game" "$work/interop"
    echo "Assembling $out"
    mkdir -p "$out"
    cp -r "$work/prepared/base/." "$out/"
    rm -rf "$out/BepInEx/interop" && cp -r "$work/interop" "$out/BepInEx/interop"
    pin_config "$out/BepInEx/config/BepInEx.cfg"
    echo "Done: copy the contents of $out into the game folder."
    ;;
esac
