// Builds the game on the player's own PC. The toolkit holds no part of the game: the player's own main.dol is
// translated to C here with DolRecomp, the runtime's hooks are added, and the toolkit's compiler builds it.
//
//   builder/dolrecomp.exe      the translator (DolRecomp, GPL-3.0), and zlib's DLL it uses
//   builder/src/               this project's runtime, as source, and replacements.txt (the hooked functions)
//   builder/dolrecomp/         DolRecomp's CPU header and helpers, compiled with the game
//   builder/lib/               the graphics library (aurora and what it uses), prebuilt, and link.rsp
//   builder/toolchain/         GCC and binutils (MSYS2's MinGW-w64 UCRT64 packages), and mingw32-make
//   builder/game.mk            the build
// Work goes to build/ beside the launcher, the game to ResortRecomp-Game.exe, a note of what was built to
// game-build.txt, and the compiler's output to build-log.txt.
#include "builder.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

namespace builder {
namespace {

std::wstring quote(const fs::path& p) { return L"\"" + p.wstring() + L"\""; }

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

// Runs a program in cwd with the toolkit's compiler first on PATH, its output appended to dir's log. tick() is
// called about once a second while it runs. Returns the exit code, or -1 if it couldn't start.
int run(const fs::path& dir, const fs::path& cwd, const std::wstring& command, const std::function<void()>& tick) {
    std::wstring path = (dir / "builder" / "toolchain" / "bin").wstring() + L";" + (dir / "builder").wstring() + L";";
    wchar_t old_path[32768];
    DWORD got = GetEnvironmentVariableW(L"PATH", old_path, 32768);
    std::wstring new_path = path + (got && got < 32768 ? std::wstring(old_path) : L"");
    // The child gets this environment block: everything as it is, with PATH changed.
    std::wstring block;
    LPWCH env = GetEnvironmentStringsW();
    for (LPWCH p = env; *p; p += wcslen(p) + 1) {
        if (_wcsnicmp(p, L"PATH=", 5) == 0) continue;
        block += p;
        block.push_back(L'\0');
    }
    FreeEnvironmentStringsW(env);
    block += L"PATH=" + new_path;
    block.push_back(L'\0');
    block.push_back(L'\0');

    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE log = CreateFileW((dir / "build-log.txt").wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &inherit, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = log;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = command;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                             block.data(), cwd.wstring().c_str(), &si, &pi);
    if (!ok) {
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        return -1;
    }
    while (WaitForSingleObject(pi.hProcess, 1000) == WAIT_TIMEOUT)
        if (tick) tick();
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    return int(code);
}

void note(const fs::path& dir, const std::string& line) {
    std::ofstream f(dir / "build-log.txt", std::ios::app);
    f << "== " << line << "\n";
}

// The runtime replaces some of the game's functions with its own. A call inside one chunk jumps straight to its
// target, so each hook goes right after the function's entry label (what scripts/44_patch_replacements.py does
// for this project's own build). Every chunk is copied to build/chunks with the include made local.
std::string add_hooks(const fs::path& dir) {
    std::vector<std::string> addresses;
    std::istringstream list(read_text(dir / "builder" / "src" / "replacements.txt"));
    std::string line;
    while (std::getline(list, line)) {
        line = line.substr(0, line.find('#'));
        std::istringstream words(line);
        std::string address;
        if (words >> address) {
            for (char& c : address) c = char(toupper((unsigned char)c));
            addresses.push_back(address);
        }
    }
    fs::path from = dir / "build" / "recomp" / "generated" / "chunks", to = dir / "build" / "chunks";
    std::error_code ec;
    fs::create_directories(to, ec);
    struct Chunk {
        uint32_t start;
        fs::path path;
    };
    std::vector<Chunk> chunks;
    static const std::regex start_of(R"(_([0-9A-F]{8})\.c$)");
    for (auto& e : fs::directory_iterator(from, ec)) {
        std::smatch m;
        std::string name = e.path().filename().string();
        if (std::regex_search(name, m, start_of)) chunks.push_back({uint32_t(std::stoul(m[1].str(), nullptr, 16)), e.path()});
    }
    if (chunks.empty()) return "The translation has no code in it. See build-log.txt.";
    std::sort(chunks.begin(), chunks.end(), [](const Chunk& a, const Chunk& b) { return a.start < b.start; });
    std::vector<std::vector<std::string>> hooks(chunks.size());
    for (const std::string& a : addresses) {
        uint32_t value = uint32_t(std::stoul(a, nullptr, 16));
        size_t owner = 0;
        for (size_t i = 0; i < chunks.size() && chunks[i].start <= value; i++) owner = i;
        hooks[owner].push_back(a);
    }
    for (size_t i = 0; i < chunks.size(); i++) {
        std::string text = read_text(chunks[i].path);
        const std::string include = "#include \"../generated.h\"";
        size_t at = text.find(include);
        if (at != std::string::npos)
            text.replace(at, include.size(), "#include \"generated.h\"\nint native_replaced(CPUState* ctx, u32 pc);");
        for (const std::string& a : hooks[i]) {
            // The translator writes Windows line endings; the hook goes after the label's line, whichever ending.
            std::string label = "label_" + a + ":";
            size_t pos = text.find(label);
            size_t end = pos == std::string::npos ? pos : text.find('\n', pos);
            if (pos == std::string::npos || end == std::string::npos || text.find(label, pos + 1) != std::string::npos)
                return "The translation doesn't have the function at " + a + " where the runtime expects it.";
            text.insert(end + 1, "    if (native_replaced(ctx, 0x" + a + "u)) return;\n");
        }
        std::ofstream out(to / chunks[i].path.filename(), std::ios::binary);
        out << text;
        if (!out) return "Couldn't write " + (to / chunks[i].path.filename()).u8string() + ".";
    }
    return "";
}

int count_files(const fs::path& folder, const std::string& extension) {
    int n = 0;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(folder, ec))
        if (e.path().extension() == extension) n++;
    return n;
}

std::string mode_name(Mode m) { return m == Mode::Measuring ? "measuring" : m == Mode::Fast ? "fast" : "plain"; }

}  // namespace

