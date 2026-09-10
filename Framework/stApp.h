#pragma once
// st::App - the Simtary application framework.
//
// Everything a project shares lives here: the ImGui backend + system UI (backlog,
// graphics settings, hierarchy/properties, scene manager), the render path, the
// scene manager, input, settings, lens flare, Faust audio and the ZMQ bridge.
//
// A project subclasses App, overrides the hooks marked "project hook" below, and
// starts it from its own src/main.cpp:
//
//     class MyGame : public st::App {
//         void RegisterScenes(SceneManager& scenes) override {
//             scenes.Register("Scene1", std::make_unique<Scene1>());
//         }
//     };
//
//     int main (int argc, char* argv[]) {
//         st::AppConfig config;          // project properties live here
//         config.name         = "MyGame";
//         config.startupScene = "Scene1";
//         MyGame app;
//         return st::Run(argc, argv, config, app);
//     }
#include "Simtary.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "SceneManager.h"
#include "scene/DayNight.h"
#include "scene/PlayControl.h"
#include "ZmqHandler.h"
#include "devui/imbacklog.h"
#include "devui/imgraphicsettings.h"
#include "devui/imhierarchy.h"
#include "devui/imeditor.h"
#include "devui/imassets.h"
#include "SubWinStatus.h"
#include "render/LensFlare.h"
#include "render/Projector.h"
#include "render/Laser.h"
#include "render/Optics.h"
#include "render/GraphicsAPI.h"
#include "display/DisplaySettings.h"
#include "io/SettingsManager.h"
#include "io/asset/AssetSystem.h"
#include "audio/faust/FaustManager.h"
#include "input/InputActions.h"

#include <SDL_scancode.h>
#include <SDL_events.h>
#include <string>

using namespace wi::graphics;

namespace st {

// How much of the framework's developer tooling a build exposes. DevUI is the menu
// bar, backlog, graphics settings, hierarchy/properties, scene manager and Faust
// panel - tooling, not game UI. A game's own UI (App::RenderUI, Scene::OnGUI) is
// never affected by this.
enum class DevUIMode {
    Visible,   // starts open - the development default
    Hidden,    // compiled in but starts closed; AppConfig::devUIToggleKey opens it
    Disabled,  // never drawn; RenderDevUI() and Scene::OnDevGUI() are not called
};

// What the framework is loading right now, for the loading screen. Scenes push into
// this with Scene::ReportProgress(); read it back with App::Loading().
struct LoadingState {
    bool        active  = false;
    int         percent = -1;   // 0..100, or -1 for indeterminate
    std::string status;         // "loading terrain"
    std::string scene;          // scene being loaded; empty during startup
};

// Project properties. Filled in the project's src/main.cpp and handed to st::Run,
// which owns the instance for the lifetime of the process.
struct AppConfig {

    // Window title, About box, crash reports and the per-user data folder
    // (%LOCALAPPDATA%Low/<organization>/<name>/).
    std::string name         = "Simtary";
    std::string organization = "DPSoftware";
    // Shown in the About window. Empty = no copyright line.
    std::string copyright;

    int windowWidth  = 1280;
    int windowHeight = 720;

    // Shown before the graphics device exists. Relative to the working directory;
    // leave empty to skip the splash entirely.
    std::string splashImage = "assets/splash.bmp";
    // Icon shown in the About window. Empty = none.
    std::string iconImage   = "assets/icon.png";

    // Scene loaded once RegisterScenes() has run. Empty = start with no scene.
    std::string startupScene;

    // Where maps live. Scanned after RegisterScenes() when sceneAutoDiscover is on, so a
    // .stsd / .wiscene dropped in there is selectable in the Scene Manager with no C++.
    // A C++ scene wins any name clash, and a map a C++ scene declares through
    // Scene::SceneFiles() is not registered a second time.
    std::string sceneFolder      = "assets/scenes";
    bool        sceneAutoDiscover = true;

    // Daylight / time-of-day (st::DayNight). What a scene load binds the system to:
    //   Off    - nothing; the game, or a "sticDayNight" component, binds it itself
    //   Adopt  - take over the scene's first directional light, if it has one
    //   Create - the same, and build a sun + weather when the scene has neither
    // Off is the default because a map whose sun was aimed by hand in the editor must
    // not be silently re-aimed on load.
    st::DayNightMode dayNight = st::DayNightMode::Off;

