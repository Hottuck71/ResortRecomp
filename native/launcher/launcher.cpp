// The launcher: set up the game from your own disc image, make Miis, and start a game alone or online.
//
// Play:      pick your Wii Sports Resort disc image (or an extracted folder). It is extracted with DolphinTool
//            from your own Dolphin, and its main.dol must match the one this build was translated from.
// Mii Maker: edits the Mii database in the game's save folder, which the game shows as its Miis.
// Wii Remote: pairs a Wii Remote with Windows through PairRemote.exe, and checks one is connected.
// Online:    hosts a room, or joins one by its code, directly or through a relay server. See docs/12-launcher.md.
// Graphics:  quality settings, with presets for slower PCs.
// About:     who made the launcher, its version, credits, and the light or dark look.
//
// Launcher by Lane Carle (crazyman12).
//
// Settings live in launcher.ini beside the launcher. Nothing is downloaded.
#include <windows.h>
#include <bcrypt.h>
#include <dwmapi.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "builder.h"
#include "mii_db.h"
#include "mii_maker.h"
#include "mii_render.h"
#include "remote_tab.h"
#include "room_code.h"

namespace fs = std::filesystem;

namespace {

// The main.dol this build was translated from: Wii Sports Resort, USA (RZTE01), first release.
constexpr const char* kMainDolSha256 = "95d273a31ebe1ef50fd1a1b847f75082525db970195126305dc967c119d8890e";
constexpr int kDefaultPort = 51010;

fs::path g_dir;  // where the launcher is
std::map<std::string, std::string> g_settings;

std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string setting(const std::string& key, const std::string& fallback = "") {
    auto it = g_settings.find(key);
    return it == g_settings.end() || it->second.empty() ? fallback : it->second;
}

void load_settings() {
    std::ifstream f(g_dir / "launcher.ini");
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq != std::string::npos) g_settings[line.substr(0, eq)] = line.substr(eq + 1);
    }
}

void save_settings() {
    std::ofstream f(g_dir / "launcher.ini", std::ios::trunc);
    for (auto& [k, v] : g_settings) f << k << '=' << v << '\n';
}

// Paths in the settings may be relative to the launcher's folder.
fs::path resolve(const std::string& p) {
    fs::path path = fs::u8path(p);
    return path.is_absolute() ? path : g_dir / path;
}

std::string sha256_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "";
    BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), std::streamsize(buf.size()));
        if (f.gcount() > 0) BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), ULONG(f.gcount()), 0);
    }
    unsigned char digest[32];
    BCryptFinishHash(hash, digest, 32, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    char hex[65];
    for (int i = 0; i < 32; i++) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return hex;
}

// An extracted game is a folder holding sys/main.dol; DolphinTool puts it under DATA.
fs::path game_folder_in(const fs::path& root) {
    if (fs::exists(root / "sys" / "main.dol")) return root;
    if (fs::exists(root / "DATA" / "sys" / "main.dol")) return root / "DATA";
    return {};
}

std::string check_game(const fs::path& folder) {
    if (folder.empty()) return "no extracted game found there";
    std::string hash = sha256_file(folder / "sys" / "main.dol");
    if (hash != kMainDolSha256)
        return "this isn't the version this build was made from: it needs Wii Sports Resort for the USA (RZTE01), "
               "first release";
    return "";
}

// Runs a program and waits. Returns its exit code, or -1 if it couldn't start.
int run_and_wait(const std::wstring& command, const fs::path& folder) {
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = command;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        folder.wstring().c_str(), &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
}

