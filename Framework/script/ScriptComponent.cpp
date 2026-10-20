#include "script/ScriptComponent.h"
#include "script/ScriptSystem.h"
#include "devui/imscripteditor.h"
#include "ui/UISystem.h"

#include "imgui.h"
#include "wiBacklog.h"
#include "wiScene.h"

#include <algorithm>
#include <cstdio>

namespace st {

namespace tb = st::taipan;

namespace {

// Vectors and colours are saved as one string of components: "x y z" / "r g b a".
int ComponentCount (tb::Type t) {
    switch (t) {
    case tb::Type::Vector2:    return 2;
    case tb::Type::Vector3:    return 3;
    case tb::Type::Quaternion:
    case tb::Type::Color:      return 4;
    default:                   return 0;
    }
}

tb::Value ValueOf (tb::Type t, const float* v) {
    switch (t) {
    case tb::Type::Vector2:    return tb::Value::Vector2(tb::Vec2{ v[0], v[1] });
    case tb::Type::Vector3:    return tb::Value::Vector3(tb::Vec3{ v[0], v[1], v[2] });
    case tb::Type::Quaternion: return tb::Value::Quaternion(tb::Quat{ v[0], v[1], v[2], v[3] });
    case tb::Type::Color:      return tb::Value::Color(tb::Rgba{ v[0], v[1], v[2], v[3] });
    default:                   return tb::Value::Null();
    }
}

} // namespace

// ---------------------------------------------------------------------------
// script fields
// ---------------------------------------------------------------------------
std::vector<std::string> ScriptComponent::RefreshFields () {
    struct Listed { std::string name; tb::Type type; };
    std::vector<Listed> listed;
    tb::VM* vm = ScriptSystem::Get().VM();
    tb::ListFields(vm, script_, [](const char* name, tb::Type type, void* user) {
        static_cast<std::vector<Listed>*>(user)->push_back(Listed{ name, type });
    }, &listed);

    std::vector<Field> next;
    std::vector<std::string> added;
    for (const Listed& l : listed) {
        auto old = std::find_if(fields_.begin(), fields_.end(),
                                [&](const Field& f) { return f.name == l.name && f.type == l.type; });
        if (old != fields_.end()) {
            next.push_back(*old);
            continue;
        }
        // New field: what the script holds now is its initializer - the default.
        Field f;
        f.name = l.name;
        f.type = l.type;
        tb::Value v;
        if (tb::GetGlobal(vm, script_, l.name.c_str(), &v)) {
            if (v.type == tb::Type::String) {
                f.defText = v.str ? v.str : "";
                v.str = nullptr;
            }
            f.def = v;
        }
        next.push_back(std::move(f));
        added.push_back(l.name);
    }
    fields_ = std::move(next);
    // A string default points into its own Field, which may have moved since: re-point.
    for (Field& f : fields_)
        if (f.type == tb::Type::String) f.def.str = f.defText.c_str();
    return added;
}

void ScriptComponent::ApplySavedFields (const std::vector<std::string>& names) {
    tb::VM* vm = ScriptSystem::Get().VM();
    for (const Field& f : fields_) {
        if (!names.empty() && std::find(names.begin(), names.end(), f.name) == names.end()) continue;
        const std::string key = FieldKey(f.name);
        if (!HasParam(key)) continue;
        tb::Value v;
        std::string text;
        switch (f.type) {
        case tb::Type::Int:    v = tb::Value::Int(GetInt(key)); break;
        case tb::Type::Float:  v = tb::Value::Float(GetFloat(key)); break;
        case tb::Type::Bool:   v = tb::Value::Bool(GetBool(key)); break;
        case tb::Type::String:
            text = GetString(key);
            v = tb::Value::String(text.c_str());
            break;
        default: {
            float c[4] = { 0, 0, 0, 1 };
            const int n = ComponentCount(f.type);
            if (std::sscanf(GetString(key).c_str(), "%f %f %f %f", &c[0], &c[1], &c[2], &c[3]) < n) continue;
            v = ValueOf(f.type, c);
            break;
        }
        }
        tb::SetGlobal(vm, script_, f.name.c_str(), v);
    }
}

void ScriptComponent::SaveField (const Field& f, const tb::Value& v) {
    const std::string key = FieldKey(f.name);
    switch (f.type) {
    case tb::Type::Int:    SetInt(key, (int)v.i); break;
    case tb::Type::Float:  SetFloat(key, (float)v.f); break;
    case tb::Type::Bool:   SetBool(key, v.b); break;
    case tb::Type::String: SetString(key, v.str ? v.str : ""); break;
    default: {
        char buffer[128];
        const int n = ComponentCount(f.type);
        if (n == 2)      std::snprintf(buffer, sizeof(buffer), "%g %g", v.v[0], v.v[1]);
        else if (n == 3) std::snprintf(buffer, sizeof(buffer), "%g %g %g", v.v[0], v.v[1], v.v[2]);
        else             std::snprintf(buffer, sizeof(buffer), "%g %g %g %g", v.v[0], v.v[1], v.v[2], v.v[3]);
        SetString(key, buffer);
        break;
    }
    }
}

void ScriptComponent::ResetField (const Field& f) {
    // Drop the saved value from every map it could be in, then put the script's back.
    if (wi::scene::MetadataComponent* m = scene ? scene->metadatas.GetComponent(entity) : nullptr) {
        const std::string full = "NCA_" + std::to_string(localID) + "_" + FieldKey(f.name);
        m->bool_values.erase(full);
        m->int_values.erase(full);
        m->float_values.erase(full);
        m->string_values.erase(full);
    }
    tb::SetGlobal(ScriptSystem::Get().VM(), script_, f.name.c_str(), f.def);
}

void ScriptComponent::DrawFields () {
    if (fields_.empty()) return;
    ImGui::SeparatorText("Script fields");
    tb::VM* vm = ScriptSystem::Get().VM();
    for (const Field& f : fields_) {
        tb::Value v;
        if (!tb::GetGlobal(vm, script_, f.name.c_str(), &v)) continue;
        ImGui::PushID(f.name.c_str());
        const bool saved = HasParam(FieldKey(f.name));
        bool changed = false;
        tb::Value out = v;
        std::string text;
        switch (f.type) {
        case tb::Type::Int: {
            int x = (int)v.i;
            if ((changed = ImGui::DragInt(f.name.c_str(), &x))) out = tb::Value::Int(x);
            break;
        }
        case tb::Type::Float: {
            float x = (float)v.f;
            if ((changed = ImGui::DragFloat(f.name.c_str(), &x, 0.05f))) out = tb::Value::Float(x);
            break;
        }
        case tb::Type::Bool: {
            bool x = v.b;
            if ((changed = ImGui::Checkbox(f.name.c_str(), &x))) out = tb::Value::Bool(x);
            break;
        }
        case tb::Type::String: {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "%s", v.str ? v.str : "");
            if ((changed = ImGui::InputText(f.name.c_str(), buffer, sizeof(buffer)))) {
                text = buffer;
                out = tb::Value::String(text.c_str());
            }
            break;
        }
        case tb::Type::Vector2:
            changed = ImGui::DragFloat2(f.name.c_str(), out.v, 0.05f);
            break;
        case tb::Type::Vector3:
            changed = ImGui::DragFloat3(f.name.c_str(), out.v, 0.05f);
            break;
        case tb::Type::Quaternion: {
            // Edited as Euler degrees, the way Unity's inspector shows rotations.
            tb::Vec3 e = tb::QuaternionToEuler(tb::Quat{ v.v[0], v.v[1], v.v[2], v.v[3] });
            float deg[3] = { e.x, e.y, e.z };
            if ((changed = ImGui::DragFloat3(f.name.c_str(), deg, 0.5f)))
                out = tb::Value::Quaternion(tb::EulerToQuaternion(tb::Vec3{ deg[0], deg[1], deg[2] }));
            break;
        }
        case tb::Type::Color:
            changed = ImGui::ColorEdit4(f.name.c_str(), out.v, ImGuiColorEditFlags_Float);
            break;
        default:
            break;
        }
        if (ImGui::BeginPopupContextItem("##fieldmenu")) { // on the field widget itself
            if (ImGui::MenuItem("Reset to script value", nullptr, false, saved)) ResetField(f);
            ImGui::EndPopup();
        }
        if (saved) {
            // Like a Unity prefab override: this entity's value differs from the script's.
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.35f, 0.7f, 1.0f, 1.0f), "*");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Saved for this entity. Right-click the field to reset.");
        }
        if (changed) {
            tb::SetGlobal(vm, script_, f.name.c_str(), out);
            SaveField(f, out);
        }
        ImGui::PopID();
    }
}

