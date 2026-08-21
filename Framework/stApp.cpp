#include "stApp.h"
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include "eventBus.h"
#include "input/InputSystem.h"
#include "wiPhysics.h"

namespace {
// Owned by st::Run for the whole process; null until Run() installs it.
const st::AppConfig* g_config = nullptr;
}

namespace st::detail {
void SetActiveConfig(const AppConfig* config) { g_config = config; }
}

const st::AppConfig& st::App::Config() {
    static const AppConfig fallback;
    return g_config ? *g_config : fallback;
}

st::App::App() : m_loadingScreen("Starting up", 300, 75) {}

void st::App::SetDevUIVisible(bool visible) {
    // DevUIMode::Disabled is a build decision, not a runtime one: a shipped game
    // cannot be talked into opening the tooling.
    devUIVisible_ = visible && Config().devUI != DevUIMode::Disabled;
}

void st::App::SetLoadingStatus(std::string status, int percent) {
    loading_.status  = std::move(status);
    loading_.percent = percent;
    // While the native window is up (startup, scene transitions) it is the only
    // thing that can paint — the main thread is blocked inside Scene::Load().
    m_loadingScreen.SetStatusText(loading_.status);
    if (percent >= 0) m_loadingScreen.SetProgress(percent);
}

void st::App::Initialize() {
    devUIVisible_ = (Config().devUI == DevUIMode::Visible);
    loading_.active = true;   // startup counts as loading until the engine is up

    // Native loading window on its own thread (Win32/X11, no SDL). It keeps
    // animating while this (main) thread blocks below building the engine.
    m_loadingScreen.Show();

    // Map-load progress: scenes Emit "loading.progress" from wi::scene::LoadModel's
    // callback (see Scene1::Load). Payload is "<percent>|<status text>". The bus is
    // main-thread and scene Load runs on this thread, so this fires in-thread; the
    // SubWinStatus setters are thread-safe regardless. Subscribe BEFORE loading scenes.
    EventBus::Get().Subscribe("loading.progress", [this](const std::string& payload) {
        const size_t bar = payload.find('|');
        if (bar == std::string::npos) return;
        SetLoadingStatus(payload.substr(bar + 1), std::atoi(payload.substr(0, bar).c_str()));
    });

    // Scene transitions reach the project as OnSceneLoaded / OnSceneUnloaded.
    sceneManager.SetCallbacks(
        [this](const std::string& name) { OnSceneLoaded(name); },
        [this](const std::string& name) { OnSceneUnloaded(name); });

    // loading system assets
    m_loadingScreen.SetStatusText("loading system assets");
    if (!Config().iconImage.empty() && !aboutEngineIcon.IsValid()) {
        aboutEngineIcon = wi::resourcemanager::Load(Config().iconImage);
    }

    m_loadingScreen.SetStatusText("starting ImGui");
    ImguiInit(window);

    // Video options (window mode, monitor, resolution, v-sync, frame cap, render
    // scale) before the render path is built, so the canvas is sized from the final
    // window rather than the one st::Run happened to open.
    displaySettings_.LoadAndApply(st::SettingsManager::Get().SubCompound("display"), *this);

    // After ImguiInit, which runs wi::Application::Initialize() and so guarantees the
    // graphics device exists and the shader path has its backend subfolder appended.
    lensFlare.Init();

    renderPath.init(canvas);
    renderPath.Load();

    // Scenes own wi::scene::Scene::Update(dt) — they run it from sceneManager.Update(),
    // after st::InputSystem refresh, which native components read. RenderPath3D::Update()
    // would update the scene a second time (and too early). A second update per frame
    // swaps MeshComponent's so_pos/so_pre streamout views twice, which cancels out: GPU
    // skinning then always writes the same half and the previous-position half is never
    // written, so skinned meshes get garbage velocity (broken motion blur, TAA, FSR2).
    renderPath.setSceneUpdateEnabled(false);

    // Project hook: last chance to configure the path (or activate one of your own)
    // before it goes live.
    OnRenderPathSetup(renderPath);

    // Only claim the path if the project did not activate one of its own.
    if (GetActivePath() == nullptr) ActivatePath(&renderPath);

    // Push saved graphics options to the engine now, so the first frame reflects them.
    // Get() constructs the manager on first call and its constructor already reads
    // options.stad (in AppData/LocalLow/PlatoonLabs/Milistry) — no explicit Load() needed.
    graphicsSettings.loadAndApply(st::SettingsManager::Get().GraphicsTag(), renderPath, *this);
    // lensFlare.Init() already ran above; this just fills its settings from disk.
    lensFlare.LoadFrom(st::SettingsManager::Get().SubCompound("lensflare"));

    // Centralized input: install the default keymap before any scene updates.
    st::InputSystem::Get().LoadDefaults();

    // Ensure the physics simulation actually steps (not just builds bodies). Some
    // load paths leave it disabled, which freezes characters (built but never moved).
    wi::physics::SetEnabled(true);
    wi::physics::SetSimulationEnabled(true);

    m_loadingScreen.SetStatusText("registering scenes");
    // Project hook: every scene the game owns is registered here, before the
    // starting scene is loaded below.
    RegisterScenes(sceneManager);

    // Background ZMQ receiver. It only enqueues bytes; messages are drained and
    // re-published on the main thread in Update() so scene subscribers stay
    // on-thread (the Wicked scene is not thread-safe).
    if (!Config().zmqEndpoint.empty()) {
        m_loadingScreen.SetStatusText("starting zmq");
        zmqHandler.Start(Config().zmqEndpoint);
    }

    // Load the starting scene immediately so entities exist before the first
    // frame's scene->Update() runs.
    if (!Config().startupScene.empty()) {
        m_loadingScreen.SetStatusText("loading scene");
        sceneManager.Load(Config().startupScene);
        sceneManager.Update(0.0f); // flush pendingLoad_ now, not on the first Update
    }

    OnInitialize();

    m_loadingScreen.Hide(); // engine ready → tear the loading window down
    loading_ = LoadingState{};   // nothing is loading any more
}

