// The launcher's Joy-Con tab. See joycon_tab.cpp.
#pragma once
#include <filesystem>

// launcher_dir holds PairJoyCon.exe; work_folder is where it runs.
void joycon_tab(const std::filesystem::path& launcher_dir, const std::filesystem::path& work_folder);
