// The launcher's Wii Remote tab: checks whether a Wii Remote is connected, and pairs one by running
// PairRemote.exe (native/tools/pair_remote.c) with --pipe, showing what it prints and passing the
// "I pressed SYNC" click to it as a line on its standard input.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "imgui.h"
#include "remote_tab.h"

extern "C" {
#include "real_remote.h"
FILE* report; // real_remote.c writes notes here; the launcher keeps them out of sight
}

namespace fs = std::filesystem;

namespace {

struct Pairing {
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

    bool start(const fs::path& exe, const fs::path& folder, const fs::path& log) {
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
        std::wstring cmd = L"\"" + exe.wstring() + L"\" --pipe 1 --seconds 300 --log \"" + log.wstring() + L"\"";
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

    ~Pairing() {
        stop();
        if (reader.joinable()) reader.join();
        close_handles();
    }
};

Pairing g_pairing;
std::atomic<int> g_found{-1}; // -1 not checked, 0 none, 1 connected
std::atomic<bool> g_checking{false};

void check_remote() {
    if (g_checking.exchange(true)) return;
    std::thread([] {
        uint32_t devices = 0;
        g_found = real_remote_scan_once(&devices) ? 1 : 0;
        g_checking = false;
    }).detach();
}

}  // namespace

void remote_tab(const fs::path& launcher_dir, const fs::path& work_folder) {
    static bool first = true;
    if (!report) report = fopen("NUL", "w");
    if (first) {
        first = false;
        check_remote();
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("Pair a Wii Remote with this PC once. Windows' own Bluetooth screen usually can't finish "
                           "pairing one, so this does it, then tests the remote's light, rumble, and A button. "
                           "With no remote connected the game uses the keyboard and mouse.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (g_checking) ImGui::TextDisabled("Looking for a Wii Remote...");
    else if (g_found == 1) ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.4f, 1), "A Wii Remote is connected.");
    else if (g_found == 0) ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.3f, 1), "No Wii Remote is connected.");
    ImGui::SameLine();
    ImGui::BeginDisabled(g_checking);
    if (ImGui::Button("Check again")) check_remote();
    ImGui::EndDisabled();
    ImGui::Separator();

    fs::path exe = launcher_dir / "PairRemote.exe";
    bool have_tool = fs::exists(exe);
    if (!have_tool) ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1), "PairRemote.exe is missing beside the launcher.");
    ImGui::BeginDisabled(!have_tool || g_pairing.running);
    if (ImGui::Button("Pair a Wii Remote", ImVec2(200, 40))) {
        if (!g_pairing.start(exe, work_folder, launcher_dir / "pair-remote.txt")) {
            const char* message = "Couldn't start PairRemote.exe.\n";
            g_pairing.add(message, std::strlen(message));
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_pairing.running);
    if (ImGui::Button("I pressed SYNC", ImVec2(200, 40))) g_pairing.press();
    ImGui::SameLine();
    if (ImGui::Button("Stop", ImVec2(100, 40))) g_pairing.stop();
    ImGui::EndDisabled();
    if (!g_pairing.running && g_pairing.exit_code >= 0) {
        ImGui::SameLine();
        if (g_pairing.exit_code == 0) {
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.4f, 1), "Done: the remote works.");
            if (g_found != 1 && !g_checking) check_remote();
        } else {
            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.3f, 1), "It didn't finish. Try again.");
        }
    }
    ImGui::TextDisabled("The SYNC button is the small red one under the battery cover. Press it, then the button "
                        "above straight away: the remote only listens for about 20 seconds.");

    ImGui::BeginChild("pair-output", ImVec2(0, 0), ImGuiChildFlags_Borders);
    std::string text = g_pairing.get();
    ImGui::TextUnformatted(text.c_str());
    if (g_pairing.scroll) {
        ImGui::SetScrollHereY(1.0f);
        g_pairing.scroll = false;
    }
    ImGui::EndChild();
}
