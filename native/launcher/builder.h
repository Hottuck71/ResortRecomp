// Builds the game on the player's own PC from their own disc, with the toolkit's translator and compiler
// (builder/ beside the launcher). See builder.cpp.
#pragma once
#include <filesystem>
#include <functional>
#include <string>

namespace builder {

namespace fs = std::filesystem;

enum class Mode { Plain, Measuring, Fast };

bool available(const fs::path& dir);        // a toolkit: builder/game.mk is there
std::string version(const fs::path& dir);   // builder/version.txt
fs::path game_exe(const fs::path& dir);     // the game this toolkit builds
// Built for this main.dol (its SHA-256) with this toolkit version; *mode says which kind.
bool built(const fs::path& dir, const std::string& dol_hash, Mode* mode = nullptr);
// Measuring counts written so far (the game writes them when it closes).
int measured_files(const fs::path& dir);

// Builds the game. Plain: translate and compile. Measuring: the same, with counters (the game runs a little
// slower and records how its code is used while the player plays). Fast: compile again with those counts.
// say() gets progress lines for the window. Returns "" or what went wrong, in a sentence.
std::string build(const fs::path& dir, const fs::path& main_dol, const std::string& dol_hash, Mode mode,
                  const std::function<void(const std::string&)>& say);

}  // namespace builder