bool ScriptComponent::Ready () const {
    return script_ && !faulted_ && generation_ == ScriptSystem::Get().Generation();
}

void ScriptComponent::ResolveCallbacks () {
    tb::VM* vm = ScriptSystem::Get().VM();
    // Unity's name first, then the 0.1 one. `takesDt` only ever applies to the 0.1 form.
    auto pick = [&](Slot& slot, const char* unity, const char* legacy, bool legacyTakesDt) {
        slot = Slot{};
        if (unity && tb::HasFunction(vm, script_, unity)) slot = Slot{ unity, false };
        else if (legacy && tb::HasFunction(vm, script_, legacy)) slot = Slot{ legacy, legacyTakesDt };
    };
    pick(awake_, "Awake", nullptr, false);
    pick(onEnable_, "OnEnable", nullptr, false);
    pick(start_, "Start", "onStart", false);
    pick(update_, "Update", "onUpdate", true);
    pick(fixedUpdate_, "FixedUpdate", "onFixedUpdate", true);
    pick(onDisable_, "OnDisable", nullptr, false);
    pick(destroy_, "OnDestroy", "onDestroy", false);
}

void ScriptComponent::Start () {
    Bind(file, "file");
    seenReload_ = ScriptSystem::Get().ReloadSerial();
    Load();
}

