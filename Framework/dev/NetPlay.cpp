#include "dev/NetPlay.h"
#include "dev/DevBuild.h"
#include "devui/imeditor.h"

#include "wiHelper.h"
#include "wiBacklog.h"

#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace st::dev {

namespace {
const char* const kMap = "netplay/netplay.stsd";   // relative to the working directory (the exe's)
}

NetPlay& NetPlay::Get () {
    static NetPlay n;
    return n;
}

bool NetPlay::Launch (const std::string& arguments) {
#ifdef _WIN32
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" ";
    cmd += std::wstring(arguments.begin(), arguments.end());
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
        error_ = "could not start " + arguments + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    processes_.push_back({ pi.hProcess, pi.dwProcessId });
    return true;
#else
    (void)arguments;
    error_ = "NetPlay launches processes on Windows only for now";
    return false;
#endif
}

bool NetPlay::Start (EditorUI& editor, wi::scene::Scene& scene, int clients, bool serverWindow) {
    error_.clear();
    Stop();
    if (!IsDevBuild()) {
        error_ = "NetPlay is a dev build tool";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories("netplay", ec);
    if (!editor.SaveSceneCopy(scene, kMap)) {
        error_ = "could not save the scene to " + std::string(kMap) + " (Play mode refuses saves - press Stop first)";
        return false;
    }
    const std::string common = std::string("--scene-file=") + kMap + " --no-devsync";
    const std::string port = std::to_string(kServerPort);
    if (!Launch(common + " --server=" + port + " --devbridge-port=" + std::to_string(kBridgePortBase) +
                (serverWindow ? " --server-window" : ""))) {
        wi::backlog::post("NetPlay: " + error_, wi::backlog::LogLevel::Error);
        return false;
    }
    // Clients retry until the server listens, so they need no head start.
    for (int i = 1; i <= clients; ++i) {
        if (!Launch(common + " --connect=127.0.0.1:" + port + " --name=Player" + std::to_string(i) +
                    " --devbridge-port=" + std::to_string(kBridgePortBase + i))) {
            wi::backlog::post("NetPlay: " + error_, wi::backlog::LogLevel::Error);
            break;
        }
    }
    wi::backlog::post("NetPlay: server (UDP " + port + ", bridge " + std::to_string(kBridgePortBase) + ") + " +
                      std::to_string(clients) + " client(s) started from " + kMap);
    return true;
}

void NetPlay::Stop () {
#ifdef _WIN32
    for (Process& p : processes_) {
        TerminateProcess((HANDLE)p.process, 0);
        CloseHandle((HANDLE)p.process);
    }
#endif
    if (!processes_.empty()) wi::backlog::post("NetPlay: stopped " + std::to_string(processes_.size()) + " process(es)");
    processes_.clear();
}

int NetPlay::Running () {
#ifdef _WIN32
    for (auto it = processes_.begin(); it != processes_.end();) {
        if (WaitForSingleObject((HANDLE)it->process, 0) == WAIT_OBJECT_0) {
            CloseHandle((HANDLE)it->process);
            it = processes_.erase(it);
        } else {
            ++it;
        }
    }
#endif
    return (int)processes_.size();
}

} // namespace st::dev
