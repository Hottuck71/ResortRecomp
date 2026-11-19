# 66_make_toolkit.ps1
# Makes the ResortRecomp toolkit: what a GitHub release offers for download. It holds no part of the game: no
# translated code, no assets, nothing made from the game. Each player's launcher translates their own main.dol
# with the bundled DolRecomp and builds it with the bundled compiler, on their own PC.
#
#   exports\ResortRecomp\           the toolkit
#   exports\ResortRecomp-<v>.zip    the same, zipped, with -Zip
#
# Needs this project's built launcher (41_build_native.ps1 -Target launcher), its build of aurora
# (48_build_aurora.ps1), DolRecomp's source in C:\Tools\DolRecomp with native\patches\dolrecomp-direct-calls.patch,
# and MSYS2 with the UCRT64 toolchain (21_setup_msys2.ps1) plus mingw-w64-ucrt-x86_64-make.
#
# Usage: powershell -ExecutionPolicy Bypass -File scripts\66_make_toolkit.ps1 [-Version 1.0.0] [-Zip]
param(
    [string]$Version = '1.0.0',
    [string]$Out = 'exports\ResortRecomp',
    [switch]$Zip,
    [string]$DolRecomp = '',
    [string]$Aurora = '',
    [string]$Msys = 'C:\msys64'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$build = Join-Path $root 'exports\native-build'
# The repository's own third_party folder when there is one (the public layout), else C:\Tools.
$tools = if (Test-Path (Join-Path $root 'third_party')) { Join-Path $root 'third_party' } else { 'C:\Tools' }
if (-not $DolRecomp) { $DolRecomp = Join-Path $tools 'DolRecomp' }
if (-not $Aurora) { $Aurora = Join-Path $tools 'aurora' }
$dest = Join-Path $root $Out
$work = Join-Path $root 'exports\toolkit-work'
foreach ($need in "$build\Launcher.exe", "$build\PairRemote.exe", "$build\aurora.rsp", "$build\obj\host_aurora_glue.o", "$DolRecomp\src\cpu\cpu.h") {
    if (-not (Test-Path $need)) { throw "Missing ${need}: build the launcher and game first (see the top of this script)" }
}
$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = '1'
function Invoke-Ucrt64([string]$Command) { & "$Msys\usr\bin\bash.exe" -lc $Command; if ($LASTEXITCODE -ne 0) { throw "Failed: $Command" } }
function To-Msys([string]$Path) { '/' + $Path.Substring(0, 1).ToLower() + ($Path.Substring(2) -replace '\\', '/') }

if (Test-Path $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
New-Item -ItemType Directory -Force $dest, $work | Out-Null
$tk = Join-Path $dest 'builder'
New-Item -ItemType Directory -Force "$tk\src", "$tk\lib", "$tk\dolrecomp\cpu", "$tk\dolrecomp\common", "$tk\toolchain" | Out-Null

# 1. DolRecomp, built with GCC: the same translation, byte for byte, as the build this project tested.
$patch = Join-Path $root 'native\patches\dolrecomp-direct-calls.patch'
if (-not (Select-String -Path "$DolRecomp\src\app\pipeline.c" -Pattern 'direct cross-chunk calls' -Quiet)) {
    git -C $DolRecomp apply --ignore-whitespace $patch
    if ($LASTEXITCODE -ne 0) { throw 'DolRecomp changed: review native\patches\dolrecomp-direct-calls.patch' }
}
Invoke-Ucrt64 "cmake -S '$(To-Msys $DolRecomp)' -B '$(To-Msys "$work\dolrecomp")' -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null && cmake --build '$(To-Msys "$work\dolrecomp")' --target dolrecomp > /dev/null"
Copy-Item "$work\dolrecomp\dolrecomp.exe", "$Msys\ucrt64\bin\libz-1.dll" $tk
Copy-Item "$DolRecomp\src\cpu\cpu.h" "$tk\dolrecomp\cpu\"
Copy-Item "$DolRecomp\src\common\types.h" "$tk\dolrecomp\common\"
& python (Join-Path $PSScriptRoot '40_make_cpu_core.py') "$DolRecomp\src\cpu\cpu.c" "$tk\dolrecomp\cpu_core.c" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'cpu_core.c was not made' }

# 2. The runtime, as source, and the build.
Copy-Item 'native\src\*.c', 'native\src\*.h' "$tk\src\"
Copy-Item 'native\replacements.txt' "$tk\src\"
Copy-Item 'native\builder\game.mk' $tk
Set-Content "$tk\version.txt" $Version -Encoding ascii -NoNewline

# 3. The graphics library, prebuilt: aurora and everything its link list names, and the glue compiled against it.
Copy-Item "$build\obj\host_aurora_glue.o" "$tk\lib\aurora_glue.o"
Copy-Item (Join-Path $tools 'aurora-link\libcomsuppw.a') "$tk\lib\"
$names = @{}
$words = (Get-Content "$build\aurora.rsp" -Raw) -split '\s+' | Where-Object { $_ }
$rsp = foreach ($w in $words) {
    if ($w -match '^[A-Za-z]:/') {
        $name = Split-Path $w -Leaf
        if ($names.ContainsKey($name) -and $names[$name] -ne $w) { throw "Two libraries are both called $name" }
        $names[$name] = $w
        if (-not (Test-Path "$tk\lib\$name")) { Copy-Item ($w -replace '/', '\') "$tk\lib\$name" }
        "builder/lib/$name"
    } else { $w }
}
Set-Content "$tk\lib\link.rsp" ($rsp -join ' ') -Encoding ascii
# The DLLs those import libraries name, from the same MSYS2, so the game finds the versions it was linked with.
$runtimeDlls = foreach ($name in $names.Keys | Where-Object { $_ -like '*.dll.a' }) {
    $dll = (& "$Msys\ucrt64\bin\dlltool.exe" --identify "$tk\lib\$name" 2>$null | Select-Object -First 1)
    if ($dll -and (Test-Path "$Msys\ucrt64\bin\$dll")) { "$Msys\ucrt64\bin\$dll" }
}

# 4. The compiler: MSYS2's GCC, binutils, MinGW-w64 headers and libraries, make, and what they need.
& "$Msys\usr\bin\bash.exe" -l (To-Msys (Join-Path $PSScriptRoot 'toolkit_toolchain_files.sh')) (To-Msys "$work\toolchain-packages.txt") > "$work\toolchain-files.txt"
if ($LASTEXITCODE -ne 0) { throw 'Could not list the compiler files' }
foreach ($f in Get-Content "$work\toolchain-files.txt") {
    $to = Join-Path "$tk\toolchain" $f
    $folder = Split-Path $to -Parent
    if (-not (Test-Path -LiteralPath $folder)) { New-Item -ItemType Directory -Force $folder | Out-Null }
    Copy-Item -LiteralPath (Join-Path "$Msys\ucrt64" $f) $to
}

# 5. The programs, their libraries, and the rest of what a player gets.
Copy-Item "$build\Launcher.exe", "$build\PairRemote.exe", "$build\*.dll" $dest
foreach ($dll in $runtimeDlls) { Copy-Item $dll $dest -Force }
foreach ($dll in 'libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll') { Copy-Item "$Msys\ucrt64\bin\$dll" $dest -Force }
Copy-Item "$build\nand-template" $dest -Recurse
New-Item -ItemType Directory "$dest\relay", "$dest\licenses" | Out-Null
Copy-Item 'native\relay\wsr_relay.c', 'native\relay\wsr-relay.service', 'native\relay\README.md', 'native\relay\SERVER-SETUP.txt' "$dest\relay"
Invoke-Ucrt64 "cd '$(To-Msys "$root\native\relay")' && gcc -O2 -Wall -static -o '$(To-Msys "$dest\relay\wsr_relay.exe")' wsr_relay.c -lws2_32"
Copy-Item "$DolRecomp\LICENSE" "$dest\licenses\GPL-3.0.txt"
Copy-Item "$build\licenses\*" "$dest\licenses\"
Copy-Item "$Aurora\LICENSE" "$dest\licenses\aurora-MIT.txt"
Copy-Item "$work\toolchain-packages.txt" "$dest\licenses\compiler-packages.txt"
# The licence texts of the graphics library's parts, and of the runtime DLLs from MSYS2.
$deps = "$Aurora\build-mingw\_deps"
foreach ($d in @{ 'abseil' = 'abseil-cpp-src\LICENSE'; 'fmt' = 'fmt-src\LICENSE'; 'freetype' = 'freetype-src\LICENSE.TXT';
                  'imgui' = 'imgui-src\LICENSE.txt'; 'nlohmann-json' = 'nlohmann_json-src\LICENSE.MIT'; 'libpng' = 'png-src\LICENSE';
                  'tracy' = 'tracy-src\LICENSE' }.GetEnumerator()) {
    Copy-Item "$deps\$($d.Value)" "$dest\licenses\$($d.Key)-LICENSE.txt"
}
Copy-Item "$deps\sdl3_prebuilt-src\licenses\SDL3" "$dest\licenses\SDL3" -Recurse
foreach ($l in 'bzip2', 'gcc', 'libgcc', 'libstdc++', 'libwinpthread', 'sqlite', 'xxhash', 'zlib', 'zstd') {
    if (Test-Path "$Msys\ucrt64\share\licenses\$l") { Copy-Item "$Msys\ucrt64\share\licenses\$l" "$dest\licenses\$l" -Recurse }
}
Copy-Item 'native\toolkit\THIRD-PARTY.txt', 'native\toolkit\README.txt' $dest
Move-Item "$dest\THIRD-PARTY.txt" "$dest\licenses\THIRD-PARTY.txt"
Set-Content "$dest\release.txt" "ResortRecomp $Version toolkit. This file keeps the launcher's save, build, and reports inside this folder. Don't delete it." -Encoding utf8

# 6. Nothing from the game may be in it.
$bad = Get-ChildItem $dest -Recurse -File | Where-Object {
    $_.Name -match '^(chunk_.*\.c|generated\.[ch]|main\.dol|.*\.(gcda|brsar|brstm|arc|tpl|wbfs|iso|rvz))$' -or
    $_.Name -in @('launcher.ini', 'RFL_DB.dat', 'Sports2.dat', 'banner.bin', 'initial_pipeline_cache.db', 'WiiSportsResort-Native.exe') -or
    $_.FullName -match '\\(game|save|build|analysis)\\'
}
if ($bad) { $bad | ForEach-Object { "Not allowed: $($_.FullName)" }; throw 'The toolkit has files it must not have' }
$sources = Get-ChildItem "$tk\src", "$tk\dolrecomp" -Recurse -File | Select-String -Pattern '^label_8[0-9A-F]{7}:' -List
if ($sources) { throw "Translated game code found in $($sources.Path)" }
$size = (Get-ChildItem $dest -Recurse -File | Measure-Object Length -Sum).Sum
"Toolkit: $dest, $(@(Get-ChildItem $dest -Recurse -File).Count) files, {0:N0} MB" -f ($size / 1MB)

if ($Zip) {
    $zipFile = Join-Path $root "exports\ResortRecomp-$Version.zip"
    if (Test-Path $zipFile) { Remove-Item $zipFile }
    & "$env:SystemRoot\System32\tar.exe" -a -c -f $zipFile -C (Split-Path $dest -Parent) (Split-Path $dest -Leaf)
    if ($LASTEXITCODE -ne 0) { throw 'The zip was not made' }
    "Zip: $zipFile, {0:N0} MB" -f ((Get-Item $zipFile).Length / 1MB)
}
