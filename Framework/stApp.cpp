#include "stApp.h"
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include "eventBus.h"
#include "input/InputSystem.h"
#include "io/asset/AssetSystem.h"
#include "io/model/ModelImporter.h"
#include "script/ScriptSystem.h"
#include "terrain/TerrainSystem.h"
#include "dev/DevBuild.h"
#include "dev/EditorBridge.h"
#include "scene/TemplatePool.h"
#include "ui/UISystem.h"
#include "xr/XRSystem.h"
#include "net/NetSession.h"
#include "stream/StreamHost.h"
#include "online/EosPlatform.h"
#include "version.h"   // generated: ST_APP_VERSION
#include "stRun.h"
#include "scene/FileScene.h"
#include "render/api/Backend.h"
#include "wiEventHandler.h"
#include "wiGraphicsDevice_Null.h"
#include "wiLua.h"
#include "wiPhysics.h"
#include "wiScene.h"
#include "wiHelper.h"
#include "wiBacklog.h"

#include <algorithm>

namespace {

// "mobile_minimal" (command line, or `adb shell am start ... --es args mobile_minimal` on
// Android): every optional render pass and scene effect off. A bring-up tool for a new GPU -
// get a stable frame first, then turn things back on one at a time to find what breaks.
bool MobileMinimal() {
    return wi::arguments::HasArgument("mobile_minimal");
}

// Weather is rewritten every frame (DayNight), so its effects are switched off every frame.
void ApplyMobileMinimalWeather(wi::scene::Scene& scene) {
    for (size_t i = 0; i < scene.weathers.GetCount(); ++i) {
        scene.weathers[i].SetVolumetricClouds(false);
        scene.weathers[i].SetOceanEnabled(false);
        // per-pixel atmosphere raymarch instead of the sky-view LUT: trips Mali's job timeout
        scene.weathers[i].SetRealisticSkyHighQuality(false);
        scene.weathers[i].SetRealisticSkyReceiveShadow(false);
    }
    scene.weather.SetVolumetricClouds(false);
    scene.weather.SetOceanEnabled(false);
    scene.weather.SetRealisticSkyHighQuality(false);
    scene.weather.SetRealisticSkyReceiveShadow(false);
}

} // namespace

