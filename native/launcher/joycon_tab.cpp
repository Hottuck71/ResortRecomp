// The launcher's Joy-Con tab, apart from the Wii Remote tab: shows which Nintendo Switch Joy-Cons are connected,
// and pairs them by running PairJoyCon.exe (native/tools/pair_joycon.c) with --pipe, showing what it prints and
// passing the "the lights are moving" click to it as a line on its standard input.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "imgui.h"
#include "joycon_tab.h"

extern "C" {
#include "joycon.h"
extern FILE* report; // remote_tab.cpp's: joycon.c writes notes there, out of sight
}

namespace fs = std::filesystem;

namespace {

// How Joy-Cons play in this game (shown on the tab).
const char* const kHowToPlay =
    "In the game the right Joy-Con is the Wii Remote and a left Joy-Con is its Nunchuk (for Archery, Cycling and "
    "Power Cruising). Hold the right one upright like a Wii Remote, its top towards the screen: A = A, ZR = B, "
    "stick = d-pad, Y = 1, B = 2, X = -, + = +, HOME = HOME. R or pressing the stick re-centres the pointer. Left "
    "Joy-Con: stick = Nunchuk stick, ZL = Z, L = C. A left Joy-Con on its own is the Wii Remote instead (right "
    "arrow = A, ZL = B). Swings, tilts and pointing come from the Joy-Con's own motion sensors: no sensor bar. SL "
    "and SR together switch a Joy-Con to sideways. A connected Wii Remote is used before Joy-Cons.";

struct PairProcess {
    std::mutex lock;
    std::string text;
    std::atomic<bool> running{false};
    std::atomic<int> exit_code{-1};
    HANDLE process = nullptr, input = nullptr, output = nullptr;
    std::thread reader;
    bool scroll = false;

    void add(const char* data, size_t size) {
        std::lock_guard<std::mutex> l(lock);
        for (size_t i = 0; i < size; i++)
            if (data[i] != '\r') text += data[i];
        scroll = true;
    }

    std::string get() {
        std::lock_guard<std::mutex> l(lock);
        return text;
    }

    void close_handles() {
        if (input) CloseHandle(input);
        if (output) CloseHandle(output);
        if (process) CloseHandle(process);
        input = output = process = nullptr;
    }

    bool start(const fs::path& exe, const fs::path& folder, const std::wstring& args) {
        if (running) return false;
        if (reader.joinable()) reader.join();
        close_handles();
        {
            std::lock_guard<std::mutex> l(lock);
            text.clear();
        }
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE out_read, out_write, in_read, in_write;
        if (!CreatePipe(&out_read, &out_write, &sa, 0)) return false;
        if (!CreatePipe(&in_read, &in_write, &sa, 0)) {
            CloseHandle(out_read);
            CloseHandle(out_write);
            return false;
        }
        SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0); // the launcher's ends stay here
        SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{sizeof si};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in_read;
        si.hStdOutput = out_write;
        si.hStdError = out_write;
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
        BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                      folder.wstring().c_str(), &si, &pi);
        CloseHandle(out_write);
        CloseHandle(in_read);
        if (!started) {
            CloseHandle(out_read);
            CloseHandle(in_write);
            return false;
        }
        CloseHandle(pi.hThread);
        process = pi.hProcess;
        input = in_write;
        output = out_read;
        exit_code = -1;
        running = true;
        reader = std::thread([this] {
            char buffer[512];
            DWORD got = 0;
            while (ReadFile(output, buffer, sizeof buffer, &got, nullptr) && got) add(buffer, got);
            WaitForSingleObject(process, INFINITE);
            DWORD code = 1;
            GetExitCodeProcess(process, &code);
            exit_code = int(code);
            running = false;
        });
        return true;
    }

    void press() {
        if (!running || !input) return;
        DWORD done = 0;
        WriteFile(input, "\n", 1, &done, nullptr);
    }

    void stop() {
        if (running && process) TerminateProcess(process, 1);
    }

    ~PairProcess() {
        stop();
        if (reader.joinable()) reader.join();
        close_handles();
    }
};

