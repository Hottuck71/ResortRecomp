# How ResortRecomp works

## From a disc to a PC program, on the player's PC

1. **Extract.** Dolphin's `DolphinTool` unpacks the player's disc image. The launcher checks `sys/main.dol`'s
   SHA-256 against the one release this project supports (USA, RZTE01, first release).
2. **Translate.** [DolRecomp](https://github.com/ExpansionPak/DolRecomp) turns the PowerPC machine code in
   `main.dol` into C: about 400 files of straight-line C, one per 16 KB of code, with every instruction spelled out
   on an emulated register file. It runs in seconds.
3. **Hook.** The launcher adds a check at the entry of each function the runtime replaces (listed in
   `native/replacements.txt`), so a call from anywhere reaches the runtime's own version.
4. **Compile.** The bundled GCC compiles the translation together with the runtime and links the graphics library:
   `native/builder/game.mk`, about 10 minutes.
5. **Optionally, tune.** A measuring build counts how the code runs while the player plays; a final build uses those
   counts (GCC's profile-guided optimization) to lay out hot code together: 36% faster after two minutes of one
   sport in testing, 61 to 68% after seven sports.

The game's other files (models, textures, sound) stay in the extracted disc folder and are read as the game
asks for them, as from the disc.

## The runtime

The translated code runs the game's own logic, including Nintendo's SDK libraries linked into it. The runtime
stands in for the hardware and the system software around it:

- **Memory and CPU state**: the Wii's 24 MB and 64 MB memories, big-endian loads and stores, paired-single floats,
  the rounding and denormal behaviour the game sets.
- **Graphics**: the game writes GX commands to the graphics FIFO as on the console; the runtime decodes them
  and draws them through aurora and Dawn (Direct3D 12, Direct3D 11, or Vulkan), on its own thread.
- **Sound**: the game's AX library fills voice blocks for the DSP; the runtime mixes them as the DSP would, ADPCM
  and PCM, with the accelerator's looping rules.
- **Disc and saves**: disc reads come from the extracted folder; the Wii's NAND file system is a folder on the PC.
- **Wii Remote**: a real Wii Remote over Bluetooth (with MotionPlus and Nunchuk), or the keyboard and mouse.
- **Online**: lockstep, like Dolphin's netplay: every PC runs the same game from the same save and only the
  players' input travels, directly or through the relay server, with a checksum every two seconds to catch any
  difference.