    // Which low-level backends may claim a gamepad.
    //
    // On Windows a plain HID pad is seen by RawInput AND by SDL, and RawInput registers
    // first, so it takes the LOWER player slot - player 0 then reads a generic HID parse
    // with no mapping database instead of SDL's mapped controller, and the same pad shows
    // up twice in Simtary > Gamepad Analog. Auto keeps RawInput as a fallback for a pad
    // the other backends do not see at all, which is what it is actually good for.
    // The DevUI panel can change it live and the choice is saved.
    wi::input::GamepadBackend gamepadBackend = wi::input::GamepadBackend::Auto;

    // Which way is up on the right stick. Off - the engine default - is DOWN-positive,
    // matching mouse delta Y so look code adds stick and mouse into one pitch with one
    // sign. On makes it up-positive like the left stick, for a game that treats it as a
    // direction; look code then needs its pitch sign flipped. Saved with the settings.
    bool rightStickUpPositive = false;

    // Background ZMQ subscriber; messages are re-published on the main thread as
    // the "zmq.message" event. Empty = do not start the bridge.
    std::string zmqEndpoint = "tcp://127.0.0.1:5556";

    // asset packages
    // .strd indexes to mount before anything loads, in order - a later pack shadows
    // an earlier one, which is how a patch pack works. Paths are relative to the
    // working directory (the exe folder), so "assets/content.strd" is the one
    // simtary_add_app(PACK_ASSETS) produces.
    //
    // Leave empty and the game reads loose files exactly as it always has: mounting is
    // additive, and any path the packs do not hold falls through to the filesystem.
    std::vector<std::string> assetPacks;

    // Prefix stripped from a requested path before it is looked up in a pack. The
    // build copies assets/contents/ into <exe>/assets/, so the running game asks for
    // "assets/scenes/s1map.stsd" while the pack stores "scenes/s1map.stsd".
    std::string assetMountPoint = "assets/";

    // Treat a listed pack that will not open as fatal. Off during development, where a
    // missing pack should just mean "run from loose files"; ON for a shipped build,
    // where it means the install is broken and should say so instead of silently
    // starting with no content.
    bool assetPacksRequired = false;

    // Hash every part on mount. A full sequential read of the whole package, so it is
    // for an installer or a "verify game files" menu item, not for every launch.
    bool assetPacksVerify = false;

    // Which graphics backend to start on. Auto is the platform default - DirectX 12
    // on Windows, Vulkan everywhere else. A player's saved choice (the Display panel)
    // and the "vulkan" / "dx12" command line arguments both outrank this, so setting
    // it here only moves the DEFAULT a fresh install starts on. The backend is fixed
    // for the lifetime of the process; changing it needs a restart.
    st::GraphicsAPI graphicsAPI = st::GraphicsAPI::Auto;

    // Where the editor writes a saved map's RAW RESOURCES - its textures, meshes and
    // sounds as loose files, under the names the map references them by. That folder is
    // the one `stpack pack --resource-dir` merges into the game package, so it is how a
    // model imported in the editor reaches the next build instead of living only in the
    // sidecar package beside the map.
    //
    // Empty means "use the project's own assets/resources", which the build bakes in as
    // ST_PROJECT_RESOURCE_DIR - so a normal project needs nothing here. Set it to point
    // somewhere else; the export is editor-only either way, and a build with no editor
    // never writes anything.
    std::string editorResourceDir;

    // Developer tooling. Ship a game with Hidden (or Disabled); Visible is the
    // development default.
    DevUIMode devUI = DevUIMode::Visible;
    // Toggles DevUI when it is Visible or Hidden. 0 = no toggle key.
    int devUIToggleKey = SDL_SCANCODE_F1;
};

struct ImGui_Impl_Data {};

class App : public wi::Application {
public:
    App();

    st::ProjectorRenderPath renderPath;
    SceneManager     sceneManager;
    ZmqHandler       zmqHandler;
    st::LensFlare    lensFlare;

    // Square/rectangular image projection with projector optics. Not a LightComponent:
    // the engine's spot light is a cone by construction, so a mask texture on one
    // always lands as a circle. See Framework/render/Projector.h.
    //
    // Also reachable from anywhere as st::ProjectorSystem::Get(), which is how a
    // scene or a native component gets at it:
    //
    //   ProjectorSystem::ID id = st::ProjectorSystem::Get().Add(projector);
    //   st::ProjectorSystem::Get().Find(id)->intensity = 12.0f;
    st::ProjectorSystem& Projectors() { return projectors_; }