PairProcess g_pairing;
std::atomic<int> g_left{-1}, g_right{-1}; // -1 not checked yet
std::atomic<bool> g_checking{false};
bool g_recheck = false; // look again once the tool finishes

void check_joycons() {
    if (g_checking.exchange(true)) return;
    std::thread([] {
        u32 left = 0, right = 0;
        joycon_scan_once(&left, &right);
        g_left = int(left);
        g_right = int(right);
        g_checking = false;
    }).detach();
}

}  // namespace

void joycon_tab(const fs::path& launcher_dir, const fs::path& work_folder) {
    static bool first = true;
    if (!report) report = fopen("NUL", "w");
    if (first) {
        first = false;
        check_joycons();
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("Nintendo Switch Joy-Cons can play instead of Wii Remotes. Pair each Joy-Con with this PC "
                           "once here; this then tests its lights, rumble, buttons and motion sensor.");
    ImGui::Spacing();
    ImGui::TextDisabled("%s", kHowToPlay);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (g_checking) {
        ImGui::TextDisabled("Looking for Joy-Cons...");
    } else if (g_left >= 0) {
        int total = g_left + g_right;
        if (total) ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.4f, 1), "Connected: %d left Joy-Con%s, %d right Joy-Con%s.",
                                      g_left.load(), g_left == 1 ? "" : "s", g_right.load(), g_right == 1 ? "" : "s");
        else ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.3f, 1), "No Joy-Con is connected.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(g_checking);
    if (ImGui::Button("Check again##joycon")) check_joycons();
    ImGui::EndDisabled();
    ImGui::Separator();

    fs::path exe = launcher_dir / "PairJoyCon.exe";
    fs::path log = launcher_dir / "pair-joycon.txt";
    bool have_tool = fs::exists(exe);
    if (!have_tool) ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1), "PairJoyCon.exe is missing beside the launcher.");
    ImGui::BeginDisabled(!have_tool || g_pairing.running);
    if (ImGui::Button("Pair Joy-Cons", ImVec2(200, 40))) {
        g_recheck = true;
        if (!g_pairing.start(exe, work_folder, L"--pipe 1 --seconds 300 --log \"" + log.wstring() + L"\"")) {
            const char* message = "Couldn't start PairJoyCon.exe.\n";
            g_pairing.add(message, std::strlen(message));
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_pairing.running);
    if (ImGui::Button("The lights are moving", ImVec2(200, 40))) g_pairing.press();
    ImGui::SameLine();
    if (ImGui::Button("Stop##joycon", ImVec2(100, 40))) g_pairing.stop();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!have_tool || g_pairing.running || g_left + g_right <= 0);
    if (ImGui::Button("Test connected", ImVec2(150, 40))) {
        if (!g_pairing.start(exe, work_folder, L"--pipe 1 --test 1 --log \"" + log.wstring() + L"\"")) {
            const char* message = "Couldn't start PairJoyCon.exe.\n";
            g_pairing.add(message, std::strlen(message));
        }
    }
    ImGui::EndDisabled();
    if (!g_pairing.running && g_pairing.exit_code >= 0) {
        ImGui::SameLine();
        if (g_pairing.exit_code == 0) {
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.4f, 1), "Done: the Joy-Cons work.");
            if (g_recheck && !g_checking) {
                g_recheck = false;
                check_joycons();
            }
        } else {
            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.3f, 1), "It didn't finish. Try again.");
        }
    }
    ImGui::TextDisabled("The sync button is the small round one on the Joy-Con's rail. Hold it until the green lights "
                        "move back and forth, then press the button above straight away.");

    ImGui::BeginChild("joycon-output", ImVec2(0, 0), ImGuiChildFlags_Borders);
    std::string text = g_pairing.get();
    ImGui::TextUnformatted(text.c_str());
    if (g_pairing.scroll) {
        ImGui::SetScrollHereY(1.0f);
        g_pairing.scroll = false;
    }
    ImGui::EndChild();
}
