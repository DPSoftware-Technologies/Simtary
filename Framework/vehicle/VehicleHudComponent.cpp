// The "stVehicleHud" native component - a test-driver readout for the stVehicle beside it.
//
//     NCI_2 = "stVehicleHud"
//
// Speed, gear and rpm, the preset's published figures next to what the simulation does
// (top speed, 0-48 and 0-80 km/h, braking 50-0), the vehicle's own telemetry table and what
// each tyre stands on. Game UI, drawn through GuiHooks, so it shows in play and in the Game
// Viewport alike; `visible` turns it off without detaching it.

#include "stNativeComponent.h"
#include "scene/GuiHooks.h"
#include "terrain/TerrainSystem.h"
#include "terrain/TerrainData.h"
#include "vehicle/VehicleComponent.h"
#include "vehicle/VehiclePreset.h"

#include "wiScene.h"
#include "wiPhysics.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace wi::scene;
using wi::ecs::Entity;

namespace st {

struct VehicleHudComponent : NativeComponent {
    ST_NATIVE_COMPONENT_MAIN_THREAD()

    bool visible = true;
    bool telemetryTable = true;

    void DescribeRequired (wi::vector<RequiredComponent>& out) override {
        out.push_back(RequiredComponent::Native("stVehicle"));
    }

    void Start () override {
        Bind(visible, "visible");
        Bind(telemetryTable, "telemetryTable");
        if (hook_ < 0) hook_ = st::GuiHooks::Get().Add([this] { Draw(); });
    }

    void Destroy () override {
        if (hook_ >= 0) st::GuiHooks::Get().Remove(hook_);
        hook_ = -1;
    }

