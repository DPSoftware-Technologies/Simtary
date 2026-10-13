// The "stChaseCamera" native component - the game camera follows the entity it is on.
//
//     NCI_0 = "stChaseCamera"   NCA_0_distance = 10   NCA_0_pitch = 17
//
// Two views, switched by the "CameraView" action of a sibling stInput (C / gamepad Y in the
// Vehicle map):
//   chase    - behind the entity's forward (+Z) axis, a little above it, eased so a bump does
//              not shake the view; drifts back behind the entity when nobody is looking around
//   internal - fixed to the entity at `interior` (a driver's head), turning with it in all
//              three axes - you feel the chassis roll and pitch
// Looking around: the "Look" action (right stick), or the right mouse button when ImGui does
// not have the mouse; the wheel zooms the chase view. Never below an authored terrain's
// ground. Runs on the REAL frame delta, so a paused world can still be looked around.

#include "stNativeComponent.h"
#include "scene/PlayControl.h"
#include "terrain/TerrainSystem.h"
#include "input/InputComponent.h"

#include "wiScene.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>

using namespace wi::scene;

namespace st {

struct ChaseCameraComponent : NativeComponent {
    ST_NATIVE_COMPONENT_MAIN_THREAD()

    float distance    = 10.0f;  // metres behind
    float pitch       = 17.0f;  // degrees above the horizon
    float lookHeight  = 1.3f;   // aim this far above the entity's origin
    float follow      = 8.0f;   // position easing, 1/s
    float turnFollow  = 3.0f;   // heading easing, 1/s
    bool  orbit       = true;   // right mouse / Look action orbits, wheel zooms
    bool  active      = true;
    int   view        = 0;      // 0 chase, 1 internal
    float interiorX   = -0.45f; // driver's head, entity space (left-hand drive)
    float interiorY   = 0.75f;
    float interiorZ   = -0.05f;
    float lookSpeed   = 2.5f;   // rad/s at full stick
    bool  invertLookY = false;
    float recenter    = 1.5f;   // s without look input before the chase view swings back

    void Start () override {
        Bind(distance, "distance");
        Bind(pitch, "pitch");
        Bind(lookHeight, "lookHeight");
        Bind(follow, "follow");
        Bind(turnFollow, "turnFollow");
        Bind(orbit, "orbit");
        Bind(active, "active");
        Bind(view, "view");
        Bind(interiorX, "interiorX");
        Bind(interiorY, "interiorY");
        Bind(interiorZ, "interiorZ");
        Bind(lookSpeed, "lookSpeed");
        Bind(invertLookY, "invertLookY");
        Bind(recenter, "recenter");
        pitch_ = XMConvertToRadians(pitch);
        lastPitch_ = pitch;
    }

    void Update (float /*sceneDt*/) override {
        if (!active || scene == nullptr) return;
        const TransformComponent* t = scene->transforms.GetComponent(entity);
        if (t == nullptr) return;
        const float dt = st::PlayControl::Get().RealDelta();
        if (pitch != lastPitch_) {  // inspector edit
            pitch_ = XMConvertToRadians(pitch);
            lastPitch_ = pitch;
        }

        ReadLookInput(dt);

        if (view == 1) {
            UpdateInternal(*t);
        } else {
            UpdateChase(*t, dt);
        }
    }

    void DescribeParams (wi::vector<NativeParam>& out) override {
        out.push_back(NativeParam::Enum("view", &view, "Chase\0Internal\0\0"));
        out.push_back(NativeParam::Float("distance", &distance, 3.0f, 60.0f));
        out.push_back(NativeParam::Float("pitch", &pitch, -10.0f, 75.0f));
        out.push_back(NativeParam::Float("lookHeight", &lookHeight, 0.0f, 5.0f));
        out.push_back(NativeParam::Float("follow", &follow, 0.5f, 30.0f));
        out.push_back(NativeParam::Float("turnFollow", &turnFollow, 0.1f, 20.0f));
        out.push_back(NativeParam::Float("interiorX", &interiorX, -2.0f, 2.0f, nullptr, "Internal view"));
        out.push_back(NativeParam::Float("interiorY", &interiorY, -1.0f, 3.0f, nullptr, "Internal view"));
        out.push_back(NativeParam::Float("interiorZ", &interiorZ, -3.0f, 3.0f, nullptr, "Internal view"));
        out.push_back(NativeParam::Float("lookSpeed", &lookSpeed, 0.2f, 8.0f, nullptr, "Look"));
        out.push_back(NativeParam::Bool("invertLookY", &invertLookY, nullptr, "Look"));
        out.push_back(NativeParam::Float("recenter", &recenter, 0.0f, 10.0f, "0 = never", "Look"));
        out.push_back(NativeParam::Bool("orbit", &orbit));
        out.push_back(NativeParam::Bool("active", &active));
    }

private:
    void ReadLookInput (float dt) {
        float dYaw = 0.0f, dPitch = 0.0f;

        // Gamepad / bound keys through the sibling stInput's map ("Look", "CameraView").
        if (input_ == nullptr) {
            input_ = GetComponent<st::InputComponent>();
            if (input_ != nullptr) {
                idLook_ = input_->Find("Look");
                idView_ = input_->Find("CameraView");
            }
        }
        if (input_ != nullptr) {
            if (idView_ >= 0 && input_->Pressed(idView_)) {
                view = view == 1 ? 0 : 1;
                yawOffset_ = 0.0f;
                lookPitch_ = 0.0f;
            }
            if (idLook_ >= 0) {
                const XMFLOAT2 look = input_->Vector2(idLook_);
                const float y = invertLookY ? -look.y : look.y;  // right stick reads down-positive
                dYaw += look.x * lookSpeed * dt;
                dPitch += y * lookSpeed * dt;
            }
        }

        const ImGuiIO& io = ImGui::GetIO();
        if (orbit && !io.WantCaptureMouse) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                dYaw += io.MouseDelta.x * 0.006f;
                dPitch += io.MouseDelta.y * 0.004f;
            }
            if (view == 0) distance = std::clamp(distance - io.MouseWheel, 3.0f, 60.0f);
        }

