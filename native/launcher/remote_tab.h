// The launcher's Wii Remote tab. See remote_tab.cpp.
#pragma once
#include <filesystem>

// launcher_dir holds PairRemote.exe; work_folder is where it runs.
void remote_tab(const std::filesystem::path& launcher_dir, const std::filesystem::path& work_folder);