    // Traced laser beams: millimetre-thin shafts that bounce off mirrors, bend
    // through lenses, stop on the first surface they meet, and leave a fading trail
    // where they land so a moving beam draws instead of blinking. The projector pass
    // ray marches and steps straight over something this thin; this one is closed
    // form. See Framework/render/Laser.h.
    //
    // Also reachable from anywhere as st::LaserSystem::Get():
    //
    //   LaserSystem::ID id = st::LaserSystem::Get().Add(laser);
    //   st::LaserSystem::Get().Path(id)->hit.entity;   // what the beam is on
    st::LaserSystem& Lasers() { return lasers_; }

    // The mirrors and lenses those beams travel through. Separate from the lasers
    // because one mirror serves every beam in the scene. See Framework/render/Optics.h.
    st::OpticsSystem& Optics() { return optics_; }

    // Create the graphics device for `api` before the engine would, so the backend
    // is the framework's choice rather than wiApplication's built-in default. st::Run
    // calls this before SetWindow(); doing it afterwards is too late, because
    // SetWindow creates its own device when none exists yet.
    void CreateGraphicsDevice(st::GraphicsAPI api, wi::platform::window_type window);

    void Initialize() override;
    void Compose(wi::graphics::CommandList cmd) override;
    void Update(float dt) override;
    void FixedUpdate() override;
    void Render() override;
    void Exit() override;

    // The config st::Run was given. Valid from Run() onwards.
    static const AppConfig& Config();

    // developer tooling
    // Whether the framework's dev panels are on screen. Always false when the
    // config says DevUIMode::Disabled, and SetDevUIVisible(true) cannot override
    // that - a shipped build stays shut.
    bool IsDevUIVisible() const { return devUIVisible_; }
    void SetDevUIVisible(bool visible);
    void ToggleDevUI() { SetDevUIVisible(!devUIVisible_); }

    // Editor mode: the DevUI becomes a docked scene editor (two viewports, hierarchy,
    // properties, ImGuizmo, scene save). Off by default and only reachable while DevUI
    // is visible, so it is developer tooling like the rest of devui/. F2 toggles it.
    // See Framework/devui/imeditor.h.
    bool IsEditorMode() const { return editor_.IsEnabled(); }
    void SetEditorMode(bool on) { editor_.SetEnabled(on); }
    void ToggleEditorMode() { editor_.Toggle(); }

    // display
    // Window mode, monitor, resolution, v-sync, frame cap and render scale. Render
    // its panel from the game's own options menu:
    //     ImGui::Begin("Options"); Display().GUI(*this); ImGui::End();
    // Applied settings persist to options.stad and come back on the next launch.
    DisplaySettings& Display() { return displaySettings_; }

    // graphics
    // The engine graphics options (AO, shadows, post processing, tonemapping,
    // upscaling). Editor mode reads these to render its extra viewports through the
    // same renderer the game uses - see GraphicsSettings::MirrorTo.
    GraphicsSettings& Graphics() { return graphicsSettings; }

    // assets
    // Mounted asset packages. Mount more at runtime (DLC, a patch pack), look an asset
    // up, or load a .stsd map:
    //     Assets().LoadScene(mapScene, "assets/scenes/s1map.stsd");
    // AppConfig::assetPacks is mounted for you before the engine starts.
    st::AssetSystem& Assets() { return st::AssetSystem::Get(); }

    // resources
    // The Resource Explorer's state. It has no window of its own: Editor mode owns the
    // window and calls Resources().GUI() into it. Public because EditorUI is a separate
    // class that needs to reach it, and because a project may want to drive an import
    // from its own tooling.
    AssetExplorer& Resources() { return assetExplorer_; }

    // loading
    // What is loading right now. Driven by SceneManager transitions and by scenes
    // calling Scene::ReportProgress().
    const LoadingState& Loading() const { return loading_; }
    // Push a status line (and optionally a percentage) into the loading screen.
    // Routed to the native startup window while it is up, and to
    // RenderLoadingScreen() afterwards.
    void SetLoadingStatus(std::string status, int percent = -1);

    // Raw SDL event, forwarded by st::Run before the engine sees it. Returns true
    // if the app consumed it. Public because st::Run calls it from the main loop.
    virtual bool OnEvent(const SDL_Event& /*event*/) { return false; }