bool start_program(const std::wstring& command, const fs::path& folder) {
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = command;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.wstring().c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

std::wstring quote(const fs::path& p) { return L"\"" + p.wstring() + L"\""; }

// DolphinTool from the player's own Dolphin: the setting, beside the launcher, or a usual install folder.
fs::path find_dolphin_tool() {
    std::vector<fs::path> places;
    if (!setting("dolphin_tool").empty()) places.push_back(resolve(setting("dolphin_tool")));
    places.push_back(g_dir / "DolphinTool.exe");
    for (const char* env : {"ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"}) {
        if (const char* base = std::getenv(env)) {
            places.push_back(fs::path(base) / "Dolphin" / "DolphinTool.exe");
            places.push_back(fs::path(base) / "Dolphin-x64" / "DolphinTool.exe");
        }
    }
    for (auto& p : places)
        if (!p.empty() && fs::exists(p)) return p;
    return {};
}

// A release copy (scripts/62_make_release.ps1) has release.txt beside the launcher: its save, reports, and
// pictures stay in its own folder, wherever it's put. Without it, this project's own layout is used.
bool is_release() { return fs::exists(g_dir / "release.txt"); }

// The save folder: the settings files a console has, from the template, if it doesn't exist yet.
fs::path nand_folder() {
    std::string fallback = is_release() ? "save" : fs::exists(g_dir / ".." / "native-nand") ? "../native-nand" : "nand";
    fs::path nand = resolve(setting("nand", fallback));
    std::error_code ec;
    for (const char* file : {"shared2/sys/SYSCONF", "title/00000001/00000002/data/setting.txt"}) {
        fs::path target = nand / fs::u8path(file), source = g_dir / "nand-template" / fs::u8path(file);
        if (!fs::exists(target) && fs::exists(source)) {
            fs::create_directories(target.parent_path(), ec);
            fs::copy_file(source, target, ec);
        }
    }
    // What a console has before any game runs: the game's data folder (made when it's installed) and the Wii
    // Menu's Mii database, empty, as the game's own format routine writes it. Without them the game stops at
    // "Could not access Wii system memory" (a new save folder, as a release copy starts with).
    fs::create_directories(nand / "title" / "00010000" / "525a5445" / "data", ec);
    fs::path db = nand / "shared2" / "menu" / "FaceLib" / "RFL_DB.dat";
    if (!fs::exists(db)) {
        fs::create_directories(db.parent_path(), ec);
        mii::Database empty;
        empty.format();
        std::string error;
        empty.save(db.u8string(), &error);
    }
    return nand;
}

fs::path mii_db_path() { return nand_folder() / "shared2" / "menu" / "FaceLib" / "RFL_DB.dat"; }

// The game writes its reports and F12 shots under analysis/native, relative to where it runs: the project
// root in this project's own layout, the launcher's folder otherwise.
fs::path play_folder() {
    fs::path root = g_dir / ".." / "..";
    if (!is_release() && fs::exists(root / "analysis" / "native")) return fs::weakly_canonical(root);
    std::error_code ec;
    fs::create_directories(g_dir / "analysis" / "native" / "shots", ec);
    return g_dir;
}

// ---- Graphics settings, passed to the game when it starts ----

struct Preset {
    const char* name;
    int scale, aniso;
};
// High is what the game did before there were settings. There's no anti-aliasing setting: the renderer can't
// copy depth out of a multisampled frame buffer, which this game does. A higher resolution smooths edges instead.
const Preset kPresets[] = {{"Low", 1, 1}, {"Medium", 2, 4}, {"High", 3, 16}, {"Ultra", 4, 16}};
const char* const kBackends[] = {"auto", "d3d12", "d3d11", "vulkan"};
const char* const kBackendNames[] = {"Automatic", "Direct3D 12", "Direct3D 11", "Vulkan"};
const char* const kWindowSizes[] = {"1280x720", "960x540", "1600x900", "1920x1080"};
const char* const kGpus[] = {"fast", "low"};

int gfx(const char* key, int fallback, int low, int high) {
    std::string v = setting(key);
    return v.empty() ? fallback : std::clamp(std::atoi(v.c_str()), low, high);
}
int gfx_scale() { return gfx("gfx_scale", 3, 1, 6); }
int gfx_aniso() { return gfx("gfx_aniso", 16, 1, 16); }
int gfx_vsync() { return gfx("gfx_vsync", 1, 0, 1); }
int gfx_fullscreen() { return gfx("gfx_fullscreen", 0, 0, 1); }
int gfx_window() { return gfx("gfx_window", 0, 0, 3); }
int gfx_backend() { return gfx("gfx_backend", 0, 0, 3); }
int gfx_gpu() { return gfx("gfx_gpu", 0, 0, 1); }
int rumble() { return gfx("rumble", 1, 0, 1); }
void set_gfx(const char* key, int value) {
    g_settings[key] = std::to_string(value);
    save_settings();
}

std::wstring graphics_args() {
    return L" --scale " + std::to_wstring(gfx_scale()) + L" --aniso " +
           std::to_wstring(gfx_aniso()) + L" --vsync " + std::to_wstring(gfx_vsync()) + L" --fullscreen " +
           std::to_wstring(gfx_fullscreen()) + L" --window-size " + wide(kWindowSizes[gfx_window()]) + L" --backend " +
           wide(kBackends[gfx_backend()]) + L" --gpu " + wide(kGpus[gfx_gpu()]) + L" --rumble " + std::to_wstring(rumble());
}

fs::path game_exe() {
    return builder::available(g_dir) ? builder::game_exe(g_dir) : g_dir / "WiiSportsResort-Native.exe";
}

std::wstring game_command(const std::wstring& extra) {
    fs::path exe = game_exe();
    fs::path game = game_folder_in(resolve(setting("game")));
    return quote(exe) + L" --window 1 --cycles 100000000000000 --report analysis/native/run-window.txt --game " +
           quote(game) + L" --nand " + quote(nand_folder()) + graphics_args() + extra;
}

// File dialogs answer on another thread, so the answer waits here until the next frame.
std::mutex g_pick_lock;
std::string g_picked, g_pick_target;

void SDLCALL on_pick(void* target, const char* const* files, int) {
    std::lock_guard<std::mutex> lock(g_pick_lock);
    if (files && files[0]) {
        g_picked = files[0];
        g_pick_target = static_cast<const char*>(target);
    }
}

struct Setup {
    std::atomic<bool> busy{false};
    std::atomic<int> runs{0}; // set-ups finished, so a check made before one is made again after it
    std::mutex lock;
    std::string status;
    void set(const std::string& s) {
        std::lock_guard<std::mutex> l(lock);
        status = s;
    }
    std::string get() {
        std::lock_guard<std::mutex> l(lock);
        return status;
    }
} g_setup;

void set_up_game(std::string image) {
    g_setup.busy = true;
    fs::path source = fs::u8path(image);
    fs::path folder;
    if (fs::is_directory(source)) {
        folder = game_folder_in(source);
    } else {
        fs::path tool = find_dolphin_tool();
        if (tool.empty()) {
            g_setup.set("DolphinTool.exe wasn't found. Point the launcher at it under Disc tools.");
            g_setup.runs++;
            g_setup.busy = false;
            return;
        }
        fs::path out = g_dir / "game";
        std::error_code ec;
        fs::remove_all(out, ec);
        fs::create_directories(out, ec);
        g_setup.set("Extracting the disc. This takes a minute or two...");
        int code = run_and_wait(quote(tool) + L" extract -i " + quote(source) + L" -o " + quote(out) + L" -g -q", g_dir);
        if (code != 0) {
            g_setup.set("DolphinTool couldn't extract that image (exit code " + std::to_string(code) + ").");
            g_setup.runs++;
            g_setup.busy = false;
            return;
        }
        folder = game_folder_in(out);
    }
    g_setup.set("Checking the game...");
    std::string problem = check_game(folder);
    if (!problem.empty()) {
        g_setup.set("Not set up: " + problem + ".");
    } else {
        std::error_code ec;
        fs::path rel = fs::relative(folder, g_dir, ec);
        g_settings["game"] = (ec || rel.empty() ? folder : rel).u8string();
        save_settings();
        std::string failed;
        if (builder::available(g_dir) && !builder::built(g_dir, kMainDolSha256))
            failed = builder::build(g_dir, folder / "sys" / "main.dol", kMainDolSha256, builder::Mode::Plain,
                                    [](const std::string& line) { g_setup.set(line); });
        g_setup.set(failed.empty() ? "Ready to play." : "Not built: " + failed);
    }
    g_setup.runs++;
    g_setup.busy = false;
}

// The speed-up for slower PCs: build a measuring copy, the player plays, then build the fast version with what
// it measured. (Profile-guided optimization, from the player's own play: nothing is shipped from the game.)
void build_again(builder::Mode mode) {
    g_setup.busy = true;
    fs::path folder = game_folder_in(resolve(setting("game")));
    std::string failed = builder::build(g_dir, folder / "sys" / "main.dol", kMainDolSha256, mode,
                                        [](const std::string& line) { g_setup.set(line); });
    g_setup.set(!failed.empty() ? "Not built: " + failed
                : mode == builder::Mode::Measuring ? "Measuring copy ready. Play normally for 10 minutes or more, in a few different sports, then close the game and press Finish."
                : mode == builder::Mode::Fast ? "Done: the faster version is ready."
                                              : "Back to the ordinary version.");
    g_setup.runs++;
    g_setup.busy = false;
}

// "" when the set-up game is the right one, or what's wrong. Hashing main.dol takes a moment, so the answer is kept
// until the folder changes or a set-up finishes; while one is running, the folder is half written.
std::string game_problem() {
    static fs::path checked;
    static int checked_run = -1;
    static std::string problem = "not set up";
    if (g_setup.busy) return "being set up";
    fs::path game = game_folder_in(resolve(setting("game")));
    if (game != checked || g_setup.runs != checked_run) {
        checked = game;
        checked_run = g_setup.runs;
        problem = game.empty() ? "not set up" : check_game(game);
        if (problem.empty() && builder::available(g_dir) && !builder::built(g_dir, kMainDolSha256))
            problem = "not built yet: press \"Set up the game\"";
    }
    return problem;
}

// ---- Save data: Miis and the game's records, to and from a zip ----

const char* const kSaveParts[] = {"shared2/menu/FaceLib/RFL_DB.dat", "title/00010000/525a5445/data"};
std::mutex g_save_lock;
std::string g_save_export_to, g_save_import_from, g_save_message;

void SDLCALL on_save_export(void*, const char* const* files, int) {
    std::lock_guard<std::mutex> l(g_save_lock);
    if (files && files[0]) g_save_export_to = files[0];
}

void SDLCALL on_save_import(void*, const char* const* files, int) {
    std::lock_guard<std::mutex> l(g_save_lock);
    if (files && files[0]) g_save_import_from = files[0];
}

fs::path windows_tar() {
    const char* windir = std::getenv("SystemRoot");
    return fs::path(windir ? windir : "C:\\Windows") / "System32" / "tar.exe";
}

std::string export_save(fs::path zip) {
    if (zip.extension() != ".zip") zip += ".zip";
    fs::path nand = nand_folder();
    std::wstring parts;
    for (const char* part : kSaveParts)
        if (fs::exists(nand / fs::u8path(part))) parts += L" " + fs::u8path(part).generic_wstring();
    if (parts.empty()) return "There's no save yet: play the game first.";
    std::error_code ec;
    fs::remove(zip, ec);
    int code = run_and_wait(quote(windows_tar()) + L" -a -c -f " + quote(zip) + L" -C " + quote(nand) + parts, g_dir);
    return code == 0 ? "Saved your Miis and records to " + zip.u8string() : "Windows' tar couldn't write the zip (exit code " +
                                                                             std::to_string(code) + ").";
}

std::string import_save(const fs::path& zip) {
    fs::path nand = nand_folder(), unpack = g_dir / "save-import";
    std::error_code ec;
    fs::remove_all(unpack, ec);
    fs::create_directories(unpack, ec);
    // tar refuses absolute paths and ".." in an archive by default; only the save's own files are copied below.
    int code = run_and_wait(quote(windows_tar()) + L" -x -f " + quote(zip) + L" -C " + quote(unpack), g_dir);
    if (code != 0) {
        fs::remove_all(unpack, ec);
        return "That zip couldn't be read (exit code " + std::to_string(code) + ").";
    }
    std::vector<std::string> files;
    if (fs::is_regular_file(unpack / fs::u8path(kSaveParts[0]))) files.push_back(kSaveParts[0]);
    if (fs::is_directory(unpack / fs::u8path(kSaveParts[1])))
        for (auto& e : fs::directory_iterator(unpack / fs::u8path(kSaveParts[1])))
            if (e.is_regular_file()) files.push_back(std::string(kSaveParts[1]) + "/" + e.path().filename().u8string());
    if (files.empty()) {
        fs::remove_all(unpack, ec);
        return "That zip has no Wii Sports Resort save in it (it needs " + std::string(kSaveParts[0]) + " or " +
               kSaveParts[1] + ").";
    }
    // The save being replaced is kept, in save-backups beside the launcher.
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", std::localtime(&now));
    fs::path backup = g_dir / "save-backups" / stamp;
    for (const char* part : kSaveParts) {
        fs::path from = nand / fs::u8path(part);
        if (!fs::exists(from)) continue;
        fs::create_directories((backup / fs::u8path(part)).parent_path(), ec);
        fs::copy(from, backup / fs::u8path(part), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    }
    for (const std::string& f : files) {
        fs::create_directories((nand / fs::u8path(f)).parent_path(), ec);
        fs::copy_file(unpack / fs::u8path(f), nand / fs::u8path(f), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            fs::remove_all(unpack, ec);
            return "Couldn't write " + f + ": " + ec.message();
        }
    }
    fs::remove_all(unpack, ec);
    return "Took " + std::to_string(files.size()) + " save files from " + zip.filename().u8string() +
           ". Your old save is in " + backup.u8string() + ".";
}

// --test-save <save folder>: exports that save to a zip, imports it into an empty folder, and compares every save
// file. Settings aren't written. Returns failures.
int save_self_test(const fs::path& source) {
    std::error_code ec;
    fs::path work = g_dir / "selftest-save", zip = work / "save.zip", into = work / "into";
    fs::remove_all(work, ec);
    fs::create_directories(into, ec);
    std::vector<fs::path> backups_before; /* the import keeps a backup; only the test's own one is removed after */
    if (fs::exists(g_dir / "save-backups"))
        for (auto& e : fs::directory_iterator(g_dir / "save-backups")) backups_before.push_back(e.path());
    g_settings["nand"] = source.u8string();
    std::string out = export_save(zip);
    std::printf("export: %s\n", out.c_str());
    g_settings["nand"] = into.u8string();
    std::string in = import_save(zip);
    std::printf("import: %s\n", in.c_str());
    int failures = 0, compared = 0;
    for (auto& e : fs::recursive_directory_iterator(source)) {
        if (!e.is_regular_file()) continue;
        std::string rel = fs::relative(e.path(), source).generic_u8string();
        bool wanted = rel == kSaveParts[0] || rel.rfind(std::string(kSaveParts[1]) + "/", 0) == 0;
        if (!wanted) continue;
        std::ifstream a(e.path(), std::ios::binary), b(into / fs::u8path(rel), std::ios::binary);
        std::string x((std::istreambuf_iterator<char>(a)), {}), y((std::istreambuf_iterator<char>(b)), {});
        bool same = !x.empty() && x == y;
        std::printf("  %-50s %s\n", rel.c_str(), same ? "same" : "DIFFERENT");
        failures += !same;
        compared++;
    }
    if (!compared) failures++;
    fs::remove_all(work, ec);
    if (fs::exists(g_dir / "save-backups"))
        for (auto& e : fs::directory_iterator(g_dir / "save-backups"))
            if (std::find(backups_before.begin(), backups_before.end(), e.path()) == backups_before.end()) fs::remove_all(e.path(), ec);
    if (backups_before.empty()) fs::remove(g_dir / "save-backups", ec); /* only if now empty */
    std::printf("%d files compared, %d failures\n", compared, failures);
    return failures;
}

void save_data_section(SDL_Window* window) {
    {
        std::string to, from;
        {
            std::lock_guard<std::mutex> l(g_save_lock);
            to.swap(g_save_export_to);
            from.swap(g_save_import_from);
        }
        if (!to.empty()) g_save_message = export_save(fs::u8path(to));
        if (!from.empty()) g_save_message = import_save(fs::u8path(from));
    }
    if (!ImGui::TreeNode("Save data")) return;
    ImGui::TextWrapped("Your Miis and the game's records, as one zip: to keep a copy, move to another PC, or share.");
    static const SDL_DialogFileFilter zip_filter[] = {{"Zip files", "zip"}};
    if (ImGui::Button("Export save...")) SDL_ShowSaveFileDialog(on_save_export, nullptr, window, zip_filter, 1, "WiiSportsResort-save.zip");
    ImGui::SameLine();
    if (ImGui::Button("Import save...")) SDL_ShowOpenFileDialog(on_save_import, nullptr, window, zip_filter, 1, nullptr, false);
    ImGui::TextDisabled("Importing replaces your Miis and records; the old ones are kept in save-backups first.");
    if (!g_save_message.empty()) ImGui::TextWrapped("%s", g_save_message.c_str());
    ImGui::TreePop();
}

void hint(const char* text); // grey help text, below

// ---- Play ----

char g_image[1024] = "", g_tool[1024] = "";

void pick_file(SDL_Window* window, const char* target, bool folder) {
    static const SDL_DialogFileFilter filters[] = {{"Wii disc images", "iso;wbfs;rvz;wia;gcz;ciso;nkit.iso"},
                                                   {"All files", "*"}};
    static const SDL_DialogFileFilter tool_filter[] = {{"DolphinTool", "exe"}};
    if (folder) SDL_ShowOpenFolderDialog(on_pick, const_cast<char*>(target), window, nullptr, false);
    else if (std::string(target) == "tool") SDL_ShowOpenFileDialog(on_pick, const_cast<char*>(target), window, tool_filter, 1, nullptr, false);
    else SDL_ShowOpenFileDialog(on_pick, const_cast<char*>(target), window, filters, 2, nullptr, false);
}

void play_tab(SDL_Window* window) {
    fs::path game = game_folder_in(resolve(setting("game")));
    std::string state = game_problem();
    bool ready = state.empty();
    ImGui::TextWrapped("This plays your own copy of Wii Sports Resort. Pick its disc image once; the launcher extracts "
                       "it with DolphinTool from your Dolphin and checks it is the USA release this build was made from.");
    ImGui::Spacing();
    ImGui::InputText("Disc image", g_image, sizeof g_image);
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) pick_file(window, "image", false);
    ImGui::SameLine();
    if (ImGui::Button("Folder...")) pick_file(window, "image", true);
    if (ImGui::TreeNode("Disc tools")) {
        fs::path found = find_dolphin_tool();
        ImGui::InputText("DolphinTool.exe", g_tool, sizeof g_tool);
        ImGui::SameLine();
        if (ImGui::Button("Browse...##tool")) pick_file(window, "tool", false);
        ImGui::TextDisabled("%s", found.empty() ? "Not found yet: it's in your Dolphin folder."
                                                : ("Using " + found.u8string()).c_str());
        if (std::string(g_tool) != setting("dolphin_tool")) {
            g_settings["dolphin_tool"] = g_tool;
            save_settings();
        }
        ImGui::TreePop();
    }
    ImGui::BeginDisabled(g_setup.busy || !g_image[0]);
    if (ImGui::Button("Set up the game", ImVec2(200, 0))) {
        std::thread(set_up_game, std::string(g_image)).detach();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextWrapped("%s", g_setup.get().c_str());

    ImGui::Separator();
    if (ready) ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.4f, 1), "Game ready: %s", game.u8string().c_str());
    else ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.3f, 1), "Game: %s", state.c_str());
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Play", ImVec2(200, 48))) {
        if (!start_program(game_command(L""), play_folder())) g_setup.set("Couldn't start the game.");
    }
    ImGui::EndDisabled();
    bool shake = rumble() != 0;
    if (ImGui::Checkbox("Wii Remote rumble", &shake)) set_gfx("rumble", shake);
    builder::Mode mode;
    if (builder::available(g_dir) && ready && builder::built(g_dir, kMainDolSha256, &mode) && ImGui::TreeNode("Speed (for slower PCs)")) {
        ImGui::BeginDisabled(g_setup.busy);
        if (mode == builder::Mode::Plain) {
            hint("The game can be built again to run faster (about 35 to 65% in tests, most in the sports you play while measuring), tuned by how it runs on this PC. Press Start measuring "
                 "(about 10 minutes), play normally for 10 minutes or more in a few different sports, close the game, then "
                 "press Finish (about 10 minutes more). While measuring, the game runs a little slower.");
            if (ImGui::Button("Start measuring")) std::thread(build_again, builder::Mode::Measuring).detach();
        } else if (mode == builder::Mode::Measuring) {
            int counts = builder::measured_files(g_dir);
            ImGui::TextWrapped(counts ? "Measured. Play more if you like, or close the game and press Finish."
                                      : "Measuring: play normally for 10 minutes or more, then close the game and press Finish.");
            ImGui::BeginDisabled(counts == 0);
            if (ImGui::Button("Finish: build the faster version")) std::thread(build_again, builder::Mode::Fast).detach();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) std::thread(build_again, builder::Mode::Plain).detach();
        } else {
            ImGui::TextWrapped("This is the faster version, tuned on this PC.");
            if (ImGui::Button("Measure again")) std::thread(build_again, builder::Mode::Measuring).detach();
        }
        ImGui::EndDisabled();
        ImGui::TreePop();
    }
    save_data_section(window);
    // A real Wii Remote's own Nunchuk works as it is: its reports pass straight to the game. The keys below are
    // for the keyboard's stand-in remote, used while no real one is connected.
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Keys: Z = A, X = B, H = Home, F12 = save a picture. Nunchuk: N plugs it in or takes it out "
                        "when the game asks (Archery, Cycling, and Power Cruising need it), I J K L stick, Q = C, "
                        "E = Z, F pumps it. With a Wii Remote and no sensor bar: C re-centres the pointer, F9 and "
                        "F10 flip it, and pressing A when asked to point at the screen centres it.");
    ImGui::PopTextWrapPos();
}

