#include "script/ScriptSystem.h"

#include "io/asset/AssetSystem.h"
#include "io/asset/StHash.h"
#include "stApp.h"
#include "wiBacklog.h"
#include "wiECS.h"
#include "wiHelper.h"

#include <algorithm>
#include <filesystem>

namespace st {

namespace {

namespace tb = st::taipan;
namespace fs = std::filesystem;

void PrintToBacklog (const char* text, void*) {
    wi::backlog::post(std::string("[taipan] ") + text);
}

// Overrides are keyed the way the asset packages key their paths, so "Assets\\Scripts\\AI.tp"
//	and "assets/scripts/ai.tp" are one script on every platform.
std::string OverrideKey (const std::string& logicalPath) {
    return asset::CanonicalPath(logicalPath);
}

bool EndsWithTp (const std::string& path) {
    const std::string c = asset::CanonicalPath(path);
    return c.size() > 3 && c.compare(c.size() - 3, 3, ".tp") == 0;
}

// Every .tp under `root`, reported as `prefix` + its path relative to `root`.
void CollectTp (const fs::path& root, const std::string& prefix, std::vector<std::string>& out) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = fs::relative(it->path(), root, ec).generic_string();
        if (!ec && EndsWithTp(rel)) out.push_back(prefix + rel);
    }
}

} // namespace

ScriptSystem& ScriptSystem::Get () {
    static ScriptSystem instance;
    return instance;
}

void ScriptSystem::ErrorToBacklog (const char* text, void* user) {
    ScriptSystem* self = static_cast<ScriptSystem*>(user);
    if (self && self->captureErrors_) {
        *self->captureErrors_ += text;
        *self->captureErrors_ += "\n";
        return;
    }
    // Diagnostics are multi-line (source excerpt + caret), and the backlog colours a
    //	line red only when it says so itself - tag every line.
    std::string all(text);
    size_t start = 0;
    while (start < all.size()) {
        size_t end = all.find('\n', start);
        if (end == std::string::npos) end = all.size();
        if (end > start)
            wi::backlog::post("[taipan] " + all.substr(start, end - start), wi::backlog::LogLevel::Error);
        start = end + 1;
    }
}

// Taipan asks for absolute paths. The asset packages are keyed by the logical path the
//	game uses ("assets/scripts/ai.tp"), which is the same path relative to the working
//	directory - so fold it back, try the editor's overrides, and then let
//	wi::helper::FileRead decide: package first, then disk, like every other asset read.
bool ScriptSystem::ReadThroughAssets (const char* path, tb::Sink* sink, void* user) {
    std::string logical(path);
    wi::helper::MakePathRelative(wi::helper::GetCurrentPath(), logical);
    if (logical.empty() || logical.rfind("..", 0) == 0) return false; // outside the game: disk

    const ScriptSystem* self = static_cast<const ScriptSystem*>(user);
    auto it = self->overrides_.find(OverrideKey(logical));
    if (it != self->overrides_.end()) {
        tb::SinkWrite(sink, it->second.data(), it->second.size());
        return true;
    }
    wi::vector<uint8_t> data;
    if (!wi::helper::FileRead(logical, data)) return false;
    tb::SinkWrite(sink, data.data(), data.size());
    return true;
}

tb::VM* ScriptSystem::VM () {
    if (!vm_ && !creationFailed_) CreateVM();
    return vm_;
}

void ScriptSystem::CreateVM () {
    vm_ = tb::Create();
    if (!vm_) {
        creationFailed_ = true;
        wi::backlog::post("[taipan] could not create the script VM", wi::backlog::LogLevel::Error);
        return;
    }
    tb::SetPrintHandler(vm_, &PrintToBacklog, nullptr);
    tb::SetErrorHandler(vm_, &ScriptSystem::ErrorToBacklog, this);
    tb::SetFileReader(vm_, &ScriptSystem::ReadThroughAssets, this);
    tb::SetScriptRoot(vm_, kScriptRoot);
    // The cache writes .tpc files to %TEMP% and re-reads imports straight off the disk
    //	to validate them, which a packaged script cannot answer. Compiling is fast enough
    //	that the cache buys nothing at game-script sizes.
    tb::SetBytecodeCache(vm_, false);
    RegisterEngineBindings(vm_);
    wi::backlog::post("[taipan] VM ready (scripts from " + std::string(kScriptRoot) + ")");
}

