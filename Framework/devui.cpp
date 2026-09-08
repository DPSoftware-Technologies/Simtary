#include "stApp.h"
#include "version.h"   // generated: ST_APP_VERSION / ST_APP_BUILD_NUMBER / ST_APP_BUILD_DATE
#include "wiVersion.h" // engine (Simtary) version + credits
#include "devui/imaudio.h"
#include "devui/iminput.h"

void st::App::DevUIMenuBar() {
    // Transport hotkeys. Checked here rather than through the action maps: these are
    // DevUI controls, they exist only while the DevUI is drawn, and a game's own keymap
    // should not have to carry bindings for a tool the player never sees. Skipped while
    // a text field has the keyboard, or typing "F5" into a rename box would reset the
    // scene out from under it.
    if (!ImGui::GetIO().WantCaptureKeyboard) {
        st::PlayControl& play = st::PlayControl::Get();
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) play.TogglePause();
        if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) play.Pause();
        if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) play.Step();
        if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) play.Reset();
    }

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
                ImGui::MenuItem("Day / Night & Weather", NULL, &showDayNight);
                ImGui::MenuItem("Hierarchy", NULL, &showHierarchy);
                ImGui::MenuItem("Properties", NULL, &showProperties);
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
            if (ImGui::MenuItem("Play", "F5 toggles", st::PlayControl::Get().State() == st::PlayState::Playing))
                st::PlayControl::Get().Play();
            if (ImGui::MenuItem("Play without physics", NULL,
                                st::PlayControl::Get().State() == st::PlayState::PlayingNoPhysics))
                st::PlayControl::Get().PlayWithoutPhysics();
            if (ImGui::MenuItem("Pause", "F6", st::PlayControl::Get().IsPaused()))
                st::PlayControl::Get().Pause();
            if (ImGui::MenuItem("Step one frame", "F7", false, st::PlayControl::Get().IsPaused()))
                st::PlayControl::Get().Step();
            if (ImGui::MenuItem("Reset scene", "F8"))
                st::PlayControl::Get().Reset();
            ImGui::Separator();
            DevUISceneSelector();
            ImGui::Separator();
            ImGui::MenuItem("Show BackLog", NULL, &showBackLog);
            ImGui::Separator();
            ImGui::MenuItem("Faust DSP", NULL, &showFaustDSP);
            ImGui::MenuItem("Audio Mixer", NULL, &showAudioMixer);
            ImGui::MenuItem("Gamepad Analog", NULL, &showGamepadAnalog);
            ImGui::MenuItem("Input Actions", NULL, &showInputActions);
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

        // The transport is NOT here. It lives on the editor's own toolbar, beside the
        // gizmo buttons - this bar is the application's menus, and five buttons and a
        // slider across it pushed Help and the fps readout off the end. The entries
        // under Simtary and F5..F8 still reach the same transport without the editor.

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
        // A frozen world looks exactly like a still one, so the state is said out loud
        // next to the frame rate whenever it is not the ordinary case.
        if (st::PlayControl::Get().State() != st::PlayState::Playing)
            sceneStr += "  [" + std::string(st::ToString(st::PlayControl::Get().State())) + "]";
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

    // New scene. st::RuntimeScene is empty apart from a sun and a sky, and SceneManager
    // unloads whatever is active before it loads, so this is a clean world to build in.
    // Save it with Scene > Save As; a .stsd in the scene folder comes back on the next
    // run as a scene of its own.
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 110.0f);
    ImGui::InputText("##newscenename", newSceneName_, sizeof(newSceneName_));
    ImGui::SameLine();
    if (ImGui::Button("New scene", ImVec2(100, 0)))
        selectedScene_ = sceneManager.NewScene(newSceneName_, newSceneLighting_);
    ImGui::Checkbox("with sun + sky", &newSceneLighting_);
    ImGui::SameLine();
    ImGui::TextDisabled("(name is made unique if taken)");

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
            // Where the scene came from: a C++ class, the scene folder, or this session.
            const char* origin = "c++";
            switch (sceneManager.OriginOf(name)) {
                case SceneManager::Origin::Folder:  origin = "folder"; break;
                case SceneManager::Origin::Runtime: origin = "new";    break;
                default: break;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("[%s]", origin);
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

    // "From scratch": full Unload() + Load() of the active scene - tears down and
    // rebuilds its entities even when reloading the same scene.
    ImGui::BeginDisabled(cur.empty());
    if (ImGui::Button("Reload current (from scratch)"))
        sceneManager.Reload();
    ImGui::EndDisabled();

    // Pick up a map dropped into the scene folder while the game was running.
    // Additive - nothing already registered is disturbed.
    if (ImGui::Button("Rescan scene folder")) {
        const int discovered = sceneManager.DiscoverScenes(Config().sceneFolder);
        wi::backlog::post("Scene Manager: rescan added " + std::to_string(discovered) +
                          " scene(s) from " + Config().sceneFolder);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Config().sceneFolder.c_str());

    // Only an unsaved runtime scene can be dropped, and only while it is not the
    // active one - a folder or C++ scene would just come back on the next scan or run.
    const bool canForget = hasSel && selectedScene_ != cur &&
                           sceneManager.OriginOf(selectedScene_) == SceneManager::Origin::Runtime;
    ImGui::BeginDisabled(!canForget);
    if (ImGui::Button("Forget selected new scene")) {
        if (sceneManager.Unregister(selectedScene_))
            selectedScene_.clear();
    }
    ImGui::EndDisabled();

    ImGui::TextDisabled("Reload re-runs Unload/Load; scene object (and its\nEventBus subscription) is reused by design.");
    ImGui::TextDisabled("[folder] scenes are maps found in the scene folder. A C++\nscene of the same name, or one that declares it loads that\nmap, overrides them.");

    ImGui::End();
}