namespace {
// Owned by st::Run for the whole process; null until Run() installs it.
const st::AppConfig* g_config = nullptr;

// --- Collision models ---
//
// A rigid body can take its collision geometry from a FILE rather than from anything in the
// scene (RigidBodyPhysicsComponent::mesh_source == ExternalFile), which is how a low-poly
// collision hull ships beside a high-poly visual model. Reading a model file is the framework's
// job - Framework/io/model sits above the engine - so the engine publishes a hook and this
// fills it in. Everything below runs on the MAIN THREAD: the engine primes its cache before it
// dispatches any physics job, precisely because importing builds render data and registers
// resources.

// Merge every mesh a loaded scene holds into one triangle soup, in the scene's own space.
// Placement matters: a model with a node hierarchy has its parts positioned by their
// transforms, and collapsing to raw mesh-local vertices would pile them all on the origin.
void AppendSceneGeometry (const wi::scene::Scene& scene, wi::physics::CollisionGeometry& out)
{
    for (size_t i = 0; i < scene.objects.GetCount(); ++i)
    {
        const wi::scene::ObjectComponent& object = scene.objects[i];
        const wi::scene::MeshComponent* mesh = scene.meshes.GetComponent(object.meshID);
        if (mesh == nullptr || mesh->vertex_positions.empty() || mesh->indices.empty())
            continue;

        const wi::ecs::Entity entity = scene.objects.GetEntity(i);
        const wi::scene::TransformComponent* transform = scene.transforms.GetComponent(entity);
        const XMMATRIX world = transform != nullptr ? XMLoadFloat4x4(&transform->world) : XMMatrixIdentity();

        const uint32_t base = (uint32_t)out.vertex_positions.size();
        out.vertex_positions.reserve(out.vertex_positions.size() + mesh->vertex_positions.size());
        for (const XMFLOAT3& position : mesh->vertex_positions)
        {
            XMFLOAT3 placed;
            XMStoreFloat3(&placed, XMVector3Transform(XMLoadFloat3(&position), world));
            out.vertex_positions.push_back(placed);
        }

        out.indices.reserve(out.indices.size() + mesh->indices.size());
        for (uint32_t index : mesh->indices)
        {
            out.indices.push_back(base + index);
        }
    }
}

// Read one collision model. Dispatches on extension the same way the editor's import does:
// a packed map, an engine archive, or one of the interchange formats.
bool LoadCollisionModel (const std::string& path, wi::physics::CollisionGeometry& out)
{
    std::string extension = wi::helper::GetExtensionFromFileName(path);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    // A scene of its own, thrown away as soon as the triangles are copied out. Nothing here
    // is ever rendered; it exists only to give the loaders somewhere to build components.
    wi::scene::Scene model;

    if (extension == "stsd")
    {
        std::string error;
        if (st::AssetSystem::Get().LoadScene(model, path, XMMatrixIdentity(), true, nullptr, &error) == wi::ecs::INVALID_ENTITY)
        {
            wi::backlog::post("Collision model '" + path + "' failed to load: " + error, wi::backlog::LogLevel::Error);
            return false;
        }
    }
    else if (extension == "wiscene")
    {
        if (wi::scene::LoadModel(model, path, XMMatrixIdentity(), true) == wi::ecs::INVALID_ENTITY)
            return false;
    }
    else if (st::model::CanImport(path))
    {
        // A collision model needs geometry and nothing else. Skipping the rest is not only
        // faster, it keeps a collision file from dragging in lights and animation data.
        st::model::ImportOptions options;
        options.importCameras    = false;
        options.importLights     = false;
        options.importAnimations = false;
        options.importSkins      = false;
        options.generateMissingNormals = false;

        const st::model::ImportResult result = st::model::Import(model, path, options);
        if (!result.ok())
        {
            wi::backlog::post("Collision model '" + path + "' failed to import: " + result.error, wi::backlog::LogLevel::Error);
            return false;
        }
    }
    else
    {
        wi::backlog::post(
            "Collision model '" + path + "' has no loader for its extension. Supported: "
            ".gltf .glb .fbx .obj .wiscene .stsd",
            wi::backlog::LogLevel::Error);
        return false;
    }

    // Resolve the hierarchy so every object's world matrix exists before it is read.
    model.Update(0.0f);
    AppendSceneGeometry(model, out);
    return out.IsValid();
}

void InstallCollisionMeshLoader ()
{
    wi::physics::SetCollisionMeshLoader(&LoadCollisionModel);
}
}

namespace st::detail {
void SetActiveConfig(const AppConfig* config) { g_config = config; }
}

const st::AppConfig& st::App::Config() {
    static const AppConfig fallback;
    return g_config ? *g_config : fallback;
}

// 300x75 fitted one short line. The loading window now carries a phase, a percentage
// and an asset path on its own row, and a path is long - at 300 wide every one of them
// elided down to nothing useful.
st::App::App() : m_loadingScreen("Starting up", 470, 124),
                 renderer_(std::make_unique<st::render::detroit::DetroitRenderer>()) {}

void st::App::CreateGraphicsDevice(st::GraphicsAPI api, wi::platform::window_type window) {
    if (graphicsDevice != nullptr)
        return;   // a resize re-enters SetWindow; the device is created once

    graphicsDevice = st::CreateGraphicsDevice(api, window);
}

void st::App::CreateHeadlessDevice() {
    if (graphicsDevice != nullptr) return;
    graphicsDevice = std::make_unique<wi::graphics::GraphicsDevice_Null>();
    wi::backlog::post("Graphics: none (headless, GraphicsDevice_Null)");
}

void st::App::SetDevUIVisible(bool visible) {
    // DevUIMode::Disabled is a build decision, not a runtime one: a shipped game
    // cannot be talked into opening the tooling.
    devUIVisible_ = visible && Config().devUI != DevUIMode::Disabled;

    // Hiding the DevUI stops EditorUI::Draw from running, and with it the per-frame
    // reconcile that decides who owns the mouse and keyboard. Hand both back explicitly,
    // or the game stays input-dead behind an editor nobody can see.
    if (!devUIVisible_) editor_.ReleaseInput();
}