void ScriptSystem::BeginFrame (float sceneDt, float realDt) {
    frameDt_ = sceneDt;
    if (vm_) UpdateEngineStatics(vm_, sceneDt, realDt);
}

void ScriptSystem::Update (float sceneDt) {
    if (vm_) tb::Update(vm_, sceneDt);
}

void ScriptSystem::Shutdown () {
    if (!vm_) return;
    tb::Destroy(vm_);
    vm_ = nullptr;
    ++generation_;
}

bool ScriptSystem::RunConsole (const std::string& source) {
    tb::VM* vm = VM();
    return vm && tb::RunString(vm, source.c_str(), "console");
}

bool ScriptSystem::Reload (const std::string& path) {
    tb::VM* vm = VM();
    if (!vm) return false;
    // Bumped even when nothing was live to reload: a stScript whose first load failed
    //	has no instance for the VM to update, and the serial is how it hears to retry.
    ++reloadSerial_;
    const bool ok = tb::Reload(vm, path.c_str());
    if (ok) wi::backlog::post("[taipan] reloaded " + path);
    return ok;
}

bool ScriptSystem::DumpBindings (const std::string& path) {
    tb::VM* vm = VM();
    return vm && tb::DumpBindings(vm, path.c_str());
}

// ---------------------------------------------------------------------------
// editing
// ---------------------------------------------------------------------------
void ScriptSystem::SetSourceOverride (const std::string& logicalPath, const std::string& text) {
    overrides_[OverrideKey(logicalPath)] = text;
}

bool ScriptSystem::HasSourceOverride (const std::string& logicalPath) const {
    return overrides_.find(OverrideKey(logicalPath)) != overrides_.end();
}

bool ScriptSystem::ReadSource (const std::string& logicalPath, std::string& out) const {
    auto it = overrides_.find(OverrideKey(logicalPath));
    if (it != overrides_.end()) {
        out = it->second;
        return true;
    }
    wi::vector<uint8_t> data;
    if (!wi::helper::FileRead(logicalPath, data)) return false;
    out.assign(reinterpret_cast<const char*>(data.data()), data.size());
    return true;
}

std::string ScriptSystem::SourcePathFor (const std::string& logicalPath) {
    const std::string logical = asset::NormalizePath(logicalPath);
    // Everything under assets/contents/ ships as "<mount point><path inside contents>",
    //	so the authoring copy is the logical path with the mount point taken off.
    std::string mount = asset::NormalizePath(App::Config().assetMountPoint);
    if (!mount.empty() && mount.back() != '/') mount += '/';
    std::string inside = logical;
    if (!mount.empty() && asset::CanonicalPath(logical).rfind(asset::CanonicalPath(mount), 0) == 0)
        inside = logical.substr(mount.size());
#ifdef ST_PROJECT_CONTENT_DIR
    const std::string contentDir = ST_PROJECT_CONTENT_DIR;
    if (!contentDir.empty()) return contentDir + "/" + inside;
#endif
    // No source tree baked in (a shipped build, a project without assets/contents): the
    //	loose file beside the game is the only place left.
    return logical;
}

bool ScriptSystem::SaveScript (const std::string& logicalPath, const std::string& text,
                               std::string* writtenTo, std::string* diagnostics, std::string* error) {
    const std::string target = SourcePathFor(logicalPath);
    if (target.empty()) {
        if (error) *error = "no place to save " + logicalPath;
        return false;
    }
    wi::helper::DirectoryCreate(wi::helper::GetDirectoryFromPath(target));
    // FileWrite refuses zero bytes; an emptied script is saved as one newline.
    const std::string body = text.empty() ? std::string("\n") : text;
    if (!wi::helper::FileWrite(target, reinterpret_cast<const uint8_t*>(body.data()), body.size())) {
        if (error) *error = "could not write " + target;
        return false;
    }
    if (writtenTo) *writtenTo = target;
    SetSourceOverride(logicalPath, body);

    // A failed reload is not a failed save: the file is written, and the compile error is
    //	what the editor shows next. "Has not been loaded" is not an error at all - nothing
    //	runs the file yet, and stScript picks the new text up when it loads.
    std::string captured;
    captureErrors_ = &captured;
    const bool reloaded = VM() && Reload(logicalPath);
    captureErrors_ = nullptr;
    if (!reloaded && captured.find("has not been loaded") == std::string::npos) {
        if (diagnostics) *diagnostics = captured;
        ErrorToBacklog(captured.c_str(), nullptr);
    }
    wi::backlog::post("[taipan] saved " + target);
    return true;
}

