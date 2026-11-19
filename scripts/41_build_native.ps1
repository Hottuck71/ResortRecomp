# 41_build_native.ps1
# Builds the program that runs the recompiled game with no Dolphin code.
# Output goes to exports\native-build\. It contains translated game code, so don't share it.
#
# Usage: powershell -ExecutionPolicy Bypass -File scripts\41_build_native.ps1 [-Target chunks] [-Jobs 20]

param(
    [string]$Target = 'all',
    [int]$Jobs = 20,
    [string]$Msys = 'C:\msys64'
)

$root = Split-Path $PSScriptRoot -Parent
$log = Join-Path $root 'analysis\recomp\native-build.log'
New-Item -ItemType Directory -Force (Split-Path $log -Parent) | Out-Null
# The game itself needs this project's translation of main.dol; the launcher and the toolkit's parts don't.
if ($Target -notin @('launcher', 'pair', 'toolkit-parts')) {
    & python (Join-Path $PSScriptRoot '40_make_cpu_core.py')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & python (Join-Path $PSScriptRoot '44_patch_replacements.py')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = '1'
$started = Get-Date
# The repository's own third_party folder when there is one (the public layout), else C:\Tools.
$tools = if (Test-Path (Join-Path $root 'third_party')) { (Join-Path $root 'third_party') -replace '\\', '/' } else { 'C:/Tools' }
$native = '/' + $root.Substring(0, 1).ToLower() + ($root.Substring(2) -replace '\\', '/') + '/native'
& "$Msys\usr\bin\bash.exe" -lc "cd '$native' && make -j$Jobs TOOLS='$tools' $Target" 2>&1 |
    ForEach-Object { "$_" } | Set-Content $log -Encoding utf8
$code = $LASTEXITCODE
Write-Host "make $Target exit code $code, $([math]::Round(((Get-Date) - $started).TotalMinutes, 1)) minutes"
Get-Content $log -Tail 15
exit $code