void st::App::SetLoadingStatus(std::string status, int percent) {
    loading_.status  = std::move(status);
    loading_.percent = percent;
    // While the native window is up (startup, scene transitions) it is the only
    // thing that can paint - the main thread is blocked inside Scene::Load().
    m_loadingScreen.SetStatusText(loading_.status);
    if (percent >= 0) m_loadingScreen.SetProgress(percent);
}

void st::App::OnAssetLoadProgress(const st::AssetLoadProgress& progress, void* userdata) {
    st::App* self = static_cast<st::App*>(userdata);
    if (self == nullptr) return;

    // OFF THE MAIN THREAD, and several of these run at once. That rules out loading_
    // (a std::string the main thread reads), the EventBus (documented main-thread
    // only), ImGui, and the scene. SubWinStatus is the one sink built for exactly this:
    // its own thread, its own window, thread-safe setters - and while a scene loads it
    // is the only thing on screen that can paint anyway, because the main thread is
    // blocked inside Scene::Load().
    //
    // The DETAIL line, not the status line: the status line is the engine's phase
    // ("Loading materials"), which changes rarely, and writing the asset there would
    // overwrite it several times per frame so the phase was never readable.
    std::string detail;
    if (progress.assetsExpected > 0) {
        const uint32_t done = progress.assetsRead < progress.assetsExpected
                            ? progress.assetsRead : progress.assetsExpected;
        // Count first, so the one part of the line that proves motion sits at a fixed
        // position and survives the path being elided.
        detail = std::to_string(done) + "/" + std::to_string(progress.assetsExpected) + "  ";
    }
    detail += progress.currentAsset;
    if (progress.currentSize > 0)
        detail += "  (" + st::asset::FormatBytes(progress.currentSize) + ")";

    self->m_loadingScreen.SetDetailText(detail);

    // The bar is deliberately left alone. The engine owns it for the whole load and is
    // mid-phase here; driving it from a second source would make it jump backwards.
    // The n/total above is what shows this is progressing.
}