    // A file or folder dropped onto the window. st::Run calls this only after
    // OnEvent() has declined the drop, so a project that wants drops of its own keeps
    // first refusal. The default hands it to the DevUI Asset Explorer's import tray,
    // and ignores it when DevUI is not visible. Public for the same reason OnEvent is.
    virtual void HandleDroppedFile(const std::string& path);

protected:
    // project hooks
    // Register the project's scenes. Called once during Initialize(), before
    // AppConfig::startupScene is loaded.
    virtual void RegisterScenes(SceneManager& /*scenes*/) {}

    // Register the project's KEYBINDS. Called once during Initialize(), before any
    // scene is registered or loaded, so a scene's st::InputComponent finds its map
    // already built.
    //
    // The registry arrives holding the framework's default "Player" and "UI" maps, so
    // a project that registers nothing still has something usable. EXTEND them by
    // naming the same map, or start from nothing with input.Clear() as the first line
    // - Map()/Action() return the EXISTING entry when the name is already taken, so
    // re-registering over a default appends bindings instead of replacing them.
    //
    //     void OnKeyRegister(st::input::Registry& input) override {
    //         using namespace st::input;
    //         input.Clear();
    //
    //         MapBuilder player = input.Map("Player");
    //         player.Action("Move", ControlType::Vector2)
    //             .Scheme("Gamepad").StickBinding(Stick::Left)
    //             .Scheme("Keyboard&Mouse").Composite2D('W', 'S', 'A', 'D');
    //         player.Action("Look", ControlType::Vector2)
    //             .Type(ActionType::PassThrough)
    //             .Scheme("Gamepad").StickBinding(Stick::Right)
    //             .Scheme("Keyboard&Mouse").MouseDelta();
    //         player.Action("Fire")
    //             .Button(wi::input::MOUSE_BUTTON_LEFT)
    //             .Button(wi::input::GAMEPAD_ANALOG_TRIGGER_R_AS_BUTTON)
    //             .TouchPress();
    //
    //         input.Map("UI").Action("Cancel")
    //             .Button(wi::input::KEYBOARD_BUTTON_ESCAPE)
    //             .Button(wi::input::GAMEPAD_BUTTON_3);
    //     }
    //
    // Attach "stInput" to the entity that should receive the map, then read it from a
    // sibling component - see Framework/input/InputComponent.h.
    virtual void OnKeyRegister(st::input::Registry& /*input*/) {}
    // After the framework is up and the startup scene is loaded.
    virtual void OnInitialize() {}
    // Once per frame, after the scene manager has updated.
    virtual void OnUpdate(float /*dt*/) {}
    // Engine fixed tick (wi::Application::FixedUpdate) - physics-rate game logic.
    virtual void OnFixedUpdate() {}

    // UI
    // The game's own ImGui UI. Drawn every frame regardless of DevUIMode.
    virtual void RenderUI() {}
    // Extra developer panels. Only called while DevUI is visible.
    virtual void RenderDevUI() {}
    // Called inside the DevUI main menu bar; add your own ImGui::BeginMenu here.
    virtual void OnDevUIMenu() {}
    // Drawn on top of everything while LoadingState::active. The default is the
    // framework's status/progress overlay; override for custom loading art.
    virtual void RenderLoadingScreen(const LoadingState& state);

    // render
    // After the render path is created and loaded, before it is activated. Set
    // render-path options here, or activate a path of your own instead.
    virtual void OnRenderPathSetup(wi::RenderPath3D& /*path*/) {}
    // The game's own GPU work, after the engine has rendered the frame.
    virtual void OnRender() {}
    // Under the composed 3D frame - a backdrop drawn before everything else.
    virtual void OnPreCompose(wi::graphics::CommandList /*cmd*/) {}
    // Additively over the composed 3D frame, before the UI is drawn.
    virtual void OnCompose(wi::graphics::CommandList /*cmd*/) {}

    // scenes
    // Fired by SceneManager around a deferred transition (Reload() fires both).
    virtual void OnSceneLoaded(const std::string& /*name*/) {}
    virtual void OnSceneUnloaded(const std::string& /*name*/) {}

    // Before the framework tears itself down.
    virtual void OnExit() {}

    // The Faust/OpenAL audio host. Register the project's AOT processors on it
    // from OnInitialize().
    st::audio::FaustManager& Audio() { return faustManager; }

