# setup-tools.ps1
# Installs what building the ResortRecomp toolkit needs, and fetches the two projects it builds on, at the
# versions it was tested with. Nothing here needs, or touches, the game.
#
#   MSYS2 (msys2.org), with its UCRT64 GCC toolchain, CMake, Ninja, git, and make    -> C:\msys64
#   DolRecomp, commit 02e27ac (GPL-3.0)                                               -> third_party\DolRecomp
#   aurora, commit f77492f (MIT)                                                      -> third_party\aurora
# Python 3 for Windows (python.org) must be on PATH for the small helper scripts.
#
# Usage: powershell -ExecutionPolicy Bypass -File scripts\setup-tools.ps1
param([string]$Msys = 'C:\msys64')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$third = Join-Path $root 'third_party'
New-Item -ItemType Directory -Force $third | Out-Null

if (-not (Get-Command python -ErrorAction SilentlyContinue)) { throw 'Python 3 is needed: install it from python.org, then run this again.' }
if (-not (Test-Path "$Msys\ucrt64.exe")) {
    winget install -e --id MSYS2.MSYS2 --source winget --silent --disable-interactivity --accept-package-agreements --accept-source-agreements
    if ($LASTEXITCODE -ne 0) { throw "winget couldn't install MSYS2 (exit code $LASTEXITCODE)" }
}
$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = '1'
function Invoke-Ucrt64([string]$Command) { & "$Msys\usr\bin\bash.exe" -lc $Command; if ($LASTEXITCODE -ne 0) { throw "Failed: $Command" } }
# The first update can replace MSYS2's core and exit, so it runs twice.
& "$Msys\usr\bin\bash.exe" -lc 'pacman -Syu --noconfirm'
Invoke-Ucrt64 'pacman -Syu --noconfirm'
Invoke-Ucrt64 'pacman -S --needed --noconfirm base-devel git mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-make'
Invoke-Ucrt64 'gcc --version | head -1; cmake --version | head -1; ninja --version; mingw32-make --version | head -1'

function Get-Pinned([string]$Url, [string]$Commit, [string]$Folder) {
    $path = Join-Path $third $Folder
    $unix = '/' + $path.Substring(0, 1).ToLower() + ($path.Substring(2) -replace '\\', '/')
    if (-not (Test-Path $path)) { Invoke-Ucrt64 "git clone --recursive '$Url' '$unix'" }
    Invoke-Ucrt64 "cd '$unix' && git fetch --quiet origin && git checkout --quiet $Commit && git submodule update --init --recursive --quiet"
    Write-Host "$Folder at $Commit"
}
Get-Pinned 'https://github.com/ExpansionPak/DolRecomp.git' '02e27ac' 'DolRecomp'
Get-Pinned 'https://github.com/encounter/aurora.git' 'f77492f' 'aurora'
Write-Host 'Ready: now run scripts\build-toolkit.ps1'
