# Building the toolkit

The toolkit is what a release offers: the launcher, the runtime as source, the graphics library, the translator,
and a C compiler. **Building it never needs the game**, and nothing in it is made from the game. The game is only
translated and compiled later, by each player's launcher, from their own disc.

## Once: tools

Windows 10 or 11, about 8 GB free, and Python 3 for Windows ([python.org](https://www.python.org)) on PATH.

```
powershell -ExecutionPolicy Bypass -File scripts\setup-tools.ps1
```

This installs [MSYS2](https://www.msys2.org) (with winget) and its UCRT64 GCC, CMake, Ninja, git, and make, then
fetches [DolRecomp](https://github.com/ExpansionPak/DolRecomp) at commit `02e27ac` and
[aurora](https://github.com/encounter/aurora) at commit `f77492f` into `third_party\` (not part of the
repository). aurora downloads its own dependencies (Dawn, SDL3, Abseil, and others) the first time it's built.

## Each release

```
powershell -ExecutionPolicy Bypass -File scripts\build-toolkit.ps1 -Version 1.0.0
```

1. `scripts\45_make_nand.py` writes the console-settings files new saves start from, from public formats.
2. `scripts\48_build_aurora.ps1` applies `native\patches\aurora-*.patch` and builds aurora.
3. `scripts\41_build_native.ps1 -Target toolkit-parts` builds `Launcher.exe`, `PairRemote.exe`, and the graphics
   glue (`native\src\aurora_glue.cpp`).
4. `scripts\66_make_toolkit.ps1` builds DolRecomp with GCC (with `native\patches\dolrecomp-direct-calls.patch`),
   copies the runtime's C source, the prebuilt graphics library, the GCC toolchain from MSYS2, the relay, and
   every licence into `exports\ResortRecomp\`, checks that nothing from a game is in it, and zips it to
   `exports\ResortRecomp-<version>.zip`.

Everyone playing together online needs the same version: the version is part of each player's build.

## Layout

| Folder | What |
|---|---|
| `native/src` | The runtime: Wii OS calls, graphics command stream, DSP sound mixer, disc, saves, Wii Remote driver, online |
| `native/launcher` | The launcher, its builder (`builder.cpp`), and the Mii Maker |
| `native/builder/game.mk` | The game build each player's PC runs |
| `native/relay` | The online relay server |
| `native/patches` | Changes to DolRecomp and aurora |
| `native/input` | Input scripts for automated testing |
| `native/toolkit` | The toolkit's README and licence notes |
| `scripts` | The build |
