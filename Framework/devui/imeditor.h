#pragma once
// Editor mode — the DevUI turned into a dockable scene editor.
//
//	Off by default. "Simtary > Editor > Editor Mode" (or the toggle key) switches it on, and
//	the DevUI stops being a set of floating debug panels and becomes a docked editor layout:
//
//	  ┌───────────────────────────┬──────────────┐
//	  │  Editor Viewport (freecam)│  Hierarchy   │
//	  │  Game Viewport   (tabbed) ├──────────────┤
//	  │                           │  Properties  │
//	  └───────────────────────────┴──────────────┘
//
//	Two viewports, two cameras, two RenderPath3D instances:
//
//	  Game Viewport   — the game's own path (st::App::renderPath) and its camera, exactly
//	                    what a player sees. Letterboxed into the panel, so the game's aspect
//	                    ratio never changes just because the editor layout moved. Its own
//	                    menu bar sets the RESOLUTION it renders at — "Free Aspect" follows
//	                    the window canvas (the shipping behaviour), or pick a fixed size and
//	                    the game renders 1920x1080 / 9:16 / 2.39:1 regardless of the window.
//	                    The override is editor-only: it is cleared the moment editor mode
//	                    leaves, so nothing a shipping build runs ever sees it.
//	  Editor Viewport — a SECOND RenderPath3D owned by this class, driven by a free camera
//	                    that lives only in the editor. It is resized to fit its panel
//	                    exactly (no letterbox), so screen-space picking and the gizmo map
//	                    1:1. It is created the first time editor mode is entered and freed
//	                    when it leaves, so a normal (non-editor) run pays nothing for it.
//
//	The scene is shared, so an edit made in either viewport shows up in both.
//
//	SHOW GAME PREVIEW (on by default, per viewport). The editor's extra paths are bare
//	RenderPath3D instances — engine defaults, none of the game's AO / bloom / tonemap /
//	exposure / colour grading, none of its custom post process passes (st::ProjectorSystem
//	and st::LaserSystem both ride on that list), and the SRGB colorspace default even on an
//	HDR10 swapchain. That is why they came out looking nothing like the Game Viewport. With
//	the preview on, the viewport is brought in line with the live game renderer every frame
//	and shows what the player would actually see; with it off it falls back to those bare
//	defaults, which is the cheaper, flatter, look-at-the-geometry view.
//
//	ImGuizmo draws the translate/rotate/scale handles over the hovered viewport. It writes
//	the manipulated WORLD matrix back through the parent transform, so dragging a child of a
//	rig moves it correctly, and re-syncs the 64-bit large-world absolute position for roots
//	that use it (see stWorldScalar.h) — otherwise the next UpdateTransform would rebase the
//	object straight back to where it started.
//
//	Save / Save As write the LIVE wi::scene::Scene to a .wiscene through wi::Archive. The
//	native file dialog runs on its own thread, so the chosen path is parked and the actual
//	serialize happens on the main thread on the next frame — the scene is not thread-safe.

#include "wiECS.h"
#include "wiRenderPath3D.h"
#include "wiScene_Components.h"
#include "imgui.h"
#include "ImGuizmo.h"
#include "imeditorhistory.h"
#include "io/model/ModelImporter.h"

#include <memory>
#include <vector>
#include <mutex>
#include <string>

namespace wi::scene { struct Scene; }
class GraphicsSettings; // Framework/devui/imgraphicsettings.h, global namespace

namespace st { class ProjectorRenderPath; } // Framework/render/Projector.h

namespace st {

class App;

class EditorUI {
public:
    bool IsEnabled() const { return enabled_; }
    // Entering creates the editor render path; leaving frees it (after a GPU wait, since
    // the render targets may still be in flight).
    void SetEnabled(bool on);
    void Toggle() { SetEnabled(!enabled_); }

