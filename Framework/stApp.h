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
#include "sysui/imbacklog.h"
#include "sysui/imgraphicsettings.h"
#include "sysui/imhierarchy.h"
#include "SubWinStatus.h"
#include "render/LensFlare.h"
#include "io/SettingsManager.h"
#include "audio/faust/FaustManager.h"

#include <string>

using namespace wi::graphics;

namespace st {

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
};

struct ImGui_Impl_Data {};

class App : public wi::Application {
public:
    App();

    wi::RenderPath3D renderPath;
    SceneManager     sceneManager;
    ZmqHandler       zmqHandler;
    st::LensFlare    lensFlare;

    void Initialize() override;
    void Compose(wi::graphics::CommandList cmd) override;
    void Update(float dt) override;
    void Exit() override;

    // The config st::Run was given. Valid from Run() onwards.
    static const AppConfig& Config();

protected:
    // ── project hooks ──────────────────────────────────────────────────────────
    // Register the project's scenes. Called once during Initialize(), before
    // AppConfig::startupScene is loaded.
    virtual void RegisterScenes(SceneManager& /*scenes*/) {}
    // After the framework is up and the startup scene is loaded.
    virtual void OnInitialize() {}
    // Once per frame, after the scene manager has updated.
    virtual void OnUpdate(float /*dt*/) {}
    // Inside the ImGui frame — draw project windows here.
    virtual void OnGUI() {}
    // Additively over the composed 3D frame, before the UI is drawn.
    virtual void OnCompose(wi::graphics::CommandList /*cmd*/) {}
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
    SubWinStatus m_loadingScreen;

    // Loads/unloads AOT Faust processors, played through the engine OpenAL stream.
    st::audio::FaustManager faustManager;

    Texture fontTexture;

    // Lazily-loaded icon shown in the About window; held so its texture stays alive.
    wi::Resource aboutEngineIcon;

    void ImGui_CreateDeviceObjects();
    ImGui_Impl_Data* ImGui_GetBackendData();

    void ImguiInit(SDL_Window *window);
    void ImguiCompose(wi::graphics::CommandList cmd);
    void ImguiUpdate();
    void ImguiExit();

    void SysUIRender();
    void SysUIMenuBar();
    void SysUISceneSelector();
    void SysUISceneManager();   // dockable window: list/select/load scenes + reload from scratch
    void SysUILoadingScreen();
    void SysUIAbout(bool *show);
    void SysUIHierarchy();      // Hierarchy (Explorer) + Properties (Inspector) windows



    bool showImguiDemo = false;
    bool showBackLog = false;
    bool showGraphicsSettings = false;
    bool showAbout = false;
    bool showSceneManager = false;
    bool showHierarchy = false;
    bool showProperties = false;
    bool showFaustDSP = false;

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

    // Startup shader/pipeline compile overlay. ImGui's own shaders load first
    // (ImguiInit), so this overlay can render while the engine compiles the rest.
    bool loadingDone_       = false;  // latched true once compilation settles
    bool loadingSawWork_    = false;  // saw IsPipelineCreationActive() > 0 at least once
    int  loadingIdleFrames_ = 0;      // consecutive frames with no active compile jobs
};

} // namespace st
