// The launcher's Mii Maker tab. See mii_maker.cpp.
#pragma once
#include <string>
#include <vector>

struct SDL_Renderer;

void mii_maker_set_renderer(SDL_Renderer* renderer);  // before the first frame: the Mii pictures are its textures
void mii_maker_release();                             // before the renderer goes
void mii_maker_tab(const std::string& db_path);
bool mii_maker_busy();  // some pictures were put off to a later frame (for --shot, which waits for them)
// For --shot: how many pages there are, and showing page n (the list, then each part with the first Mii).
int mii_maker_pages();
// --test-mii-import: the Import Miis path without the dialog. Returns what the Mii Maker would say.
std::string mii_maker_import_test(const std::string& db_path, const std::vector<std::string>& files);
void mii_maker_show(const std::string& db_path, int page);