void st::App::Initialize() {
    devUIVisible_ = (Config().devUI == DevUIMode::Visible);
    loading_.active = true;   // startup counts as loading until the engine is up

    // Native loading window on its own thread (Win32/X11, no SDL). It keeps
    // animating while this (main) thread blocks below building the engine.
    //
    // Titled here rather than in the constructor: the project's AppConfig is installed
    // by st::Run, which runs after the App object is built, so the name is not known
    // until now. SetTitle is read once, at CreateWindow time on the window thread.
    if (!Config().name.empty()) m_loadingScreen.SetTitle(Config().name);
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

    // Per-asset progress out of the mounted packages. This covers the stretch the
    // callback above cannot: while the engine waits for a scene's textures to finish
    // loading it reports nothing at all, and on a big map that wait IS the load. Those
    // reads happen on job-system workers, so this callback lands off the main thread
    // see OnAssetLoadProgress for what that rules out.
    st::AssetSystem::Get().SetLoadProgressCallback(&st::App::OnAssetLoadProgress, this);

    // Scene transitions reach the project as OnSceneLoaded / OnSceneUnloaded.
    //
    // The daylight system is bound and released here rather than by the scenes: its target
    // is an entity, and every entity a scene owns is gone by the time OnSceneUnloaded
    // returns. Binding runs BEFORE the project hook so a game's OnSceneLoaded can already
    // retune the cycle it was given.
    sceneManager.SetCallbacks(
        [this](const std::string& name) {
            // The scene updates in this same frame, before TerrainSystem::Update runs again:
            // bind now, or its first chunks are generated from the engine's noise.
            st::terrain::TerrainSystem::Get().BindAll(wi::scene::GetScene());
            // A different scene: the Play-mode snapshot belongs to the one that left.
            playSnapshot_.Clear();
            switch (Config().dayNight) {
                case st::DayNightMode::Adopt:  st::DayNight::Get().Adopt(wi::scene::GetScene()); break;
                case st::DayNightMode::Create: st::DayNight::Get().Create(wi::scene::GetScene()); break;
                case st::DayNightMode::Off:    break;
            }
            OnSceneLoaded(name);
        },
        [this](const std::string& name) {
            OnSceneUnloaded(name);
            st::DayNight::Get().Detach();
        });

    // loading system assets
    m_loadingScreen.SetStatusText("loading system assets");
    if (!Config().iconImage.empty() && !aboutEngineIcon.IsValid()) {
        aboutEngineIcon = wi::resourcemanager::Load(Config().iconImage);
    }

    m_loadingScreen.SetStatusText("starting ImGui");
    ImguiInit(window);   // also runs wi::Application::Initialize: the device and canvas are up

    // The render interface (render/api) - from here on Framework code draws through it.
    st::render::Install(renderer_.get());

    // Video options (window mode, monitor, resolution, v-sync, frame cap, render
    // scale) before the render path is built, so the canvas is sized from the final
    // window rather than the one st::Run happened to open.
    displaySettings_.LoadAndApply(st::SettingsManager::Get().SubCompound("display"), *this);

    // Optics first: it has no GPU state of its own, and the laser system traces through
    // it from the moment it starts running.
    optics_.Init();

    // The game path and what rides on it (lens flare, projectors, lasers), the saved
    // graphics options. The project's render hooks go with it.
    st::render::detroit::DetroitRenderer::Hooks hooks;
    hooks.onPathSetup = [this](wi::RenderPath3D& path) { OnRenderPathSetup(path); };
    hooks.onPreCompose = [this](wi::graphics::CommandList cmd) { OnPreCompose(cmd); };
    hooks.onCompose = [this](wi::graphics::CommandList cmd) { OnCompose(cmd); };
    renderer_->Initialize(*this, std::move(hooks));
#if defined(SIMTARY_HAS_HURRICANES)
    if (st::render::ChosenBackend() == st::render::Backend::Hurricanes && window != nullptr) {
        hurricanes_ = std::make_unique<st::render::hurricanes::HurricanesRenderer>(
            (void*)wi::platform::GetWindowHandle(window));
        if (hurricanes_->Ready()) st::render::Install(hurricanes_.get());
    }
#endif
    optics_.LoadFrom(st::SettingsManager::Get().SubCompound("optics"));
    st::DayNight::Get().LoadFrom(st::SettingsManager::Get().SubCompound("daynight"));

    // Edit / Play mode transitions (snapshot, restore, reload) are applied by
    // ApplyPlayModeTransition at the top of each frame. --edit opens straight into the
    // editor in Edit mode, the way the Unity editor opens. Not on Hurricanes: that edition is
    // play-only (EDITIONS.md), and Edit mode without an editor is just a frozen game.
    if (st::dev::IsDevBuild() && (wi::arguments::HasArgument("--edit") || wi::arguments::HasArgument("edit")) &&
        Config().devUI != st::DevUIMode::Disabled) {
        if (st::render::ChosenBackend() != st::render::Backend::Detroit)
            wi::backlog::post("--edit ignored: the editor runs on Detroit only (Hurricanes is play-only)",
                              wi::backlog::LogLevel::Warning);
    }
    if (st::dev::IsDevBuild() && (wi::arguments::HasArgument("--edit") || wi::arguments::HasArgument("edit")) &&
        Config().devUI != st::DevUIMode::Disabled && st::render::ChosenBackend() == st::render::Backend::Detroit) {
        st::PlayControl::Get().StartInEditMode();
        startInEdit_ = true;
    }

    // Gamepad backend policy: the project's default, overridden by whatever the player
    // (or a developer in the Gamepad Analog panel) last chose. Applied before the first
    // Update, so the very first frame already registers the pads the chosen way rather
    // than registering them twice and re-shuffling the player slots a frame later.
    {
        const int stored = st::SettingsManager::Get().SubCompound("input")
                               .getInt("gamepadBackend", int(Config().gamepadBackend));
        const int clamped = (stored < 0 || stored > int(wi::input::GamepadBackend::RawInputOnly))
                          ? int(Config().gamepadBackend) : stored;
        wi::input::SetGamepadBackend(wi::input::GamepadBackend(clamped));

        wi::input::GetAnalogSettings().right_stick_up_positive =
            st::SettingsManager::Get().SubCompound("input")
                .getBool("rightStickUpPositive", Config().rightStickUpPositive);
    }

    // Centralized input: install the default keymap before any scene updates. This
    // seeds both halves - the legacy flat keymap AND the default "Player"/"UI" action
    // maps - and then hands the registry to the project.
    st::InputSystem::Get().LoadDefaults();

    // Project hook: the game's keybinds. Before RegisterScenes, so a scene loaded
    // below already finds its action map registered when its stInput components start.
    m_loadingScreen.SetStatusText("registering keybinds");
    OnKeyRegister(st::InputSystem::Get().Actions());

    // Ensure the physics simulation actually steps (not just builds bodies). Some
    // load paths leave it disabled, which freezes characters (built but never moved).
    wi::physics::SetEnabled(true);
    wi::physics::SetSimulationEnabled(true);
    InstallCollisionMeshLoader();

    m_loadingScreen.SetStatusText("registering scenes");
    // Project hook: every scene the game owns is registered here, before the
    // starting scene is loaded below.
    RegisterScenes(sceneManager);

    // Folder scan, after the C++ registrations on purpose: whatever RegisterScenes() put
    // in wins both the name and its own map files, and everything else in the folder is
    // added next to it. See SceneManager::DiscoverScenes.
    if (Config().sceneAutoDiscover && !Config().sceneFolder.empty()) {
        m_loadingScreen.SetStatusText("scanning scenes");
        const int discovered = sceneManager.DiscoverScenes(Config().sceneFolder);
        wi::backlog::post("SceneManager: scanned " + Config().sceneFolder + ", " +
                          std::to_string(discovered) + " scene(s) added");
    }

    // Background ZMQ receiver. It only enqueues bytes; messages are drained and
    // re-published on the main thread in Update() so scene subscribers stay
    // on-thread (the Wicked scene is not thread-safe).
    if (!Config().zmqEndpoint.empty()) {
        m_loadingScreen.SetStatusText("starting zmq");
        zmqHandler.Start(Config().zmqEndpoint);
    }

    // Dev build: the editor bridge, the socket tools (and tools/mcp) drive the game through.
    // Never started in a normal build - a shipped game opens no port.
    if (st::dev::IsDevBuild() && !wi::arguments::HasArgument("--no-devbridge") &&
        !wi::arguments::HasArgument("no-devbridge")) {
        std::string portArg;
        const int bridgePort = wi::arguments::GetArgumentValue("--devbridge-port=", portArg) ? atoi(portArg.c_str()) : 0;
        st::dev::EditorBridge::Get().Start(*this, bridgePort);
    }

    // VR (Simtary/VR.md): opt-in with --xr, inert otherwise. After the device exists.
    st::xr::XRSystem::Get().Initialize(Config().name);

    // Multiplayer (Simtary/MULTIPLAYER.md): --server[=port] / --connect=<ip>[:port], off otherwise.
    st::net::NetSession::Get().Initialize(Config().name);

    // Epic Online Services (Simtary/ONLINE.md): on when the project set its ids, off otherwise.
    st::online::EosPlatform::Get().Initialize(Config().eos, Config().name, ST_APP_VERSION);

    // Streaming (Simtary/CLOUD.md): --stream, off otherwise. After the device and the window.
    st::stream::StreamHost::Get().Initialize(*this);

    // Load the starting scene immediately so entities exist before the first
    // frame's scene->Update() runs.
    // --scene-file=<map>: start in a map file that is not in the scene folder (the editor's
    // Net Play saves the open scene to one and starts servers / clients on it).
    std::string startupScene = Config().startupScene;
    {
        std::string sceneFile;
        if ((wi::arguments::GetArgumentValue("--scene-file=", sceneFile) || wi::arguments::GetArgumentValue("scene-file=", sceneFile)) &&
            !sceneFile.empty()) {
            sceneManager.Register("scene-file", std::make_unique<st::FileScene>(sceneFile));
            startupScene = "scene-file";
        }
    }
    if (!startupScene.empty()) {
        m_loadingScreen.SetStatusText("loading scene");
        sceneManager.Load(startupScene);
        sceneManager.Update(0.0f); // flush pendingLoad_ now, not on the first Update
    }

    OnInitialize();

    m_loadingScreen.Hide(); // engine ready → tear the loading window down
    loading_ = LoadingState{};   // nothing is loading any more
}