    // Raise the docked Resource Explorer. Called when a file is dropped on the window,
    // so the drop lands somewhere visible instead of into a hidden panel.
    void ShowResources() { showResources_ = true; }

    // Queue a model / scene file to be merged into the CURRENT scene, landing in front of
    // the editor free camera. Safe to call from the SDL event loop or a dialog thread: the
    // path is parked and the load runs on the main thread in the next Draw().
    void QueueImportModel(const std::string& path);
    // True for the extensions QueueImportModel can merge into a scene. Used by the drop
    // handler to tell "put this model in the world" from "add this file to the package".
    static bool IsSceneImportPath(const std::string& path);

    // Everything ImGui. Called from st::App::DevUIRender() while editor mode is on, after
    // the DevUI main menu bar so the dock host lands under it.
    void Draw(App& app, wi::RenderPath3D& gamePath, wi::ecs::Entity& selected);

    // The editor viewport's own render, driven manually because the engine only renders
    // wi::Application::activePath. Called from st::App::Render(), after the game path.
    void RenderEditorView(float dt);
    // Same, for every spawned Camera View. Called right after RenderEditorView.
    void RenderCameraViews(float dt);

    // Menu entries for the DevUI menu bar ("Simtary > Editor").
    void MenuItems();

    // Give the cursor and the keyboard back to the game. Called when the DevUI is hidden
    // (F1) while editor mode is still on — Draw() stops running then, so nothing else would
    // ever lower the input capture and the game would sit deaf behind a hidden editor.
    void ReleaseInput();

    // Frees the editor render path. Called from st::App::Exit().
    void Shutdown();

private:
    void EnsureEditorPath();
    void BuildDefaultLayout(ImGuiID dockspaceID, ImVec2 size);
    void DrawDockHost(App& app, wi::scene::Scene& scene);
    void DrawToolbar();

    // ── camera views ─────────────────────────────────────────────────────────
    // One spawnable, VIEW-ONLY window per camera. Not one panel that follows the
    //	selection: you can have as many open at once as you like, each pinned to its own
    //	camera and framed at its own resolution and aspect — a 1080p hero shot next to a
    //	9:16 phone crop next to a 2.39:1 letterbox, all live, all from the same scene.
    //
    //	They take no input at all: no gizmo, no picking, no camera control. The Game
    //	Viewport is where the main camera plays and where input goes; the Editor Viewport is
    //	where you edit. These are monitors.
    struct CameraView {
        int  id = 0;                 // stable; only used to build a unique ImGui window id
        bool open = true;
        wi::ecs::Entity camera = wi::ecs::INVALID_ENTITY; // the CameraComponent it renders

        int   width  = 1920;
        int   height = 1080;
        int   ratio  = 0;            // index into kAspectRatios; 0 = free (W and H independent)
        bool  matchPanel = false;    // render at the panel's pixel size instead
        bool  showGamePreview = true; // render through the game's renderer (see below)

        bool  live = false;          // panel was on screen and had a camera this frame
        std::unique_ptr<wi::RenderPath3D> path;
        wi::scene::CameraComponent        cam; // a COPY; the path writes width/height into it
    };