void ScriptComponent::OnReloaded () {
    seenReload_ = ScriptSystem::Get().ReloadSerial();
    if (!script_ || generation_ != ScriptSystem::Get().Generation()) {
        // Never compiled (or the VM restarted): the saved text may compile now.
        Unload(false);
        Load();
        return;
    }
    // The VM swapped the new functions in place; the set of callbacks and fields may have
    //	changed. Globals keep their values across a reload, so only a field that is new
    //	takes its saved value.
    ResolveCallbacks();
    ApplySavedFields(RefreshFields());
    if (faulted_) {
        faulted_ = false;
        status_ = "running (reloaded)";
    }
}

void ScriptComponent::Load () {
    loadedFile_ = file;
    faulted_ = false;
    awake_ = onEnable_ = start_ = update_ = fixedUpdate_ = onDisable_ = destroy_ = Slot{};
    fields_.clear();
    if (file.empty()) {
        status_ = "no file";
        return;
    }
    tb::VM* vm = ScriptSystem::Get().VM();
    if (!vm) {
        status_ = "no script VM";
        return;
    }
    script_ = tb::NewScript(vm, file.c_str());
    if (!script_) {
        status_ = "compile error (see backlog)";
        return;
    }
    generation_ = ScriptSystem::Get().Generation();
    if (!tb::RunScript(vm, script_)) {
        faulted_ = true;
        status_ = "error in top-level code (see backlog)";
        return;
    }
    // Top-level code has run, so every field holds its initializer: record those as the
    //	defaults, then let this entity's saved values replace them before anything starts.
    RefreshFields();
    ApplySavedFields({});
    ResolveCallbacks();
    status_ = "running";
    // Unity's order for a fresh, enabled behaviour.
    if (awake_.Present()) Invoke(awake_, 0.0f);
    if (onEnable_.Present() && Ready()) Invoke(onEnable_, 0.0f);
    if (start_.Present() && Ready()) Invoke(start_, 0.0f);
}

void ScriptComponent::Unload (bool callDestroy) {
    if (script_ && generation_ == ScriptSystem::Get().Generation()) {
        if (callDestroy && destroy_.Present() && !faulted_) Invoke(destroy_, 0.0f);
        tb::FreeScript(ScriptSystem::Get().VM(), script_);
    }
    // A script from a VM that has since shut down is already gone with it.
    script_ = nullptr;
    status_ = "not loaded";
}

bool ScriptComponent::CallEvent (const char* name, const tb::Value* args, int argc) {
    if (!Ready() || name == nullptr || name[0] == 0) return false;
    tb::VM* vm = ScriptSystem::Get().VM();
    if (!tb::HasFunction(vm, script_, name)) return false;
    if (!tb::CallFunction(vm, script_, name, args, argc))
        wi::backlog::post("[taipan] " + file + ": error in UI event handler " + name,
                          wi::backlog::LogLevel::Warning);
    return true;
}

