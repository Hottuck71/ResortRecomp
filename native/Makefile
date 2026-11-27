# Builds the Dolphin-free program. Run through scripts/41_build_native.ps1.
# Where things are: this project's research layout by default. The public repository passes its third_party
# folder (scripts/41_build_native.ps1 does).
TOOLS ?= C:/Tools
GEN ?= ../exports/recomp-c/generated
OUT ?= ../exports/native-build
DOLRECOMP_SRC ?= $(TOOLS)/DolRecomp/src
CC := gcc
CXX := g++
AURORA ?= $(TOOLS)/aurora
AURORA_LINK ?= $(TOOLS)/aurora-link
AURORA_BUILD := $(AURORA)/build-mingw
AURORA_FLAGS := -DAURORA -DAURORA_ENABLE_GX -DREVOLUTION=1 -DTARGET_PC=1 -DWEBGPU_DAWN \
	-I$(AURORA)/include -I$(AURORA_BUILD)/_deps/sdl3_prebuilt-src/include
# x86-64-v3 is any CPU since about 2015. It adds movbe, which loads big-endian data in one instruction.
# Contraction stays off, because fused multiply-add would change the game's float results.
CPU_FLAGS := -march=x86-64-v3 -ffp-contract=off
# Profile-guided optimization (scripts/63_build_pgo.ps1): PGO=gen builds with counters, which a training run
# fills (.gcda files beside the objects); PGO=use builds with them. Code the training never reached is
# optimized as usual (-fprofile-partial-training).
# Ordinary builds use the counts while they're there.
PGO ?= $(if $(wildcard $(OUT)/obj/*.gcda),use,)
ifeq ($(PGO),gen)
PGO_FLAGS := -fprofile-generate -DPGO_GEN
PGO_LINK := -fprofile-generate
else ifeq ($(PGO),use)
PGO_FLAGS := -fprofile-use -fprofile-partial-training -Wno-missing-profile -fprofile-correction -Wno-error=coverage-mismatch
PGO_LINK :=
endif
CFLAGS := -O2 -g0 -w -fwrapv -fno-strict-aliasing $(CPU_FLAGS) $(PGO_FLAGS) -I$(GEN) -I$(DOLRECOMP_SRC) -Isrc

CHUNKS := $(wildcard $(GEN)/chunks/*.c)
CHUNK_OBJS := $(patsubst $(GEN)/chunks/%.c,$(OUT)/obj/%.o,$(CHUNKS))
HOST_SRCS := $(wildcard src/*.c)
GLUE_OBJ := $(OUT)/obj/host_aurora_glue.o
HOST_OBJS := $(patsubst src/%.c,$(OUT)/obj/host_%.o,$(HOST_SRCS))

all: $(OUT)/WiiSportsResort-Native.exe pair

# What the toolkit (scripts/66_make_toolkit.ps1) takes from here: the launcher, the pairing tool, and the
# graphics glue. None of it needs the game.
toolkit-parts: launcher $(GLUE_OBJ)

# Separate from the game, so the game never depends on the Bluetooth pairing library.
pair: $(OUT)/PairRemote.exe $(OUT)/PairJoyCon.exe

$(OUT)/PairRemote.exe: tools/pair_remote.c src/real_remote.c src/real_remote.h
	$(CC) -O2 -Wall -Isrc tools/pair_remote.c src/real_remote.c -o $@ -static-libgcc -lbthprops -lhid -lsetupapi

# Pairs and tests Nintendo Switch Joy-Cons, with the game's own Joy-Con code (src/joycon.c).
$(OUT)/PairJoyCon.exe: tools/pair_joycon.c src/joycon.c src/joycon.h src/remote_sample.h
	$(CC) -O2 -Wall -Isrc tools/pair_joycon.c src/joycon.c -o $@ -static-libgcc -lbthprops -lhid -lsetupapi -lm

# The launcher: disc setup, Mii Maker, and online rooms. Dear ImGui and SDL3 come from aurora's build.
IMGUI := $(AURORA_BUILD)/_deps/imgui-src
SDL3_DIR := $(AURORA_BUILD)/_deps/sdl3_prebuilt-src
LAUNCHER_SRCS := launcher/launcher.cpp launcher/mii_db.cpp launcher/mii_maker.cpp launcher/mii_render.cpp \
	launcher/room_code.cpp launcher/remote_tab.cpp launcher/joycon_tab.cpp launcher/builder.cpp \
	$(IMGUI)/imgui.cpp $(IMGUI)/imgui_draw.cpp $(IMGUI)/imgui_tables.cpp $(IMGUI)/imgui_widgets.cpp \
	$(IMGUI)/backends/imgui_impl_sdl3.cpp $(IMGUI)/backends/imgui_impl_sdlrenderer3.cpp
launcher: $(OUT)/Launcher.exe $(OUT)/PairRemote.exe $(OUT)/PairJoyCon.exe

# The Mii art (scripts/59_export_mii_art.py) is one big array, built on its own so it compiles once.
$(OUT)/obj-launcher/mii_art.o: launcher/mii_art.cpp
	mkdir -p $(OUT)/obj-launcher
	$(CXX) -std=c++17 -O0 -c $< -o $@

# The Wii Remote tab checks for a connected remote with the game's own HID code.
$(OUT)/obj-launcher/real_remote.o: src/real_remote.c src/real_remote.h
	mkdir -p $(OUT)/obj-launcher
	$(CC) -O2 -Wall -Isrc -c $< -o $@

# The Joy-Con tab counts connected Joy-Cons with the game's own Joy-Con code.
$(OUT)/obj-launcher/joycon.o: src/joycon.c src/joycon.h src/remote_sample.h
	mkdir -p $(OUT)/obj-launcher
	$(CC) -O2 -Wall -Isrc -c $< -o $@

# Version details (author, version) shown under Properties > Details.
$(OUT)/obj-launcher/launcher_rc.o: launcher/launcher.rc
	mkdir -p $(OUT)/obj-launcher
	windres $< -O coff -o $@

$(OUT)/Launcher.exe: $(LAUNCHER_SRCS) launcher/mii_db.h launcher/mii_maker.h launcher/mii_render.h launcher/room_code.h \
		launcher/remote_tab.h launcher/joycon_tab.h launcher/builder.h $(OUT)/obj-launcher/real_remote.o $(OUT)/obj-launcher/joycon.o $(OUT)/obj-launcher/mii_art.o $(OUT)/obj-launcher/launcher_rc.o
	$(CXX) -std=c++17 -O2 -Wall -Ilauncher -Isrc -I$(IMGUI) -I$(IMGUI)/backends -I$(SDL3_DIR)/include $(LAUNCHER_SRCS) \
		$(OUT)/obj-launcher/real_remote.o $(OUT)/obj-launcher/joycon.o $(OUT)/obj-launcher/mii_art.o $(OUT)/obj-launcher/launcher_rc.o \
		$(SDL3_DIR)/bin/SDL3.dll -o $@ -mwindows -static-libgcc -static-libstdc++ -lws2_32 -lbcrypt -lhid -lsetupapi -ldwmapi
	mkdir -p $(OUT)/licenses
	cp launcher/third_party/myavatareditor/License.txt $(OUT)/licenses/MyAvatarEditor.txt
	mkdir -p $(OUT)/nand-template/shared2/sys $(OUT)/nand-template/title/00000001/00000002/data
	cp ../exports/native-nand/shared2/sys/SYSCONF $(OUT)/nand-template/shared2/sys/
	cp ../exports/native-nand/title/00000001/00000002/data/setting.txt $(OUT)/nand-template/title/00000001/00000002/data/

# Saves frames of a screen recording as BMP files, with Windows' own video support. Analysis only.
video: $(OUT)/video_frames.exe

$(OUT)/video_frames.exe: tools/video_frames.c
	$(CC) -O2 -Wall tools/video_frames.c -o $@ -static-libgcc -lmfplat -lmfreadwrite -lmfuuid -lole32 -lpropsys -luuid

chunks: $(CHUNK_OBJS)

# aurora.rsp lists aurora's libraries, written by scripts/48_build_aurora.ps1.
$(OUT)/WiiSportsResort-Native.exe: $(CHUNK_OBJS) $(HOST_OBJS) $(GLUE_OBJ) $(OUT)/obj/cpu_core.o
	$(CXX) $(PGO_LINK) -o $@ $(OUT)/obj/*.o -L$(AURORA_LINK) -L$(AURORA_BUILD) -Wl,-u,WinMain -Wl,--stack,67108864 @$(OUT)/aurora.rsp -lhid -lsetupapi -lwinmm -lws2_32 -lm

$(GLUE_OBJ): src/aurora_glue.cpp src/aurora_glue.h | $(OUT)/obj
	$(CXX) -std=c++20 -O2 -g -Wall -Isrc $(AURORA_FLAGS) -c $< -o $@

# A patched copy of a chunk, when there is one, is used instead of the original.
vpath chunk_%.c $(OUT)/chunks-patched $(GEN)/chunks

$(OUT)/obj/chunk_%.o: chunk_%.c src/fastmem.h | $(OUT)/obj
	$(CC) $(CFLAGS) -include src/fastmem.h -c $< -o $@

$(OUT)/obj/host_%.o: src/%.c $(wildcard src/*.h) | $(OUT)/obj
	$(CC) -O2 -g -Wall -Wno-unused-function -fwrapv -fno-strict-aliasing $(CPU_FLAGS) $(PGO_FLAGS) -Dmain=aurora_main -I$(GEN) -I$(DOLRECOMP_SRC) -Isrc -c $< -o $@

$(OUT)/obj/cpu_core.o: $(OUT)/cpu_core.c | $(OUT)/obj
	$(CC) $(CFLAGS) -I$(DOLRECOMP_SRC)/cpu -c $< -o $@

$(OUT)/obj:
	mkdir -p $@
