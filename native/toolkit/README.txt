ResortRecomp
============
Play your own copy of Wii Sports Resort on a Windows 10 or 11 PC.
Launcher and toolkit by Lane Carle (crazyman12).

ResortRecomp contains NO part of the game: no code, no pictures, no sounds,
nothing made from them. You need your own Wii Sports Resort disc (USA, RZTE01,
first release), copied to your PC as a disc image, and Dolphin installed (only
its DolphinTool is used, to extract the disc). The launcher then translates
your copy into a PC program and builds it, here, on your own PC.

Wii Sports Resort is Nintendo's. ResortRecomp isn't made, sponsored, or
endorsed by Nintendo.


FIRST TIME
----------
1. Unzip this folder somewhere with about 6 GB free (not inside Program Files).
2. Open Launcher.exe. (Windows may say it protected your PC: "More info",
   then "Run anyway".)
3. Play tab: pick your disc image, and DolphinTool.exe from your Dolphin
   folder under "Disc tools". Press "Set up the game".
4. Wait. The disc is extracted (a minute or two), your game's code is
   translated (seconds), and built with the compiler in the builder folder:
   about 10 minutes on a recent PC, up to 30 on an older one. The launcher
   shows how far it's got.
5. Press Play.

FASTER ON SLOWER PCs
--------------------
Play tab > Speed: press "Start measuring", play normally for 10 minutes or
more in a few sports, close the game, and press "Finish". The game is built
again, tuned by how it ran on your PC: about 35 to 65% faster in tests,
most in the sports you played while measuring.

CONTROLLERS
-----------
Wii Remote Plus (or a Wii Remote with MotionPlus): pair it once on the
launcher's Wii Remote tab. Its Nunchuk works as on the Wii.

Nintendo Switch Joy-Cons play instead of a Wii Remote: pair them once on the
launcher's Joy-Con tab (hold the small sync button on the rail until the green
lights move, then click "The lights are moving"). A Wii Remote is used first.
  Right Joy-Con = the Wii Remote, held upright, top towards the screen:
    A = A   ZR = B   stick = d-pad   Y = 1   B = 2   X = -   + = +
    HOME = HOME   R or pressing the stick re-centres the pointer
  Left Joy-Con = the Nunchuk: stick, ZL = Z, L = C
  On its own a left Joy-Con is the Wii Remote (right arrow = A, ZL = B).
  Swings, tilts and pointing come from the Joy-Con's motion sensors: no
  sensor bar. SL and SR together switch a Joy-Con to sideways.

ONLINE
------
Everyone needs the same ResortRecomp version. See relay\SERVER-SETUP.txt to run
your own room server on Ubuntu or Windows.

YOUR SAVE
---------
Kept in the "save" folder here. Play tab > Save data exports and imports it.
Mii Maker imports and exports .mii files.

WHAT'S IN THIS FOLDER
---------------------
Launcher.exe            the launcher
PairRemote.exe          pairs a Wii Remote with Windows
PairJoyCon.exe          pairs Nintendo Switch Joy-Cons with Windows
builder\                the translator (DolRecomp), this project's runtime as
                        source, the graphics library, and a C compiler (GCC)
relay\                  the online room server, and how to run it
licenses\               the licence of every part (ResortRecomp is GPL-3.0)
*.dll                   libraries the game and launcher use
After setting up, you'll also have: game\ (your extracted disc),
ResortRecomp-Game.exe (your build), save\, and build-log.txt.
Don't share those: they're made from your copy of the game.
