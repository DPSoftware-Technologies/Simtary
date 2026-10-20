#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "dev/EditorBridge.h"
#include "dev/DevBuild.h"

#include "stApp.h"
#include "scene/PlayControl.h"
#include "scene/SceneFile.h"
#include "scene/ObjectTemplate.h"
#include "scene/TemplatePool.h"
#include "render/MeshLOD.h"
#include "script/ScriptSystem.h"
#include "input/InputActions.h"
#include "io/model/ModelImporter.h"
#include "terrain/TerrainSystem.h"
#include "terrain/TerrainGenerate.h"
#include "vehicle/VehicleComponent.h"
#include "ui/UISystem.h"
#include "ui/UIRml.h"

#include "wiScene.h"
#include "wiPhysics.h"
#include "wiHelper.h"
#include "wiBacklog.h"
#include "wiTimer.h"
#include "stNativeComponent.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

using st::json::Value;
using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;
using WScene = wi::scene::Scene; // not "Scene": Framework/stScene.h has a global one

namespace st::dev {

namespace {

constexpr int kDefaultPort = 57230;

#ifdef _WIN32
using Socket = SOCKET;
const Socket kBadSocket = INVALID_SOCKET;
void CloseSocket (Socket s) { closesocket(s); }
#else
using Socket = int;
const Socket kBadSocket = -1;
void CloseSocket (Socket s) { close(s); }
#endif

// ------------------------------------------------------------- json helpers ---

bool ReadVec3 (const Value& v, XMFLOAT3& out) {
    if (!v.IsArray() || v.size() < 3) return false;
    out = XMFLOAT3(v.At(0).AsFloat(), v.At(1).AsFloat(), v.At(2).AsFloat());
    return true;
}

Value Vec3 (const XMFLOAT3& v) {
    Value a = Value::Array();
    a.push_back(v.x); a.push_back(v.y); a.push_back(v.z);
    return a;
}

Value Vec4 (const XMFLOAT4& v) {
    Value a = Value::Array();
    a.push_back(v.x); a.push_back(v.y); a.push_back(v.z); a.push_back(v.w);
    return a;
}

// Euler degrees (pitch X, yaw Y, roll Z: XMQuaternionRotationRollPitchYaw's order) <-> quaternion.
XMFLOAT4 QuatFromEulerDeg (const XMFLOAT3& deg) {
    XMFLOAT4 q;
    XMStoreFloat4(&q, XMQuaternionRotationRollPitchYaw(XMConvertToRadians(deg.x), XMConvertToRadians(deg.y),
                                                       XMConvertToRadians(deg.z)));
    return q;
}

XMFLOAT3 EulerDegFromQuat (const XMFLOAT4& q) {
    const float sinp = std::clamp(2.0f * (q.w * q.x - q.y * q.z), -1.0f, 1.0f);
    const float pitch = std::asin(sinp);
    const float yaw = std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float roll = std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.x * q.x + q.z * q.z));
    return XMFLOAT3(XMConvertToDegrees(pitch), XMConvertToDegrees(yaw), XMConvertToDegrees(roll));
}

std::string ShortName (const std::string& managerName) {
    const size_t p = managerName.rfind("::");
    return p == std::string::npos ? managerName : managerName.substr(p + 2);
}

bool Exists (const WScene& scene, Entity e) {
    if (e == INVALID_ENTITY) return false;
    for (const auto& kv : scene.componentLibrary.entries)
        if (kv.second.component_manager && kv.second.component_manager->Contains(e)) return true;
    return false;
}

// "id": 123  or  "name": "HMMWV" (first match), with an optional "under" ancestor.
Entity ResolveEntity (WScene& scene, const Value& args, std::string& error, const char* idKey = "id") {
    if (args[idKey].IsNumber()) {
        const Entity e = Entity(args[idKey].AsInt());
        if (!Exists(scene, e)) { error = "no entity " + std::to_string(e); return INVALID_ENTITY; }
        return e;
    }
    const std::string name = args[idKey].IsString() ? args[idKey].AsString() : args["name"].AsString();
    if (!name.empty()) {
        const Entity e = scene.Entity_FindByName(name);
        if (e == INVALID_ENTITY) error = "no entity named '" + name + "'";
        return e;
    }
    error = std::string("give '") + idKey + "' (number) or 'name'";
    return INVALID_ENTITY;
}

Value ComponentNames (const WScene& scene, Entity e) {
    Value list = Value::Array();
    for (const auto& kv : scene.componentLibrary.entries)
        if (kv.second.component_manager && kv.second.component_manager->Contains(e))
            list.push_back(ShortName(kv.first));
    return list;
}

Value EntitySummary (const WScene& scene, Entity e) {
    Value o = Value::Object();
    o.Set("id", uint64_t(e));
    const wi::scene::NameComponent* n = scene.names.GetComponent(e);
    o.Set("name", n ? n->name : std::string());
    const wi::scene::HierarchyComponent* h = scene.hierarchy.GetComponent(e);
    o.Set("parent", h ? uint64_t(h->parentID) : uint64_t(0));
    o.Set("components", ComponentNames(scene, e));
    return o;
}

Value TransformJson (const wi::scene::TransformComponent& t) {
    Value o = Value::Object();
    o.Set("local_position", Vec3(t.translation_local));
    o.Set("local_rotation_deg", Vec3(EulerDegFromQuat(t.rotation_local)));
    o.Set("local_scale", Vec3(t.scale_local));
    o.Set("world_position", Vec3(t.GetPosition()));
    o.Set("world_rotation_deg", Vec3(EulerDegFromQuat(t.GetRotation())));
    o.Set("world_rotation_quat", Vec4(t.GetRotation()));
    return o;
}

Value MetadataJson (const wi::scene::MetadataComponent& m) {
    Value o = Value::Object();
    for (size_t i = 0; i < m.bool_values.names.size(); ++i)   o.Set(m.bool_values.names[i], bool(m.bool_values.values[i]));
    for (size_t i = 0; i < m.int_values.names.size(); ++i)    o.Set(m.int_values.names[i], m.int_values.values[i]);
    for (size_t i = 0; i < m.float_values.names.size(); ++i)  o.Set(m.float_values.names[i], m.float_values.values[i]);
    for (size_t i = 0; i < m.string_values.names.size(); ++i) o.Set(m.string_values.names[i], m.string_values.values[i]);
    return o;
}

// A JSON value into a metadata key. Native component parameters are TYPED (GetFloat reads
// only floats, GetInt only ints), and JSON has one number type, so a number is written as a
// float and, when it is whole, as an int as well - whichever the component binds, it finds.
// {"int": n} / {"float": n} force one.
bool SetMetadata (wi::scene::MetadataComponent& m, const std::string& key, const Value& v, std::string& error) {
    if (v.IsBool()) { m.bool_values.set(key, v.AsBool()); return true; }
    if (v.IsString()) { m.string_values.set(key, v.AsString()); return true; }
    if (v.IsNumber()) {
        const double d = v.AsNumber();
        m.float_values.set(key, float(d));
        if (d == std::floor(d) && std::fabs(d) < 2e9) m.int_values.set(key, int(d));
        return true;
    }
    if (v.IsObject() && v["int"].IsNumber()) { m.int_values.set(key, int(v["int"].AsInt())); return true; }
    if (v.IsObject() && v["float"].IsNumber()) { m.float_values.set(key, v["float"].AsFloat()); return true; }
    error = "value for '" + key + "' must be bool, number, string, {\"int\":n} or {\"float\":n}";
    return false;
}

