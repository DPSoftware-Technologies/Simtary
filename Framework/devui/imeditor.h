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
//	                    what a player sees. Kept at the window canvas size and letterboxed
//	                    into the panel, so the game's aspect ratio never changes just
//	                    because the editor layout moved.
//	  Editor Viewport — a SECOND RenderPath3D owned by this class, driven by a free camera
//	                    that lives only in the editor. It is resized to fit its panel
//	                    exactly (no letterbox), so screen-space picking and the gizmo map
//	                    1:1. It is created the first time editor mode is entered and freed
//	                    when it leaves, so a normal (non-editor) run pays nothing for it.
//
//	The scene is shared, so an edit made in either viewport shows up in both.
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

#include <memory>
#include <vector>
#include <mutex>
#include <string>

namespace wi::scene { struct Scene; }
class GraphicsSettings; // Framework/devui/imgraphicsettings.h, global namespace

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

    void SaveScene(wi::scene::Scene& scene, const std::string& path);
    void RequestSaveAs();
    void FlushPendingSave(wi::scene::Scene& scene);

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
    // ── renderer preview ─────────────────────────────────────────────────────
    // The editor's extra paths are bare RenderPath3D instances: engine defaults, none of
    // the game's AO / bloom / tonemap / exposure / colour grading. That is why they came
    // out looking nothing like the Game Viewport. When this is on, every editor path is
    // brought in line with the live graphics settings each frame (GraphicsSettings::MirrorTo)
    // and given the game path's colorspace, which is the other half of "wrong colour": the
    // engine only assigns colorspace to the ACTIVE path, so on an HDR swapchain the extra
    // paths were tonemapping for SRGB while the game tonemapped for HDR10.
    bool matchGameRenderer_ = true;
    bool stableExposure_    = true;  // see GraphicsSettings::MirrorTo
    // Captured in Draw(); RenderEditorView/RenderCameraViews run without an App& to hand.
    GraphicsSettings*        gfxSettings_    = nullptr;
    wi::graphics::ColorSpace gameColorSpace_ = wi::graphics::ColorSpace::SRGB;
    // Bring one editor-owned path in line with the game's renderer.
    void MatchGameRenderer(wi::RenderPath3D& path) const;

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
    std::string scenePath_;        // last saved/chosen path; "" until the first Save As
    std::string lastSaveMessage_;  // shown in the toolbar
    bool        saveDialogOpen_ = false;
    // Written by the file dialog's own thread, drained on the main thread in Draw().
    std::mutex  pendingSaveMutex_;
    std::string pendingSavePath_;

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
