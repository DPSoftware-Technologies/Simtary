#include "vehicle/VehicleComponent.h"
#include "vehicle/VehiclePreset.h"

#include "input/InputComponent.h"
#include "terrain/TerrainSystem.h"

#include "wiScene.h"
#include "wiBacklog.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace wi::scene;
using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;

namespace st {

namespace {

float MoveTowards (float current, float target, float maxDelta) {
    if (current < target) return std::min(current + maxDelta, target);
    return std::max(current - maxDelta, target);
}

} // namespace

void VehicleComponent::DescribeRequired (wi::vector<RequiredComponent>& out) {
    out.push_back(RequiredComponent::For<RigidBodyPhysicsComponent>());
    out.push_back(RequiredComponent::Native("stInput"));
}

void VehicleComponent::Start () {
    Bind(preset, "preset");
    Bind(wheelFL, "wheelFL");
    Bind(wheelFR, "wheelFR");
    Bind(wheelRL, "wheelRL");
    Bind(wheelRR, "wheelRR");
    Bind(steerSpeed, "steerSpeed");
    Bind(steerReturn, "steerReturn");
    Bind(brakeToReverse, "brakeToReverse");
    Bind(applyPreset, "applyPreset");

    RigidBodyPhysicsComponent* rb = GetComponent<RigidBodyPhysicsComponent>();
    if (rb == nullptr) {
        wi::backlog::post("stVehicle: entity has no rigid body", wi::backlog::LogLevel::Error);
        return;
    }
    if (applyPreset || !rb->IsVehicle()) {
        if (!vehicle::ApplyPreset(preset, *rb)) {
            wi::backlog::post("stVehicle: unknown preset '" + preset + "'", wi::backlog::LogLevel::Error);
            return;
        }
    }
    BindWheels(*rb);
    ResolveInput();
}

std::string VehicleComponent::FitVisual (Entity model) {
    if (scene == nullptr) return "no scene";
    const std::string* names[4] = { &wheelFL, &wheelFR, &wheelRL, &wheelRR };

    // The model: the one given, or the first direct child that carries the wheel nodes.
    if (model == INVALID_ENTITY) {
        for (size_t i = 0; i < scene->hierarchy.GetCount() && model == INVALID_ENTITY; ++i) {
            if (scene->hierarchy[i].parentID != entity) continue;
            const Entity child = scene->hierarchy.GetEntity(i);
            if (scene->Entity_FindByName(*names[0], child) != INVALID_ENTITY) model = child;
        }
    }
    if (model == INVALID_ENTITY) return "no child model with a '" + wheelFL + "' node";

    const vehicle::PresetReference ref = vehicle::Reference(preset);
    RigidBodyPhysicsComponent* rb = GetComponent<RigidBodyPhysicsComponent>();
    const float tyreRadius = (rb != nullptr && !rb->vehicle.wheels.empty()) ? rb->vehicle.wheels[0].radius : 0.47f;
    if (ref.wheelbaseM <= 0.0f) return "preset has no wheelbase";

    // Measure in the model's OWN space: detach it first (keeping its world transform), so
    // the numbers do not depend on where the vehicle happens to stand.
    if (scene->hierarchy.Contains(model)) scene->Component_Detach(model);
    TransformComponent* root = scene->transforms.GetComponent(model);
    if (root == nullptr) return "model has no transform";
    root->ClearTransform();
    root->UpdateTransform();
    scene->RefreshHierarchyTopdownFromParent(model);

    XMFLOAT3 w[4];
    for (int i = 0; i < 4; ++i) {
        const Entity e = scene->Entity_FindByName(*names[i], model);
        const TransformComponent* t = e != INVALID_ENTITY ? scene->transforms.GetComponent(e) : nullptr;
        if (t == nullptr) return "model has no '" + *names[i] + "' node";
        w[i] = t->GetPosition();
        // The MESH centre, not the pivot: exported wheels are often pivoted off-centre.
        if (const ObjectComponent* o = scene->objects.GetComponent(e)) {
            if (const MeshComponent* m = scene->meshes.GetComponent(o->meshID)) {
                const XMFLOAT3 local = m->aabb.getCenter();
                XMFLOAT3 c;
                XMStoreFloat3(&c, XMVector3Transform(XMLoadFloat3(&local), XMLoadFloat4x4(&t->world)));
                w[i] = c;
            }
        }
    }
    const XMFLOAT3 front((w[0].x + w[1].x) * 0.5f, (w[0].y + w[1].y) * 0.5f, (w[0].z + w[1].z) * 0.5f);
    const XMFLOAT3 rear((w[2].x + w[3].x) * 0.5f, (w[2].y + w[3].y) * 0.5f, (w[2].z + w[3].z) * 0.5f);
    const XMFLOAT3 fwd(front.x - rear.x, front.y - rear.y, front.z - rear.z);
    const float wheelbase = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
    if (wheelbase < 1e-3f) return "front and rear wheels coincide";
    const float scale = ref.wheelbaseM / wheelbase;
    const float yaw = std::atan2(fwd.x, fwd.z);
    const XMFLOAT3 centre((front.x + rear.x) * 0.5f, (front.y + rear.y) * 0.5f, (front.z + rear.z) * 0.5f);
    const float trackModel = std::sqrt((w[1].x - w[0].x) * (w[1].x - w[0].x) + (w[1].z - w[0].z) * (w[1].z - w[0].z));

    // fit = scale, turn the front onto +Z, put the wheel centre on the simulated one -
    // applied on top of whatever the importer put on the root (its axis conversion).
    const XMVECTOR q = XMQuaternionRotationRollPitchYaw(0.0f, -yaw, 0.0f);
    XMFLOAT3 c;
    XMStoreFloat3(&c, XMVector3Rotate(XMVectorScale(XMLoadFloat3(&centre), scale), q));
    const float wheelCentreY = tyreRadius - ref.boxCentreHeightM; // chassis space
    const XMMATRIX fit = XMMatrixScaling(scale, scale, scale) * XMMatrixRotationQuaternion(q) *
                         XMMatrixTranslation(-c.x, wheelCentreY - c.y, -c.z);
    XMVECTOR S, R, T;
    XMMatrixDecompose(&S, &R, &T, root->GetLocalMatrix() * fit);
    root->ClearTransform();
    XMStoreFloat3(&root->scale_local, S);
    XMStoreFloat4(&root->rotation_local, R);
    XMStoreFloat3(&root->translation_local, T);
    root->SetDirty();
    root->UpdateTransform();
    scene->Component_Attach(model, entity, true);

    if (rb != nullptr) {
        BindWheels(*rb);
        rb->physicsobject = {}; // rebuild, so the wheels' bind pose is captured from the fitted model
    }
    char buf[200];
    std::snprintf(buf, sizeof(buf), "fitted: wheelbase %.2f -> %.2f m (x%.3f), model track %.2f m vs simulated %.2f m",
                  wheelbase, ref.wheelbaseM, scale, trackModel * scale, ref.trackM);
    return buf;
}

Entity VehicleComponent::FindDescendant (const std::string& name) const {
    if (name.empty() || scene == nullptr) return INVALID_ENTITY;
    for (size_t i = 0; i < scene->names.GetCount(); ++i) {
        if (scene->names[i].name != name) continue;
        Entity e = scene->names.GetEntity(i);
        // Walk up: the wheel has to belong to THIS vehicle, not to the next one along.
        for (Entity p = e; p != INVALID_ENTITY;) {
            if (p == entity) return e;
            const HierarchyComponent* h = scene->hierarchy.GetComponent(p);
            p = h ? h->parentID : INVALID_ENTITY;
        }
    }
    return INVALID_ENTITY;
}

void VehicleComponent::BindWheels (RigidBodyPhysicsComponent& rb) {
    const std::string* names[4] = { &wheelFL, &wheelFR, &wheelRL, &wheelRR };
    int bound = 0;
    for (size_t i = 0; i < rb.vehicle.wheels.size() && i < 4; ++i) {
        rb.vehicle.wheels[i].entity = FindDescendant(*names[i]);
        if (rb.vehicle.wheels[i].entity != INVALID_ENTITY) ++bound;
    }
    if (bound < 4)
        wi::backlog::post("stVehicle: " + std::to_string(bound) + " of 4 visual wheels found by name - the "
                          "rest simulate but are not drawn turning", wi::backlog::LogLevel::Warning);
}

void VehicleComponent::ResolveInput () {
    if (input_ != nullptr) return;
    input_ = GetComponent<st::InputComponent>();
    if (input_ == nullptr) return;
    idThrottle_  = input_->Find("Throttle");
    idBrake_     = input_->Find("Brake");
    idSteer_     = input_->Find("Steer");
    idHandbrake_ = input_->Find("Handbrake");
    idReset_     = input_->Find("ResetVehicle");
}

void VehicleComponent::Update (float dt) {
    RigidBodyPhysicsComponent* rb = GetComponent<RigidBodyPhysicsComponent>();
    if (rb == nullptr) return;

    if (rb->physicsobject == nullptr) return; // body not built yet
    if (!grounded_ && !HoldUntilGround(*rb, dt)) return;
    CatchFallThrough(*rb);

    hasTelemetry = wi::physics::GetVehicleTelemetry(*rb, telemetry);

    ResolveInput();
    float wantThrottle = 0.0f, wantBrake = 0.0f, wantSteer = 0.0f;
    bool  wantHandbrake = false;
    if (remoteInput) {
        wantThrottle  = std::clamp(remote.throttle, 0.0f, 1.0f);
        wantBrake     = std::clamp(remote.brake, 0.0f, 1.0f);
        wantSteer     = std::clamp(remote.steer, -1.0f, 1.0f);
        wantHandbrake = remote.handbrake;
    } else if (input_ != nullptr) {
        wantThrottle  = std::clamp(input_->Value(idThrottle_), 0.0f, 1.0f);
        wantBrake     = std::clamp(input_->Value(idBrake_), 0.0f, 1.0f);
        wantSteer     = std::clamp(input_->Value(idSteer_), -1.0f, 1.0f);
        wantHandbrake = input_->Down(idHandbrake_);
        if (input_->Pressed(idReset_)) ResetUpright();
    }

    wanted.throttle = wantThrottle;
    wanted.brake = wantBrake;
    wanted.steer = wantSteer;
    wanted.handbrake = wantHandbrake;

    // A real wheel cannot go from straight to full lock in one frame, and a keyboard
    // otherwise asks it to. Rate-limit towards the input, faster back to centre.
    const float rate = (std::fabs(wantSteer) < std::fabs(steer) || wantSteer * steer < 0.0f) ? steerReturn : steerSpeed;
    steer = MoveTowards(steer, wantSteer, rate * dt);

    // An automatic is driven with two pedals: brake to a stop, keep holding it and the
    // selector goes to reverse, where the brake pedal is now the accelerator. Throttle
    // takes it back to drive.
    const float speed = hasTelemetry ? telemetry.forward_speed : 0.0f;
    if (brakeToReverse) {
        if (!reversing_ && wantBrake > 0.1f && wantThrottle < 0.05f && speed < 0.5f) reversing_ = true;
        if (reversing_ && (wantThrottle > 0.05f || speed > 1.0f)) reversing_ = false;
    }
    if (reversing_) {
        throttle = -wantBrake;
        brake = wantThrottle;
    } else {
        throttle = wantThrottle;
        brake = wantBrake;
    }
    handbrake = wantHandbrake ? 1.0f : 0.0f;

    wi::physics::DriveVehicle(*rb, throttle, steer, brake, handbrake);
}

bool VehicleComponent::HoldUntilGround (RigidBodyPhysicsComponent& rb, float dt) {
    if (!holdInit_) {
        holdPos_ = wi::physics::GetPosition(rb);
        holdRot_ = wi::physics::GetRotation(rb);
        holdInit_ = true;
    }
    // From just under the chassis, so the ray cannot start inside the vehicle's own body.
    const float below = std::max(rb.box.halfextents.y, 0.3f) + 0.1f;
    const wi::primitive::Ray down(XMFLOAT3(holdPos_.x, holdPos_.y - below, holdPos_.z), XMFLOAT3(0, -1, 0));
    const wi::physics::RayIntersectionResult hit = wi::physics::Intersects(*scene, down);
    holdTime_ += dt;
    if (hit.IsValid() || holdTime_ > 20.0f) {
        if (!hit.IsValid())
            wi::backlog::post("stVehicle: no ground under the vehicle after 20 s - releasing it anyway",
                              wi::backlog::LogLevel::Warning);
        grounded_ = true;
        return true;
    }
    wi::physics::SetPositionAndRotation(rb, holdPos_, holdRot_);
    wi::physics::SetLinearVelocity(rb, XMFLOAT3(0, 0, 0));
    wi::physics::SetAngularVelocity(rb, XMFLOAT3(0, 0, 0));
    return false;
}

void VehicleComponent::CatchFallThrough (RigidBodyPhysicsComponent& rb) {
    const XMFLOAT3 pos = wi::physics::GetPosition(rb);
    st::terrain::GroundSample ground;
    if (!st::terrain::TerrainSystem::Get().Sample(pos.x, pos.z, ground)) return;
    if (pos.y > ground.height - 5.0f) return;
    wi::backlog::post("stVehicle: fell through the terrain - put back on top", wi::backlog::LogLevel::Warning);
    const XMFLOAT4 rot = wi::physics::GetRotation(rb);
    XMFLOAT3 fwd;
    XMStoreFloat3(&fwd, XMVector3Rotate(XMVectorSet(0, 0, 1, 0), XMLoadFloat4(&rot)));
    XMFLOAT4 upright;
    XMStoreFloat4(&upright, XMQuaternionRotationRollPitchYaw(0.0f, std::atan2(fwd.x, fwd.z), 0.0f));
    wi::physics::SetPositionAndRotation(rb, XMFLOAT3(pos.x, ground.height + 2.0f, pos.z), upright);
    wi::physics::SetLinearVelocity(rb, XMFLOAT3(0, 0, 0));
    wi::physics::SetAngularVelocity(rb, XMFLOAT3(0, 0, 0));
}

void VehicleComponent::ResetUpright () {
    ++resetCount;
    RigidBodyPhysicsComponent* rb = GetComponent<RigidBodyPhysicsComponent>();
    if (rb == nullptr) return;
    XMFLOAT3 pos = wi::physics::GetPosition(*rb);
    const XMFLOAT4 rot = wi::physics::GetRotation(*rb);
    // Keep the heading, drop the roll and pitch.
    XMFLOAT3 fwd;
    XMStoreFloat3(&fwd, XMVector3Rotate(XMVectorSet(0, 0, 1, 0), XMLoadFloat4(&rot)));
    const float yaw = std::atan2(fwd.x, fwd.z);
    XMFLOAT4 upright;
    XMStoreFloat4(&upright, XMQuaternionRotationRollPitchYaw(0.0f, yaw, 0.0f));
    pos.y += 1.5f;
    wi::physics::SetPositionAndRotation(*rb, pos, upright);
    wi::physics::SetLinearVelocity(*rb, XMFLOAT3(0, 0, 0));
    wi::physics::SetAngularVelocity(*rb, XMFLOAT3(0, 0, 0));
}

float VehicleComponent::WheelLoad (size_t wheel) const {
    if (!hasTelemetry || wheel >= telemetry.wheels.size()) return 0.0f;
    return telemetry.wheels[wheel].suspension_impulse * wi::physics::GetFrameRate();
}

void VehicleComponent::DrawDebug () {
    ImGui::Text("Preset: %s", preset.c_str());
    if (ImGui::Checkbox("Re-apply preset on start", &applyPreset)) SaveBoundParams();
    if (ImGui::SmallButton("Fit child model to the simulated wheels")) {
        fitReport_ = FitVisual();
        wi::backlog::post("stVehicle: " + fitReport_);
    }
    if (!fitReport_.empty()) ImGui::TextDisabled("%s", fitReport_.c_str());
    if (!hasTelemetry) {
        ImGui::TextDisabled("no vehicle body yet");
        return;
    }
    ImGui::Text("%.1f km/h   %.0f rpm   gear %d   clutch %.2f",
                telemetry.forward_speed * 3.6f, telemetry.engine_rpm, telemetry.gear,
                telemetry.clutch_friction);
    ImGui::Text("throttle %.2f  brake %.2f  steer %+.2f  handbrake %.0f%s",
                throttle, brake, steer, handbrake, reversing_ ? "  [R]" : "");
    if (ImGui::BeginTable("##wheels", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("wheel");
        ImGui::TableSetupColumn("load N");
        ImGui::TableSetupColumn("susp m");
        ImGui::TableSetupColumn("slip ratio");
        ImGui::TableSetupColumn("slip deg");
        ImGui::TableSetupColumn("mu long");
        ImGui::TableSetupColumn("mu lat");
        ImGui::TableHeadersRow();
        static const char* kNames[] = { "FL", "FR", "RL", "RR" };
        for (size_t i = 0; i < telemetry.wheels.size(); ++i) {
            const auto& w = telemetry.wheels[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s%s", i < 4 ? kNames[i] : "?", w.contact ? "" : " (air)");
            ImGui::TableNextColumn(); ImGui::Text("%.0f", WheelLoad(i));
            ImGui::TableNextColumn(); ImGui::Text("%.3f", w.suspension_length);
            ImGui::TableNextColumn(); ImGui::Text("%+.3f", w.longitudinal_slip);
            ImGui::TableNextColumn(); ImGui::Text("%+.1f", w.lateral_slip * 57.2958f);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", w.longitudinal_friction);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", w.lateral_friction);
        }
        ImGui::EndTable();
    }
}

} // namespace st

using StVehicleComponent = st::VehicleComponent;
ST_REGISTER_FRAMEWORK_COMPONENT_AS(StVehicleComponent, "stVehicle", "Vehicle")
