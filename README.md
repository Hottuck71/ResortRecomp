# ResortRecomp

Play **your own copy** of Wii Sports Resort on a Windows 10 or 11 PC, as a native PC program: real Wii Remotes
(with MotionPlus and Nunchuk) or Nintendo Switch Joy-Cons, online play with friends, a Mii Maker, and graphics
settings for slower PCs.

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
5. "note you can download the dolphin tool from the docs folder"

On a slower PC, **Play > Speed > Start measuring**, play for 10 minutes, then **Finish**: the game is rebuilt
tuned by how it ran on that PC (profile-guided optimization): about 35 to 65% faster in tests, most in the
sports played while measuring.

## Controllers

- **Wii Remote Plus** (or a Wii Remote with a MotionPlus), paired once on the launcher's Wii Remote tab. Its
  Nunchuk works as on the Wii. No sensor bar needed: the pointer comes from the gyro.
- **Nintendo Switch Joy-Cons**, instead of a Wii Remote. Pair them with Windows once (hold the small sync button
  on the Joy-Con's rail until its lights run, then **Settings > Bluetooth & devices > Add device**). The game
  picks them up by itself; a connected Wii Remote is used first.
  - **Right Joy-Con = Wii Remote.** Hold it upright like a Wii Remote, top towards the screen. A = A, ZR = B,
    stick = d-pad, Y = 1, B = 2, X = −, + = +, HOME = HOME. R or pressing the stick re-centres the pointer. Its
    gyro and accelerometer are the remote's MotionPlus and motion, so swings, tilts and pointing all work.
  - **Left Joy-Con = Nunchuk** when both are connected: stick = Nunchuk stick, ZL = Z, L = C (for Archery,
    Cycling and Power Cruising). On its own, a left Joy-Con is the Wii Remote instead (right arrow = A, ZL = B).
  - SL and SR together switch a Joy-Con between upright and sideways. Rumble and player lights work.
  - When MotionPlus asks you to set the remote down, lay the Joy-Con flat and still the same way.

## Playing online

Online play needs everyone on the same ResortRecomp version. Players connect through a small relay server that
anyone can run, so only the server's owner ever opens a port:

- **Same house / LAN:** double-click `relay\wsr_relay.exe` on any PC and everyone types that PC's address. No
  router changes.
- **Friends over the internet:** run the relay on a Windows PC, an Ubuntu home server or Raspberry Pi, or a rented
  VPS, and forward one TCP port.

Step by step: **[docs/ONLINE-SERVER.md](docs/ONLINE-SERVER.md)** (also in the download as
`relay\SERVER-SETUP.txt`).

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
