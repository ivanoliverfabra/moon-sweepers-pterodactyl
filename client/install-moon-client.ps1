<#
.SYNOPSIS
  Installs the Moon Sweepers dedicated-server client module, downloading only
  the files it needs.

.DESCRIPTION
  Upstream's install-client.ps1 only works from inside the unpacked release
  because it reads native\out\version.dll and scripts\launch-client.ps1 next to
  itself. This wrapper downloads the release ZIP, extracts just those files
  (plus install-client.ps1), checks them against the SHA256SUMS manifest inside
  the ZIP, runs upstream's installer, then deletes the download.

  The installed launcher and desktop shortcut live in your game folder, so
  nothing from the download is needed afterwards.

.EXAMPLE
  .\install-moon-client.ps1 -ServerId 90296897008781313 -Launch

.EXAMPLE
  # One-liner (passes parameters through a script block)
  & ([scriptblock]::Create((irm <raw url of this file>))) -ServerId 90296897008781313 -Launch

.EXAMPLE
  # Server restarted and its Steam ID changed
  .\install-moon-client.ps1 -Action Shortcut -ServerId NEW_ID

.PARAMETER GamePath
  Moon Sweepers install folder. Auto-detected from your Steam libraries if omitted.
.PARAMETER ServerId
  Current server SteamID64 (17 digits), from the panel console's `status` command.
.PARAMETER Action
  Install (default), Launch, Shortcut or Uninstall; same meanings as upstream.
.PARAMETER Tag
  Upstream release tag to download. Must match the server image's upstream commit.
.PARAMETER ZipUrl
  Override the download URL (for a mirror or a different asset name).
.PARAMETER ZipPath
  Use an already-downloaded release ZIP instead of downloading.
.PARAMETER ZipSha256
  Optional SHA-256 of the whole ZIP; the script refuses to continue on mismatch.
#>
[CmdletBinding()]
param(
    [string]$GamePath = '',
    [string]$ServerId = '',
    [ValidateSet('Install', 'Launch', 'Shortcut', 'Uninstall')][string]$Action = 'Install',
    [switch]$Launch,
    [switch]$NoDesktopShortcut,
    [string]$Tag = 'v0.1.0-alpha.2',
    [string]$ZipUrl = '',
    [string]$ZipPath = '',
    [string]$ZipSha256 = '',
    [switch]$KeepDownload
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is very slow in Windows PowerShell with progress on
Add-Type -AssemblyName System.IO.Compression.FileSystem

# Only these entries are taken from the ZIP. Paths are relative to its single root folder.
$needed = @('scripts/install-client.ps1', 'scripts/launch-client.ps1', 'native/out/version.dll')

function Find-GamePath {
    $steamRoots = @()
    $registry = Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue
    if ($registry -and $registry.SteamPath) { $steamRoots += $registry.SteamPath }
    if (${env:ProgramFiles(x86)}) { $steamRoots += (Join-Path ${env:ProgramFiles(x86)} 'Steam') }

    $libraries = @()
    foreach ($root in ($steamRoots | Select-Object -Unique)) {
        $libraries += $root
        $vdf = Join-Path $root 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf -PathType Leaf) {
            foreach ($match in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libraries += $match.Groups[1].Value.Replace('\\', '\')
            }
        }
    }
    foreach ($library in ($libraries | Select-Object -Unique)) {
        $candidate = Join-Path $library 'steamapps\common\Moon Sweepers'
        if (Test-Path -LiteralPath (Join-Path $candidate 'MoonSweepers.exe') -PathType Leaf) { return $candidate }
    }
    return $null
}

if ($ServerId -and $ServerId -notmatch '^\d{17}$') {
    throw "ServerId must be the 17-digit SteamID64 printed by the panel's 'status' command (got '$ServerId')."
}

if (-not $GamePath) {
    $GamePath = Find-GamePath
    if (-not $GamePath) {
        throw 'Could not find Moon Sweepers in your Steam libraries. Pass -GamePath "D:\SteamLibrary\steamapps\common\Moon Sweepers".'
    }
    Write-Output "Found Moon Sweepers at: $GamePath"
}

$work = Join-Path ([IO.Path]::GetTempPath()) ('moon-sweepers-client-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null

try {
    # 1. Get the release ZIP.
    if ($ZipPath) {
        $zip = (Resolve-Path -LiteralPath $ZipPath).Path
    } else {
        if (-not $ZipUrl) {
            $ZipUrl = "https://github.com/hexonMD/moon-sweepers-dedicated-server/releases/download/$Tag/moon-sweepers-dedicated-server.zip"
        }
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $zip = Join-Path $work 'release.zip'
        Write-Output "Downloading $ZipUrl"
        try {
            Invoke-WebRequest -Uri $ZipUrl -OutFile $zip -UseBasicParsing
        } catch {
            throw "Download failed: $($_.Exception.Message)`nDownload the client/server ZIP from the upstream Releases page yourself and re-run with -ZipPath, or pass the correct -ZipUrl."
        }
    }
    $zipHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
    Write-Output "Release ZIP SHA-256: $zipHash"
    if ($ZipSha256 -and ($zipHash -ne $ZipSha256.Trim())) {
        throw "ZIP SHA-256 does not match -ZipSha256 (expected $($ZipSha256.Trim()))."
    }

    # 2. Extract only the needed files, plus the manifest used to check them.
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $entries = @{}
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName.Replace('\', '/')
            if ($name -match '^[^/]+/(.+)$') { $entries[$Matches[1]] = $entry }
        }
        foreach ($name in ($needed + 'SHA256SUMS')) {
            if (-not $entries.ContainsKey($name)) { throw "The release ZIP does not contain '$name'. Wrong asset, or a different release layout." }
            $destination = Join-Path $work ($name.Replace('/', [IO.Path]::DirectorySeparatorChar))
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entries[$name], $destination, $true)
        }
    } finally {
        $archive.Dispose()
    }

    # 3. Check the extracted files against the manifest shipped in the ZIP.
    # This detects a damaged or inconsistent download; it does not prove who built the ZIP.
    $manifest = @{}
    foreach ($line in (Get-Content -LiteralPath (Join-Path $work 'SHA256SUMS'))) {
        if ($line -match '^([0-9a-fA-F]{64})\s+\*?(.+)$') { $manifest[$Matches[2].Trim()] = $Matches[1].ToUpperInvariant() }
    }
    foreach ($name in $needed) {
        $path = Join-Path $work ($name.Replace('/', [IO.Path]::DirectorySeparatorChar))
        if (-not $manifest.ContainsKey($name)) { throw "SHA256SUMS in the ZIP has no entry for '$name'." }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $manifest[$name]) {
            throw "Checksum mismatch for '$name'. Refusing to install; re-download the release."
        }
    }
    Write-Output 'Extracted and verified: install-client.ps1, launch-client.ps1, version.dll'

    # 4. Run upstream's installer from the extracted layout it expects.
    # Upstream resolves its files from $PSScriptRoot and '..\native\out\version.dll'.
    # On Windows both separators work; the Join-Path call there builds the backslash form.
    $installer = Join-Path $work 'scripts\install-client.ps1'
    if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) {
        $installer = Join-Path $work 'scripts/install-client.ps1'
    }
    $arguments = @{ Action = $Action; GamePath = $GamePath }
    if ($ServerId) { $arguments.ServerId = $ServerId }
    if ($Launch) { $arguments.Launch = $true }
    if ($NoDesktopShortcut) { $arguments.NoDesktopShortcut = $true }
    & $installer @arguments
} finally {
    if ($KeepDownload) {
        Write-Output "Kept download at: $work"
    } else {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}