    // Ask the app to quit at the top of the next frame.
    void RequestQuit() { isStop = true; }

private:
    Shader        imguiVS, imguiPS;
    Sampler       sampler;
    InputLayout   imguiInputLayout;
    PipelineState imguiPSO;
    BacklogViewer backlogViewer;
    GraphicsSettings graphicsSettings;
    DisplaySettings  displaySettings_;
    SubWinStatus m_loadingScreen;

    // Loads/unloads AOT Faust processors, played through the engine OpenAL stream.
    st::audio::FaustManager faustManager;

    // Reached through Projectors(); the framework drives Init/Bind/Update itself.
    st::ProjectorSystem projectors_;

    // Reached through Optics() and Lasers(). Declared in this order deliberately: the
    // laser system traces through the optics, so the optics must outlive it, and
    // members are destroyed in reverse declaration order.
    st::OpticsSystem optics_;
    st::LaserSystem lasers_;

    Texture fontTexture;

    // Lazily-loaded icon shown in the About window; held so its texture stays alive.
    wi::Resource aboutEngineIcon;

    void ImGui_CreateDeviceObjects();
    ImGui_Impl_Data* ImGui_GetBackendData();

    void ImguiInit(SDL_Window *window);
    void ImguiCompose(wi::graphics::CommandList cmd);
    void ImguiUpdate();
    void ImguiExit();

    void DevUIRender();
    void DevUIMenuBar();
    void DevUISceneSelector();
    void DevUISceneManager();   // dockable window: list/select/load scenes + reload from scratch
    void DevUIDayNight();       // dockable window: the daylight system's clock, place and look
    void DevUIAbout(bool *show);
    void DevUIHierarchy();      // Hierarchy (Explorer) + Properties (Inspector) windows

    // Per-asset load progress out of st::AssetSystem. Static because the callback is a
    // plain function pointer, and called from loading worker threads - see the
    // definition for what that means it may touch.
    static void OnAssetLoadProgress(const st::AssetLoadProgress& progress, void* userdata);

    // Editor mode. Owns the second render path + free camera, so it has to outlive any
    // single frame; st::App::Exit() tears it down.
    EditorUI editor_;

    // Asset Explorer (DevUI). Holds the import tray across frames, so a dropped file
    // survives until the package is written.
    AssetExplorer assetExplorer_;



    bool showImguiDemo = false;
    bool showBackLog = false;
    bool showGraphicsSettings = false;
    bool showAbout = false;
    bool showSceneManager = false;
    bool showDayNight = false;
    bool showHierarchy = false;
    bool showProperties = false;
    bool showFaustDSP = false;
    bool showAudioMixer = false;
    bool showGamepadAnalog = false;
    bool showInputActions = false;

    bool STDDBoneLines = false;
    bool STDDCameras = false;
    bool STDDColliders = false;
    bool STDDEmitters = false;
    bool STDDEnvProbes = false;
    bool STDDForceFields = false;
    bool STDDPartitionTree = false;
    bool STDDSprings = false;

    bool STDVoxelHelper = false;
    bool STDGridHelper = false;
    int STDGridHelper_clipmap_level = 1;

    struct DevRenderFlagsState {
        bool BoneLines, Cameras, Colliders, Emitters, EnvProbes, ForceFields, PartitionTree, Springs;
    } lastState{};

    bool flagsChangedThisFrame = false;


    // Entity selected in the Hierarchy window; the Properties window inspects it.
    wi::ecs::Entity selectedEntity_ = wi::ecs::INVALID_ENTITY;

    // Highlighted (not yet loaded) scene in the Scene Manager window.
    std::string selectedScene_;

    // "New scene" name box in the Scene Manager window.
    char newSceneName_[64] = "Untitled";
    bool newSceneLighting_  = true;

    bool isStop = false;

    // Last dt seen by Update(). Render() needs it to step the editor's own render path,
    // which the engine does not drive because it is not the active path.
    float lastDt_ = 0.0f;

    // DevUI + loading state (see the public accessors above).
    bool         devUIVisible_ = true;
    LoadingState loading_;

    // Startup shader/pipeline compile overlay. ImGui's own shaders load first
    // (ImguiInit), so this overlay can render while the engine compiles the rest.
    bool loadingDone_       = false;  // latched true once compilation settles
    bool loadingSawWork_    = false;  // saw IsPipelineCreationActive() > 0 at least once
    int  loadingIdleFrames_ = 0;      // consecutive frames with no active compile jobs
    int  loadingFrames_     = 0;      // frames the overlay has been up (hard-stop guard)
};

} // namespace st