void EraseMetadata (wi::scene::MetadataComponent& m, const std::string& key) {
    m.bool_values.erase(key);
    m.int_values.erase(key);
    m.float_values.erase(key);
    m.string_values.erase(key);
}

std::string LogTail (int lines, const std::string& filter) {
    const std::string text = wi::backlog::getText();
    std::vector<std::string> all;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && (filter.empty() || line.find(filter) != std::string::npos)) all.push_back(line);
        start = end + 1;
    }
    std::string out;
    const size_t from = all.size() > size_t(lines) ? all.size() - size_t(lines) : 0;
    for (size_t i = from; i < all.size(); ++i) { out += all[i]; out += '\n'; }
    return out;
}

} // namespace

EditorBridge& EditorBridge::Get () {
    static EditorBridge instance;
    return instance;
}

void EditorBridge::Register (const std::string& name, const std::string& help, Handler fn) {
    handlers_[name] = std::move(fn);
    help_[name] = help;
}

// ------------------------------------------------------------------ socket side ---

bool EditorBridge::Start (App& app, int port) {
    if (running_.load()) return true;
    app_ = &app;
    port_ = port > 0 ? port : kDefaultPort;
    // A second instance on the same folder (a network client next to its server) passes
    // --devbridge-port and gets its own info file, so the two never overwrite each other.
    infoFile_ = port_ == kDefaultPort ? "devbridge.json" : "devbridge_" + std::to_string(port_) + ".json";
    if (handlers_.empty()) RegisterAll();

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
    Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kBadSocket) return false;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port_));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr); // never reachable from another machine
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(s, 2) != 0) {
        CloseSocket(s);
        wi::backlog::post("EditorBridge: port " + std::to_string(port_) + " is busy - bridge off",
                          wi::backlog::LogLevel::Warning);
        return false;
    }
    listenSocket_ = uintptr_t(s);

    std::random_device rd;
    char token[33];
    std::snprintf(token, sizeof(token), "%08x%08x%08x%08x", rd(), rd(), rd(), rd());
    token_ = token;

    Value info = Value::Object();
    info.Set("port", port_);
    info.Set("token", token_);
    info.Set("app", App::Config().name);
#ifdef _WIN32
    info.Set("pid", uint64_t(GetCurrentProcessId()));
#else
    info.Set("pid", uint64_t(getpid()));
#endif
    std::ofstream(infoFile_, std::ios::trunc) << info.Dump() << "\n";

    running_ = true;
    thread_ = std::thread([this] { ThreadMain(); });
    wi::backlog::post("EditorBridge: listening on 127.0.0.1:" + std::to_string(port_) + " (token in devbridge.json)");
    return true;
}

void EditorBridge::Stop () {
    if (!running_.exchange(false)) return;
    CloseSocket(Socket(listenSocket_));
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::error_code ec;
    std::filesystem::remove(infoFile_, ec);
#ifdef _WIN32
    WSACleanup();
#endif
}

void EditorBridge::ThreadMain () {
    const Socket listener = Socket(listenSocket_);
    while (running_.load()) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        timeval tv = { 0, 200000 };
        if (select(int(listener + 1), &set, nullptr, nullptr, &tv) <= 0) continue;
        const Socket client = accept(listener, nullptr, nullptr);
        if (client == kBadSocket) continue;

        std::string buffer;
        char chunk[4096];
        while (running_.load()) {
            const int got = int(recv(client, chunk, int(sizeof(chunk)), 0));
            if (got <= 0) break;
            buffer.append(chunk, size_t(got));
            size_t nl;
            while ((nl = buffer.find('\n')) != std::string::npos) {
                const std::string line = buffer.substr(0, nl);
                buffer.erase(0, nl + 1);
                if (line.find_first_not_of(" \r\t") == std::string::npos) continue;
                std::string reply = Execute(line);
                reply += '\n';
                size_t sent = 0;
                while (sent < reply.size()) {
                    const int n = int(send(client, reply.data() + sent, int(reply.size() - sent), 0));
                    if (n <= 0) break;
                    sent += size_t(n);
                }
            }
        }
        CloseSocket(client);
    }
}

std::string EditorBridge::Execute (const std::string& line) {
    // Hand the request to the main thread and wait for its answer: the scene is only ever
    // touched from there, between frames.
    Pending pending;
    pending.request = line;
    std::unique_lock<std::mutex> lock(mutex_);
    queue_.push_back(&pending);
    cv_.wait(lock, [&] { return pending.done || !running_.load(); });
    if (!pending.done) return "{\"ok\":false,\"error\":\"bridge stopped\"}";
    return pending.response;
}