bool ScriptSystem::CheckScript (const std::string& logicalPath, const std::string& text,
                                std::string* diagnostics) {
    tb::VM* vm = VM();
    if (!vm) {
        if (diagnostics) *diagnostics = "no script VM";
        return false;
    }
    // Check the buffer by standing it in for the file for the length of the call, then
    //	put back whatever was there - the edit is not saved until Save says so.
    const std::string key = OverrideKey(logicalPath);
    auto previous = overrides_.find(key);
    const bool hadPrevious = previous != overrides_.end();
    const std::string saved = hadPrevious ? previous->second : std::string();
    overrides_[key] = text;

    std::string captured;
    captureErrors_ = &captured;
    const bool ok = tb::Check(vm, logicalPath.c_str());
    captureErrors_ = nullptr;

    if (hadPrevious) overrides_[key] = saved;
    else             overrides_.erase(key);
    if (diagnostics) *diagnostics = captured;
    return ok;
}

void ScriptSystem::ListScripts (std::vector<std::string>& out) const {
    out.clear();
    AssetSystem& assets = AssetSystem::Get();
    for (uint32_t m = 0; m < assets.MountCount(); ++m) {
        const asset::AssetPack* pack = assets.PackAt(m);
        if (pack == nullptr) continue;
        const std::string mount = assets.MountAt(m).mountPoint;
        for (uint32_t i = 0; i < pack->AssetCount(); ++i) {
            const std::string name = pack->NameString(*pack->AssetAt(i));
            if (EndsWithTp(name)) out.push_back(asset::NormalizePath(mount + name));
        }
    }
    for (const auto& kv : overrides_) out.push_back(kv.first);
    CollectTp(fs::u8path(kScriptRoot), std::string(kScriptRoot) + "/", out);
#ifdef ST_PROJECT_CONTENT_DIR
    const std::string contentDir = ST_PROJECT_CONTENT_DIR;
    if (!contentDir.empty()) {
        std::string mount = asset::NormalizePath(App::Config().assetMountPoint);
        if (!mount.empty() && mount.back() != '/') mount += '/';
        CollectTp(fs::u8path(contentDir), mount, out);
    }
#endif
    // One entry per script whatever spelling each source used.
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return asset::CanonicalPath(a) < asset::CanonicalPath(b);
    });
    out.erase(std::unique(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return asset::CanonicalPath(a) == asset::CanonicalPath(b);
    }), out.end());
}

// ---------------------------------------------------------------------------
// bindings for games
// ---------------------------------------------------------------------------
bool ScriptSystem::RegisterFunction (const char* dottedName, tb::NativeFn fn, void* user,
                                     const char* signature) {
    tb::VM* vm = VM();
    return vm && tb::RegisterFunction(vm, dottedName, fn, user, signature);
}

bool ScriptSystem::DefineClass (const char* name, const char* alias) {
    tb::VM* vm = VM();
    return vm && tb::DefineClass(vm, name, alias);
}

bool ScriptSystem::RegisterMethod (const char* className, const char* name, tb::MethodFn fn,
                                   void* user, const char* signature) {
    tb::VM* vm = VM();
    return vm && tb::RegisterMethod(vm, className, name, fn, user, signature);
}

bool ScriptSystem::RegisterProperty (const char* className, const char* name, const char* type,
                                     tb::GetterFn getter, tb::SetterFn setter, void* user) {
    tb::VM* vm = VM();
    return vm && tb::RegisterProperty(vm, className, name, type, getter, setter, user);
}

tb::Value EntityValue (uint64_t entity) {
    // The id rides in the handle's pointer slot, so it must fit in one.
    static_assert(sizeof(void*) >= sizeof(wi::ecs::Entity), "Entity ids need 64-bit handles");
    return tb::Value::Object(kEntityClass, reinterpret_cast<void*>(static_cast<uintptr_t>(entity)));
}

} // namespace st
