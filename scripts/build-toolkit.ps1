# build-toolkit.ps1
# Builds the ResortRecomp toolkit from source and zips it: exports\ResortRecomp-<version>.zip, what a GitHub
# release offers. Run scripts\setup-tools.ps1 once first. The game is never needed or used here.
#
#   1. the console-settings template the launcher gives new saves (from public formats)  45_make_nand.py
#   2. aurora, with this project's patches                                               48_build_aurora.ps1
#   3. the launcher, the pairing tool, and the graphics glue                             41_build_native.ps1
#   4. the toolkit: DolRecomp, the runtime as source, the compiler, licences              66_make_toolkit.ps1
#
# Usage: powershell -ExecutionPolicy Bypass -File scripts\build-toolkit.ps1 -Version 1.0.0
param([Parameter(Mandatory = $true)][string]$Version)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
& python scripts\45_make_nand.py
if ($LASTEXITCODE -ne 0) { throw 'The settings template was not made' }
& powershell -ExecutionPolicy Bypass -File scripts\48_build_aurora.ps1
if ($LASTEXITCODE -ne 0) { throw 'aurora did not build: see analysis\native\aurora-build.log' }
& powershell -ExecutionPolicy Bypass -File scripts\41_build_native.ps1 -Target toolkit-parts
if ($LASTEXITCODE -ne 0) { throw 'The launcher did not build: see analysis\recomp\native-build.log' }
& powershell -ExecutionPolicy Bypass -File scripts\66_make_toolkit.ps1 -Version $Version -Zip
if ($LASTEXITCODE -ne 0) { throw 'The toolkit was not made' }