void st::App::ApplyPlayModeTransition() {
    // Applied here, between frames: no ImGui window is mid-draw over the entity list and
    // no scene update is running, so whole entities can come and go.
    st::PlayControl& play = st::PlayControl::Get();
    wi::scene::Scene& scene = wi::scene::GetScene();
    switch (play.ConsumeTransition()) {
        case st::PlayControl::Transition::None:
            return;
        case st::PlayControl::Transition::EnterPlay:
            playDayTime_ = st::DayNight::Get().Time();
            playSnapshot_.Capture(scene);
            break;
        case st::PlayControl::Transition::EnterEdit:
        case st::PlayControl::Transition::Restart: {
            const bool restart = !play.IsEditing(); // a Restart leaves the mode at Play
            if (playSnapshot_.Valid()) {
                playSnapshot_.Restore(scene);
                st::DayNight::Get().SetTime(playDayTime_);
                if (restart) playSnapshot_.Capture(scene);   // and play on from it
            } else {
                // Launched playing: there is no edit state to go back to but the file.
                sceneManager.Reload();
            }
            // Pooled copies were Play-mode objects; the restore or reload took them away.
            st::TemplatePools::Get().ForgetInstances();
            // Unity's domain reload: script module state (globals, counters) does not
            //	carry from one Play into the next. Every stScript instance is gone by now;
            //	the VM is recreated on first use.
            st::ScriptSystem::Get().Shutdown();
            break;
        }
    }
}

