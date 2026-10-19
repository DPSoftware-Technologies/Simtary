// The "stMainCamera" native component - Unity's "MainCamera": the game view looks through
// the entity it is on.
//
//     NCI_0 = "stMainCamera"   NCA_0_priority = 0
//
// Put it on a camera the map already carries (a drone's onboard camera, a cockpit view, a
// fixed security camera) and that camera IS the game camera while the scene runs - saved
// with the map, no code, no second camera rig to keep in step with the model. The lens
// (fov, near/far, focal length, aperture) comes from the entity's CameraComponent when it
// has one; an entity without one still works, with the defaults below.
//
// The view follows the entity's WORLD MATRIX, re-orthonormalised: +Z is where it looks and
// +Y is up. Imported rigs often hang the camera under a non-uniformly scaled or mirrored
// node, which leaves a skewed matrix whose axes are neither unit length nor square to each
// other - Gram-Schmidt on forward then up turns that back into a camera, where a plain
// decompose would tilt or roll the view.
//
// Several in one scene: the enabled one with the highest `priority` drives the camera (ties
// go to the lower entity id), so switching cameras is enabling one or raising its priority.
// It runs in LateUpdate, after physics and the hierarchy, so a camera riding a simulated
// body sees this frame's pose rather than the previous step's. Do not combine it with an
// stChaseCamera that is active - both write the same game camera.
//
// Like stChaseCamera it also carries the audio LISTENER: spatial sound is rendered for a
// collector, and without one nothing 3D is heard from anywhere.

#include "stNativeComponent.h"

#include "stAudioEngine.h"

#include "wiScene.h"
#include "imgui.h"

#include <cmath>

using namespace wi::scene;

namespace st {

struct MainCameraComponent : NativeComponent {
    ST_NATIVE_COMPONENT_MAIN_THREAD()

    int   priority = 0;
    bool  useLens  = true;       // take fov / near / far from the entity's CameraComponent
    float fovDeg   = 60.0f;      // used when there is no CameraComponent (or useLens is off)
    float zNear    = 0.05f;
    float zFar     = 5000.0f;

    void Start () override {
        Bind(priority, "priority");
        Bind(useLens, "useLens");
        Bind(fovDeg, "fov");
        Bind(zNear, "near");
        Bind(zFar, "far");
    }

    void LateUpdate (float /*sceneDt*/) override {
        driving_ = false;
        if (scene == nullptr || !IsWinner()) return;
        const TransformComponent* t = scene->transforms.GetComponent(entity);
        if (t == nullptr) return;

        const XMMATRIX W = XMLoadFloat4x4(&t->world);
        XMVECTOR fwd = XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), W);
        XMVECTOR up = XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), W);
        if (XMVectorGetX(XMVector3LengthSq(fwd)) < 1e-12f) return;   // scaled to nothing
        fwd = XMVector3Normalize(fwd);
        up = XMVectorSubtract(up, XMVectorScale(fwd, XMVectorGetX(XMVector3Dot(up, fwd))));
        if (XMVectorGetX(XMVector3LengthSq(up)) < 1e-12f) {
            // Up collapsed onto forward: pick any perpendicular rather than drop the frame.
            up = XMVector3Cross(fwd, XMVectorSet(1, 0, 0, 0));
            if (XMVectorGetX(XMVector3LengthSq(up)) < 1e-12f) up = XMVector3Cross(fwd, XMVectorSet(0, 0, 1, 0));
        }
        up = XMVector3Normalize(up);
        const XMVECTOR right = XMVector3Cross(up, fwd);   // left-handed: up x forward = right

        XMMATRIX basis = XMMatrixIdentity();
        basis.r[0] = right;
        basis.r[1] = up;
        basis.r[2] = fwd;
        TransformComponent cam;
        XMStoreFloat4(&cam.rotation_local, XMQuaternionNormalize(XMQuaternionRotationMatrix(basis)));
        const XMFLOAT3 p = t->GetPosition();
        cam.SetWorldPosition(p.x, p.y, p.z);
        cam.SetDirty();
        cam.UpdateTransform();

        CameraComponent& camera = wi::scene::GetCamera();
        const CameraComponent* lens = useLens ? scene->cameras.GetComponent(entity) : nullptr;
        if (lens != nullptr) {
            camera.fov = lens->fov;
            camera.zNearP = lens->zNearP;
            camera.zFarP = lens->zFarP;
            camera.focal_length = lens->focal_length;
            camera.aperture_size = lens->aperture_size;
        } else {
            camera.fov = XMConvertToRadians(fovDeg);
            camera.zNearP = zNear;
            camera.zFarP = zFar;
        }
        camera.TransformCamera(cam);
        camera.UpdateCamera();
        driving_ = true;

        UpdateListener(camera);
    }

    void DescribeParams (wi::vector<NativeParam>& out) override {
        out.push_back(NativeParam::Int("priority", &priority, -10, 10, "Highest enabled stMainCamera wins"));
        out.push_back(NativeParam::Bool("useLens", &useLens, "fov / near / far from this entity's Camera component"));
        out.push_back(NativeParam::Float("fov", &fovDeg, 10.0f, 150.0f, "Degrees, without a Camera component", "Lens"));
        out.push_back(NativeParam::Float("near", &zNear, 0.01f, 10.0f, nullptr, "Lens"));
        out.push_back(NativeParam::Float("far", &zFar, 10.0f, 50000.0f, nullptr, "Lens"));
    }

    void DrawDebug () override {
        ImGui::Text("%s", driving_ ? "driving the game camera" : "idle - another stMainCamera wins");
        if (useLens && scene != nullptr && !scene->cameras.Contains(entity)) {
            ImGui::TextDisabled("no Camera component here - using the fov below");
        }
    }

    void Destroy () override {
        if (listener_ != nullptr) st::audio::AudioEngine::Get().Destroy(listener_);
        listener_ = nullptr;
    }

private:
    // The enabled stMainCamera with the highest priority, ties to the lower entity id.
    bool IsWinner () const {
        for (const auto& kv : scene->nativeComponents.instances) {
            for (const auto& inst : kv.second) {
                if (inst.name != "stMainCamera" || !inst.component || inst.component.get() == this) continue;
                if (!inst.component->IsEnabled()) continue;
                const auto* other = static_cast<const MainCameraComponent*>(inst.component.get());
                if (other->priority > priority) return false;
                if (other->priority == priority && kv.first < entity) return false;
            }
        }
        return true;
    }

    void UpdateListener (const CameraComponent& camera) {
        st::audio::AudioEngine& audio = st::audio::AudioEngine::Get();
        if (!audio.IsInitialized()) return;
        if (listener_ == nullptr) listener_ = audio.CreateCollector("main camera listener");
        if (listener_ != nullptr) {
            st::audio::SpatialTransform ears;
            ears.position = camera.Eye;
            ears.forward = camera.At;
            ears.up = camera.Up;
            listener_->SetTransform(ears);
        }
    }

    st::audio::CollectorRef listener_;
    bool driving_ = false;
};

} // namespace st

using StMainCameraComponent = st::MainCameraComponent;
ST_REGISTER_FRAMEWORK_COMPONENT_AS(StMainCameraComponent, "stMainCamera", "Scene")
