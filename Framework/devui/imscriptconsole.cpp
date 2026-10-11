#include "devui/imscriptconsole.h"
#include "script/ScriptSystem.h"

#include "imgui.h"
#include "wiBacklog.h"

#include <string>

namespace st::devui {

namespace {

char source[8192] = "Entity? e = scene.find(\"Sun\")\nif e != null {\n    log.info(f\"{e.name} at {e.y}\")\n}\n";
std::string lastResult;
bool lastOk = true;

void Run () {
    lastOk = st::ScriptSystem::Get().RunConsole(source);
    lastResult = lastOk ? "ok" : st::taipan::LastError(st::ScriptSystem::Get().VM());
}

} // namespace

void ScriptConsoleWindow (bool* show) {
    if (!ImGui::Begin("Taipan Console", show)) {
        ImGui::End();
        return;
    }

    st::ScriptSystem& scripts = st::ScriptSystem::Get();
    st::taipan::VM* vm = scripts.VM();
    if (!vm) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Script VM could not be created (see backlog).");
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("Taipan VM - %d coroutine(s) running. Output goes to the backlog.",
                        st::taipan::CoroutineCount(vm));

    ImGui::InputTextMultiline("##source", source, sizeof(source),
                              ImVec2(-1, ImGui::GetTextLineHeight() * 12),
                              ImGuiInputTextFlags_AllowTabInput);
    const bool runKey = ImGui::IsItemFocused() && ImGui::GetIO().KeyCtrl &&
                        ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    if (ImGui::Button("Run (Ctrl+Enter)") || runKey) Run();
    ImGui::SameLine();
    if (ImGui::Button("Write bindings stub")) {
        const char* path = "taipan_bindings.tpi";
        lastOk = scripts.DumpBindings(path);
        lastResult = lastOk ? std::string("wrote ") + path : "could not write " + std::string(path);
        wi::backlog::post("[taipan] " + lastResult);
    }

    if (!lastResult.empty()) {
        ImGui::Separator();
        if (lastOk) {
            ImGui::TextColored(ImVec4(0.5f, 1, 0.6f, 1), "%s", lastResult.c_str());
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.5f, 0.4f, 1));
            ImGui::TextUnformatted(lastResult.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
}

} // namespace st::devui