// ---- Graphics ----

// Grey help text that wraps at the window's edge.
void hint(const char* text) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
}

// A combo box over a list of values; returns true when the choice changed.
bool choose(const char* label, const char* key, int current, const int* values, const char* const* names, int count) {
    int at = 0;
    for (int i = 0; i < count; i++)
        if (values[i] == current) at = i;
    if (!ImGui::Combo(label, &at, names, count)) return false;
    set_gfx(key, values[at]);
    return true;
}

void graphics_tab() {
    ImGui::TextWrapped("Lower settings make the game easier on slower PCs. Changes apply the next time the game "
                       "starts. Resolution makes the biggest difference: each step draws many more pixels.");
    ImGui::Spacing();
    int preset = -1;
    for (int i = 0; i < 4; i++)
        if (kPresets[i].scale == gfx_scale() && kPresets[i].aniso == gfx_aniso()) preset = i;
    ImGui::Text("Preset:");
    for (int i = 0; i < 4; i++) {
        ImGui::SameLine();
        bool on = preset == i;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(kPresets[i].name, ImVec2(110, 0))) {
            set_gfx("gfx_scale", kPresets[i].scale);
            set_gfx("gfx_aniso", kPresets[i].aniso);
        }
        if (on) ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::TextDisabled(preset < 0 ? "(custom)" : "");
    hint("Low: for old or low-power PCs and laptops. Medium: most laptops. High: the default. "
                        "Ultra: strong graphics cards.");
    ImGui::Separator();

    static const int scales[] = {1, 2, 3, 4, 5, 6};
    static const char* const scale_names[] = {"1x  640 x 528 (fastest)", "2x  1280 x 1056", "3x  1920 x 1584 (default)",
                                              "4x  2560 x 2112", "5x  3200 x 2640", "6x  3840 x 3168 (4K screens)"};
    choose("Resolution", "gfx_scale", gfx_scale(), scales, scale_names, 6);
    hint("The size the game draws at, before it's fitted to the window. Above the window's own "
                        "size it also smooths jagged edges.");

    static const int anisos[] = {1, 2, 4, 8, 16};
    static const char* const aniso_names[] = {"Off (fastest)", "2x", "4x", "8x", "16x (default)"};
    choose("Texture filtering", "gfx_aniso", gfx_aniso(), anisos, aniso_names, 5);
    hint("Keeps the ground and far-off textures sharp.");

    bool vsync = gfx_vsync() != 0;
    if (ImGui::Checkbox("VSync", &vsync)) set_gfx("gfx_vsync", vsync);
    ImGui::SameLine();
    hint("waits for the screen; turn off if the picture stutters. The game's speed doesn't change.");
    bool fullscreen = gfx_fullscreen() != 0;
    if (ImGui::Checkbox("Start in full screen", &fullscreen)) set_gfx("gfx_fullscreen", fullscreen);

    static const int windows[] = {0, 1, 2, 3};
    static const char* const window_names[] = {"1280 x 720 (default)", "960 x 540", "1600 x 900", "1920 x 1080"};
    choose("Window size", "gfx_window", gfx_window(), windows, window_names, 4);

    static const int gpus[] = {0, 1};
    static const char* const gpu_names[] = {"Fastest graphics card (default)", "Power-saving graphics card"};
    choose("Graphics card", "gfx_gpu", gfx_gpu(), gpus, gpu_names, 2);
    hint("On a PC with two graphics chips (most laptops), the fastest one is used unless you pick power saving. The "
         "game always draws with a graphics card: the processor is busy running the game itself.");

    static const int backends[] = {0, 1, 2, 3};
    choose("Graphics API", "gfx_backend", gfx_backend(), backends, kBackendNames, 4);
    hint("Automatic picks Direct3D 12. If the game won't start or draws wrongly, try another; "
                        "Direct3D 11 suits older graphics cards.");
    ImGui::Separator();
    if (ImGui::Button("Back to the defaults")) {
        for (const char* key : {"gfx_scale", "gfx_aniso", "gfx_vsync", "gfx_fullscreen", "gfx_window", "gfx_backend", "gfx_gpu"})
            g_settings.erase(key);
        save_settings();
    }
    ImGui::PushTextWrapPos(0.0f);
    hint("Still slow on Low? These settings only lighten the graphics card's work, and the game itself runs on the "
         "processor (it's the Wii's code, translated), which is usually what holds a slow PC back. Close other "
         "programs, and on a laptop plug in the charger and pick the best performance power mode in Windows.");
    ImGui::PopTextWrapPos();
}

