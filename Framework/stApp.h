#pragma once
// st::App — the Simtary application framework.
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
#include "display/DisplaySettings.h"
#include "io/SettingsManager.h"
#include "io/asset/AssetSystem.h"
#include "audio/faust/FaustManager.h"

#include <SDL_scancode.h>
#include <SDL_events.h>
#include <string>

using namespace wi::graphics;

namespace st {

// How much of the framework's developer tooling a build exposes. DevUI is the menu
// bar, backlog, graphics settings, hierarchy/properties, scene manager and Faust
// panel — tooling, not game UI. A game's own UI (App::RenderUI, Scene::OnGUI) is
// never affected by this.
enum class DevUIMode {
    Visible,   // starts open — the development default
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

    // Background ZMQ subscriber; messages are re-published on the main thread as
    // the "zmq.message" event. Empty = do not start the bridge.
    std::string zmqEndpoint = "tcp://127.0.0.1:5556";

    // ── asset packages ─────────────────────────────────────────────────────────
    // .staod indexes to mount before anything loads, in order — a later pack shadows
    // an earlier one, which is how a patch pack works. Paths are relative to the
    // working directory (the exe folder), so "assets/content.staod" is the one
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

    void Initialize() override;
    void Compose(wi::graphics::CommandList cmd) override;
    void Update(float dt) override;
    void FixedUpdate() override;
    void Render() override;
    void Exit() override;

    // The config st::Run was given. Valid from Run() onwards.
    static const AppConfig& Config();

    // ── developer tooling ──────────────────────────────────────────────────────
    // Whether the framework's dev panels are on screen. Always false when the
    // config says DevUIMode::Disabled, and SetDevUIVisible(true) cannot override
    // that — a shipped build stays shut.
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

    // ── display ────────────────────────────────────────────────────────────────
    // Window mode, monitor, resolution, v-sync, frame cap and render scale. Render
    // its panel from the game's own options menu:
    //     ImGui::Begin("Options"); Display().GUI(*this); ImGui::End();
    // Applied settings persist to options.stad and come back on the next launch.
    DisplaySettings& Display() { return displaySettings_; }

    // ── graphics ───────────────────────────────────────────────────────────────
    // The engine graphics options (AO, shadows, post processing, tonemapping,
    // upscaling). Editor mode reads these to render its extra viewports through the
    // same renderer the game uses — see GraphicsSettings::MirrorTo.
    GraphicsSettings& Graphics() { return graphicsSettings; }

    // ── assets ─────────────────────────────────────────────────────────────────
    // Mounted asset packages. Mount more at runtime (DLC, a patch pack), look an asset
    // up, or load a .stsd map:
    //     Assets().LoadScene(mapScene, "assets/scenes/s1map.stsd");
    // AppConfig::assetPacks is mounted for you before the engine starts.
    st::AssetSystem& Assets() { return st::AssetSystem::Get(); }

    // ── loading ────────────────────────────────────────────────────────────────
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
    // ── project hooks ──────────────────────────────────────────────────────────
    // Register the project's scenes. Called once during Initialize(), before
    // AppConfig::startupScene is loaded.
    virtual void RegisterScenes(SceneManager& /*scenes*/) {}
    // After the framework is up and the startup scene is loaded.
    virtual void OnInitialize() {}
    // Once per frame, after the scene manager has updated.
    virtual void OnUpdate(float /*dt*/) {}
    // Engine fixed tick (wi::Application::FixedUpdate) — physics-rate game logic.
    virtual void OnFixedUpdate() {}

    // ── UI ─────────────────────────────────────────────────────────────────────
    // The game's own ImGui UI. Drawn every frame regardless of DevUIMode.
    virtual void RenderUI() {}
    // Extra developer panels. Only called while DevUI is visible.
    virtual void RenderDevUI() {}
    // Called inside the DevUI main menu bar; add your own ImGui::BeginMenu here.
    virtual void OnDevUIMenu() {}
    // Drawn on top of everything while LoadingState::active. The default is the
    // framework's status/progress overlay; override for custom loading art.
    virtual void RenderLoadingScreen(const LoadingState& state);

    // ── render ─────────────────────────────────────────────────────────────────
    // After the render path is created and loaded, before it is activated. Set
    // render-path options here, or activate a path of your own instead.
    virtual void OnRenderPathSetup(wi::RenderPath3D& /*path*/) {}
    // The game's own GPU work, after the engine has rendered the frame.
    virtual void OnRender() {}
    // Under the composed 3D frame — a backdrop drawn before everything else.
    virtual void OnPreCompose(wi::graphics::CommandList /*cmd*/) {}
    // Additively over the composed 3D frame, before the UI is drawn.
    virtual void OnCompose(wi::graphics::CommandList /*cmd*/) {}

    // ── scenes ─────────────────────────────────────────────────────────────────
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
    void DevUIAbout(bool *show);
    void DevUIHierarchy();      // Hierarchy (Explorer) + Properties (Inspector) windows
    void DevUIAssetExplorer();  // dockable window: browse/import/extract mounted packs

    // Per-asset load progress out of st::AssetSystem. Static because the callback is a
    // plain function pointer, and called from loading worker threads — see the
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
    bool showHierarchy = false;
    bool showProperties = false;
    bool showFaustDSP = false;
    bool showAssetExplorer = false;

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
