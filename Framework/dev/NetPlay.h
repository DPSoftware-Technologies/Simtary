#pragma once
// NetPlay - "Play: server + N clients" from the editor (MULTIPLAYER.md v2, step 4), Roblox
// Studio's local server test.
//
// Start() saves the open scene to <exe>/netplay/netplay.stsd (a copy: the editor's own document
// path is kept), then starts this executable again N + 1 times:
//
//   server   --scene-file=netplay/netplay.stsd --server=27100 --devbridge-port=57240
//            (headless unless `serverWindow`)
//   client i --scene-file=... --connect=127.0.0.1:27100 --name=Player<i> --devbridge-port=5724<i>
//
// The editor itself stays in Edit mode and is not part of the game. Stop() (or closing the
// editor) ends every process it started. Dev build, Windows only for now.
//
// Main thread only.

#include <string>
#include <vector>

namespace wi::scene { struct Scene; }

namespace st {
class EditorUI;
}

namespace st::dev {

class NetPlay {
public:
    static NetPlay& Get ();

    static constexpr int kServerPort = 27100;
    static constexpr int kBridgePortBase = 57240;   // server; clients 57241..

    bool Start (EditorUI& editor, wi::scene::Scene& scene, int clients, bool serverWindow);
    void Stop ();
    // Processes still alive (reaps the ones that exited).
    int Running ();
    const std::string& LastError () const { return error_; }

private:
    NetPlay () = default;
    bool Launch (const std::string& arguments);

    struct Process {
        void* process = nullptr;   // HANDLE
        unsigned long id = 0;
    };
    std::vector<Process> processes_;
    std::string error_;
};

} // namespace st::dev
