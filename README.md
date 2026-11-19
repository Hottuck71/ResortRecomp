# ResortRecomp

Play **your own copy** of Wii Sports Resort on a Windows 10 or 11 PC, as a native PC program: real Wii Remotes
(with MotionPlus and Nunchuk), online play with friends, a Mii Maker, and graphics settings for slower PCs.

By **Lane Carle (crazyman12)**.

> **ResortRecomp contains no part of the game.** No code, no graphics, no sound, nothing made from them, in this
> repository or in its downloads. Each player provides their own Wii Sports Resort disc, and the launcher
> translates and builds that copy into a PC program **on their own PC**. Wii Sports Resort is © Nintendo.
> ResortRecomp is not made, sponsored, or endorsed by Nintendo.

## Playing

1. Download `ResortRecomp-<version>.zip` from [Releases](../../releases) and unzip it (about 6 GB free space
   needed once set up; not inside Program Files).
2. You need: your own **Wii Sports Resort (USA, RZTE01, first release)** as a disc image you made from your disc,
   and [Dolphin](https://dolphin-emu.org) installed (only its `DolphinTool.exe` is used, to extract the image).
3. Open `Launcher.exe`. On the **Play** tab pick your disc image and `DolphinTool.exe`, then press
   **Set up the game**. The launcher extracts the disc, translates the game's code with
   [DolRecomp](https://github.com/ExpansionPak/DolRecomp), and compiles it with the bundled GCC:
   about 10 minutes on a recent PC, up to 30 on an older one.
4. Press **Play**.

On a slower PC, **Play > Speed > Start measuring**, play for 10 minutes, then **Finish**: the game is rebuilt
tuned by how it ran on that PC (profile-guided optimization): about 35 to 65% faster in tests, most in the
sports played while measuring.

Online play needs everyone on the same ResortRecomp version. A small relay server lets friends play without
port forwarding: [native/relay/SERVER-SETUP.txt](native/relay/SERVER-SETUP.txt).

**Please don't share** your extracted game, your built `ResortRecomp-Game.exe`, or anything else made from your
copy. Everyone sets up from their own disc.

## How it works

The game's PowerPC code (`main.dol`) is translated statically to C by DolRecomp, then compiled for x86-64 with
this project's runtime: a hand-written replacement for the Wii's operating system, graphics (through
[aurora](https://github.com/encounter/aurora) and Dawn), sound mixer, disc and save access, Wii Remote driver,
and online lockstep. See [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md).

## Building the toolkit from source

[docs/BUILDING.md](docs/BUILDING.md): install the tools with `scripts/setup-tools.ps1`, then
`scripts/build-toolkit.ps1` makes `exports/ResortRecomp-<version>.zip`, the same download as a release.
Building the toolkit never needs the game.

## Licence

ResortRecomp is free software under the [GNU GPL, version 3](LICENSE), because it builds on DolRecomp (GPL-3.0).
Third-party parts keep their own licences: [native/toolkit/THIRD-PARTY.txt](native/toolkit/THIRD-PARTY.txt).
Mii pictures in the launcher: My Avatar Editor, © 2009 Trevor McCauley, MIT.