// ---- Look: light, dark, or as Windows is set ----

constexpr const char* kVersion = "1.0.0";
constexpr const char* kAuthor = "Lane Carle (crazyman12)";

int theme_setting() { return gfx("theme", 0, 0, 2); }  // 0 as Windows is set, 1 light, 2 dark

bool dark_now() {
    int t = theme_setting();
    return t == 2 || (t == 0 && SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK);
}

// The colors, and the window's title bar to match (Windows 10 20H1 and later, Windows 11).
void apply_theme(SDL_Window* window) {
    bool dark = dark_now();
    if (dark) ImGui::StyleColorsDark();
    else ImGui::StyleColorsLight();
    if (HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)) {
        BOOL on = dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &on, sizeof on);
    }
}

void about_tab(SDL_Window* window) {
    std::string v = builder::available(g_dir) ? builder::version(g_dir) : kVersion;
    ImGui::Text("ResortRecomp %s", v.c_str());
    ImGui::Text("Launcher and toolkit by %s", kAuthor);
    ImGui::Spacing();
    static const int themes[] = {0, 1, 2};
    static const char* const theme_names[] = {"As Windows is set", "Light", "Dark"};
    if (choose("Look", "theme", theme_setting(), themes, theme_names, 3)) apply_theme(window);
    ImGui::Separator();
    hint("ResortRecomp plays your own copy of Wii Sports Resort (USA, RZTE01) on a PC. It contains no part of the game: "
         "your own disc is translated and built on this PC. Wii Sports Resort is Nintendo's; this project isn't made or "
         "endorsed by Nintendo.");
    hint("Game code translated with DolRecomp (GPL-3.0). This project is GPL-3.0; see licenses/ for every part's licence.");
    hint("Mii pictures and icons: My Avatar Editor, (c) 2009 Trevor McCauley, MIT licence (licenses/MyAvatarEditor.txt).");
    hint("Built with Dear ImGui, SDL3, aurora with Dawn, and MinGW-w64 GCC.");
}

