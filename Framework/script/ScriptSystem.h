#pragma once
// st::ScriptSystem - Taipan scripting for every game, at the framework level.
//
// Lua stays where it was: the engine's wi::lua, ScriptComponent, startup.lua and the
// backlog's Lua console are untouched. Taipan sits beside it, one level up, and the two
// never see each other.
//
//	what           where
//	VM + bindings  this file. One VM for the process, created on first use, main thread.
//	per entity     the "stScript" native component (script/ScriptComponent.h): one
//	               script INSTANCE per attachment - own globals, shared bytecode.
//	console        Simtary > Taipan Console (devui/imscriptconsole.h).
//	sources        "assets/scripts/..." through wi::helper::FileRead, so a .tp inside a
//	               mounted asset package loads exactly like one on disk. Imports resolve
//	               from assets/scripts: `import util.math` is assets/scripts/util/math.tp.
//
// A script attached through stScript is driven by these functions, each optional:
//
//	func onStart(Entity self)                       first frame the component runs
//	func onUpdate(Entity self, float dt)            every frame (scene time: paused = 0)
//	func onFixedUpdate(Entity self, float dt)       fixed 60 Hz step
//	func onDestroy(Entity self)                     component or entity removed
//
// (`start` is a Taipan keyword - it launches a coroutine - hence the on- prefix.)
// Coroutines started from a script resume on scene time, after the scene update.
//
// Engine API scripts see (registered in ScriptBindings.cpp; the Script Console can
// write it as a .tpi stub for the language server):
//
//	scene.find(str name) -> Entity?
//	log.info(str) / log.warn(str) / log.error(str)
//	class Entity { id, name, x, y, z, translate, setPosition, rotate, setScale,
//	               isValid, inputDown/Pressed/Released/Value/X/Y (via its stInput) }
//
// The VM is single-threaded and so is everything here: call it from the main thread.
// stScript opts out of the parallel component stages for exactly that reason.
//
// Games add their own natives through Register*() below (st::App::OnInitialize is the
// place), BEFORE any script that uses them loads - the type checker reads signatures
// at load time.

#include "StTaipan.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace st {

class ScriptSystem {
public:
    static ScriptSystem& Get ();

    // The VM, created on first call. Null only if creation failed (logged).
    st::taipan::VM* VM ();
    bool Alive () const { return vm_ != nullptr; }

    // Per-frame: resume due coroutines. Called by st::App after the scene update.
    void Update (float sceneDt);
    // Per-frame: refresh Time / Input for scripts. Called by st::App BEFORE the scene update.
    void BeginFrame (float sceneDt, float realDt);
    // This frame's Time.deltaTime (stScript restores it after a FixedUpdate).
    float FrameDeltaTime () const { return frameDt_; }
    // Tear down. Script instances still held by components become no-ops.
    void Shutdown ();

    // Console: run a chunk in the VM's main environment. Output goes to the backlog.
    bool RunConsole (const std::string& source);
    // Hot reload every live instance of a file ("assets/scripts/ai.tp"). Bumps
    //	ReloadSerial(), which is how a faulted or never-loaded stScript knows to retry.
    bool Reload (const std::string& path);
    unsigned ReloadSerial () const { return reloadSerial_; }

    // ---- editing (the Script Editor, devui/imscripteditor.h) -----------------------
    // Paths here are LOGICAL ("assets/scripts/ai.tp"), the same ones stScript names.
    //
    // A source override is text the VM reads INSTEAD of the package or the disk. It is
    // what lets an edit take effect in a packaged build: the mounted package is
    // read-only and wins over loose files, so writing a file alone would change nothing
    // until the next build. Overrides last for the session.
    void SetSourceOverride (const std::string& logicalPath, const std::string& text);
    bool HasSourceOverride (const std::string& logicalPath) const;
    // Current text of a script: the override if there is one, else package / disk.
    bool ReadSource (const std::string& logicalPath, std::string& out) const;
    // Save: write the authoring copy (assets/contents/... in the project's source tree
    //	when this build knows where that is, else the loose file beside the game), set the
    //	override, and hot reload every instance running it. `writtenTo` gets the file
    //	actually written; `diagnostics` gets the reload's compile errors, if any (the
    //	file is saved either way - instances keep the last good code until it compiles).
    bool SaveScript (const std::string& logicalPath, const std::string& text,
                     std::string* writtenTo, std::string* diagnostics, std::string* error);
    // Type-check `text` as if it were saved at `logicalPath`, without running it or
    //	keeping it. Diagnostics go to `diagnostics`, not to the backlog.
    bool CheckScript (const std::string& logicalPath, const std::string& text,
                      std::string* diagnostics);
    // Every .tp the game can see: mounted packages, overrides, loose files, and the
    //	project's source tree. Sorted, de-duplicated, logical paths.
    void ListScripts (std::vector<std::string>& out) const;
    // Where Save writes a logical path. Empty when there is nowhere sensible.
    static std::string SourcePathFor (const std::string& logicalPath);
    // Write every registered binding as a .tpi stub (for the Taipan language server).
    bool DumpBindings (const std::string& path);

    // Native bindings for games. Thin forwards to st::taipan, kept here so game code
    //	never has to create or find the VM itself.
    bool RegisterFunction (const char* dottedName, st::taipan::NativeFn fn, void* user,
                           const char* signature);
    bool DefineClass (const char* name, const char* alias = nullptr);
    bool RegisterMethod (const char* className, const char* name, st::taipan::MethodFn fn,
                         void* user, const char* signature);
    bool RegisterProperty (const char* className, const char* name, const char* type,
                           st::taipan::GetterFn getter, st::taipan::SetterFn setter, void* user);

    // Generation counter: bumped by Shutdown(), so a component holding a Script* from a
    //	VM that is gone can tell and drop it instead of freeing into a dead VM.
    unsigned Generation () const { return generation_; }

    static constexpr const char* kScriptRoot = "assets/scripts";

private:
    ScriptSystem () = default;
    void CreateVM ();

    static bool ReadThroughAssets (const char* path, st::taipan::Sink* sink, void* user);
    static void ErrorToBacklog (const char* text, void* user);

    st::taipan::VM* vm_ = nullptr;
    bool creationFailed_ = false;
    unsigned generation_ = 1;
    unsigned reloadSerial_ = 0;
    float frameDt_ = 0.0f;
    // CheckScript diverts diagnostics here instead of the backlog.
    std::string* captureErrors_ = nullptr;
    std::unordered_map<std::string, std::string> overrides_; // logical path -> text
};

// Engine bindings (ScriptBindings.cpp): scene, log, Entity. Called once by CreateVM.
void RegisterEngineBindings (st::taipan::VM* vm);
// Time.* / Input.* statics and the action map, once per frame before scripts run.
void UpdateEngineStatics (st::taipan::VM* vm, float sceneDt, float realDt);
// Time.deltaTime while FixedUpdate runs.
void SetFixedDeltaTime (st::taipan::VM* vm, float fixedDt);

// Entity <-> script handle. Entities are ids, not pointers: the id IS the handle value,
//	and every native re-resolves it against the scene, so a removed entity reads as
//	"invalid" instead of dangling.
st::taipan::Value EntityValue (uint64_t entity); // wi::ecs::Entity
constexpr const char* kEntityClass = "GameObject"; // "Entity" is its 0.1 alias

} // namespace st