void st::App::DevUIDayNight() {
    // The window is the system's own - st::App only owns the flag that opens it, the same
    // split the other subsystem panels use.
    st::DayNight::Get().DevGUI(&showDayNight);
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
    if (showDayNight) DevUIDayNight();
    if (showHierarchy || showProperties) DevUIHierarchy();
    if (showFaustDSP) faustManager.DrawPanel("Faust DSP", &showFaustDSP);
    if (showAudioMixer) st::devui::AudioMixerWindow(&showAudioMixer);
    if (showGamepadAnalog) st::devui::GamepadAnalogWindow(&showGamepadAnalog);
    if (showInputActions) st::devui::InputActionsWindow(&showInputActions);

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

// Resource Explorer
// The panel itself lives in Editor mode (F2), docked along the bottom - see
// EditorUI::DrawDockHost. It has no floating DevUI window of its own, because
// managing content is an editing job and everything it can do writes files.
void st::App::HandleDroppedFile(const std::string& path) {
    // Only Editor mode consumes drops. A shipped build (DevUIMode::Disabled) and a
    // plain DevUI session both ignore them, so dragging a file onto a running game
    // cannot quietly start rewriting asset packages. A project that wants its own
    // drag-and-drop overrides OnEvent and consumes SDL_DROPFILE before this is reached.
    if (!IsDevUIVisible() || !IsEditorMode()) {
        wi::backlog::post("Dropped file ignored: open Editor mode (F2) to import assets.",
                          wi::backlog::LogLevel::Warning);
        return;
    }

    // A dropped MODEL goes into the world, not into the package: it lands in front of the
    // editor camera as a new entity tree. That is what dragging a .stsd or .wiscene onto an
    // editor means, and the resource side loses nothing - the Resource Explorer has its own
    // "Add files..." / "Add folder..." buttons for putting one in the asset package, and a
    // scene the game actually loads gets there through Save As, not through a drop.
    if (EditorUI::IsSceneImportPath(path)) {
        editor_.QueueImportModel(path);
        return;
    }

    editor_.ShowResources();
    assetExplorer_.QueueImport(path);
}