void st::App::Update(float dt) {
    lastDt_ = dt;
    // Before ImGui builds this frame's draw data: last frame's may name these textures.
    editor_.ReleaseRetired();
    if (isStop) {
        isStop = false;
        SDL_Event quitEvent;
        quitEvent.type = SDL_QUIT;
        SDL_PushEvent(&quitEvent);
        return;
    }

    // VR (Framework/xr): while a session runs the headset is the view, so the game path is
    // not updated or rendered at all - the eyes take over its per-frame scene work and the
    // window shows the left eye (Compose). The editor keeps its own viewport, so editor mode
    // leaves the path alone.
    xrDesktopOff_ = st::xr::XRSystem::Get().IsRunning() && !editor_.IsEnabled();
    st::xr::XRSystem::Get().SetEyesOwnScene(xrDesktopOff_);
    // A headless server draws nothing either: the path's culling and render-data upkeep would
    // be CPU spent for a device that executes none of it.
    pathOff_ = xrDesktopOff_ || st::IsHeadless();
    {
        wi::RenderPath* path = activePath;
        if (pathOff_) activePath = nullptr;   // the one call that would update it
        wi::Application::Update(dt);
        activePath = path;
    }

    ImguiUpdate();

    // Drain ZMQ messages received on the background thread and publish them on
    // the main thread, before scenes update, so subscribers see them this frame.
    for (const std::string& msg : zmqHandler.Drain())
        EventBus::Get().Emit("zmq.message", msg);

    // Frame-rate standby: caps the frame rate while the window is unfocused or the
    // player has been idle, and restores their own cap when they come back.
    displaySettings_.UpdateStandby(*this, dt);

    // Refresh action/axis state + relative-mouse delta once per frame, after
    // wi::Application::Update (wi::input fresh) + ImguiUpdate (io.WantCapture* fresh),
    // before scenes/components read it in sceneManager.Update.
    st::InputSystem::Get().Update(dt);

    // Tool requests run here: main thread, between frames, before the scene sees this frame.
    st::dev::EditorBridge::Get().Pump();

    // OpenXR events + (while a session runs) the headset frame loop. xrWaitFrame paces
    // the main loop to the HMD from here.
    st::xr::XRSystem::Get().Update();
    st::net::NetSession::Get().Update(dt);
    st::online::EosPlatform::Get().Update();   // every EOS callback fires in here, on this thread
    st::stream::StreamHost::Get().Update();   // the viewer's keys / mouse, before the scene reads input
    // Two blocking waits per frame (xrWaitFrame and the desktop present) land the frame on
    // every other headset interval, so the desktop v-sync is off while a session runs.
    if (wi::input::Press(wi::input::KEYBOARD_BUTTON_F9)) st::xr::XRSystem::Get().Recenter();
    const bool xrRunning = st::xr::XRSystem::Get().IsRunning();
    if (xrRunning != xrVsyncOff_) {
        wi::eventhandler::SetVSync(xrRunning ? false : displaySettings_.vsync);
        xrVsyncOff_ = xrRunning;
    }

    // Scene::Load() blocks this thread, so no ImGui frame can be drawn while a
    // transition runs. The native loading window lives on its own thread and keeps
    // painting, which is the only thing that works here - same reason startup uses it.
    const bool transitioning = sceneManager.HasPendingLoad();
    if (transitioning) {
        loading_.active  = true;
        loading_.scene   = sceneManager.PendingName();
        loading_.percent = -1;
        loading_.status  = "loading " + loading_.scene;
        m_loadingScreen.SetStatusText(loading_.status);
        m_loadingScreen.Show();
    }

    // The transport decides how much time the SCENE sees. Everything above this line -
    // input, the loading screen, the standby cap - ran on the real frame delta, and
    // everything the DevUI does still will: a paused world is one you can still look
    // around, click on and drive the editor camera through.
    if (startInEdit_) {
        startInEdit_ = false;
        SetDevUIVisible(true);
        SetEditorMode(true);
    }
    ApplyPlayModeTransition();
    const bool editing = st::PlayControl::Get().IsEditing();
    // Edit mode builds native components for the inspector but never starts them.
    wi::scene::GetScene().nativeComponents.lifecycleEnabled = !editing;

    const float sceneDt = st::PlayControl::Get().Apply(dt);

    // Before the scene update, not after: the scene's light system is what copies the
    // sun's direction and colour into the weather, so a sun aimed afterwards would reach
    // the sky a frame late - which reads as the sky lagging behind the sun at dawn.
    // On the scene's clock too, or a paused world would keep moving through its own day.
    st::DayNight::Get().Update(wi::scene::GetScene(), sceneDt);

    // Before the scene update too: an edit queued this frame lands in the terrain data
    // before the scene kicks the chunk generator off again (Framework/terrain).
    st::terrain::TerrainSystem::Get().Update(wi::scene::GetScene(), sceneDt);

    st::ScriptSystem::Get().BeginFrame(sceneDt, dt); // Time.deltaTime, Input.* for this frame
    sceneManager.Update(sceneDt, editing);

    // Taipan coroutines, on scene time so a paused world pauses its scripts too. After
    // the scene update: stScript's onUpdate ran inside it, and a coroutine it started
    // this frame should not also take its first step in the same frame.
    st::ScriptSystem::Get().Update(sceneDt);

    if (transitioning) {
        m_loadingScreen.Hide();
        loading_ = LoadingState{};
    }

    OnUpdate(dt);
    UpdateGameUI(dt, editing);
    if (MobileMinimal()) ApplyMobileMinimalWeather(wi::scene::GetScene());

    // After sceneManager.Update: followed entities carry this frame's transform only after
    // the scene update. Optics FIRST, then the renderer's effects that reflect through
    // them. A mirror bolted to a moving rig has to be where it will be DRAWN before a beam
    // is traced off it or a virtual projector is placed behind it, or the reflection lags
    // the mirror by a frame - which reads as the reflection sliding around on its own.
    optics_.Update(wi::scene::GetScene());
    renderer_->Update(dt);
    renderer_->SetDebugOverlays(debugOverlays_);

}