    void Update (float dt) override {
        VehicleComponent* v = GetComponent<VehicleComponent>();
        if (v == nullptr || !v->hasTelemetry || scene == nullptr) return;
        const TransformComponent* t = scene->transforms.GetComponent(entity);
        if (t == nullptr) return;
        const float kmh = v->telemetry.forward_speed * 3.6f;
        topSpeedKmh_ = std::max(topSpeedKmh_, kmh);

        // Acceleration: the clock starts when a stopped vehicle starts to move under throttle.
        if (!running_ && std::fabs(kmh) < 0.5f && v->throttle > 0.5f) {
            running_ = true;
            runTime_ = 0.0f;
            t48_ = t80_ = -1.0f;
        }
        if (running_) {
            runTime_ += dt;
            if (t48_ < 0.0f && kmh >= 48.3f) t48_ = runTime_;   // 0-30 mph
            if (t80_ < 0.0f && kmh >= 80.5f) t80_ = runTime_;   // 0-50 mph
            if (v->throttle < 0.1f || runTime_ > 120.0f) running_ = false;
        }

        // Braking 50-0: starts the moment the brake goes on above 50 km/h.
        const XMFLOAT3 p = t->GetPosition();
        if (!braking_ && kmh >= 50.0f && v->brake > 0.8f && v->throttle < 0.05f) {
            braking_ = true;
            brakeStart_ = p;
        }
        if (braking_ && std::fabs(kmh) < 0.3f) {
            braking_ = false;
            const float dx = p.x - brakeStart_.x, dy = p.y - brakeStart_.y, dz = p.z - brakeStart_.z;
            brakeDist_ = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        if (braking_ && v->brake < 0.5f) braking_ = false; // released early: not a measurement
    }

    void DescribeParams (wi::vector<NativeParam>& out) override {
        out.push_back(NativeParam::Bool("visible", &visible));
        out.push_back(NativeParam::Bool("telemetryTable", &telemetryTable));
        out.push_back(NativeParam::Action("Reset measurements", [](NativeComponent& c) {
            static_cast<VehicleHudComponent&>(c).ResetMeasurements();
        }));
    }

    void ResetMeasurements () {
        topSpeedKmh_ = 0.0f;
        t48_ = t80_ = brakeDist_ = -1.0f;
        running_ = braking_ = false;
    }

private:
    void Draw () {
        if (!visible || scene == nullptr) return;
        VehicleComponent* v = GetComponent<VehicleComponent>();
        if (v == nullptr) return;

        ImGui::SetNextWindowPos(ImVec2(12, 40), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(470, 0), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Vehicle")) { ImGui::End(); return; }
        if (!v->hasTelemetry) {
            ImGui::TextDisabled("building the vehicle...");
            ImGui::End();
            return;
        }

        const st::vehicle::PresetReference ref = st::vehicle::Reference(v->preset);
        const float kmh = v->telemetry.forward_speed * 3.6f;
        ImGui::SetWindowFontScale(1.6f);
        ImGui::Text("%5.1f km/h   gear %d   %4.0f rpm", kmh, v->telemetry.gear, v->telemetry.engine_rpm);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("W/S or triggers, A/D or left stick, Space handbrake, R reset, RMB orbit, wheel zoom");
        bool showPhysics = wi::physics::IsDebugDrawEnabled();
        if (ImGui::Checkbox("Show physics (chassis box, wheels, contacts)", &showPhysics))
            wi::physics::SetDebugDrawEnabled(showPhysics);

        if (ImGui::CollapsingHeader("Measured vs real", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto row = [](const char* what, float sim, const char* unit, float real) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(what);
                ImGui::TableNextColumn();
                if (sim >= 0.0f) ImGui::Text("%.1f %s", sim, unit); else ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                if (real > 0.0f) ImGui::Text("%.1f %s", real, unit); else ImGui::TextDisabled("not published");
            };
            if (ImGui::BeginTable("##ref", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("");
                ImGui::TableSetupColumn("simulated");
                ImGui::TableSetupColumn("real");
                ImGui::TableHeadersRow();
                row("Top speed", topSpeedKmh_, "km/h", ref.topSpeedKmh);
                row("0-48 km/h (0-30 mph)", t48_, "s", 0.0f);
                row("0-80 km/h (0-50 mph)", t80_, "s", 0.0f);
                row("Braking 50-0 km/h", brakeDist_, "m", 0.0f);
                ImGui::EndTable();
            }
            if (ref.notes != nullptr) ImGui::TextDisabled("%s", ref.notes);
            if (running_) ImGui::Text("timing... %.1f s", runTime_);
            if (ImGui::SmallButton("Reset measurements")) ResetMeasurements();
        }

        if (telemetryTable && ImGui::CollapsingHeader("Telemetry", ImGuiTreeNodeFlags_DefaultOpen)) {
            v->DrawDebug();
            DrawSurfaces(*v);
        }
        ImGui::End();
    }

    // What each tyre is standing on, from the terrain's paint at the contact point.
    void DrawSurfaces (const VehicleComponent& v) {
        st::terrain::TerrainSystem& terrain = st::terrain::TerrainSystem::Get();
        const st::terrain::TerrainData* data = terrain.Data(terrain.FindTerrain(*scene));
        if (data == nullptr) return;
        static const char* kCorner[] = { "FL", "FR", "RL", "RR" };
        std::string line = "Surface:";
        for (size_t i = 0; i < v.telemetry.wheels.size() && i < 4; ++i) {
            const auto& w = v.telemetry.wheels[i];
            line += std::string("  ") + kCorner[i] + " ";
            if (!w.contact) { line += "air"; continue; }
            st::terrain::GroundSample g;
            if (terrain.Sample(w.contact_position.x, w.contact_position.z, g) && g.dominantLayer < data->layers.size())
                line += data->layers[g.dominantLayer].name;
        }
        ImGui::TextUnformatted(line.c_str());
        ImGui::TextDisabled("Step 1: Jolt's stock tyre model - surface does not change grip yet.");
    }

    int hook_ = -1;
    float topSpeedKmh_ = 0.0f;
    float t48_ = -1.0f, t80_ = -1.0f, brakeDist_ = -1.0f;
    float runTime_ = 0.0f;
    bool running_ = false, braking_ = false;
    XMFLOAT3 brakeStart_ = XMFLOAT3(0, 0, 0);
};

} // namespace st

using StVehicleHudComponent = st::VehicleHudComponent;
ST_REGISTER_FRAMEWORK_COMPONENT_AS(StVehicleHudComponent, "stVehicleHud", "Vehicle")