void ScriptComponent::Invoke (const Slot& slot, float dt) {
    tb::Value args[2] = { EntityValue(entity), tb::Value::Float(dt) };
    if (tb::CallFunction(ScriptSystem::Get().VM(), script_, slot.name.c_str(), args, slot.withDt ? 2 : 1)) return;
    faulted_ = true;
    status_ = "faulted in " + slot.name + " (see backlog)";
    wi::backlog::post("[taipan] " + file + ": stopped after an error in " + slot.name +
                      " - fix it and save, or press Reload", wi::backlog::LogLevel::Warning);
}

void ScriptComponent::Update (float dt) {
    // The inspector edited the path: this instance switches files.
    if (file != loadedFile_) {
        Unload(true);
        Load();
    } else if (seenReload_ != ScriptSystem::Get().ReloadSerial()) {
        OnReloaded();
    }
    if (Ready() && update_.Present()) Invoke(update_, dt);
}

void ScriptComponent::FixedUpdate (float fixedDt) {
    if (!Ready() || !fixedUpdate_.Present()) return;
    // Inside FixedUpdate, Unity's Time.deltaTime is the fixed step. Swap it in for the
    //	call and put the frame's value back after, so Update and coroutines see theirs.
    tb::VM* vm = ScriptSystem::Get().VM();
    SetFixedDeltaTime(vm, fixedDt);
    Invoke(fixedUpdate_, fixedDt);
    SetFixedDeltaTime(vm, ScriptSystem::Get().FrameDeltaTime());
}

void ScriptComponent::OnEnable () {
    // The first enable happens before Start - before the script exists - and Load runs
    //	the script's OnEnable itself, so only later transitions arrive here with work to do.
    if (Ready() && onEnable_.Present()) Invoke(onEnable_, 0.0f);
}

void ScriptComponent::OnDisable () {
    if (Ready() && onDisable_.Present()) Invoke(onDisable_, 0.0f);
}

void ScriptComponent::Destroy () {
    Unload(true);
    st::ui::UISystem::Get().UnsubscribeTarget(entity);   // its UI.On handlers
}

void ScriptComponent::Reload () {
    if (script_ && generation_ == ScriptSystem::Get().Generation()) {
        // Every instance of the file picks up the new code and keeps its globals.
        if (ScriptSystem::Get().Reload(file)) {
            ResolveCallbacks();
            ApplySavedFields(RefreshFields());
            faulted_ = false;
            status_ = "running (reloaded)";
        } else {
            status_ = "reload failed (see backlog)";
        }
        return;
    }
    // Never loaded (compile error, missing file, VM restarted): start over.
    Unload(false);
    Load();
}

void ScriptComponent::DescribeParams (wi::vector<NativeParam>& out) {
    out.push_back(NativeParam::Asset("file", &file,
        "Taipan script, e.g. assets/scripts/spinner.tp. Drop one from the Resource Explorer."));
    out.push_back(NativeParam::Action("Edit", [](NativeComponent& c) {
        const std::string& f = static_cast<ScriptComponent&>(c).file;
        if (!f.empty()) st::devui::OpenScriptEditor(f);
    }, "Open this script in the Script Editor. Saving there hot-reloads it."));
    out.push_back(NativeParam::Action("Reload", [](NativeComponent& c) {
        static_cast<ScriptComponent&>(c).Reload();
    }, "Hot reload this file for every entity running it (globals are kept).", nullptr, true));
}

void ScriptComponent::DrawDebug () {
    const bool bad = faulted_ || !script_;
    ImGui::TextColored(bad ? ImVec4(1, 0.6f, 0.3f, 1) : ImVec4(0.5f, 1, 0.6f, 1), "%s", status_.c_str());
    if (Ready()) {
        std::string list;
        for (const Slot* s : { &awake_, &onEnable_, &start_, &update_, &fixedUpdate_, &onDisable_, &destroy_ })
            if (s->Present()) list += " " + s->name;
        ImGui::TextDisabled("callbacks:%s", list.empty() ? " (none)" : list.c_str());
        DrawFields();
    }
}

} // namespace st

// The registration macro pastes the type name into an identifier, so it cannot take a
// qualified one; the alias is the same type (see InputComponent.cpp).
using StScriptComponent = st::ScriptComponent;
ST_REGISTER_FRAMEWORK_COMPONENT_AS(StScriptComponent, "stScript", "Scripting")