// ---- Online ----

char g_join[32] = "", g_public_ip[64] = "", g_name[32] = "", g_relay[128] = "", g_relay_room[8] = "";
int g_players = 2, g_port = kDefaultPort;

// Relay rooms: 6 characters from the room code alphabet. A new one each time the launcher starts.
void new_relay_room() {
    static const char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    unsigned char bytes[6];
    BCryptGenRandom(nullptr, bytes, sizeof bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    for (int i = 0; i < 6; i++) g_relay_room[i] = alphabet[bytes[i] % 32];
    g_relay_room[6] = 0;
}

// A relay room as typed: letters and digits only, read the way room codes are (O is 0, I and L are 1).
std::string relay_room_of(const char* typed) {
    std::string out;
    for (const char* c = typed; *c; c++) {
        char ch = char(std::toupper((unsigned char)*c));
        if (ch == 'O') ch = '0';
        if (ch == 'I' || ch == 'L') ch = '1';
        if (std::isalnum((unsigned char)ch)) out += ch;
    }
    return out;
}

void host_direct() {
    ImGui::InputInt("Port", &g_port);
    ImGui::InputText("Your internet address (optional)", g_public_ip, sizeof g_public_ip);
    hint("For players outside your home network: give them the internet code, forward this TCP port on your router to this PC, and let the game through Windows Firewall. A relay server above avoids all that.");
    std::string lan = room::local_address();
    std::string lan_code = room::encode(lan, uint16_t(g_port));
    ImGui::Text("Room code on your network: %s", lan_code.c_str());
    if (g_public_ip[0]) {
        std::string code = room::encode(g_public_ip, uint16_t(g_port));
        if (code.empty()) ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1), "That isn't an IPv4 address.");
        else ImGui::Text("Room code over the internet: %s", code.c_str());
    }
    if (ImGui::Button("Host", ImVec2(160, 0))) {
        g_settings["public_ip"] = g_public_ip;
        save_settings();
        std::wstring extra = L" --net-host " + std::to_wstring(g_port) + L" --net-players " + std::to_wstring(g_players) +
                             L" --net-name \"" + wide(g_name) + L"\"";
        start_program(game_command(extra), play_folder());
    }
}