void EditorBridge::Pump () {
    std::deque<Pending*> work;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        work.swap(queue_);
    }
    for (Pending* p : work) {
        Value request, reply = Value::Object();
        std::string error;
        if (!json::Parse(p->request, request, error)) {
            reply.Set("ok", false);
            reply.Set("error", "bad JSON: " + error);
        } else if (request["token"].AsString() != token_) {
            reply.Set("ok", false);
            reply.Set("error", "bad token (read it from devbridge.json)");
        } else {
            const std::string cmd = request["cmd"].AsString();
            auto it = handlers_.find(cmd);
            if (it == handlers_.end()) {
                reply.Set("ok", false);
                reply.Set("error", "unknown command '" + cmd + "' - try 'help'");
            } else {
                Value result = it->second(request["args"], error);
                if (!error.empty()) {
                    reply.Set("ok", false);
                    reply.Set("error", error);
                } else {
                    reply.Set("ok", true);
                    reply.Set("result", std::move(result));
                }
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        p->response = reply.Dump();
        p->done = true;
    }
    if (!work.empty()) cv_.notify_all();
}

// --------------------------------------------------------------------- commands ---

void EditorBridge::RegisterAll () {
    App& app = *app_;
    auto S = []() -> WScene& { return wi::scene::GetScene(); };

    Register("help", "{} -> every command with its arguments", [this](const Value&, std::string&) {
        Value o = Value::Object();
        for (const auto& kv : help_) o.Set(kv.first, kv.second);
        return o;
    });

    Register("ping", "{} -> app, scene, play state, editor mode, dev build", [&app, S](const Value&, std::string&) {
        Value o = Value::Object();
        o.Set("app", App::Config().name);
        o.Set("scene", app.sceneManager.CurrentName());
        o.Set("loading", app.sceneManager.HasPendingLoad());
        o.Set("play", st::ToString(st::PlayControl::Get().State()));
        o.Set("mode", st::PlayControl::Get().IsEditing() ? "edit" : "play");
        o.Set("editor", app.IsEditorMode());
        o.Set("dev_build", IsDevBuild());
        o.Set("entities", uint64_t(S().names.GetCount()));
        return o;
    });

    Register("log", "{lines=40, filter=''} -> the backlog's last lines", [](const Value& a, std::string&) {
        return Value(LogTail(int(a["lines"].AsInt(40)), a["filter"].AsString()));
    });

    // scenes
    Register("scene.list", "{} -> registered scenes and where they come from", [&app](const Value&, std::string&) {
        Value list = Value::Array();
        for (const std::string& n : app.sceneManager.Names()) {
            Value o = Value::Object();
            o.Set("name", n);
            const SceneManager::Origin origin = app.sceneManager.OriginOf(n);
            o.Set("origin", origin == SceneManager::Origin::Cpp ? "c++" : origin == SceneManager::Origin::Folder ? "file" : "runtime");
            list.push_back(std::move(o));
        }
        return list;
    });
    Register("scene.load", "{name} -> queues the load; poll ping until loading is false", [&app](const Value& a, std::string& err) {
        const std::string name = a["name"].AsString();
        if (!app.sceneManager.Has(name)) { err = "no scene '" + name + "' (scene.list, or scene.rescan after saving a new map)"; return Value(); }
        app.sceneManager.Load(name);
        return Value("loading " + name);
    });
    Register("scene.new", "{name='Untitled'} -> an empty scene with sun and sky, loaded", [&app](const Value& a, std::string&) {
        const std::string base = a["name"].AsString().empty() ? std::string("Untitled") : a["name"].AsString();
        return Value(app.sceneManager.NewScene(base));
    });
    Register("scene.save", "{path} -> writes the scene (.stsd native, .wiscene archive); dev builds mirror into the project's assets/scenes/",
        [&app, S](const Value& a, std::string& err) {
        const std::string path = a["path"].AsString();
        if (path.empty()) { err = "path required, e.g. assets/scenes/mymap.stsd"; return Value(); }
        if (!app.Editor().SaveSceneForTool(app, S(), path)) { err = "save failed - see log"; return Value(); }
        return Value(path);
    });
    Register("template.save", "{id|name, name?} -> saves the object and everything under it to assets/templates/<name>.sttd",
        [&app, S](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        std::string name = a["name"].AsString();
        if (name.empty()) {
            const wi::scene::NameComponent* n = scene.names.GetComponent(e);
            name = (n && !n->name.empty()) ? n->name : std::string("Template");
        }
        const std::string path = std::string(st::kTemplateFolder) + "/" + name + "." + st::kTemplateExtension;
        if (!app.Editor().SaveTemplateForTool(scene, e, path)) { err = "template save failed - see log"; return Value(); }
        return Value(path);
    });
    Register("template.place", "{path, position=[0,0,0]} -> root id of the placed copy (one undo step)",
        [&app, S](const Value& a, std::string& err) {
        const std::string path = a["path"].AsString();
        if (path.empty()) { err = "path required, e.g. assets/templates/Player.sttd"; return Value(); }
        XMFLOAT3 p(0, 0, 0);
        ReadVec3(a["position"], p);
        const Entity e = app.Editor().PlaceTemplateForTool(S(), path, p);
        if (e == INVALID_ENTITY) { err = "place failed - see log"; return Value(); }
        return Value(uint64_t(e));
    });
    Register("template.open", "{path} -> opens the template's preview window (editor mode)",
        [&app](const Value& a, std::string& err) {
        const std::string path = a["path"].AsString();
        if (path.empty()) { err = "path required"; return Value(); }
        app.Editor().OpenTemplate(path);
        return Value(true);
    });
    Register("template.preview_shot", "{path} -> PNG of the newest template preview's view",
        [&app](const Value& a, std::string& err) {
        if (!app.Editor().HasTemplatePreviewFrame()) { err = "no template preview has rendered"; return Value(); }
        std::string path = a["path"].AsString();
        if (path.empty()) path = "devbridge_shots/template_preview.png";
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        if (!app.Editor().SaveTemplatePreviewFrame(path)) { err = "could not write " + path; return Value(); }
        return Value(std::filesystem::absolute(path, ec).generic_u8string());
    });
    Register("pool.prewarm", "{path, count} -> parked copies the pool now holds",
        [S](const Value& a, std::string& err) {
        if (st::PlayControl::Get().IsEditing()) { err = "Edit mode: pools hold Play-mode objects - play first"; return Value(); }
        std::string error;
        st::TemplatePool* pool = st::TemplatePools::Get().Acquire(a["path"].AsString(), &error);
        if (pool == nullptr) { err = error; return Value(); }
        return Value(uint64_t(pool->Prewarm(S(), (uint32_t)std::max(0.0, a["count"].AsNumber()))));
    });
    Register("pool.spawn", "{path, position=[0,0,0]} -> root id (reuses a parked copy when there is one)",
        [S](const Value& a, std::string& err) {
        if (st::PlayControl::Get().IsEditing()) { err = "Edit mode: pools hold Play-mode objects - play first"; return Value(); }
        std::string error;
        st::TemplatePool* pool = st::TemplatePools::Get().Acquire(a["path"].AsString(), &error);
        if (pool == nullptr) { err = error; return Value(); }
        XMFLOAT3 p(0, 0, 0);
        ReadVec3(a["position"], p);
        const Entity e = pool->Spawn(S(), p);
        if (e == INVALID_ENTITY) { err = "spawn failed (pool at maxActive?)"; return Value(); }
        return Value(uint64_t(e));
    });
    Register("pool.release", "{id} -> parks a copy a pool spawned", [S](const Value& a, std::string& err) {
        const Entity e = (Entity)a["id"].AsNumber();
        st::TemplatePool* pool = st::TemplatePools::Get().Owner(e);
        if (pool == nullptr || !pool->Release(S(), e)) { err = "not an active pooled copy"; return Value(); }
        return Value(true);
    });
    Register("pool.stats", "{} -> per pool: active, parked, created, reused, recycled", [](const Value&, std::string&) {
        Value out = Value::Object();
        st::TemplatePools::Get().ForEach([&out](const std::string& key, st::TemplatePool& pool) {
            const st::TemplatePoolStats s = pool.Stats();
            Value v = Value::Object();
            v.Set("active", uint64_t(s.active));
            v.Set("parked", uint64_t(s.parked));
            v.Set("created", uint64_t(s.created));
            v.Set("reused", uint64_t(s.reused));
            v.Set("recycled", uint64_t(s.recycled));
            out.Set(key, std::move(v));
        });
        return out;
    });
    // ---- game UI (Framework/ui) ----
    Register("ui.create", "{preset: canvas|world_canvas|panel|image|text|button|toggle|slider|document|vertical|horizontal|grid, parent?, name?} -> {id, created}",
        [S](const Value& a, std::string& err) {
        static const std::pair<const char*, st::ui::Preset> kPresets[] = {
            { "canvas", st::ui::Preset::Canvas }, { "world_canvas", st::ui::Preset::WorldCanvas },
            { "panel", st::ui::Preset::Panel }, { "image", st::ui::Preset::Image },
            { "text", st::ui::Preset::Text }, { "button", st::ui::Preset::Button },
            { "toggle", st::ui::Preset::Toggle }, { "slider", st::ui::Preset::Slider },
            { "document", st::ui::Preset::Document }, { "vertical", st::ui::Preset::VerticalLayout },
            { "horizontal", st::ui::Preset::HorizontalLayout }, { "grid", st::ui::Preset::Grid },
        };
        const std::string want = a["preset"].AsString();
        const st::ui::Preset* preset = nullptr;
        for (const auto& p : kPresets)
            if (want == p.first) preset = &p.second;
        if (preset == nullptr) { err = "unknown preset '" + want + "'"; return Value(); }
        WScene& scene = S();
        Entity parent = INVALID_ENTITY;
        if (a.Has("parent")) {
            Value pa = Value::Object();
            pa.Set("id", a["parent"]);
            parent = ResolveEntity(scene, pa, err);
            if (parent == INVALID_ENTITY) return Value();
        }
        wi::vector<Entity> created;
        const Entity e = st::ui::Create(scene, *preset, parent, a["name"].AsString(), &created);
        Value out = Value::Object();
        out.Set("id", uint64_t(e));
        Value list = Value::Array();
        for (Entity c : created) list.push_back(uint64_t(c));
        out.Set("created", std::move(list));
        return out;
    });
    Register("ui.set", "{id|name, + any of: anchorMin anchorMax pivot position size [x,y], order, opacity, hidden, clip, "
                       "text, fontSize, font, textColor [rgba], wrap, shadow, align [h,v] (0 start 1 centre 2 end), color [rgba], "
                       "texture, border [l,t,r,b], cornerRadius, imageType (0 simple 1 sliced 2 fillH 3 fillV), fill, source, fonts, "
                       "onClick, onValueChanged, eventTarget, value, isOn, interactable, scaleMode, reference [w,h], match, "
                       "sortOrder, spacing [x,y], padding [l,t,r,b], childAlignment, layoutFlags, cellSize [x,y]} -> true",
        [S](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        auto v2 = [&a](const char* key, XMFLOAT2& out) {
            const Value& v = a[key];
            if (v.IsArray() && v.size() >= 2) out = XMFLOAT2(v.At(0).AsFloat(), v.At(1).AsFloat());
        };
        auto v4 = [&a](const char* key, XMFLOAT4& out) {
            const Value& v = a[key];
            if (v.IsArray() && v.size() >= 4) out = XMFLOAT4(v.At(0).AsFloat(), v.At(1).AsFloat(), v.At(2).AsFloat(), v.At(3).AsFloat());
        };
        auto flag = [&a](const char* key, uint32_t& flags, uint32_t bit) {
            if (!a.Has(key)) return;
            if (a[key].AsBool()) flags |= bit;
            else flags &= ~bit;
        };
        auto entityArg = [&](const char* key, Entity& out) {
            if (!a.Has(key)) return;
            Value pa = Value::Object();
            pa.Set("id", a[key]);
            std::string ignored;
            out = ResolveEntity(scene, pa, ignored);
        };
        if (st::ui::UIRectComponent* r = st::ui::Rects(scene).GetComponent(e)) {
            v2("anchorMin", r->anchorMin); v2("anchorMax", r->anchorMax); v2("pivot", r->pivot);
            v2("position", r->position); v2("size", r->size);
            if (a.Has("order")) r->order = (int)a["order"].AsInt();
            if (a.Has("opacity")) r->opacity = a["opacity"].AsFloat();
            flag("hidden", r->_flags, st::ui::UIRectComponent::HIDDEN);
            flag("clip", r->_flags, st::ui::UIRectComponent::CLIP_CHILDREN);
        }
        if (st::ui::UITextComponent* t = st::ui::Texts(scene).GetComponent(e)) {
            if (a.Has("text")) t->text = a["text"].AsString();
            if (a.Has("fontSize")) t->size = a["fontSize"].AsFloat();
            if (a.Has("font")) t->fontName = a["font"].AsString();
            v4("textColor", t->color);
            flag("wrap", t->_flags, st::ui::UITextComponent::WRAP);
            flag("shadow", t->_flags, st::ui::UITextComponent::SHADOW);
            const Value& al = a["align"];
            if (al.IsArray() && al.size() >= 2) {
                t->hAlign = (st::ui::UITextComponent::Align)al.At(0).AsInt();
                t->vAlign = (st::ui::UITextComponent::Align)al.At(1).AsInt();
            }
        }
        if (st::ui::UIImageComponent* i = st::ui::Images(scene).GetComponent(e)) {
            v4("color", i->color);
            if (a.Has("texture")) i->textureName = a["texture"].AsString();
            if (a.Has("cornerRadius")) i->cornerRadius = a["cornerRadius"].AsFloat();
            if (a.Has("imageType")) i->type = (st::ui::UIImageComponent::Type)a["imageType"].AsInt();
            if (a.Has("fill")) i->fillAmount = a["fill"].AsFloat();
            v4("border", i->border);
        }
        if (st::ui::UIDocumentComponent* d = st::ui::Documents(scene).GetComponent(e)) {
            if (a.Has("source")) d->source = a["source"].AsString();
            if (a.Has("fonts")) d->fonts = a["fonts"].AsString();
            entityArg("eventTarget", d->eventTarget);
        }
        if (st::ui::UIButtonComponent* b = st::ui::Buttons(scene).GetComponent(e)) {
            if (a.Has("onClick")) b->onClick = a["onClick"].AsString();
            if (a.Has("interactable")) b->SetInteractable(a["interactable"].AsBool());
            entityArg("eventTarget", b->eventTarget);
        }
        if (st::ui::UIToggleComponent* t = st::ui::Toggles(scene).GetComponent(e)) {
            if (a.Has("isOn")) t->SetOn(a["isOn"].AsBool());
            if (a.Has("onValueChanged")) t->onValueChanged = a["onValueChanged"].AsString();
            entityArg("eventTarget", t->eventTarget);
        }
        if (st::ui::UISliderComponent* s = st::ui::Sliders(scene).GetComponent(e)) {
            if (a.Has("value")) s->value = a["value"].AsFloat();
            if (a.Has("onValueChanged")) s->onValueChanged = a["onValueChanged"].AsString();
            entityArg("eventTarget", s->eventTarget);
        }
        if (st::ui::UICanvasComponent* c = st::ui::Canvases(scene).GetComponent(e)) {
            if (a.Has("scaleMode")) c->scaleMode = (st::ui::UICanvasComponent::ScaleMode)a["scaleMode"].AsInt();
            v2("reference", c->referenceResolution);
            if (a.Has("match")) c->match = a["match"].AsFloat();
            if (a.Has("sortOrder")) c->sortOrder = (int)a["sortOrder"].AsInt();
        }
        if (st::ui::UILayoutComponent* l = st::ui::Layouts(scene).GetComponent(e)) {
            v2("spacing", l->spacing);
            v4("padding", l->padding);
            if (a.Has("childAlignment")) l->childAlignment = (uint32_t)a["childAlignment"].AsInt();
            if (a.Has("layoutFlags")) l->_flags = (uint32_t)a["layoutFlags"].AsInt();
            v2("cellSize", l->cellSize);
        }
        return Value(true);
    });
    Register("ui.get", "{id|name} -> rect (canvas units), pixels, visible, components, document status", [S](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        Value out = Value::Object();
        if (const st::ui::UIRectComponent* r = st::ui::Rects(scene).GetComponent(e)) {
            out.Set("rect", Vec4(r->_rect));
            out.Set("pixels", Vec4(r->_pixels));
            out.Set("visible", r->_visible);
        }
        if (const st::ui::UITextComponent* t = st::ui::Texts(scene).GetComponent(e)) out.Set("text", t->text);
        if (const st::ui::UISliderComponent* s = st::ui::Sliders(scene).GetComponent(e)) out.Set("value", s->value);
        if (const st::ui::UIToggleComponent* t = st::ui::Toggles(scene).GetComponent(e)) out.Set("isOn", t->IsOn());
        if (st::ui::Documents(scene).Contains(e)) out.Set("document", st::ui::RmlHost::Get().Status(e));
        out.Set("clicked", st::ui::UISystem::Get().WasClicked(e));
        out.Set("components", ComponentNames(scene, e));
        return out;
    });
    Register("ui.list", "{} -> visible elements in draw order: id, name, pixels", [S](const Value&, std::string&) {
        WScene& scene = S();
        Value out = Value::Array();
        for (Entity e : st::ui::UISystem::Get().DrawOrder()) {
            Value v = Value::Object();
            v.Set("id", uint64_t(e));
            const wi::scene::NameComponent* n = scene.names.GetComponent(e);
            v.Set("name", n ? n->name : std::string());
            if (const st::ui::UIRectComponent* r = st::ui::Rects(scene).GetComponent(e)) v.Set("pixels", Vec4(r->_pixels));
            out.push_back(std::move(v));
        }
        return out;
    });
    Register("ui.click", "{id|name} -> fires the element as a click (Button onClick, Toggle flip)", [S](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        if (!st::ui::UISystem::Get().Click(scene, e)) { err = "not a button or toggle (or not interactable)"; return Value(); }
        return Value(true);
    });
    Register("ui.pick", "{x, y} (game image pixels) -> topmost element id or null", [S](const Value& a, std::string&) {
        const Entity e = st::ui::UISystem::Get().Pick(S(), XMFLOAT2(a["x"].AsFloat(), a["y"].AsFloat()));
        return e == INVALID_ENTITY ? Value() : Value(uint64_t(e));
    });
    Register("ui.reload", "{} -> reload every RmlUi document from its files", [](const Value&, std::string&) {
        st::ui::RmlHost::Get().ReloadAll();
        return Value(true);
    });
    Register("ui.doc", "{id|name, element, rml?, class?, on?, property?, value?} -> edits an element of a document; returns its value",
        [S](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        st::ui::RmlHost& host = st::ui::RmlHost::Get();
        const std::string id = a["element"].AsString();
        if (a.Has("rml") && !host.SetInnerRml(e, id, a["rml"].AsString())) { err = "no such document element"; return Value(); }
        if (a.Has("class")) host.SetClass(e, id, a["class"].AsString(), a["on"].AsBool(true));
        if (a.Has("property")) host.SetProperty(e, id, a["property"].AsString(), a["value"].AsString());
        return Value(host.GetValue(e, id));
    });
    Register("mesh.generate_lods", "{id|name (an entity with a mesh, or an object), levels=4, keep=0.5, error=0.02} or {all:true} -> levels / meshes",
        [S](const Value& a, std::string& err) {
        WScene& scene = S();
        st::render::MeshLODSettings settings;
        if (a.Has("levels")) settings.lodCount = (uint32_t)a["levels"].AsNumber();
        if (a.Has("keep")) settings.reduction = (float)a["keep"].AsNumber();
        if (a.Has("error")) settings.targetError = (float)a["error"].AsNumber();
        if (a["all"].AsBool())
            return Value(uint64_t(st::render::GenerateSceneLODs(scene, settings)));
        Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        if (const wi::scene::ObjectComponent* o = scene.objects.GetComponent(e)) e = o->meshID;
        wi::scene::MeshComponent* mesh = scene.meshes.GetComponent(e);
        if (mesh == nullptr) { err = "no mesh on that entity"; return Value(); }
        st::render::MeshLODReport report;
        if (!st::render::GenerateMeshLODs(*mesh, settings, &report)) { err = report.reason; return Value(); }
        Value out = Value::Object();
        out.Set("levels", uint64_t(report.lods));
        out.Set("lod0_triangles", uint64_t(report.lod0Triangles));
        out.Set("last_triangles", uint64_t(report.lastTriangles));
        return out;
    });
    Register("scene.rescan", "{} -> register maps found in the scene folder", [&app](const Value&, std::string&) {
        return Value(app.sceneManager.DiscoverScenes(App::Config().sceneFolder));
    });

    // entities
    Register("entity.list", "{filter='', roots_only=false, limit=300} -> [{id,name,parent,components}]", [S](const Value& a, std::string&) {
        WScene& scene = S();
        const std::string filter = wi::helper::toLower(a["filter"].AsString());
        const bool rootsOnly = a["roots_only"].AsBool(false);
        const size_t limit = size_t(a["limit"].AsInt(300));
        Value list = Value::Array();
        for (size_t i = 0; i < scene.names.GetCount() && list.size() < limit; ++i) {
            const Entity e = scene.names.GetEntity(i);
            if (rootsOnly && scene.hierarchy.Contains(e)) continue;
            if (!filter.empty() && wi::helper::toLower(scene.names[i].name).find(filter) == std::string::npos) continue;
            list.push_back(EntitySummary(scene, e));
        }
        return list;
    });
    Register("entity.get", "{id|name} -> components, transform, metadata, children, native components", [S](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        Value o = EntitySummary(scene, e);
        if (const auto* t = scene.transforms.GetComponent(e)) o.Set("transform", TransformJson(*t));
        if (const auto* m = scene.metadatas.GetComponent(e)) o.Set("metadata", MetadataJson(*m));
        Value children = Value::Array();
        for (size_t i = 0; i < scene.hierarchy.GetCount(); ++i)
            if (scene.hierarchy[i].parentID == e) children.push_back(EntitySummary(scene, scene.hierarchy.GetEntity(i)));
        o.Set("children", std::move(children));
        auto it = scene.nativeComponents.instances.find(e);
        if (it != scene.nativeComponents.instances.end()) {
            Value natives = Value::Array();
            for (const auto& inst : it->second) {
                Value n = Value::Object();
                n.Set("name", inst.name);
                n.Set("localID", inst.localID);
                n.Set("started", inst.started);
                n.Set("enabled", inst.enabled);
                natives.push_back(std::move(n));
            }
            o.Set("native", std::move(natives));
        }
        if (const auto* rb = scene.rigidbodies.GetComponent(e)) {
            Value r = Value::Object();
            r.Set("shape", int(rb->shape));
            r.Set("mass", rb->mass);
            r.Set("vehicle", rb->IsVehicle());
            r.Set("body_built", wi::physics::HasValidBody(*const_cast<wi::scene::RigidBodyPhysicsComponent*>(rb)));
            o.Set("rigidbody", std::move(r));
        }
        return o;
    });
    Register("entity.create", "{name, primitive=empty|cube|sphere|plane, parent?, position?, rotation_deg?, scale?} -> id",
        [S](const Value& a, std::string& err) {
        WScene& scene = S();
        const std::string name = a["name"].AsString().empty() ? std::string("Entity") : a["name"].AsString();
        const std::string prim = a["primitive"].AsString();
        Entity e;
        if (prim == "cube") e = scene.Entity_CreateCube(name);
        else if (prim == "sphere") e = scene.Entity_CreateSphere(name);
        else if (prim == "plane") e = scene.Entity_CreatePlane(name);
        else {
            e = wi::ecs::CreateEntity();
            scene.names.Create(e) = name;
            scene.layers.Create(e);
            scene.transforms.Create(e);
        }
        Entity parent = INVALID_ENTITY;
        if (a.Has("parent")) {
            parent = ResolveEntity(scene, a, err, "parent");
            if (parent == INVALID_ENTITY) return Value();
        }
        wi::scene::TransformComponent* t = scene.transforms.GetComponent(e);
        XMFLOAT3 v;
        if (t != nullptr) {
            if (ReadVec3(a["position"], v)) {
                if (parent == INVALID_ENTITY) t->SetWorldPosition(v.x, v.y, v.z);
                else t->translation_local = v;
            }
            if (ReadVec3(a["rotation_deg"], v)) t->rotation_local = QuatFromEulerDeg(v);
            if (ReadVec3(a["scale"], v)) t->scale_local = v;
            t->SetDirty();
            t->UpdateTransform();
        }
        if (parent != INVALID_ENTITY) scene.Component_Attach(e, parent, true);
        return Value(uint64_t(e));
    });
    Register("entity.delete", "{id|name} -> removes it and its children", [S](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        scene.Entity_Remove(e);
        return Value(true);
    });
    Register("entity.rename", "{id|name, new_name}", [S](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        scene.names.Create(e) = a["new_name"].AsString();
        return Value(true);
    });
    Register("entity.transform", "{id|name, position?, rotation_deg?, scale?} (local; a root's position is its world position)",
        [S](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        wi::scene::TransformComponent* t = scene.transforms.GetComponent(e);
        if (t == nullptr) { err = "entity has no transform"; return Value(); }
        XMFLOAT3 v;
        if (ReadVec3(a["position"], v)) {
            if (!scene.hierarchy.Contains(e)) t->SetWorldPosition(v.x, v.y, v.z);
            // As a Translate by the difference: a plain write to translation_local is
            //	overwritten from the large-world absolute position on the next update.
            else t->Translate(XMFLOAT3(v.x - t->translation_local.x, v.y - t->translation_local.y,
                                       v.z - t->translation_local.z));
        }
        if (ReadVec3(a["rotation_deg"], v)) t->rotation_local = QuatFromEulerDeg(v);
        if (ReadVec3(a["scale"], v)) t->scale_local = v;
        t->SetDirty();
        t->UpdateTransform();
        // A simulated body would snap straight back to where the solver has it.
        if (wi::scene::RigidBodyPhysicsComponent* rb = scene.rigidbodies.GetComponent(e)) {
            if (wi::physics::HasValidBody(*rb)) {
                wi::physics::SetPositionAndRotation(*rb, t->GetPosition(), t->GetRotation());
                wi::physics::SetLinearVelocity(*rb, XMFLOAT3(0, 0, 0));
                wi::physics::SetAngularVelocity(*rb, XMFLOAT3(0, 0, 0));
            }
        }
        return TransformJson(*t);
    });
    Register("entity.parent", "{id|name, parent (id, name, or 0 to detach), keep_world=true}", [S](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        if (a["parent"].IsNumber() && a["parent"].AsInt() == 0) {
            scene.Component_Detach(e);
            return Value(true);
        }
        const Entity parent = ResolveEntity(scene, a, err, "parent");
        if (parent == INVALID_ENTITY) return Value();
        scene.Component_Attach(e, parent, !a["keep_world"].AsBool(true));
        return Value(true);
    });

    // metadata + native components (the data the editor itself writes)
    Register("metadata.set", "{id|name, values:{key: bool|number|string|{int}|{float}}}", [S](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        wi::scene::MetadataComponent* m = scene.metadatas.GetComponent(e);
        if (m == nullptr) m = &scene.metadatas.Create(e);
        for (const auto& kv : a["values"].Members())
            if (!SetMetadata(*m, kv.first, kv.second, err)) return Value();
        return MetadataJson(*m);
    });
    Register("metadata.remove", "{id|name, keys:[...]}", [S](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        wi::scene::MetadataComponent* m = scene.metadatas.GetComponent(e);
        if (m != nullptr)
            for (const Value& k : a["keys"].Items()) EraseMetadata(*m, k.AsString());
        return m ? MetadataJson(*m) : Value();
    });
    Register("native.list", "{} -> every registered native component name", [](const Value&, std::string&) {
        wi::vector<std::string> names;
        wi::scene::GetRegisteredNativeComponentNames(names);
        Value list = Value::Array();
        for (const std::string& n : names) list.push_back(n);
        return list;
    });
    Register("native.attach", "{id|name, component, params:{name: value}} -> localID (writes NCI_n / NCA_n_*)",
        [S](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        const std::string component = a["component"].AsString();
        if (component.empty()) { err = "component required (native.list)"; return Value(); }
        wi::scene::MetadataComponent* m = scene.metadatas.GetComponent(e);
        if (m == nullptr) m = &scene.metadatas.Create(e);
        int localID = 0;
        while (m->string_values.has("NCI_" + std::to_string(localID))) ++localID;
        m->string_values.set("NCI_" + std::to_string(localID), component);
        for (const auto& kv : a["params"].Members())
            if (!SetMetadata(*m, "NCA_" + std::to_string(localID) + "_" + kv.first, kv.second, err)) return Value();
        return Value(localID);
    });
    Register("native.detach", "{id|name, localID}", [S](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        wi::scene::DetachNativeComponent(scene, e, int(a["localID"].AsInt()));
        return Value(true);
    });

    Register("rigidbody.set", "{id|name, shape=box|sphere|capsule|cylinder|convex|mesh, mass, box:[hx,hy,hz], radius, height, friction, restitution, kinematic}",
        [S](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        wi::scene::RigidBodyPhysicsComponent* rb = scene.rigidbodies.GetComponent(e);
        if (rb == nullptr) rb = &scene.rigidbodies.Create(e);
        using RB = wi::scene::RigidBodyPhysicsComponent;
        const std::string shape = a["shape"].AsString();
        if (shape == "box") rb->shape = RB::BOX;
        else if (shape == "sphere") rb->shape = RB::SPHERE;
        else if (shape == "capsule") rb->shape = RB::CAPSULE;
        else if (shape == "cylinder") rb->shape = RB::CYLINDER;
        else if (shape == "convex") rb->shape = RB::CONVEX_HULL;
        else if (shape == "mesh") rb->shape = RB::TRIANGLE_MESH;
        if (a.Has("mass")) rb->mass = a["mass"].AsFloat();
        XMFLOAT3 v;
        if (ReadVec3(a["box"], v)) rb->box.halfextents = v;
        if (a.Has("radius")) { rb->sphere.radius = a["radius"].AsFloat(); rb->capsule.radius = a["radius"].AsFloat(); }
        if (a.Has("height")) rb->capsule.height = a["height"].AsFloat();
        if (a.Has("friction")) rb->friction = a["friction"].AsFloat();
        if (a.Has("restitution")) rb->restitution = a["restitution"].AsFloat();
        if (a.Has("kinematic")) rb->SetKinematic(a["kinematic"].AsBool());
        rb->physicsobject = {}; // rebuild with the new settings
        return Value(true);
    });
    Register("rigidbody.remove", "{id|name} -> removes the rigid body (the editor's Properties 'x')",
        [S](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        const bool had = scene.rigidbodies.Contains(e);
        if (had) scene.rigidbodies.Remove(e);
        return Value(had);
    });

    Register("material.set", "{id|name, color:[r,g,b,a]?, opacity?, roughness?, metalness?, transmission?, blend=opaque|alpha|premultiplied|additive?}",
        [S](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        wi::scene::MaterialComponent* m = scene.materials.GetComponent(e);
        if (m == nullptr) { err = "that entity has no material"; return Value(); }
        if (a["color"].IsArray()) {
            XMFLOAT4 c = m->baseColor;
            c.x = a["color"].At(0).AsFloat(c.x);
            c.y = a["color"].At(1).AsFloat(c.y);
            c.z = a["color"].At(2).AsFloat(c.z);
            c.w = a["color"].At(3).AsFloat(c.w);
            m->SetBaseColor(c);
        }
        if (a.Has("opacity")) m->SetOpacity(a["opacity"].AsFloat());
        if (a.Has("roughness")) m->SetRoughness(a["roughness"].AsFloat());
        if (a.Has("metalness")) m->SetMetalness(a["metalness"].AsFloat());
        if (a.Has("transmission")) m->SetTransmissionAmount(a["transmission"].AsFloat());
        const std::string blend = a["blend"].AsString();
        if (blend == "opaque") m->userBlendMode = wi::enums::BLENDMODE_OPAQUE;
        else if (blend == "alpha") m->userBlendMode = wi::enums::BLENDMODE_ALPHA;
        else if (blend == "premultiplied") m->userBlendMode = wi::enums::BLENDMODE_PREMULTIPLIED;
        else if (blend == "additive") m->userBlendMode = wi::enums::BLENDMODE_ADDITIVE;
        m->SetDirty();
        return Value(true);
    });

    Register("mesh.set_material", "{id|name (object or mesh), material (id or name)} -> every subset of that mesh uses the material",
        [S](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        const Entity mat = ResolveEntity(scene, a, err, "material");
        if (mat == INVALID_ENTITY) return Value();
        if (!scene.materials.Contains(mat)) { err = "'material' is not a material entity"; return Value(); }
        Entity meshEntity = e;
        if (const wi::scene::ObjectComponent* o = scene.objects.GetComponent(e)) meshEntity = o->meshID;
        wi::scene::MeshComponent* mesh = scene.meshes.GetComponent(meshEntity);
        if (mesh == nullptr) { err = "no mesh on that entity"; return Value(); }
        for (auto& subset : mesh->subsets) subset.materialID = mat;
        return Value(double(mesh->subsets.size()));
    });

    Register("model.import", "{path, name?, parent?, position?, rotation_deg?, scale?} -> root id", [S](const Value& a, std::string& err) {
        WScene& scene = S();
        const st::model::ImportResult r = st::model::Import(scene, a["path"].AsString());
        if (!r.ok()) { err = "import failed: " + r.error; return Value(); }
        if (a["name"].IsString()) scene.names.Create(r.root) = a["name"].AsString();
        Entity parent = INVALID_ENTITY;
        if (a.Has("parent")) {
            parent = ResolveEntity(scene, a, err, "parent");
            if (parent == INVALID_ENTITY) return Value();
        }
        if (wi::scene::TransformComponent* t = scene.transforms.GetComponent(r.root)) {
            XMFLOAT3 v;
            if (ReadVec3(a["position"], v)) {
                if (parent == INVALID_ENTITY) t->SetWorldPosition(v.x, v.y, v.z);
                else t->translation_local = v;
            }
            if (ReadVec3(a["rotation_deg"], v)) t->rotation_local = QuatFromEulerDeg(v);
            if (ReadVec3(a["scale"], v)) t->scale_local = v;
            t->SetDirty();
            t->UpdateTransform();
        }
        if (parent != INVALID_ENTITY) scene.Component_Attach(r.root, parent, true);
        Value o = Value::Object();
        o.Set("root", uint64_t(r.root));
        o.Set("meshes", r.meshes);
        o.Set("materials", r.materials);
        o.Set("nodes", r.nodes);
        return o;
    });

    // terrain
    Register("terrain.create", "{name='Terrain', spacing=1, base=0, radius=8} -> terrain id", [S](const Value& a, std::string&) {
        st::terrain::CreateParams p;
        if (a["name"].IsString()) p.name = a["name"].AsString();
        p.spacing = a["spacing"].AsFloat(1.0f);
        p.baseHeight = a["base"].AsFloat(0.0f);
        p.generationRadius = int(a["radius"].AsInt(8));
        return Value(uint64_t(st::terrain::TerrainSystem::Get().CreateTerrain(S(), p)));
    });
    Register("terrain.generate", "{terrain?, x,z,size_x,size_z, biomes:[plains,hills,mountains,dunes,canyons,marsh,lakes], height, base, seed, erosion, warp, blending, biome_size, auto_paint, water_level}",
        [S](const Value& a, std::string& err) {
        WScene& scene = S();
        st::terrain::TerrainSystem& sys = st::terrain::TerrainSystem::Get();
        const Entity t = a.Has("terrain") ? ResolveEntity(scene, a, err, "terrain") : sys.FindTerrain(scene);
        if (t == INVALID_ENTITY || !sys.IsAuthored(t)) { if (err.empty()) err = "no authored terrain (terrain.create)"; return Value(); }
        st::terrain::GenerateParams g;
        g.x = a["x"].AsFloat(g.x); g.z = a["z"].AsFloat(g.z);
        g.sizeX = a["size_x"].AsFloat(g.sizeX); g.sizeZ = a["size_z"].AsFloat(g.sizeZ);
        if (a["biomes"].IsArray()) {
            for (bool& b : g.biomes) b = false;
            for (const Value& b : a["biomes"].Items())
                for (uint32_t k = 0; k < uint32_t(st::terrain::Biome::Count); ++k)
                    if (wi::helper::toLower(st::terrain::ToString(st::terrain::Biome(k))) == wi::helper::toLower(b.AsString()))
                        g.biomes[k] = true;
        }
        g.height = a["height"].AsFloat(g.height);
        g.base = a["base"].AsFloat(g.base);
        g.seed = uint32_t(a["seed"].AsInt(g.seed));
        g.erosion = a["erosion"].AsFloat(g.erosion);
        g.warp = a["warp"].AsFloat(g.warp);
        g.blending = a["blending"].AsFloat(g.blending);
        g.biomeSize = a["biome_size"].AsFloat(g.biomeSize);
        g.autoPaint = a["auto_paint"].AsBool(g.autoPaint);
        g.waterLevel = a["water_level"].AsFloat(g.waterLevel);
        sys.Edit(t, [g](st::terrain::TerrainData& d) { return st::terrain::Generate(d, g); }, true);
        return Value("queued");
    });
    Register("terrain.brush", "{terrain?, tool=raise|lower|smooth|flatten|paint|noise, x, z, radius=8, amount=1, hardness=0.3, layer=0, target?}",
        [S](const Value& a, std::string& err) {
        WScene& scene = S();
        st::terrain::TerrainSystem& sys = st::terrain::TerrainSystem::Get();
        const Entity t = a.Has("terrain") ? ResolveEntity(scene, a, err, "terrain") : sys.FindTerrain(scene);
        if (t == INVALID_ENTITY || !sys.IsAuthored(t)) { if (err.empty()) err = "no authored terrain"; return Value(); }
        st::terrain::Brush b;
        b.radius = a["radius"].AsFloat(8.0f);
        b.hardness = a["hardness"].AsFloat(0.3f);
        const std::string tool = a["tool"].AsString();
        const float x = a["x"].AsFloat(), z = a["z"].AsFloat(), amount = a["amount"].AsFloat(1.0f);
        const uint32_t layer = uint32_t(a["layer"].AsInt(0));
        const bool hasTarget = a["target"].IsNumber();
        const float target = a["target"].AsFloat();
        sys.Edit(t, [=](st::terrain::TerrainData& d) {
            if (tool == "lower") return d.Raise(x, z, b, -amount);
            if (tool == "smooth") return d.Smooth(x, z, b, std::min(1.0f, amount));
            if (tool == "flatten") return d.Flatten(x, z, b, hasTarget ? target : d.SampleHeight(x, z), std::min(1.0f, amount));
            if (tool == "paint") return d.Paint(x, z, b, layer, amount);
            if (tool == "noise") return d.Noise(x, z, b, amount, 12.0f, 7);
            return d.Raise(x, z, b, amount);
        });
        return Value("queued");
    });
    Register("terrain.sample", "{x, z} -> height, normal, dominant layer", [](const Value& a, std::string& err) {
        st::terrain::GroundSample g;
        if (!st::terrain::TerrainSystem::Get().Sample(a["x"].AsFloat(), a["z"].AsFloat(), g)) { err = "no authored terrain"; return Value(); }
        Value o = Value::Object();
        o.Set("height", g.height);
        o.Set("normal", Vec3(g.normal));
        o.Set("layer", g.dominantLayer);
        return o;
    });

    // play, input, camera, capture
    Register("play", "{state=play|nophysics|pause|step|reset|stop, frames=1} - play/step from Edit mode snapshot the scene, stop puts it back",
        [](const Value& a, std::string& err) {
        st::PlayControl& pc = st::PlayControl::Get();
        const std::string s = a["state"].AsString();
        if (s == "stop") pc.Stop();
        else if (s == "play") pc.Play();
        else if (s == "nophysics") pc.PlayWithoutPhysics();
        else if (s == "pause") pc.Pause();
        else if (s == "step") pc.Step(int(a["frames"].AsInt(1)));
        else if (s == "reset") pc.Reset();
        else if (!s.empty() && s != "stop") { err = "state must be play, nophysics, pause, step, reset or stop"; return Value(); }
        return Value(st::ToString(pc.State()));
    });
    Register("input.inject", "{action, value (number or [x,y]), seconds=1 (<=0 holds until input.clear)} - works while the window is unfocused",
        [](const Value& a, std::string& err) {
        const std::string action = a["action"].AsString();
        if (action.empty()) { err = "action required (e.g. Throttle, Steer)"; return Value(); }
        XMFLOAT2 v(0, 0);
        if (a["value"].IsArray()) v = XMFLOAT2(a["value"].At(0).AsFloat(), a["value"].At(1).AsFloat());
        else v.x = a["value"].AsFloat(1.0f);
        st::input::InjectAction(action, v, a["seconds"].AsFloat(1.0f));
        return Value(true);
    });
    Register("input.clear", "{} -> drop every injected input", [](const Value&, std::string&) {
        st::input::ClearInjectedActions();
        return Value(true);
    });
    Register("camera.get", "{} -> the game camera", [](const Value&, std::string&) {
        const wi::scene::CameraComponent& c = wi::scene::GetCamera();
        Value o = Value::Object();
        o.Set("position", Vec3(c.Eye));
        o.Set("forward", Vec3(c.At));
        o.Set("fov_deg", XMConvertToDegrees(c.fov));
        return o;
    });
    Register("camera.set", "{position, target} -> points the game camera (a scene that drives its own camera overrides it next frame)",
        [](const Value& a, std::string& err) {
        XMFLOAT3 p, t;
        if (!ReadVec3(a["position"], p) || !ReadVec3(a["target"], t)) { err = "position and target required"; return Value(); }
        const XMFLOAT3 d(t.x - p.x, t.y - p.y, t.z - p.z);
        const float yaw = std::atan2(d.x, d.z);
        const float pitch = -std::atan2(d.y, std::sqrt(d.x * d.x + d.z * d.z));
        wi::scene::TransformComponent tr;
        tr.SetWorldPosition(p.x, p.y, p.z);
        tr.RotateRollPitchYaw(XMFLOAT3(pitch, yaw, 0.0f));
        tr.UpdateTransform();
        wi::scene::GetCamera().TransformCamera(tr);
        wi::scene::GetCamera().UpdateCamera();
        return Value(true);
    });
    Register("screenshot", "{path?} -> PNG of the game view (last rendered frame), absolute path", [this, &app](const Value& a, std::string& err) {
        const st::render::TextureObject* tex = st::render::Get().GameImage();
        if (tex == nullptr) { err = "nothing rendered yet"; return Value(); }
        std::string path = a["path"].AsString();
        if (path.empty()) {
            std::error_code ec;
            std::filesystem::create_directories("devbridge_shots", ec);
            char name[64];
            std::snprintf(name, sizeof(name), "devbridge_shots/shot_%03d.png", ++shotCounter_);
            path = name;
        }
        if (!st::render::Get().SaveImage(*tex, path)) { err = "could not write " + path; return Value(); }
        std::error_code ec;
        return Value(std::filesystem::absolute(path, ec).generic_u8string());
    });
    Register("editor.mode", "{on} -> editor mode on/off", [&app](const Value& a, std::string&) {
        if (a.Has("on")) {
            if (a["on"].AsBool() && !app.IsDevUIVisible()) app.SetDevUIVisible(true);
            app.SetEditorMode(a["on"].AsBool());
        }
        return Value(app.IsEditorMode());
    });
    Register("script.run", "{code} -> runs Taipan in the game's script VM; returns the log lines it produced",
        [](const Value& a, std::string& err) {
        const std::string before = wi::backlog::getText();
        const bool ok = st::ScriptSystem::Get().RunConsole(a["code"].AsString());
        const std::string after = wi::backlog::getText();
        Value o = Value::Object();
        o.Set("ok", ok);
        o.Set("output", after.size() > before.size() ? after.substr(before.size()) : std::string());
        if (!ok) err = "script error: " + o["output"].AsString();
        return o;
    });

    // vehicles
    // The live stVehicle instance on an entity. Attaching is a metadata write, so a vehicle
    // attached this frame has no instance until the next scene update.
    auto findVehicle = [](WScene& scene, Entity e, std::string& err) -> st::VehicleComponent* {
        auto it = scene.nativeComponents.instances.find(e);
        if (it != scene.nativeComponents.instances.end())
            for (auto& inst : it->second)
                if (inst.name == "stVehicle") return static_cast<st::VehicleComponent*>(inst.component.get());
        err = "no stVehicle instance on that entity (just attached? wait one frame)";
        return nullptr;
    };
    Register("vehicle.fit_visual", "{id|name, model?} fit a child model to the simulated vehicle (scale to wheelbase, front to +Z, wheels on wheels)", [S, findVehicle](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        st::VehicleComponent* v = findVehicle(scene, e, err);
        if (v == nullptr) return Value();
        Entity model = INVALID_ENTITY;
        if (a.Has("model")) {
            model = ResolveEntity(scene, a, err, "model");
            if (model == INVALID_ENTITY) return Value();
        }
        Value o = Value::Object();
        o.Set("report", v->FitVisual(model));
        return o;
    });
    Register("vehicle.reset", "{id|name} put the vehicle back on its wheels where it stands", [S, findVehicle](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        st::VehicleComponent* v = findVehicle(scene, e, err);
        if (v == nullptr) return Value();
        v->ResetUpright();
        return Value::Object();
    });
    Register("vehicle.telemetry", "{id|name} -> speed, rpm, gear, inputs, per-wheel load/slip/contact", [S, findVehicle](const Value& a, std::string& err) {
        WScene& scene = S();
        const Entity e = ResolveEntity(scene, a, err);
        if (e == INVALID_ENTITY) return Value();
        st::VehicleComponent* v = findVehicle(scene, e, err);
        if (v == nullptr) return Value();
        Value o = Value::Object();
        o.Set("has_body", v->hasTelemetry);
        o.Set("speed_kmh", v->telemetry.forward_speed * 3.6f);
        o.Set("rpm", v->telemetry.engine_rpm);
        o.Set("gear", v->telemetry.gear);
        o.Set("throttle", v->throttle);
        o.Set("brake", v->brake);
        o.Set("steer", v->steer);
        if (const auto* t = scene.transforms.GetComponent(e)) {
            o.Set("position", Vec3(t->GetPosition()));
            o.Set("rotation_deg", Vec3(EulerDegFromQuat(t->GetRotation())));
        }
        Value wheels = Value::Array();
        for (size_t i = 0; i < v->telemetry.wheels.size(); ++i) {
            const auto& w = v->telemetry.wheels[i];
            Value wj = Value::Object();
            wj.Set("contact", w.contact);
            wj.Set("load_n", v->WheelLoad(i));
            wj.Set("suspension_m", w.suspension_length);
            wj.Set("slip_ratio", w.longitudinal_slip);
            wj.Set("slip_angle_deg", XMConvertToDegrees(w.lateral_slip));
            wj.Set("mu_long", w.longitudinal_friction);
            wj.Set("mu_lat", w.lateral_friction);
            wheels.push_back(std::move(wj));
        }
        o.Set("wheels", std::move(wheels));
        return o;
    });
}

} // namespace st::dev