    // Open a new one, optionally already pointed at a camera.
    void SpawnCameraView(wi::ecs::Entity camera);
    void DrawCameraViews(wi::scene::Scene& scene, wi::ecs::Entity selected);
    void DrawCameraViewToolbar(wi::scene::Scene& scene, CameraView& view);
    // Free every camera view's render path (after a GPU wait).
    void DestroyCameraViews();
    // One viewport panel. `path` supplies the image; `cam` is the camera the gizmo and the
    // (editor-only) freecam act through. `exactFit` resizes the path to the panel instead
    // of letterboxing into it.
    void DrawViewport(const char* title, bool* p_open, wi::RenderPath3D& path,
        wi::scene::CameraComponent& cam, bool exactFit,
        wi::scene::Scene& scene, wi::ecs::Entity& selected);
    // Menu bars for the two built-in viewports, drawn inside DrawViewport.
    void DrawEditorViewportToolbar();
    void DrawGameViewportToolbar();
    // Returns true while the pointer is on (or dragging) a gizmo handle, so the caller
    // knows not to treat the same click as a viewport pick.
    bool DrawGizmo(wi::scene::Scene& scene, wi::ecs::Entity selected,
        const wi::scene::CameraComponent& cam, ImVec2 imagePos, ImVec2 imageSize);
    void UpdateFreeCam(float dt, bool interactive);
    // Hand the cursor back to the game (and un-hide it) if the free camera still holds it.
    void ReleaseFreeCamLook();
    // Hand keyboard/mouse back to the game if Editor mode had taken them.
    void ReleaseInputCapture();
    // Put the free camera exactly where the game camera is looking from. Used once when
    // the editor first opens, and from the Camera menu.
    void SnapCameraToGameCamera();
    // A point a few metres in front of the free camera — where "Create" drops new objects.
    XMFLOAT3 SpawnPoint() const;
    // Label the current gizmo operation goes into the undo list under.
    const char* GizmoLabel() const;
    // Pull the free camera back to where the selected entity fills the view (F over the
    // editor viewport), keeping the current view angle.
    void FrameSelected(wi::scene::Scene& scene, wi::ecs::Entity selected);

    // Dispatches on the extension: ".wiscene" writes a plain wi::Archive, anything
    // else (including no extension) writes the native .stsd.
    void SaveScene(wi::scene::Scene& scene, const std::string& path);
    bool SaveSceneArchive(wi::scene::Scene& scene, const std::string& path);
    bool SaveSceneDescriptor(wi::scene::Scene& scene, const std::string& path);
    // `defaultExt` is appended when the user types a bare name in the dialog.
    void RequestSaveAs(const char* defaultExt = "stsd");
    void FlushPendingSave(wi::scene::Scene& scene);

    // Open the "import into this scene" file dialog. The result goes through
    // QueueImportModel, so the load itself happens on the main thread.
    void RequestImportModel();
    void FlushPendingImport(wi::scene::Scene& scene);
    // The options panel that stands between choosing a file and loading it. Drawn with
    // ImGui rather than bolted onto the OS file dialog: a native dialog can only carry
    // custom controls through a platform-specific hook (a Win32 OFNHookProc), which would
    // make the one part of the editor a Linux user needs most into a Windows-only feature.
    void DrawImportOptions(wi::scene::Scene& scene);
    // The filter label the file dialog shows, built from the loaders that exist:
    // "Model or scene (*.stsd;*.wiscene;*.gltf;...)".
    static std::string ImportFilterDescription();
    // Merge `path` into `scene` at SpawnPoint(). Returns the imported root, or
    // INVALID_ENTITY on failure (the reason lands in the backlog and in lastImportMessage_).
    wi::ecs::Entity ImportModelAtSpawn(wi::scene::Scene& scene, const std::string& path);

    bool enabled_ = false;

    // Panels. Hierarchy/Properties are separate from the DevUI's own floating copies so
    // toggling editor mode never fights the plain-DevUI window state.
    bool showEditorViewport_ = true;
    bool showGameViewport_   = true;
    bool showHierarchy_      = true;
    bool showProperties_     = true;
    bool showResources_      = true;

    bool layoutBuilt_ = false;   // dock layout authored (or restored from imgui.ini)
    bool resetLayout_ = false;   // "Reset Layout" was clicked; rebuild next frame

    // ── gizmo ────────────────────────────────────────────────────────────────
    ImGuizmo::OPERATION gizmoOp_   = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE      gizmoMode_ = ImGuizmo::WORLD;
    bool  gizmoSnap_        = false;
    // Undo bracket for a gizmo drag. The pre-drag transform is captured every frame the gizmo
    // is idle, so it is already in hand on the frame ImGuizmo reports the drag has begun.
    bool             gizmoDragging_   = false;
    wi::ecs::Entity  gizmoDragEntity_ = wi::ecs::INVALID_ENTITY;
    TransformSnapshot gizmoPreDrag_;
    float snapTranslate_    = 0.5f;
    float snapRotateDeg_    = 15.0f;
    float snapScale_        = 0.1f;