void online_tab() {
    if (!game_problem().empty()) {
        ImGui::TextWrapped("Set up the game on the Play tab first.");
        return;
    }
    ImGui::TextWrapped("Everyone plays the host's game: its save and Miis are sent to the others when the room "
                       "starts, and each player's Wii Remote is one of the players. Any sport with a 2 to 4 player "
                       "mode works. Everyone needs this same build and their own copy of the game.");
    ImGui::Spacing();
    if (ImGui::InputText("Your name", g_name, sizeof g_name)) {
        g_settings["name"] = g_name;
        save_settings();
    }
    if (ImGui::InputText("Relay server (optional)", g_relay, sizeof g_relay)) {
        g_settings["relay"] = g_relay;
        save_settings();
    }
    hint("A server running wsr_relay, as address or address:port (port 51020 if left out). With one, "
                        "nobody forwards ports: everyone puts the same address here. Leave it empty to connect "
                        "straight to the host.");
    const bool relay = g_relay[0] != 0;
    if (ImGui::CollapsingHeader("Host a room", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderInt("Players", &g_players, 2, 4);
        if (relay) {
            ImGui::Text("Room code: %s", g_relay_room);
            ImGui::SameLine();
            if (ImGui::SmallButton("New code")) new_relay_room();
            hint("Give this code to the others. The room opens on the relay when you press Host.");
            if (ImGui::Button("Host", ImVec2(160, 0))) {
                std::wstring extra = L" --net-relay \"" + wide(g_relay) + L"\" --net-host-room " + wide(g_relay_room) +
                                     L" --net-players " + std::to_wstring(g_players) + L" --net-name \"" + wide(g_name) + L"\"";
                start_program(game_command(extra), play_folder());
            }
        } else {
            host_direct();
        }
    }
    if (ImGui::CollapsingHeader("Join a room", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputText("Room code", g_join, sizeof g_join, ImGuiInputTextFlags_CharsUppercase);
        std::string address;
        uint16_t port = 0;
        std::string relay_room = relay_room_of(g_join);
        bool relay_code = relay_room.size() == 6;
        bool good = relay_code ? relay : room::decode(g_join, &address, &port);
        if (g_join[0] && relay_code && !relay)
            ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1), "That's a relay room code: put the host's relay server above.");
        else if (g_join[0] && !good)
            ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1), "That code isn't complete or has a typo.");
        ImGui::BeginDisabled(!good);
        if (ImGui::Button("Join", ImVec2(160, 0))) {
            std::wstring extra = relay_code ? L" --net-relay \"" + wide(g_relay) + L"\" --net-join-room " + wide(relay_room)
                                            : L" --net-join " + wide(address) + L":" + std::to_wstring(port);
            extra += L" --net-name \"" + wide(g_name) + L"\"";
            start_program(game_command(extra), play_folder());
        }
        ImGui::EndDisabled();
    }
}

}  // namespace

