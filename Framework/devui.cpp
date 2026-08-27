#include "stApp.h"
#include "version.h"   // generated: ST_APP_VERSION / ST_APP_BUILD_NUMBER / ST_APP_BUILD_DATE
#include "wiVersion.h" // engine (Simtary) version + credits

void st::App::DevUIMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(Config().name.c_str())) {
            ImGui::MenuItem("Graphics Settings", NULL, &showGraphicsSettings);
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                isStop = true;
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Simtary")) {
            editor_.MenuItems();
            ImGui::Separator();
            if (ImGui::BeginMenu("Window")) {
                ImGui::MenuItem("Scene Manager", NULL, &showSceneManager);
                ImGui::MenuItem("Hierarchy", NULL, &showHierarchy);
                ImGui::MenuItem("Properties", NULL, &showProperties);
        ImGui::MenuItem("Asset Explorer", NULL, &showAssetExplorer);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Render")) {
                if (ImGui::BeginMenu("Debug")) {
                    ImGui::MenuItem("BoneLines", NULL, &STDDBoneLines);
                    ImGui::MenuItem("Cameras", NULL, &STDDCameras);
                    ImGui::MenuItem("Colliders", NULL, &STDDColliders);
                    ImGui::MenuItem("Emitters", NULL, &STDDEmitters);
                    ImGui::MenuItem("EnvProbes", NULL, &STDDEnvProbes);
                    ImGui::MenuItem("ForceFields", NULL, &STDDForceFields);
                    ImGui::MenuItem("PartitionTree", NULL, &STDDPartitionTree);
                    ImGui::MenuItem("Springs", NULL, &STDDSprings);
                    ImGui::EndMenu();
                }
                ImGui::MenuItem("VoxelHelper", NULL, &STDVoxelHelper);
                ImGui::InputInt("VoxelHelper clipmap_level", &STDGridHelper_clipmap_level, 0, 100);
                ImGui::MenuItem("GridHelper", NULL, &STDGridHelper);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            DevUISceneSelector();
            ImGui::Separator();
            ImGui::MenuItem("Show BackLog", NULL, &showBackLog);
            ImGui::Separator();
            ImGui::MenuItem("Faust DSP", NULL, &showFaustDSP);
            ImGui::Separator();
            if (ImGui::MenuItem("Crash")) {
                volatile int* p = nullptr;
                *p = 42;
            }
            ImGui::Separator();
            ImGui::MenuItem("Show IMGUI Demo", NULL, &showImguiDemo);

            ImGui::EndMenu();
        }

        // Project hook: add your own ImGui::BeginMenu(...) here.
        OnDevUIMenu();

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About", "")) {
                showAbout = true;
            }
            ImGui::EndMenu();
        }

        wi::graphics::GraphicsDevice::MemoryUsage vram = wi::graphics::GetDevice()->GetMemoryUsage();

        
        char fpsStr[32];
        snprintf(fpsStr, sizeof(fpsStr), "fps: %.1f", ImGui::GetIO().Framerate);
        std::string sceneStr = "scene: " + sceneManager.CurrentName();
        std::string vramStr = "VRAM: " + std::to_string(vram.usage / 1024 / 1024) + "/" + std::to_string(vram.budget / 1024 / 1024) + "MB";

        float textWidth = ImGui::CalcTextSize(fpsStr).x 
                        + ImGui::CalcTextSize(vramStr.c_str()).x 
                        + ImGui::CalcTextSize(sceneStr.c_str()).x 
                        + ImGui::GetStyle().ItemSpacing.x; // Account for the gap between them

        // 2. Calculate the target X position (Right edge minus text width minus padding)
        float rightPosX = ImGui::GetWindowWidth() - textWidth - ImGui::GetStyle().ItemSpacing.x;

        // 3. Only push to the right if there is actually enough space available
        if (ImGui::GetCursorPosX() < rightPosX) {
            ImGui::SetCursorPosX(rightPosX);
        }

        // 4. Render the text items
        ImGui::Text("%s", fpsStr);
        ImGui::Text("%s", vramStr.c_str());
        ImGui::Text("%s", sceneStr.c_str());

        ImGui::EndMainMenuBar();
    }
}



void st::App::DevUISceneSelector() {
    if (ImGui::BeginMenu("Scene")) {
        const std::string& cur = sceneManager.CurrentName();
        for (const std::string& name : sceneManager.Names()) {
            const bool selected = (name == cur);
            // Switching is deferred to the next Update() by SceneManager::Load,
            // so it's safe to trigger from inside ImGui here.
            if (ImGui::MenuItem(name.c_str(), nullptr, selected) && !selected) {
                sceneManager.Load(name);
            }
        }
        ImGui::EndMenu();
    }
}