void st::App::Update(float dt) {
    if (isStop) {
        isStop = false;
        SDL_Event quitEvent;
        quitEvent.type = SDL_QUIT;
        SDL_PushEvent(&quitEvent);
        return;
    }

    wi::Application::Update(dt);

    ImguiUpdate();

    // Drain ZMQ messages received on the background thread and publish them on
    // the main thread, before scenes update, so subscribers see them this frame.
    for (const std::string& msg : zmqHandler.Drain())
        EventBus::Get().Emit("zmq.message", msg);

    // Refresh action/axis state + relative-mouse delta once per frame, after
    // wi::Application::Update (wi::input fresh) + ImguiUpdate (io.WantCapture* fresh),
    // before scenes/components read it in sceneManager.Update.
    st::InputSystem::Get().Update(dt);

    // Scene::Load() blocks this thread, so no ImGui frame can be drawn while a
    // transition runs. The native loading window lives on its own thread and keeps
    // painting, which is the only thing that works here — same reason startup uses it.
    const bool transitioning = sceneManager.HasPendingLoad();
    if (transitioning) {
        loading_.active  = true;
        loading_.scene   = sceneManager.PendingName();
        loading_.percent = -1;
        loading_.status  = "loading " + loading_.scene;
        m_loadingScreen.SetStatusText(loading_.status);
        m_loadingScreen.Show();
    }

    sceneManager.Update(dt);

    if (transitioning) {
        m_loadingScreen.Hide();
        loading_ = LoadingState{};
    }

    OnUpdate(dt);

    // After sceneManager.Update: the scene's light system has refreshed the sun's
    // direction and the camera has already moved this frame, so the projected sun
    // position matches what the render path is about to draw.
    lensFlare.Update(wi::scene::GetScene(), wi::scene::GetCamera(), dt);

    flagsChangedThisFrame = 
        (STDDBoneLines != lastState.BoneLines) ||
        (STDDCameras != lastState.Cameras) ||
        (STDDColliders != lastState.Colliders) ||
        (STDDEmitters != lastState.Emitters) ||
        (STDDEnvProbes != lastState.EnvProbes) ||
        (STDDForceFields != lastState.ForceFields) ||
        (STDDPartitionTree != lastState.PartitionTree) ||
        (STDDSprings != lastState.Springs);

    if (flagsChangedThisFrame) {
        lastState = { STDDBoneLines, STDDCameras, STDDColliders, STDDEmitters, STDDEnvProbes, STDDForceFields, STDDPartitionTree, STDDSprings };
        
        wi::renderer::SetToDrawDebugBoneLines(STDDBoneLines);
        wi::renderer::SetToDrawDebugCameras(STDDCameras);
        wi::renderer::SetToDrawDebugColliders(STDDColliders);
        wi::renderer::SetToDrawDebugEmitters(STDDEmitters);
        wi::renderer::SetToDrawDebugEnvProbes(STDDEnvProbes);
        wi::renderer::SetToDrawDebugForceFields(STDDForceFields);
        wi::renderer::SetToDrawDebugPartitionTree(STDDPartitionTree);
        wi::renderer::SetToDrawDebugSprings(STDDSprings);
        wi::renderer::SetToDrawVoxelHelper(STDVoxelHelper, STDGridHelper_clipmap_level);
        wi::renderer::SetToDrawGridHelper(STDGridHelper);
    }
}

void st::App::FixedUpdate() {
    wi::Application::FixedUpdate();
    OnFixedUpdate();
}

void st::App::Render() {
    wi::Application::Render();
    OnRender();
}

void st::App::Compose(wi::graphics::CommandList cmd) {
    // Under everything: a backdrop drawn before the engine composes the frame.
    OnPreCompose(cmd);

    wi::Application::Compose(cmd);

    // Between the two: additively over the composed 3D frame, but under the UI, so
    // the flare never washes out ImGui windows.
    lensFlare.Draw(canvas, cmd);

    OnCompose(cmd);

    ImguiCompose(cmd);
}

void st::App::Exit() {
    OnExit();

    // Persist current graphics + any option edits on the way out, so quitting without
    // pressing Apply still keeps the live settings.
    graphicsSettings.SaveTo(st::SettingsManager::Get().GraphicsTag());
    displaySettings_.SaveTo(st::SettingsManager::Get().SubCompound("display"));
    lensFlare.SaveTo(st::SettingsManager::Get().SubCompound("lensflare"));
    st::SettingsManager::Get().Save();
    zmqHandler.Stop(); // join receiver thread before tearing anything else down
    faustManager.Unload(); // join audio thread + close OpenAL before teardown
    ImguiExit();
    wi::Application::Exit();
}