void st::App::UpdateGameUI(float dt, bool editing) {
    XMUINT2 size = st::render::Get().GameResolution();
    if (const st::render::TextureObject* last = st::render::Get().GameImage())
        size = XMUINT2(last->width, last->height);

    // The pointer in game-image pixels: from the Game Viewport panel in the editor, from the
    //	window otherwise. Not while the game holds the cursor (mouse look), and not while a
    //	DevUI window has it.
    const ImGuiIO& io = ImGui::GetIO();
    st::ui::Pointer p;
    p.down = io.MouseDown[0];
    p.wheel = io.MouseWheel;
    if (!st::InputSystem::Get().IsMouseCaptured() && size.x > 0 && size.y > 0) {
        if (editor_.IsEnabled()) {
            ImVec2 pos, extent;
            bool hovered = false;
            if (editor_.GameImage(pos, extent, hovered) && hovered && extent.x > 0 && extent.y > 0) {
                p.valid = true;
                p.pos = XMFLOAT2((io.MousePos.x - pos.x) / extent.x * size.x,
                                 (io.MousePos.y - pos.y) / extent.y * size.y);
            }
        } else if (!io.WantCaptureMouse && io.DisplaySize.x > 0 && io.DisplaySize.y > 0) {
            p.valid = true;
            p.pos = XMFLOAT2(io.MousePos.x / io.DisplaySize.x * size.x, io.MousePos.y / io.DisplaySize.y * size.y);
        }
    }
    st::ui::UISystem& ui = st::ui::UISystem::Get();
    ui.SetPointer(p);
    ui.Update(wi::scene::GetScene(), dt, size, !editing);
}

