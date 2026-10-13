// The "stChaseCamera" native component - the game camera follows the entity it is on.
//
//     NCI_0 = "stChaseCamera"   NCA_0_distance = 10   NCA_0_pitch = 17
//
// Sits behind the entity's forward (+Z) axis, a little above it, and eases after it so a
// bump does not shake the view. Right mouse button orbits, the wheel zooms - when the game
// has the mouse, not ImGui. It never goes below an authored terrain's ground. Runs on the
// REAL frame delta, so a paused world can still be looked around.

#include "stNativeComponent.h"
#include "scene/PlayControl.h"
#include "terrain/TerrainSystem.h"

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
    bool  orbit       = true;   // right mouse orbits, wheel zooms
    bool  active      = true;

    void Start () override {
        Bind(distance, "distance");
        Bind(pitch, "pitch");
        Bind(lookHeight, "lookHeight");
        Bind(follow, "follow");
        Bind(turnFollow, "turnFollow");
        Bind(orbit, "orbit");
        Bind(active, "active");
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

        const ImGuiIO& io = ImGui::GetIO();
        if (orbit && !io.WantCaptureMouse) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                yawOffset_ += io.MouseDelta.x * 0.006f;
                pitch_ = std::clamp(pitch_ + io.MouseDelta.y * 0.004f, -0.2f, 1.3f);
            }
            distance = std::clamp(distance - io.MouseWheel, 3.0f, 60.0f);
        }

        const XMFLOAT4 rot = t->GetRotation();
        XMFLOAT3 fwd;
        XMStoreFloat3(&fwd, XMVector3Rotate(XMVectorSet(0, 0, 1, 0), XMLoadFloat4(&rot)));
        const float heading = std::atan2(fwd.x, fwd.z);
        if (!init_) heading_ = heading;
        float diff = heading - heading_;
        while (diff > XM_PI) diff -= XM_2PI;
        while (diff < -XM_PI) diff += XM_2PI;
        heading_ += diff * std::min(1.0f, dt * turnFollow);

        const XMFLOAT3 p = t->GetPosition();
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
        wi::scene::GetCamera().TransformCamera(cam);
        wi::scene::GetCamera().UpdateCamera();
    }

    void DescribeParams (wi::vector<NativeParam>& out) override {
        out.push_back(NativeParam::Float("distance", &distance, 3.0f, 60.0f));
        out.push_back(NativeParam::Float("pitch", &pitch, -10.0f, 75.0f));
        out.push_back(NativeParam::Float("lookHeight", &lookHeight, 0.0f, 5.0f));
        out.push_back(NativeParam::Float("follow", &follow, 0.5f, 30.0f));
        out.push_back(NativeParam::Float("turnFollow", &turnFollow, 0.1f, 20.0f));
        out.push_back(NativeParam::Bool("orbit", &orbit));
        out.push_back(NativeParam::Bool("active", &active));
    }

private:
    float yawOffset_ = 0.0f;
    float pitch_ = 0.3f;
    float lastPitch_ = 0.0f;
    float heading_ = 0.0f;
    XMFLOAT3 pos_ = XMFLOAT3(0, 0, 0);
    bool init_ = false;
};

} // namespace st

using StChaseCameraComponent = st::ChaseCameraComponent;
ST_REGISTER_FRAMEWORK_COMPONENT_AS(StChaseCameraComponent, "stChaseCamera", "Scene")