void st::App::DevUISceneManager() {
    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Scene Manager", &showSceneManager)) {
        ImGui::End();
        return;
    }

    const std::string& cur = sceneManager.CurrentName();
    ImGui::Text("Active scene: %s", cur.empty() ? "(none)" : cur.c_str());
    ImGui::Separator();

    // Scene list. Single-click highlights, double-click loads immediately. All
    // switching goes through SceneManager::Load, which defers the Unload()/Load()
    // to the next Update(), so triggering from inside ImGui here is safe.
    ImGui::TextDisabled("Registered scenes (double-click to load)");
    const float listH = 6.0f * ImGui::GetTextLineHeightWithSpacing();
    if (ImGui::BeginListBox("##scenelist", ImVec2(ImGui::GetContentRegionAvail().x, listH))) {
        for (const std::string& name : sceneManager.Names()) {
            const bool isActive = (name == cur);
            const bool isSel    = (name == selectedScene_);
            if (ImGui::Selectable(name.c_str(), isSel, ImGuiSelectableFlags_AllowDoubleClick)) {
                selectedScene_ = name;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    sceneManager.Load(name);
            }
            if (isActive) {
                ImGui::SameLine();
                ImGui::TextDisabled("(active)");
            }
        }
        ImGui::EndListBox();
    }

    // Load the highlighted scene. SceneManager::Load already unloads the current
    // scene before loading the new one.
    const bool hasSel = !selectedScene_.empty();
    ImGui::BeginDisabled(!hasSel);
    if (ImGui::Button("Load selected"))
        sceneManager.Load(selectedScene_);
    ImGui::EndDisabled();

    ImGui::SameLine();

    // "From scratch": full Unload() + Load() of the active scene — tears down and
    // rebuilds its entities even when reloading the same scene.
    ImGui::BeginDisabled(cur.empty());
    if (ImGui::Button("Reload current (from scratch)"))
        sceneManager.Reload();
    ImGui::EndDisabled();

    ImGui::TextDisabled("Reload re-runs Unload/Load; scene object (and its\nEventBus subscription) is reused by design.");

    ImGui::End();
}

void st::App::DevUIAbout(bool *show) {
    ImGui::Begin("About", show);

    if (ImGui::BeginTabBar("AboutTab")) {
        if (ImGui::BeginTabItem(Config().name.c_str())) {
            ImGui::Text("%s", Config().name.c_str());
            ImGui::Separator();
            ImGui::Text("Version %s (build %d)", ST_APP_VERSION, ST_APP_BUILD_NUMBER);
            ImGui::Text("Built %s", ST_APP_BUILD_DATE);
            if (!Config().copyright.empty()) {
                ImGui::Separator();
                ImGui::Text("%s", Config().copyright.c_str());
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Simtary")) {
            if (aboutEngineIcon.IsValid()) {
                const wi::graphics::Texture& tex = aboutEngineIcon.GetTexture();
                if (tex.IsValid()) {
                    const TextureDesc& d = tex.GetDesc();
                    ImGui::Image((ImTextureID)(uintptr_t)&tex, ImVec2((float)d.width, (float)d.height));
                }
            }

            ImGui::Text("Simtary (HMMWV4)");
            ImGui::Separator();
            ImGui::Text("Version      %s", wi::version::GetVersionString());
            ImGui::Text("Descriptor   %s", wi::version::GetDescriptorString());
            ImGui::Text("Codename     %s", wi::version::GetCodename());
            ImGui::Text("Architecture %s", wi::version::GetArchitecture());
            ImGui::Text("Based on Wicked Engine %s", wi::version::GetOGVersionString());
            ImGui::Separator();

            if (ImGui::CollapsingHeader("Credits")) {
                ImGui::BeginChild("##credits", ImVec2(0, 220), true);
                ImGui::PushTextWrapPos(0.0f);  // wrap at child width (long supporter list)
                ImGui::TextUnformatted(wi::version::GetCreditsString());
                ImGui::PopTextWrapPos();
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

void st::App::DevUIRender() {
    // F2 toggles editor mode. Read before the menu bar so the flag is settled for the
    // whole frame; gated on DevUI being visible, so a shipped build never sees it.
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F2, false))
        editor_.Toggle();

    DevUIMenuBar();

    // Editor mode owns the screen: the dock host plus its own Hierarchy/Properties. The
    // floating DevUI panels below still work and dock into it like any other window.
    editor_.Draw(*this, renderPath, selectedEntity_);

    if (showBackLog) backlogViewer.render(&showBackLog);
    if (showImguiDemo) ImGui::ShowDemoWindow(&showImguiDemo);
    if (showGraphicsSettings) graphicsSettings.render(&showGraphicsSettings, renderPath, *this, lensFlare, displaySettings_, projectors_, lasers_, optics_);
    if (showAbout) DevUIAbout(&showAbout);
    if (showSceneManager) DevUISceneManager();
    if (showAssetExplorer) DevUIAssetExplorer();
    if (showHierarchy || showProperties) DevUIHierarchy();
    if (showFaustDSP) faustManager.DrawPanel("Faust DSP", &showFaustDSP);

    // Project hook: the game's own developer panels.
    RenderDevUI();

}

void st::App::DevUIHierarchy() {
    // Single live scene in Wicked is the global GetScene(); both windows act on it.
    // Selection (selectedEntity_) is owned by st::App so the two panels share it.
    wi::scene::Scene& scene = wi::scene::GetScene();

    if (showHierarchy) HierarchyWindow(scene, selectedEntity_, &showHierarchy);
    if (showProperties) PropertiesWindow(scene, selectedEntity_, &showProperties);
}

// ─── Asset Explorer ────────────────────────────────────────────────────────────
// Browse the mounted .staod packages, inspect where an asset actually lives, drop
// files in to build a new package. Developer tooling: it is only reachable while
// DevUI is visible, and a shipped build with DevUIMode::Disabled never draws it.
void st::App::DevUIAssetExplorer() {
    assetExplorer_.Draw(&showAssetExplorer);
}

void st::App::HandleDroppedFile(const std::string& path) {
    // Dropping onto a shipped build must do nothing: the explorer is DevUI, and the
    // import tray writes packages. A project that wants player-facing drag-and-drop
    // overrides OnEvent and consumes SDL_DROPFILE before this is reached.
    if (!IsDevUIVisible()) return;
    showAssetExplorer = true;
    assetExplorer_.QueueDrop(path);
}
