#include "devui/imscripteditor.h"
#include "devui/imcodeedit.h"
#include "script/ScriptSystem.h"

#include "imgui.h"
#include "io/asset/StHash.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace st::devui {

namespace {

// One problem out of a Taipan diagnostics block:
//
//	error: syntax error
//	  --> spinner.tp:3:1
//	   |
//	 3 | func start(Entity self) {
//	   | ^^^^ expected an expression, found 'func'
struct Problem {
    bool        error = true;
    int         line = 0;     // 1-based; 0 = no location
    int         column = 0;
    std::string text;
};

struct Document {
    std::string path;          // logical, "assets/scripts/ai.tp"
    CodeEditor  editor;        // the text, cursor, highlighting, completion
    std::string text;          // editor's text as of `seenVersion` (what Dirty/Save read)
    uint64_t    seenVersion = 0;
    std::string savedText;     // what is on disk / in the package as far as we know
    std::string savedTo;       // file the last save wrote
    std::string diagnostics;   // raw text of the last check / save
    std::vector<Problem> problems;
    bool   checked = false;    // diagnostics reflect `checkedText`
    bool   checkOk = true;
    std::string checkedText;
    double lastEdit = 0.0;     // ImGui time of the last keystroke, for the idle re-check
    bool   selectTab = false;