    // ── editor viewport debug draw ───────────────────────────────────────────
    // wi::renderer's debug draws are GLOBAL switches, so these are applied around the editor
    // path's render and put back afterwards — the Game Viewport keeps showing the shipping
    // picture while the editor viewport shows the scaffolding. The game path has already
    // rendered by the time RenderEditorView runs, which is what makes that possible.
    struct DebugDraw {
        bool cameras       = true;  // wireframe frustum per CameraComponent — on by default
        bool gameCamera    = true;  // the ACTIVE game camera, which is not a scene component
        bool colliders     = false;
        bool emitters      = false;
        bool envProbes     = false;
        bool forceFields   = false;
        bool springs       = false;
        bool boneLines     = false;
        bool partitionTree = false;
        bool grid          = false;
        bool voxels        = false;
        int  voxelClipmap  = 1;
    } debug_;

    // ── show game preview ────────────────────────────────────────────────────
    // Per viewport, on by default. See the file header for what it buys. The Editor
    // Viewport's flag lives here; each Camera View carries its own (CameraView::
    // showGamePreview), so one window can sit on the shipping look while another shows
    // flat geometry.
    bool showGamePreview_ = true;
    bool stableExposure_  = true;  // see GraphicsSettings::MirrorTo
    // Captured in Draw(); RenderEditorView/RenderCameraViews run without an App& to hand.
    GraphicsSettings*        gfxSettings_    = nullptr;
    wi::graphics::ColorSpace gameColorSpace_ = wi::graphics::ColorSpace::SRGB;
    // The live game path, read (never written) for the parts of "same renderer" that are
    // not graphics settings: its layer mask and its custom post process list, which is
    // where st::ProjectorSystem and st::LaserSystem hang their passes.
    const wi::RenderPath3D* gamePath_ = nullptr;
    // Same object, typed, for the Game Viewport's resolution override. Held so SetEnabled
    // and Shutdown can clear the override without an App& to hand.
    ProjectorRenderPath* gameRenderPath_ = nullptr;
    // Bring one editor-owned path in line with the game's renderer, or (preview off) back
    // to RenderPath3D's own defaults — the path object is reused, not rebuilt, so the
    // second half has to be spelled out or MirrorTo's writes would simply stick.
    void ApplyViewportRenderer(wi::RenderPath3D& path, bool gamePreview) const;

    // ── game viewport resolution ─────────────────────────────────────────────
    // "Free Aspect" (fixed == false) is the shipping behaviour: the game path follows the
    // window canvas. Anything else forces st::ProjectorRenderPath::forcedResolution, and
    // the Game Viewport letterboxes the result into whatever shape the dock gives it.
    struct GameViewResolution {
        bool fixed  = false;
        int  width  = 1920;
        int  height = 1080;
        int  ratio  = 0;     // index into kAspectRatios; 0 = free (W and H independent)
    } gameRes_;
    // Push gameRes_ at the game path, or clear the override. Called every frame from Draw()
    // and once with `false` on the way out of editor mode.
    void ApplyGameViewResolution(bool enabled);

    // ── camera views ─────────────────────────────────────────────────────────
    //	unique_ptr elements because each view's RenderPath3D points at that view's own
    //	CameraComponent — the address has to survive the vector growing.
    std::vector<std::unique_ptr<CameraView>> cameraViews_;
    int nextCameraViewID_ = 1;

