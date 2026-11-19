#!/bin/bash
# Lists the files of the C compiler the ResortRecomp toolkit carries: GCC, binutils, the MinGW-w64 headers and
# libraries, make, and every package they depend on, from this MSYS2's UCRT64 packages. One path per line,
# relative to /ucrt64. Documentation, manuals, and translations are left out; licences are kept.
# Also writes the package names and versions to the file given as $1 (for the toolkit's licence notes).
set -e
want="mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-binutils mingw-w64-ucrt-x86_64-make mingw-w64-ucrt-x86_64-crt mingw-w64-ucrt-x86_64-headers mingw-w64-ucrt-x86_64-winpthreads mingw-w64-ucrt-x86_64-windows-default-manifest"
declare -A seen
queue=($want)
while [ ${#queue[@]} -gt 0 ]; do
  p=${queue[0]}; queue=("${queue[@]:1}")
  # a dependency may name a virtual package or a version: resolve it to an installed package
  p=${p%%[<>=]*}
  real=$(pacman -Qq "$p" 2>/dev/null || pacman -Qqs "^${p}$" 2>/dev/null | head -1)
  [ -z "$real" ] && real=$(pacman -Qq 2>/dev/null | while read q; do pacman -Qi "$q" | grep -E "^Provides.*\b${p}\b" >/dev/null && echo "$q" && break; done)
  [ -z "$real" ] && continue
  [ -n "${seen[$real]}" ] && continue
  seen[$real]=1
  for d in $(pacman -Qi "$real" | sed -n 's/^Depends On *: //p'); do
    [ "$d" = "None" ] && continue
    case "$d" in mingw-w64-ucrt-x86_64-*) queue+=("$d");; esac
  done
done
: > "$1"
for p in "${!seen[@]}"; do
  pacman -Q "$p" >> "$1"
  pacman -Qlq "$p" | grep '^/ucrt64/.*[^/]$' | sed 's|^/ucrt64/||'
done | grep -v -E '^share/(doc|man|info|locale|gtk-doc|aclocal|bash-completion|vim|emacs|gdb)/' | sort -u
