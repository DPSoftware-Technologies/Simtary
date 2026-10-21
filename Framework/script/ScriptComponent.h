#pragma once
// The "stScript" native component - a Taipan script on an entity.
//
//     NCI_0      = "stScript"
//     NCA_0_file = "assets/scripts/spinner.tp"
//
// Each attachment is its own script INSTANCE (own globals, bytecode shared with every
// other instance of the same file), so ten crates running crate.tp keep ten separate
// states. The callbacks it drives, each optional, Unity's names first:
//
//     func Awake(GameObject self)          func OnEnable(GameObject self)
//     func Start(GameObject self)          func OnDisable(GameObject self)
//     func Update(GameObject self)         func OnDestroy(GameObject self)
//     func FixedUpdate(GameObject self)    (Time.deltaTime is the fixed step inside it)
//
// Fields: the script's top-level variables are its inspector fields, Unity's public
// fields - `float speed = 4.0` shows as a "speed" slider on this component.
//
//     float speed = 4.0           // int, float, bool, str, Vector2, Vector3,
//     Color tint = Color.red      // Quaternion (edited as Euler degrees), Color
//     float _timer = 0.0          // a leading '_' (or `const`) keeps it hidden
//
// An edit changes the running value at once and is saved with the scene as
// NCA_<id>_v.<name>; on load the saved value replaces the script's initializer before
// Awake/Start run, so each entity using the same script can be tuned on its own.
// Right-click a field to reset it to the script's value.
//
// Awake, OnEnable and Start run in that order when the script loads. The 0.1 names
// still work: onStart(self), onUpdate(self, dt), onFixedUpdate(self, dt), onDestroy(self).
//
// Errors: a compile error leaves the component idle with the diagnostics in the
// backlog. A runtime error that escapes a callback FAULTS the instance - it stops
// calling into the script rather than repeating the same error every frame. Fix the
// file - "Edit" in the inspector opens it in the Script Editor - and save, or press
// Reload: either hot-reloads every live instance of the file (state kept), and every
// stScript that was faulted or never loaded retries on its next frame.
//
// Main thread only: the VM is single-threaded, so this component opts out of the
// parallel Compute/FixedUpdate/Update stages.

#include "stNativeComponent.h"
#include "StTaipan.h"

#include <string>
#include <vector>

namespace st {

struct ScriptComponent : wi::scene::NativeComponent {
    ST_NATIVE_COMPONENT_MAIN_THREAD()

    std::string file; // bound from NCA_<localID>_file
    // Where it runs in a network game (MULTIPLAYER.md v2): 0 everywhere, 1 only on the server
    //	(dedicated or host), 2 only where there is a player (client, or the host). Offline
    //	every script runs. Bound from NCA_<localID>_runOn.
    int runOn = 0;

    // What the inspector shows: "running", "faulted in onUpdate", "compile error", ...
    const std::string& Status () const { return status_; }

    void Start () override;
    void Update (float dt) override;
    void FixedUpdate (float fixedDt) override;
    void OnEnable () override;
    void OnDisable () override;
    void Destroy () override;
    void DescribeParams (wi::vector<NativeParam>& out) override;
    void DrawDebug () override;

    // Hot reload the file for every instance, then retry this one if it never loaded.
    void Reload ();

    // Call `name` with `args` if the running script defines it - what UI events use
    //	(a Button's onClick, a document's onclick="..."). False when the script is not
    //	running or has no such function; an error inside it is logged, not a fault.
    bool CallEvent (const char* name, const st::taipan::Value* args, int argc);

private:
    void Load ();
    void Unload (bool callDestroy);
    bool Ready () const;
    // A reload happened somewhere (Script Editor save, another instance's Reload).
    void OnReloaded ();
    // One callback slot: the script function it calls ("" = the script has none) and
    //	whether it takes dt as a second argument (the 0.1 on* form) or reads
    //	Time.deltaTime (Unity's form).
    struct Slot {
        std::string name;
        bool withDt = false;
        bool Present () const { return !name.empty(); }
    };
    // Looks the slots up again - after a load and after every reload, since an edit
    //	can add or remove callbacks.
    void ResolveCallbacks ();
    // Calls a slot with (self[, dt]). A false return faults the instance.
    void Invoke (const Slot& slot, float dt);

    // One inspector field. `def` is the script's own initial value (strings live in
    //	defText, which def.str points into).
    struct Field {
        std::string name;
        st::taipan::Type type = st::taipan::Type::Other;
        st::taipan::Value def;
        std::string defText;
    };
    // Re-list the script's fields. A field seen for the first time records its current
    //	value as the default. Returns the names that are new.
    std::vector<std::string> RefreshFields ();
    // Saved (NCA_) values -> the running script, for `names` (all fields when empty).
    void ApplySavedFields (const std::vector<std::string>& names);
    void SaveField (const Field& f, const st::taipan::Value& v);
    void ResetField (const Field& f);
    void DrawFields ();
    static std::string FieldKey (const std::string& name) { return "v." + name; }

    std::vector<Field> fields_;
    st::taipan::Script* script_ = nullptr;
    unsigned generation_ = 0;   // ScriptSystem generation script_ belongs to
    unsigned seenReload_ = 0;   // ScriptSystem::ReloadSerial() last acted on
    std::string loadedFile_;
    std::string status_ = "not loaded";
    bool faulted_ = false;
    Slot awake_, onEnable_, start_, update_, fixedUpdate_, onDisable_, destroy_;
};

} // namespace st
