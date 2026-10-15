#pragma once
// EditorBridge - lets a tool drive the running game: the dev build's back door for
// automation, and what tools/mcp/simtary_mcp.py exposes to an AI assistant as MCP tools.
//
// Protocol: TCP on 127.0.0.1 only, one JSON object per line each way.
//
//     -> {"token": "...", "cmd": "entity.list", "args": {"filter": "Wheel"}}
//     <- {"ok": true, "result": {...}}            or  {"ok": false, "error": "..."}
//
// The port and a random token are written to <working dir>/devbridge.json at startup; a
// request without the token is refused. Requests are only EXECUTED on the main thread,
// between frames (st::App::Update calls Pump()), so the scene is never touched from the
// socket thread. Commands that take time (a scene load, an injected key hold) return at
// once; the caller polls.
//
// Compiled into every build, started only in a dev build (SIMTARY_DEV_BUILD) - a shipped
// game opens no port. `--no-devbridge` keeps it shut in a dev build too.
//
// Commands: see CommandList() / the "help" command, which returns every name with its
// argument shape. The same list is what the MCP server turns into tools.

#include "dev/Json.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace st { class App; }

namespace st::dev {

class EditorBridge {
public:
    static EditorBridge& Get ();

    // Bind 127.0.0.1:port (0 = the default, 57230) and write devbridge.json. False when the
    // port could not be opened; the game runs on either way.
    bool Start (App& app, int port = 0);
    void Stop ();
    bool Running () const { return running_.load(); }
    int  Port () const { return port_; }

    // Main thread, once a frame: run whatever requests arrived.
    void Pump ();

    // name -> one-line description with the argument shape, for "help" and the MCP server.
    const std::map<std::string, std::string>& CommandList () const { return help_; }

    // Add a command. Public so a GAME can expose its own (Milistry: explosion.detonate);
    // the handler runs on the main thread inside Pump(), like the built-in ones.
    using Handler = std::function<json::Value(const json::Value& args, std::string& error)>;
    void Register (const std::string& name, const std::string& help, Handler fn);

private:
    void RegisterAll ();
    void ThreadMain ();
    std::string Execute (const std::string& line);

    struct Pending {
        std::string request;
        std::string response;
        bool done = false;
    };

    App* app_ = nullptr;
    int  port_ = 0;
    std::string infoFile_ = "devbridge.json";
    std::string token_;
    std::atomic<bool> running_{ false };
    std::thread thread_;
    uintptr_t listenSocket_ = ~uintptr_t(0);

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Pending*> queue_;

    std::map<std::string, Handler> handlers_;
    std::map<std::string, std::string> help_;
    int shotCounter_ = 0;
};

} // namespace st::dev