bool available(const fs::path& dir) { return fs::exists(dir / "builder" / "game.mk"); }

std::string version(const fs::path& dir) { return trim(read_text(dir / "builder" / "version.txt")); }

fs::path game_exe(const fs::path& dir) { return dir / "ResortRecomp-Game.exe"; }

bool built(const fs::path& dir, const std::string& dol_hash, Mode* mode) {
    if (!fs::exists(game_exe(dir))) return false;
    std::istringstream s(read_text(dir / "game-build.txt"));
    std::string v, h, m;
    std::getline(s, v);
    std::getline(s, h);
    std::getline(s, m);
    if (trim(v) != version(dir) || trim(h) != dol_hash) return false;
    if (mode) *mode = trim(m) == "measuring" ? Mode::Measuring : trim(m) == "fast" ? Mode::Fast : Mode::Plain;
    return true;
}

int measured_files(const fs::path& dir) { return count_files(dir / "build" / "obj", ".gcda"); }

std::string build(const fs::path& dir, const fs::path& main_dol, const std::string& dol_hash, Mode mode,
                  const std::function<void(const std::string&)>& say) {
    std::error_code ec;
    fs::path work = dir / "build", obj = work / "obj";
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
    note(dir, "building (" + mode_name(mode) + ") with toolkit " + version(dir));

    if (mode != Mode::Fast) {
        // A fresh translation each time: it takes seconds, and old objects or counts can't leak in.
        fs::remove_all(work, ec);
        if (fs::exists(work)) return "Couldn't clear the old build folder: is the game still running?";
        fs::create_directories(work / "recomp", ec);
        say("Translating your game's code (this part takes seconds)...");
        // It runs in its own output folder, as it may write beside where it runs.
        int code = run(dir, work / "recomp", quote(dir / "builder" / "dolrecomp.exe") + L" --cpu broadway -j" + std::to_wstring(jobs) + L" " +
                                quote(main_dol) + L" RZTE01 " + quote(work / "recomp"), nullptr);
        if (code != 0) return "The translator stopped (exit code " + std::to_string(code) + "). See build-log.txt.";
        say("Adding the runtime's hooks...");
        std::string problem = add_hooks(dir);
        if (!problem.empty()) return problem;
        fs::remove_all(work / "recomp" / "generated" / "chunks", ec);  // build/chunks has the copies now
    } else {
        if (measured_files(dir) == 0) return "There are no measurements yet: play the game for a while first, then close it.";
        // The counts stay; the objects go, so everything is compiled again with them.
        for (auto& e : fs::directory_iterator(obj, ec))
            if (e.path().extension() == ".o") fs::remove(e.path(), ec);
    }

    int total = count_files(work / "chunks", ".c") + count_files(dir / "builder" / "src", ".c") + 1;
    std::atomic<bool> done{false};
    auto progress = [&]() {
        int made = count_files(obj, ".o");
        const char* what = mode == Mode::Fast ? "Building the faster version" : mode == Mode::Measuring ? "Building the measuring version"
                                                                                                         : "Building the game";
        say(std::string(what) + ": " + std::to_string(std::min(made, total)) + " of " + std::to_string(total) +
            " parts compiled. This takes a while; the launcher can stay open.");
    };
    progress();
    std::wstring pgo = mode == Mode::Measuring ? L" PGO=gen" : mode == Mode::Fast ? L" PGO=use" : L"";
    fs::remove(game_exe(dir), ec);
    if (fs::exists(game_exe(dir))) return "Couldn't replace the game: close it first.";
    std::string v = version(dir);  // one copy: a range needs both ends from the same string
    int code = run(dir, dir, quote(dir / "builder" / "toolchain" / "bin" / "mingw32-make.exe") + L" -f builder/game.mk -j" +
                            std::to_wstring(jobs) + L" VERSION=" + std::wstring(v.begin(), v.end()) + pgo,
                   progress);
    if (code != 0 || !fs::exists(game_exe(dir)))
        return "The compiler stopped (exit code " + std::to_string(code) + "). The end of build-log.txt says why.";

    std::ofstream stamp(dir / "game-build.txt", std::ios::trunc);
    stamp << version(dir) << "\n" << dol_hash << "\n" << mode_name(mode) << "\n";
    if (mode == Mode::Plain) fs::remove_all(work, ec);  // nothing more is needed from it
    note(dir, "built (" + mode_name(mode) + ")");
    return "";
}

}  // namespace builder