        const bool looking = std::fabs(dYaw) > 1e-5f || std::fabs(dPitch) > 1e-5f;
        idle_ = looking ? 0.0f : idle_ + dt;
        if (view == 1) {
            lookYaw_ = std::clamp(lookYaw_ + dYaw, -2.6f, 2.6f);      // about +-150 degrees
            lookPitch_ = std::clamp(lookPitch_ - dPitch, -1.2f, 1.0f);
        } else {
            yawOffset_ += dYaw;
            pitch_ = std::clamp(pitch_ + dPitch, -0.2f, 1.3f);
        }
    }

    void UpdateChase (const TransformComponent& t, float dt) {
        // Nobody looking around: swing back behind the vehicle, the way a racing game does.
        if (recenter > 0.0f && idle_ > recenter) {
            while (yawOffset_ > XM_PI) yawOffset_ -= XM_2PI;
            while (yawOffset_ < -XM_PI) yawOffset_ += XM_2PI;
            yawOffset_ *= std::max(0.0f, 1.0f - dt * 2.0f);
        }

        const XMFLOAT4 rot = t.GetRotation();
        XMFLOAT3 fwd;
        XMStoreFloat3(&fwd, XMVector3Rotate(XMVectorSet(0, 0, 1, 0), XMLoadFloat4(&rot)));
        const float heading = std::atan2(fwd.x, fwd.z);
        if (!init_) heading_ = heading;
        float diff = heading - heading_;
        while (diff > XM_PI) diff -= XM_2PI;
        while (diff < -XM_PI) diff += XM_2PI;
        heading_ += diff * std::min(1.0f, dt * turnFollow);

        const XMFLOAT3 p = t.GetPosition();
        const XMFLOAT3 target(p.x, p.y + lookHeight, p.z);
        const float yaw = heading_ + yawOffset_;
        const XMFLOAT3 desired(target.x - std::sin(yaw) * std::cos(pitch_) * distance,
                               target.y + std::sin(pitch_) * distance,
                               target.z - std::cos(yaw) * std::cos(pitch_) * distance);
        const float k = init_ ? std::min(1.0f, dt * follow) : 1.0f;
        pos_.x += (desired.x - pos_.x) * k;
        pos_.y += (desired.y - pos_.y) * k;
        pos_.z += (desired.z - pos_.z) * k;
        st::terrain::GroundSample ground;
        if (st::terrain::TerrainSystem::Get().Sample(pos_.x, pos_.z, ground))
            pos_.y = std::max(pos_.y, ground.height + 1.0f);
        init_ = true;

        const XMFLOAT3 look(target.x - pos_.x, target.y - pos_.y, target.z - pos_.z);
        TransformComponent cam;
        cam.SetWorldPosition(pos_.x, pos_.y, pos_.z);
        cam.RotateRollPitchYaw(XMFLOAT3(-std::atan2(look.y, std::sqrt(look.x * look.x + look.z * look.z)),
                                        std::atan2(look.x, look.z), 0.0f));
        cam.UpdateTransform();
        Apply(cam);
    }

    void UpdateInternal (const TransformComponent& t) {
        // head = look rotation, then the seat offset, then the chassis itself
        const XMMATRIX head = XMMatrixRotationRollPitchYaw(lookPitch_, lookYaw_, 0.0f) *
                              XMMatrixTranslation(interiorX, interiorY, interiorZ) *
                              XMLoadFloat4x4(&t.world);
        XMVECTOR S, R, T;
        XMMatrixDecompose(&S, &R, &T, head);
        TransformComponent cam;
        XMStoreFloat4(&cam.rotation_local, R);
        XMStoreFloat3(&cam.translation_local, T);
        cam.SetDirty();
        cam.UpdateTransform();
        XMStoreFloat3(&pos_, T);  // so switching back to chase starts from here
        Apply(cam);
    }

    // Depth of field focuses at the camera's focal_length (1 m by default, which blurred the
    // vehicle 10 m away). Focus on what the view is about: the vehicle, or the windshield.
    void Apply (TransformComponent& cam) const {
        wi::scene::CameraComponent& camera = wi::scene::GetCamera();
        camera.focal_length = view == 1 ? 3.0f : distance;
        camera.TransformCamera(cam);
        camera.UpdateCamera();
    }

    st::InputComponent* input_ = nullptr;
    int idLook_ = -1, idView_ = -1;
    float yawOffset_ = 0.0f;
    float pitch_ = 0.3f;
    float lastPitch_ = 0.0f;
    float lookYaw_ = 0.0f, lookPitch_ = 0.0f;
    float idle_ = 0.0f;
    float heading_ = 0.0f;
    XMFLOAT3 pos_ = XMFLOAT3(0, 0, 0);
    bool init_ = false;
};

} // namespace st

using StChaseCameraComponent = st::ChaseCameraComponent;
ST_REGISTER_FRAMEWORK_COMPONENT_AS(StChaseCameraComponent, "stChaseCamera", "Scene")