void st::App::FixedUpdate() {
    wi::Application::FixedUpdate();
    OnFixedUpdate();
}

void st::App::Render() {
    renderer_->BeginFrame();
    if (pathOff_) {
        wi::lua::Render();             // what wi::Application::Render does besides the path
    } else {
        wi::Application::Render();
        // Game UI onto the path's final image, so the HUD is in the fullscreen frame, the
        //	editor's Game Viewport and every screenshot alike.
        st::render::Renderer& r = st::render::Get();
        st::ui::UISystem::Get().Render(wi::scene::GetScene(), r.UI(), r.GameImage());
    }

    // The editor's second viewport has its own RenderPath3D and its own camera. The
    // engine only ever renders wi::Application::activePath, so it is stepped by hand
    // here - after the game path, against the scene both of them share.
    editor_.RenderEditorView(lastDt_);
    editor_.RenderCameraViews(lastDt_);
    editor_.RenderTemplatePreviews(lastDt_);

    // The headset's two eyes: their own paths, configured like the game's.
    renderer_->RenderXREyes(lastDt_);

    OnRender();

    // Hurricanes: Filament draws the frame and presents it itself (Wicked's device is the null one).
    if (hurricanes_ && hurricanes_->Ready())
        hurricanes_->RenderFrame(canvas.GetPhysicalWidth(), canvas.GetPhysicalHeight());
}

void st::App::Compose(wi::graphics::CommandList cmd) {
    using Mode = st::render::detroit::DetroitRenderer::ComposeMode;
    renderer_->Compose(cmd, editor_.IsEnabled() ? Mode::EditorOnly : xrDesktopOff_ ? Mode::XRMirror : Mode::Game);
}

void st::App::Exit() {
    OnExit();

    st::dev::EditorBridge::Get().Stop();

    // The OpenXR session holds the engine's D3D12 device and queue: gone before they are.
    st::xr::XRSystem::Get().Shutdown();
    st::net::NetSession::Get().Shutdown();
    st::online::EosPlatform::Get().Shutdown();
    st::stream::StreamHost::Get().Shutdown();

    // Frees the editor's render path (and waits for the GPU) before the device goes away.
    editor_.Shutdown();
    st::ui::UISystem::Get().Shutdown();   // RmlUi and the world-canvas targets, before the device
    if (hurricanes_) hurricanes_->Shutdown();
    if (renderer_) renderer_->Shutdown();

    // Persist current graphics + any option edits on the way out, so quitting without
    // pressing Apply still keeps the live settings.
    renderer_->SaveSettings();
    displaySettings_.SaveTo(st::SettingsManager::Get().SubCompound("display"));
    optics_.SaveTo(st::SettingsManager::Get().SubCompound("optics"));
    st::DayNight::Get().SaveTo(st::SettingsManager::Get().SubCompound("daynight"));
    st::SettingsManager::Get().SubCompound("input")
        .putInt("gamepadBackend", int(wi::input::GetGamepadBackend()));
    st::SettingsManager::Get().SubCompound("input")
        .putBool("rightStickUpPositive", wi::input::GetAnalogSettings().right_stick_up_positive);
    st::SettingsManager::Get().Save();
    zmqHandler.Stop(); // join receiver thread before tearing anything else down
    faustManager.Unload(); // join audio thread + close OpenAL before teardown
    // The script VM goes before the engine. stScript instances still alive in the scene
    // see the generation change and drop their handles instead of freeing into it.
    st::ScriptSystem::Get().Shutdown();
    ImguiExit();
    wi::Application::Exit();
}