    // ── editor viewport ──────────────────────────────────────────────────────
    std::unique_ptr<wi::RenderPath3D> editorPath_;
    wi::scene::CameraComponent        editorCamera_;
    XMUINT2 editorViewSize_    = XMUINT2(0, 0); // panel size requested this frame, physical px
    float   editorViewDPI_     = 96.0f;
    bool    editorViewLive_    = false;         // panel was visible and non-degenerate this frame
    bool    editorCamActive_   = false;         // freecam may read input this frame
    // Cursor ownership for the free camera, reconciled once per frame in Draw() so the
    // look is always released — even if the panel stops being drawn mid-drag.
    bool    freeCamLookWanted_ = false;
    bool    freeCamLookActive_ = false;
    // Cursor confined to the window for the duration of a gizmo drag (visible cursor, so
    // the free camera's relative-mouse mode does not apply).
    bool    gizmoConfineActive_ = false;
    // Input ownership. The game only gets keyboard/mouse while the Game Viewport panel is
    // the focused one — click into it to play, click anywhere else and the editor has input.
    bool    gameViewFocused_    = false;
    bool    gameViewHovered_    = false;
    bool    inputCaptureActive_ = false;

    // Free camera state (position + euler), driven only from ImGui input so it keeps
    // working while ImGui owns the mouse — which it always does over a viewport panel.
    XMFLOAT3 camPos_ = XMFLOAT3(0, 3, -8);
    XMFLOAT3 camRot_ = XMFLOAT3(0.25f, 0, 0); // x=pitch, y=yaw, z=roll (radians)
    float camFov_    = 1.0472f;               // 60 degrees
    float camSpeed_  = 8.0f;
    float camBoost_  = 4.0f;
    float camSens_   = 0.004f;
    float camNear_   = 0.1f;
    float camFar_    = 5000.0f;
    bool  camInitialized_ = false; // snapped to the game camera on first open

    // ── scene save ───────────────────────────────────────────────────────────
    std::string saveDefaultExt_ = "stsd";  // what Save As appends to a bare name
    std::string scenePath_;        // last saved/chosen path; "" until the first Save As
    std::string lastSaveMessage_;  // shown in the toolbar
    bool        saveDialogOpen_ = false;
    // Written by the file dialog's own thread, drained on the main thread in Draw().
    std::mutex  pendingSaveMutex_;
    std::string pendingSavePath_;

    // ── scene import ─────────────────────────────────────────────────────────
    // Same contract as the save path: the dialog thread (and the SDL drop handler) only
    // park a path here; the load runs on the main thread, because it builds entities.
    std::string lastImportMessage_;
    bool        importDialogOpen_ = false;
    std::mutex  pendingImportMutex_;
    std::vector<std::string> pendingImportPaths_;

    // What the options panel is editing. Kept for the whole session, so importing forty
    // props in a row is one decision rather than forty.
    st::model::ImportOptions importOptions_;
    // Put EVERYTHING the import created under the one root, including the mesh, material
    // and animation-data entities that carry no transform and would otherwise sit at the
    // top of the Hierarchy as hundreds of loose rows.
    bool importGroupUnderRoot_ = true;
    // Off places the import at the world origin instead of in front of the free camera,
    // which is what you want when a model was authored around its own origin.
    bool importPlaceAtCamera_  = true;
    // Unticked, the panel stops appearing and later imports use the settings as they stand.
    bool importAskEveryTime_   = true;
    // The file the panel is currently asking about; empty when it is closed.
    std::string importOptionsPath_;
    bool        importOptionsRequested_ = false;  // open the popup on the next frame

    // ── undo / redo ──────────────────────────────────────────────────────────
    EditorHistory history_;
    // Entity a menu action (create, undo, redo) wants selected. Applied at the end of the
    // frame so the panels above are not handed a selection that changed underneath them.
    wi::ecs::Entity pendingSelection_ = wi::ecs::INVALID_ENTITY;
    // Entity handles are only meaningful within one loaded scene, so the history is dropped
    // whenever the scene manager switches. Tracked by name because that is what changes.
    std::string historySceneName_;
};

} // namespace st