    // Replace the buffer (open, revert, template). Tabs become four spaces on the way in.
    void Load (const std::string& t) {
        editor.SetText(t);
        text = editor.GetText();
        seenVersion = editor.Version();
        checked = false;
    }
    // Pick up edits made in the widget since the last look.
    void Sync () {
        if (editor.Version() == seenVersion) return;
        text = editor.GetText();
        seenVersion = editor.Version();
        lastEdit = ImGui::GetTime();
    }
    bool Dirty () const { return text != savedText; }
    std::string Title () const {
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }
};

bool visible = false;
std::vector<Document> docs;
int active = -1;                  // index into docs of the selected tab
int closeRequest = -1;            // a dirty tab asked to close: confirm first
bool openClosePopup = false;      // set with closeRequest; the popup is opened once
char newPath[256] = "assets/scripts/new_script.tp";
std::string status;
bool statusError = false;

constexpr double kCheckDelay = 0.6;  // seconds of no typing before the live check

const char* kTemplate =
    "// %s\n"
    "//\n"
    "// Attach with an stScript component (NCA_<id>_file = \"%s\").\n"
    "// Every callback is optional; `self` is the entity the component is on.\n"
    "\n"
    "func onStart(Entity self) {\n"
    "    log.info(f\"{self.name}: started\")\n"
    "}\n"
    "\n"
    "func onUpdate(Entity self, float dt) {\n"
    "}\n"
    "\n"
    "func onFixedUpdate(Entity self, float dt) {\n"
    "}\n"
    "\n"
    "func onDestroy(Entity self) {\n"
    "}\n";

// What completion offers beyond the text: the VM's whole native API. Rebuilt when the VM
//	is (bindings are registered once, at creation).
CodeEditor::Api api;
unsigned apiGeneration = 0;

void AddSymbol (st::taipan::SymbolKind kind, const char* owner, const char* name, const char* detail, void*) {
    using K = st::taipan::SymbolKind;
    CodeEditor::Symbol sym{ name, detail ? detail : "", kind == K::Function || kind == K::Method };
    switch (kind) {
    case K::Function:
    case K::Value:
        if (owner && *owner) api.namespaces[owner].push_back(sym);
        else                 api.globals.push_back(sym);
        break;
    case K::Class: {
        api.classes[name];
        const std::string aliases = detail ? detail : "";
        size_t start = 0;
        while (start < aliases.size()) {
            size_t end = aliases.find(',', start);
            if (end == std::string::npos) end = aliases.size();
            if (end > start) api.aliases[aliases.substr(start, end - start)] = name;
            start = end + 1;
        }
        break;
    }
    case K::Method:
    case K::Property:
        api.classes[owner].push_back(sym);
        break;
    }
}

const CodeEditor::Api* CompletionApi () {
    st::ScriptSystem& scripts = st::ScriptSystem::Get();
    st::taipan::VM* vm = scripts.VM();
    if (vm && apiGeneration != scripts.Generation()) {
        api = CodeEditor::Api();
        st::taipan::ListSymbols(vm, &AddSymbol, nullptr);
        auto byName = [](const CodeEditor::Symbol& a, const CodeEditor::Symbol& b) { return a.name < b.name; };
        std::sort(api.globals.begin(), api.globals.end(), byName);
        for (auto& kv : api.namespaces) std::sort(kv.second.begin(), kv.second.end(), byName);
        for (auto& kv : api.classes) std::sort(kv.second.begin(), kv.second.end(), byName);
        apiGeneration = scripts.Generation();
    }
    return &api;
}

void SetStatus (const std::string& text, bool isError) {
    status = text;
    statusError = isError;
}

int FindDoc (const std::string& path) {
    const std::string key = asset::CanonicalPath(path);
    for (size_t i = 0; i < docs.size(); ++i)
        if (asset::CanonicalPath(docs[i].path) == key) return (int)i;
    return -1;
}

void ParseProblems (Document& doc) {
    doc.problems.clear();
    Problem* current = nullptr;
    size_t start = 0;
    const std::string& all = doc.diagnostics;
    while (start < all.size()) {
        size_t end = all.find('\n', start);
        if (end == std::string::npos) end = all.size();
        std::string ln = all.substr(start, end - start);
        start = end + 1;

        const size_t first = ln.find_first_not_of(' ');
        if (first == std::string::npos) continue;
        const std::string trimmed = ln.substr(first);

        if (trimmed.rfind("error:", 0) == 0 || trimmed.rfind("warning:", 0) == 0) {
            Problem p;
            p.error = trimmed[0] == 'e';
            p.text = trimmed.substr(trimmed.find(':') + 1);
            if (!p.text.empty() && p.text[0] == ' ') p.text.erase(0, 1);
            doc.problems.push_back(p);
            current = &doc.problems.back();
        } else if (current && trimmed.rfind("-->", 0) == 0) {
            // "--> file.tp:LINE:COL" - read the last two numbers.
            int l = 0, c = 0;
            const size_t lastColon = trimmed.find_last_of(':');
            if (lastColon != std::string::npos && lastColon > 0) {
                const size_t prevColon = trimmed.find_last_of(':', lastColon - 1);
                if (prevColon != std::string::npos) {
                    l = std::atoi(trimmed.c_str() + prevColon + 1);
                    c = std::atoi(trimmed.c_str() + lastColon + 1);
                }
            }
            current->line = l;
            current->column = c;
        } else if (current && trimmed[0] == '|') {
            // "| ^^^^ label" - the label says what is wrong at the caret.
            const size_t caret = trimmed.find('^');
            if (caret != std::string::npos) {
                const size_t label = trimmed.find_first_not_of("^ ", caret);
                if (label != std::string::npos) current->text += " - " + trimmed.substr(label);
            }
        }
    }
    // Anything that did not follow the usual shape is still worth showing.
    if (doc.problems.empty() && !doc.diagnostics.empty() && !doc.checkOk) {
        Problem p;
        p.text = doc.diagnostics.substr(0, doc.diagnostics.find('\n'));
        doc.problems.push_back(p);
    }
    // the same list as squiggles in the text
    std::vector<CodeEditor::Marker> markers;
    for (const Problem& p : doc.problems)
        if (p.line > 0) markers.push_back(CodeEditor::Marker{ p.line, p.column, p.error, p.text });
    doc.editor.SetMarkers(std::move(markers));
}

void RunCheck (Document& doc) {
    doc.Sync();
    std::string diag;
    doc.checkOk = st::ScriptSystem::Get().CheckScript(doc.path, doc.text, &diag);
    doc.diagnostics = diag;
    doc.checked = true;
    doc.checkedText = doc.text;
    ParseProblems(doc);
}

bool Save (Document& doc) {
    doc.Sync();
    std::string written, diag, error;
    if (!st::ScriptSystem::Get().SaveScript(doc.path, doc.text, &written, &diag, &error)) {
        SetStatus(error, true);
        return false;
    }
    doc.savedText = doc.text;
    doc.savedTo = written;
    if (!diag.empty()) {
        // Saved, but the reload refused it: running instances keep the old code.
        doc.diagnostics = diag;
        doc.checkOk = false;
        doc.checked = true;
        doc.checkedText = doc.text;
        ParseProblems(doc);
        SetStatus("Saved " + written + " - but it does not compile; running entities keep the last good code.", true);
    } else {
        RunCheck(doc); // refresh warnings
        SetStatus("Saved " + written + " and reloaded.", false);
    }
    return true;
}

void NewScriptPopup () {
    if (!ImGui::BeginPopupModal("New script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Logical path (under assets/scripts/ resolves imports):");
    ImGui::SetNextItemWidth(420);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("##newpath", newPath, sizeof(newPath), ImGuiInputTextFlags_EnterReturnsTrue);
    std::string path = asset::NormalizePath(newPath);
    const bool valid = path.size() > 3 && asset::CanonicalPath(path).compare(path.size() - 3, 3, ".tp") == 0;
    if (!valid) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "Must end in .tp");
    ImGui::TextDisabled("Saved to %s", st::ScriptSystem::SourcePathFor(path).c_str());

    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Create") || (enter && valid)) {
        int existing = FindDoc(path);
        if (existing < 0) {
            Document doc;
            doc.path = path;
            std::string current;
            if (st::ScriptSystem::Get().ReadSource(path, current)) {
                // It already exists: open it rather than overwrite it with the template.
                doc.Load(current);
                doc.savedText = doc.text;
            } else {
                char buffer[2048];
                std::snprintf(buffer, sizeof(buffer), kTemplate, doc.Title().c_str(), path.c_str());
                doc.Load(buffer);   // savedText stays empty: new and unsaved
            }
            docs.push_back(std::move(doc));
            existing = (int)docs.size() - 1;
        }
        docs[existing].selectTab = true;
        docs[existing].editor.Focus();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void ConfirmClosePopup () {
    if (openClosePopup) {
        ImGui::OpenPopup("Unsaved script");
        openClosePopup = false;
    }
    if (!ImGui::BeginPopupModal("Unsaved script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (closeRequest < 0 || closeRequest >= (int)docs.size()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::Text("%s has unsaved changes.", docs[closeRequest].path.c_str());
    if (ImGui::Button("Save")) {
        if (Save(docs[closeRequest])) docs.erase(docs.begin() + closeRequest);
        closeRequest = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard")) {
        docs.erase(docs.begin() + closeRequest);
        closeRequest = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        closeRequest = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MenuBar (bool& openNew) {
    if (!ImGui::BeginMenuBar()) return;
    Document* doc = (active >= 0 && active < (int)docs.size()) ? &docs[active] : nullptr;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New script...")) openNew = true;
        if (ImGui::BeginMenu("Open")) {
            std::vector<std::string> scripts;
            st::ScriptSystem::Get().ListScripts(scripts);
            if (scripts.empty()) ImGui::TextDisabled("no .tp scripts found");
            for (const std::string& s : scripts)
                if (ImGui::MenuItem(s.c_str())) OpenScriptEditor(s);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, doc != nullptr)) Save(*doc);
        if (ImGui::MenuItem("Save all", nullptr, false, !docs.empty())) {
            for (Document& d : docs)
                if (d.Dirty()) Save(d);
        }
        if (ImGui::MenuItem("Revert", nullptr, false, doc && doc->Dirty())) doc->Load(doc->savedText);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Script")) {
        if (ImGui::MenuItem("Check now", "F7", false, doc != nullptr)) RunCheck(*doc);
        if (ImGui::MenuItem("Reload running instances", nullptr, false, doc != nullptr))
            st::ScriptSystem::Get().Reload(doc->path);
        if (ImGui::MenuItem("Copy path", nullptr, false, doc != nullptr))
            ImGui::SetClipboardText(doc->path.c_str());
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void DrawProblems (Document& doc, float height) {
    ImGui::BeginChild("##problems", ImVec2(0, height), ImGuiChildFlags_Borders);
    if (!doc.checked) {
        ImGui::TextDisabled("checking...");
    } else if (doc.problems.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 1, 0.6f, 1), "No problems.");
    } else {
        for (size_t i = 0; i < doc.problems.size(); ++i) {
            const Problem& p = doc.problems[i];
            ImGui::PushID((int)i);
            ImGui::PushStyleColor(ImGuiCol_Text, p.error ? ImVec4(1, 0.45f, 0.4f, 1) : ImVec4(1, 0.8f, 0.35f, 1));
            char label[512];
            if (p.line > 0) std::snprintf(label, sizeof(label), "%s  %d:%d  %s", p.error ? "error" : "warn ",
                                          p.line, p.column, p.text.c_str());
            else            std::snprintf(label, sizeof(label), "%s  %s", p.error ? "error" : "warn ", p.text.c_str());
            if (ImGui::Selectable(label) && p.line > 0) doc.editor.JumpTo(p.line, p.column);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && !doc.diagnostics.empty()) ImGui::SetTooltip("%s", doc.diagnostics.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void DrawDocument (Document& doc) {
    const float problemsHeight = ImGui::GetTextLineHeightWithSpacing() * 4.5f;
    const float statusHeight = ImGui::GetFrameHeightWithSpacing();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float textHeight = std::max(80.0f, avail.y - problemsHeight - statusHeight - ImGui::GetStyle().ItemSpacing.y);

    doc.editor.SetApi(CompletionApi());
    doc.editor.Render("##source", ImVec2(-1, textHeight));
    doc.Sync();

    // Live check once typing pauses.
    if (doc.text != doc.checkedText && ImGui::GetTime() - doc.lastEdit > kCheckDelay) RunCheck(doc);

    DrawProblems(doc, problemsHeight);

    ImGui::TextDisabled("%s", doc.path.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("  Ln %d, Col %d", doc.editor.CursorLine(), doc.editor.CursorColumn());
    ImGui::SameLine();
    if (doc.Dirty()) ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "  modified");
    else if (!doc.savedTo.empty()) ImGui::TextDisabled("  saved to %s", doc.savedTo.c_str());
    else if (st::ScriptSystem::Get().HasSourceOverride(doc.path)) ImGui::TextDisabled("  (edited this session)");
}

} // namespace

bool& ScriptEditorVisible () {
    return visible;
}

void OpenScriptEditor (const std::string& logicalPath) {
    const std::string path = asset::NormalizePath(logicalPath);
    visible = true;
    int index = FindDoc(path);
    if (index < 0) {
        Document doc;
        doc.path = path;
        std::string text;
        if (!st::ScriptSystem::Get().ReadSource(path, text)) {
            SetStatus("Could not read " + path + " - it opens empty; Save creates it.", true);
            text.clear();
        }
        doc.Load(text);
        doc.savedText = doc.text;
        docs.push_back(std::move(doc));
        index = (int)docs.size() - 1;
    }
    docs[index].selectTab = true;
    docs[index].editor.Focus();
}

void ScriptEditorWindow () {
    if (!visible) return;
    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Script Editor", &visible, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }

    bool openNew = false;
    MenuBar(openNew);

    // Shortcuts while any part of this window has focus.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && active >= 0 && active < (int)docs.size()) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) Save(docs[active]);
        if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) RunCheck(docs[active]);
    }

    if (docs.empty()) {
        ImGui::TextDisabled("No script open.");
        if (ImGui::Button("New script...")) openNew = true;
        ImGui::SameLine();
        ImGui::TextDisabled("or File > Open, or double-click a .tp in the Resource Explorer.");
    } else if (ImGui::BeginTabBar("##scripts", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs |
                                                ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (int i = 0; i < (int)docs.size(); ++i) {
            Document& doc = docs[i];
            bool open = true;
            ImGuiTabItemFlags flags = doc.Dirty() ? ImGuiTabItemFlags_UnsavedDocument : 0;
            if (doc.selectTab) {
                flags |= ImGuiTabItemFlags_SetSelected;
                doc.selectTab = false;
            }
            const std::string label = doc.Title() + "###" + doc.path;
            if (ImGui::BeginTabItem(label.c_str(), &open, flags)) {
                active = i;
                DrawDocument(doc);
                ImGui::EndTabItem();
            }
            if (!open) {
                if (doc.Dirty()) {
                    closeRequest = i;
                    openClosePopup = true;
                } else {
                    docs.erase(docs.begin() + i);
                    --i;
                }
            }
        }
        ImGui::EndTabBar();
    }
    if (active >= (int)docs.size()) active = (int)docs.size() - 1;

    if (!status.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, statusError ? ImVec4(1, 0.5f, 0.4f, 1)
                                                         : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::PopStyleColor();
    }

    if (openNew) ImGui::OpenPopup("New script");
    NewScriptPopup();
    ConfirmClosePopup();
    ImGui::End();
}

} // namespace st::devui