int main(int argc, char** argv) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_dir = fs::path(self).parent_path();
    if (argc > 1 && std::string(argv[1]) == "--test") {  // headless checks
        int failures = mii::self_test() + room::self_test() + mii_render::self_test();
        return failures ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[1]) == "--test-build") { /* <disc image or folder> [measuring|fast|plain]: Set up without the window */
        load_settings();
        std::string mode = argc > 3 ? argv[3] : "";
        if (mode.empty()) set_up_game(argv[2]);
        else build_again(mode == "measuring" ? builder::Mode::Measuring : mode == "fast" ? builder::Mode::Fast : builder::Mode::Plain);
        std::string said = g_setup.get();
        std::printf("%s\n", said.c_str());
        return said.rfind("Not", 0) == 0 ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[1]) == "--test-save") return save_self_test(fs::u8path(argv[2])) ? 1 : 0;
    if (argc > 2 && std::string(argv[1]) == "--test-new-save") { /* sets up a save folder as Play would */
        g_settings["nand"] = argv[2];
        fs::path nand = nand_folder();
        return fs::exists(nand / "shared2" / "menu" / "FaceLib" / "RFL_DB.dat") ? 0 : 1;
    }
    if (argc > 3 && std::string(argv[1]) == "--test-mii-import") { /* <database> <.mii files...> */
        std::vector<std::string> files(argv + 3, argv + argc);
        std::string said = mii_maker_import_test(argv[2], files);
        std::printf("%s\n", said.c_str());
        return said.rfind("Added 0 ", 0) == 0 ? 1 : 0;
    }
    if (argc > 2 && std::string(argv[1]) == "--test-write-db") {  // two Miis in a test save, for the game to show
        mii::Database db;
        db.format();
        mii::Mii a;
        a.name = u"Ripley";
        a.creator = u"Launcher";
        a.mii_id = mii::new_mii_id();
        a.system_id = 0x52A1C0DEu;
        a.favorite_color = 5;
        mii::Mii b = a;
        b.name = u"Nova";
        b.female = 1;
        b.hair = 12;
        b.hair_color = 7;
        b.eye = 9;
        b.mouth = 4;
        b.favorite_color = 7;
        b.mii_id = mii::new_mii_id();
        db.put(0, a);
        db.put(1, b);
        std::string error;
        return db.save(argv[2], &error) ? 0 : 1;
    }
    load_settings();
    std::snprintf(g_tool, sizeof g_tool, "%s", setting("dolphin_tool").c_str());
    std::snprintf(g_public_ip, sizeof g_public_ip, "%s", setting("public_ip").c_str());
    std::snprintf(g_name, sizeof g_name, "%s", setting("name", "Player").c_str());
    std::snprintf(g_relay, sizeof g_relay, "%s", setting("relay").c_str());
    new_relay_room();
    if (argc > 3 && std::string(argv[1]) == "--shot") std::snprintf(g_relay, sizeof g_relay, "relay.example.net"); /* pictures show the relay layout; not saved */

    // --shot <folder> <database>: draws each Mii Maker page off-screen in a hidden window and saves it as a BMP,
    // so the look can be checked without capturing the screen.
    const bool shot = argc > 3 && std::string(argv[1]) == "--shot";
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    SDL_Window* window = SDL_CreateWindow("ResortRecomp", 1100, 720,
                                          shot ? SDL_WINDOW_HIDDEN : (SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY));
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    SDL_SetRenderVSync(renderer, 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    float scale = SDL_GetWindowDisplayScale(window);
    if (scale <= 0) scale = 1;
    if (fs::exists("C:/Windows/Fonts/segoeui.ttf")) io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 18.0f * scale);
    apply_theme(window);
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FrameRounding = 4;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    mii_maker_set_renderer(renderer);

    SDL_Texture* target = nullptr;
    int shot_page = -1, shot_wait = 0;
    if (shot) {
        target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 1100, 720);
        io.DisplaySize = ImVec2(1100, 720);
    }
    bool running = true;
    while (running) {
        if (shot) { /* each page gets a few frames to settle and its pictures made, then is saved */
            if (shot_page < 0 || (++shot_wait >= 4 && (!mii_maker_busy() || shot_wait > 200))) {
                if (shot_page >= 0) {
                    SDL_Surface* pixels = SDL_RenderReadPixels(renderer, nullptr);
                    if (pixels) {
                        std::string name = shot_page < mii_maker_pages() ? "mii-maker-" + std::to_string(shot_page)
                                         : shot_page == mii_maker_pages() ? std::string("online")
                                         : shot_page == mii_maker_pages() + 1 ? std::string("graphics")
                                         : shot_page == mii_maker_pages() + 2 ? std::string("about-light") : std::string("about-dark");
                        std::string out = std::string(argv[2]) + "/" + name + ".bmp";
                        SDL_SaveBMP(pixels, out.c_str());
                        SDL_DestroySurface(pixels);
                    }
                }
                if (++shot_page >= mii_maker_pages() + 4) break; /* then Online, Graphics, About light and dark */
                if (shot_page >= mii_maker_pages() + 2) {
                    g_settings["theme"] = shot_page == mii_maker_pages() + 2 ? "1" : "2"; /* not saved */
                    apply_theme(window);
                }
                if (shot_page < mii_maker_pages()) mii_maker_show(argv[3], shot_page);
                shot_wait = 0;
            }
            SDL_SetRenderTarget(renderer, target);
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) running = false;
            if (e.type == SDL_EVENT_SYSTEM_THEME_CHANGED && theme_setting() == 0) apply_theme(window);
        }
        {
            std::lock_guard<std::mutex> lock(g_pick_lock);
            if (!g_picked.empty()) {
                if (g_pick_target == "tool") std::snprintf(g_tool, sizeof g_tool, "%s", g_picked.c_str());
                else std::snprintf(g_image, sizeof g_image, "%s", g_picked.c_str());
                g_picked.clear();
            }
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        if (shot) io.DisplaySize = ImVec2(1100, 720);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Play")) {
                play_tab(window);
                ImGui::EndTabItem();
            }
            const int pages = mii_maker_pages();
            auto pick = [&](bool on) { return shot && on ? ImGuiTabItemFlags_SetSelected : 0; };
            if (ImGui::BeginTabItem("Mii Maker", nullptr, pick(shot_page < pages))) {
                mii_maker_tab(shot ? std::string(argv[3]) : mii_db_path().u8string());
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Wii Remote")) {
                remote_tab(g_dir, play_folder());
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Online", nullptr, pick(shot_page == pages))) {
                online_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Graphics", nullptr, pick(shot_page == pages + 1))) {
                graphics_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("About", nullptr, pick(shot_page >= pages + 2))) {
                about_tab(window);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
        ImGui::Render();
        ImVec4 back = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        SDL_SetRenderDrawColor(renderer, Uint8(back.x * 255), Uint8(back.y * 255), Uint8(back.z * 255), 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (!shot) SDL_RenderPresent(renderer);
        else SDL_FlushRenderer(renderer);
    }
    mii_maker_release();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
