#pragma once
// TemplatePreview - a .sttd opened on its own, Unity "prefab mode" style.
//
// Double-clicking a template in the Resource Explorer opens it here instead of dropping it
// into the world: a window with its own wi::scene::Scene, its own render path and an orbit
// camera, plus a Hierarchy and a Properties panel scoped to the template's object. Values
// can be changed and things added (Create child, Add Component, another template dropped on
// the view); Save writes the .sttd back.
//
// Nothing RUNS in here. The scene is stepped by a microsecond with physics off and every
// script, animation, sound and video paused for the step (see StepFrozen for why it cannot
// simply be dt = 0), and its native components are built for the inspector but never
// started (NativeComponentManager::lifecycleEnabled = false) - a vehicle template does not
// drive off and a scripted one does not fire its onStart while you are editing it.
//
// The preview adds a sun, a sky and a ground grid of its own. They are not under the
// template's root, and a save only takes the root's subtree, so they never reach the file.

#include "imeditorhistory.h"
#include "wiECS.h"
#include "wiRenderPath3D.h"
#include "wiScene_Components.h"
#include "imgui.h"

#include <functional>
#include <memory>
#include <string>

namespace wi::scene { struct Scene; }

namespace st {

class TemplatePreview {
public:
    // The editor's template save (resource export, sidecar package, dev-build mirror).
    using SaveFn  = std::function<bool(wi::scene::Scene& scene, wi::ecs::Entity root, const std::string& path)>;
    // Place a copy of a template file in the main scene, in front of the editor camera.
    using PlaceFn = std::function<void(const std::string& path)>;

    TemplatePreview (int id, SaveFn save, PlaceFn place);
    ~TemplatePreview ();

    bool Open (const std::string& path, std::string* error = nullptr);

    const std::string& FilePath () const { return path_; }
    bool IsOpen () const { return open_; }
    // Keyboard focus was in this window last frame - the editor's own Ctrl+S / Ctrl+Z
    // stand aside so they do not act on the main scene.
    bool IsFocused () const { return focused_; }
    bool Owns (const wi::scene::Scene* scene) const { return scene != nullptr && scene == scene_.get(); }
    bool IsDirty () const;

    // ImGui half; between NewFrame and Render.
    void Draw ();
    // GPU half; from the editor's render step, after Draw.
    void Render (float dt);

    // The view's last rendered frame (nullptr before the first one). For tools.
    const wi::graphics::Texture* LastFrame () const { return path3D_ ? path3D_->GetLastPostprocessRT() : nullptr; }

private:
    void Save ();
    void Revert ();
    void Frame ();
    void DrawView ();
    void DrawCloseConfirm ();
    void AddHelpers ();
    void StepFrozen ();
    void DropTemplate (const std::string& path, const XMFLOAT3& point);
    bool PickPoint (float u, float v, XMFLOAT3& point, wi::ecs::Entity* hit) const;

    int     id_ = 0;
    SaveFn  save_;
    PlaceFn place_;

    std::string path_;
    std::string name_;
    bool        open_    = true;
    bool        focused_ = false;
    bool        closeAsked_ = false;
    std::string lastMessage_;

    std::unique_ptr<wi::scene::Scene> scene_;
    wi::ecs::Entity root_     = wi::ecs::INVALID_ENTITY;
    wi::ecs::Entity selected_ = wi::ecs::INVALID_ENTITY;

    EditorHistory history_;
    uint64_t      savedRevision_ = 0;
    bool          touched_ = false;  // a Properties widget was edited outside the history

    std::unique_ptr<wi::RenderPath3D> path3D_;
    wi::scene::CameraComponent        camera_;
    XMUINT2  viewSize_ = XMUINT2(0, 0);
    bool     live_     = false;      // the view was on screen this frame
    bool     needFrame_ = true;      // frame the object after the next scene update
    XMFLOAT3 target_   = XMFLOAT3(0, 1, 0);
    float    yaw_      = 0.6f;
    float    pitch_    = 0.35f;
    float    distance_ = 5.0f;
    float    fov_      = 0.9f;
};

} // namespace st
