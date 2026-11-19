# 48_build_aurora.ps1
# Configures and builds aurora in C:\Tools\aurora with the MSYS2 UCRT64 tools. The first configure
# downloads Dawn, SDL3, and small libraries. Two small changes are made to aurora's source: see below.
#
# DVD, CARD, and THP are off: the game has its own disc layer, and CARD holds the only code that
# refers to Dolphin (folder lookup) plus a file taken from libogc.
# NTDDI_VERSION turns off a WinRT time-zone lookup in abseil that doesn't compile with MinGW.
# native/aurora-link fills two link gaps, explained in comsuppw.cpp. -u WinMain pulls in aurora's
# entry point, which nothing else refers to.
#
# Usage: powershell -ExecutionPolicy Bypass -File scripts\48_build_aurora.ps1 [-Target simple] [-Jobs 12]

param(
    [string]$Target = 'simple',
    [int]$Jobs = 12,
    [string]$Aurora = '',
    [string]$Msys = 'C:\msys64'
)

$root = Split-Path $PSScriptRoot -Parent
# The repository's own third_party folder when there is one (the public layout), else C:\Tools.
$tools = if (Test-Path (Join-Path $root 'third_party')) { Join-Path $root 'third_party' } else { 'C:\Tools' }
if (-not $Aurora) { $Aurora = Join-Path $tools 'aurora' }
New-Item -ItemType Directory -Force (Join-Path $root 'analysis\native'), (Join-Path $root 'exports\native-build') | Out-Null
$log = Join-Path $root 'analysis\native\aurora-build.log'
$env:PATH = "$Msys\ucrt64\bin;$Msys\usr\bin;$env:PATH"
$flags = '-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000000'

& cmake -S $Aurora -B "$Aurora\build-mingw" -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ `
    "-DCMAKE_C_FLAGS=$flags" "-DCMAKE_CXX_FLAGS=$flags" `
    "-DCMAKE_EXE_LINKER_FLAGS=-LC:/Tools/aurora-link -Wl,-u,WinMain" `
    -DAURORA_RVL=ON -DAURORA_ENABLE_DVD=OFF -DAURORA_ENABLE_CARD=OFF -DAURORA_ENABLE_THP=OFF `
    -DAURORA_ENABLE_TESTS=OFF *> (Join-Path $root 'analysis\native\aurora-configure.log')
if ($LASTEXITCODE -ne 0) { throw 'Configure failed, see analysis\native\aurora-configure.log' }

# GCC starts this field inside the base struct's padding, but the MSVC-built Dawn expects it after,
# so every later field would be 8 bytes off. Dawn's other structs already pin this with alignas.
$header = "$Aurora\build-mingw\_deps\dawn_prebuilt-src\include\dawn\native\DawnNative.h"
$text = [IO.File]::ReadAllText($header)
$fixed = $text -replace '(?m)^    uint32_t additionalRuntimeSearchPathsCount = 0;', '    alignas(8) uint32_t additionalRuntimeSearchPathsCount = 0;'
if ($fixed -ne $text) { [IO.File]::WriteAllText($header, $fixed) }
if ($fixed -notmatch 'alignas\(8\) uint32_t additionalRuntimeSearchPathsCount') { throw 'Dawn header changed, review the layout fix' }

# Aurora's frame buffer scale is the same on both axes. This game draws 16:9 squeezed into 608x456, so a
# width ratio of 4/3 gives 4x by 3x: whole numbers, which keep its tile copies exact, and exactly 16:9.
$efb = "$Aurora\lib\gfx\efb.cpp"
$text = [IO.File]::ReadAllText($efb)
$fixed = $text
if ($fixed -notmatch 'g_widthRatio') {
    $fixed = $fixed.Replace('AuroraWindowSize calculate_size(AuroraWindowSize size,', "float g_widthRatio = 1.f; // set by the native program`nAuroraWindowSize calculate_size(AuroraWindowSize size,")
    $fixed = $fixed.Replace('size.fb_width = extent(baseWidth * static_cast<double>(frameBufferScale));', 'size.fb_width = extent(baseWidth * static_cast<double>(frameBufferScale) * g_widthRatio);')
}
if ($fixed -ne $text) { [IO.File]::WriteAllText($efb, $fixed) }
if ($fixed -notmatch 'frameBufferScale\) \* g_widthRatio') { throw 'aurora efb.cpp changed, review the width ratio patch' }

# native/patches/aurora-gx-fixes.patch changes aurora's GX code for an unmodified game:
# - Z textures, which aurora didn't have. This game pastes its Miis back over the scene with one.
# - A texture coordinate with no texgen, or a texgen reading a vertex attribute that isn't there: aurora stopped,
#   because ported decompiled games shouldn't do it, but 100-Pin Bowling does. Now it reads a fixed value.
# Applied once: "has no texgen source" in shader.cpp means it's in. --ignore-whitespace copes with CRLF.
$patch = Join-Path (Split-Path $PSScriptRoot -Parent) 'native/patches/aurora-gx-fixes.patch'
if (-not (Select-String -Path "$Aurora\lib\gx\shader.cpp" -Pattern 'has no texgen source' -Quiet)) {
    & git -C $Aurora apply --ignore-whitespace $patch
    if ($LASTEXITCODE -ne 0) { throw 'aurora changed, review native/patches/aurora-gx-fixes.patch' }
}

# native/patches/aurora-adapter-choice.patch lets the launcher's "Graphics card" setting reach Dawn: the
# power-saving card, or the fallback adapter that draws on the processor. Applied once, the same way.
$patch = Join-Path (Split-Path $PSScriptRoot -Parent) 'native/patches/aurora-adapter-choice.patch'
if (-not (Select-String -Path "$Aurora\lib\webgpu\gpu.cpp" -Pattern 'AURORA_POWER_PREFERENCE' -Quiet)) {
    & git -C $Aurora apply --ignore-whitespace $patch
    if ($LASTEXITCODE -ne 0) { throw 'aurora changed, review native/patches/aurora-adapter-choice.patch' }
}

$link = Join-Path $tools 'aurora-link'
New-Item -ItemType Directory -Force $link | Out-Null
& g++ -std=c++20 "-I$Aurora/build-mingw/_deps/dawn_prebuilt-src/include" -c (Join-Path $root 'native\aurora-link\comsuppw.cpp') -o "$link\comsuppw.o"
if ($LASTEXITCODE -ne 0) { throw 'The comsuppw stand-in did not compile' }
Remove-Item "$link\libcomsuppw.a" -ErrorAction SilentlyContinue
& ar rcs "$link\libcomsuppw.a" "$link\comsuppw.o"

& cmake --build "$Aurora\build-mingw" --target $Target -j $Jobs *> $log
$code = $LASTEXITCODE
Get-Content $log -Tail 3

# The library list the native program links with, taken from what aurora's example used.
$ninja = Get-Content "$Aurora\build-mingw\build.ninja"
$start = ($ninja | Select-String -Pattern '^build examples/simple.exe' | Select-Object -First 1).LineNumber
$line = ($ninja[$start..($start + 8)] | Where-Object { $_ -match '^\s+LINK_LIBRARIES = ' }) -replace '^\s+LINK_LIBRARIES = ', ''
$items = $line -split '\s+' | Where-Object { $_ } | ForEach-Object {
    if ($_ -match '\.(a|lib)$' -and $_ -notmatch '^[A-Za-z]:') { "$($Aurora -replace '\\', '/')/build-mingw/$_" } else { $_ }
}
$out = Join-Path $root 'exports\native-build\aurora.rsp'
[IO.File]::WriteAllText($out, ($items -join "`n") + "`n")
exit $code
