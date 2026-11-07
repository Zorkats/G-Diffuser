<#
.SYNOPSIS
    Assemble a Windows release zip from a finished build tree.

.DESCRIPTION
    The Windows counterpart of tools/package_linux.sh, and deliberately the same shape. The
    payload is listed by name rather than copied with a wildcard, so a release that quietly lost
    decomp-recipes/ fails here instead of on a stranger's machine. Nothing is written until
    check_windows_deps.py confirms the executables need no DLL beside them, which is what tells
    the static build tree apart from the dynamic one.

    With -Companion, the G-Diffuser Companion is staged into G-Diffuser/companion/: its executable,
    music_targets.json, and the frozen worker folder (tools/build_companion_worker.py must have run,
    then a build of G-Diffuser-Companion-worker). The staged copy must pass its own --selftest. User
    themes in the build tree are test data and are not shipped. Without -Companion the package is
    unchanged, so the release workflow, which does not freeze the worker, keeps working.

.EXAMPLE
    pwsh tools/package_windows.ps1 -BuildDir build/x64 -Version 1.0.1

.EXAMPLE
    pwsh tools/package_windows.ps1 -BuildDir build/x64 -Version 1.0.1 -Companion
#>

param(
    [Parameter(Mandatory = $true)][string] $BuildDir,
    [Parameter(Mandatory = $true)][string] $Version,
    [string] $OutDir = "dist",
    [switch] $Companion
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

# The Visual Studio generator is multi-config, so the executables land under a config subfolder
# rather than directly in the build tree.
$binDir = Join-Path $BuildDir "port\Release"
if (-not (Test-Path (Join-Path $binDir "G-Diffuser.exe"))) {
    Write-Error "no G-Diffuser.exe in $binDir`nbuild first: cmake --build $BuildDir --config Release --target G-Diffuser"
}

# The zip's top folder is unversioned. Saves live next to the executable, so extracting an update
# over the previous install keeps them; a versioned folder would start every update without saves
# (GitHub #41). The staging root stays versioned so packages of different versions never collide.
$stageRoot = Join-Path $OutDir "G-Diffuser-v$Version-windows-x64"
if (Test-Path $stageRoot) { Remove-Item -Recurse -Force $stageRoot }
$stage = Join-Path $stageRoot "G-Diffuser"
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# Everything a fresh install needs. gdx-extract and decomp-recipes/ are what turn the user's ROM
# into generic.o2r on first boot; without them the game starts and then has no assets. gdx-update is
# the helper that replaces the build later, so it has to ship with the thing it updates.
$payload = @(
    "G-Diffuser.exe",
    "gdx-extract.exe",
    "gdx-update.exe",
    "gdiffuser.o2r",
    "gamecontrollerdb.txt",
    "fonts",
    "decomp-recipes",
    "shaders",
    "LICENSE",
    "THIRD_PARTY_NOTICES.md",
    "LICENSES"
)
foreach ($item in $payload) {
    $source = Join-Path $binDir $item
    if (-not (Test-Path $source)) {
        Write-Error "build tree is missing $item"
    }
    Copy-Item -Recurse -Force $source (Join-Path $stage $item)
}

python (Join-Path $repoRoot "tools\check_windows_deps.py") `
    (Join-Path $stage "G-Diffuser.exe") (Join-Path $stage "gdx-extract.exe") (Join-Path $stage "gdx-update.exe")
if ($LASTEXITCODE -ne 0) {
    Write-Error "packaging refused: the executables are not self-contained"
}

if ($Companion) {
    # The Companion finds the game in the folder above its own, so it lives in a companion subfolder.
    $companionBin = Join-Path $BuildDir "companion\Release"
    $companionStage = Join-Path $stage "companion"
    New-Item -ItemType Directory -Force -Path $companionStage | Out-Null
    $companionPayload = @(
        "G-Diffuser-Companion.exe",
        "music_targets.json",
        "worker"
    )
    foreach ($item in $companionPayload) {
        $source = Join-Path $companionBin $item
        if (-not (Test-Path $source)) {
            Write-Error "companion build is missing $item`nfreeze the worker: python -I tools/build_companion_worker.py, then cmake --build $BuildDir --config Release --target G-Diffuser-Companion-worker"
        }
        Copy-Item -Recurse -Force $source (Join-Path $companionStage $item)
    }
    foreach ($license in @("MIT-CMU-Pillow.txt", "PSF-2.0-Python.txt")) {
        if (-not (Test-Path (Join-Path $companionStage "worker\_internal\LICENSES\$license"))) {
            Write-Error "packaging refused: the frozen worker has no $license; freeze it again"
        }
    }

    python (Join-Path $repoRoot "tools\check_windows_deps.py") (Join-Path $companionStage "G-Diffuser-Companion.exe")
    if ($LASTEXITCODE -ne 0) {
        Write-Error "packaging refused: G-Diffuser-Companion.exe is not self-contained"
    }
    # The staged copy runs its worker handshake, which also proves the worker folder is complete.
    $selftest = & (Join-Path $companionStage "G-Diffuser-Companion.exe") --selftest | Out-String
    if ($LASTEXITCODE -ne 0 -or $selftest -notmatch "selftest: PASS") {
        Write-Error "packaging refused: the staged Companion failed its selftest`n$selftest"
    }
}

$zip = Join-Path $OutDir "G-Diffuser-v$Version-windows-x64.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip

Write-Host ""
Write-Host $zip

# Computed through .NET rather than Get-FileHash, which is missing from some constrained hosts.
$stream = [System.IO.File]::OpenRead((Resolve-Path $zip))
try {
    $digest = [System.Security.Cryptography.SHA256]::Create().ComputeHash($stream)
    ([BitConverter]::ToString($digest) -replace '-', '').ToLower()
} finally {
    $stream.Dispose()
}
