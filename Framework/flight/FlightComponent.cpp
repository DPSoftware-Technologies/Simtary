#include "flight/FlightComponent.h"

#if SIMTARY_HAS_JSBSIM

#include "jsbsim_c.h"
#include "input/InputComponent.h"
#include "io/UserData.h"
#include "terrain/TerrainSystem.h"

#include "wiScene.h"
#include "wiPhysics.h"
#include "wiBacklog.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

using namespace wi::scene;
using wi::ecs::Entity;
using wi::ecs::INVALID_ENTITY;

namespace st {

namespace {

constexpr double kPi = 3.14159265358979;
constexpr double kFtPerM = 3.28083990;
constexpr double kStep = 1.0 / 120.0;
constexpr int kMaxStepsPerFrame = 12;   // 100 ms: a longer frame slows the simulation instead
// The local frame's geodetic origin and the radii that turn metres into degrees there. Any
//	point works; the equator keeps the two radii simple. Only consistency matters - JSBSim's
//	position goes out and comes back through the same two functions.
constexpr double kMeridianRadius = 6335439.0;   // a (1 - e^2), WGS84 at the equator
constexpr double kNormalRadius = 6378137.0;     // a

void ToLocal (double latDeg, double lonDeg, double& x, double& z) {
    z = latDeg * kPi / 180.0 * kMeridianRadius;
    x = lonDeg * kPi / 180.0 * kNormalRadius;
}

void ToGeodetic (double x, double z, double& latDeg, double& lonDeg) {
    latDeg = z / kMeridianRadius * 180.0 / kPi;
    lonDeg = x / kNormalRadius * 180.0 / kPi;
}

double Ground (void* user, double latDeg, double lonDeg) {
    double x, z;
    ToLocal(latDeg, lonDeg, x, z);
    return static_cast<const FlightComponent*>(user)->GroundHeight(x, z);
}

double Get (stj_fdm* f, const char* p) { return stj_get(f, p, nullptr); }

} // namespace

void FlightComponent::DescribeRequired (wi::vector<RequiredComponent>& out) {
    out.push_back(RequiredComponent::For<TransformComponent>());
    out.push_back(RequiredComponent::Native("stInput"));
}

void FlightComponent::Start () {
    Bind(preset, "preset");
    Bind(mass, "mass");
    Bind(maxPowerKW, "maxPowerKW");
    Bind(engineRunning, "engineRunning");
    Bind(assist, "assist");
    Bind(collectiveRate, "collectiveRate");
    Bind(mainRotorNode, "mainRotorNode");
    Bind(tailRotorNode, "tailRotorNode");
    Bind(weatherWind, "weatherWind");
    engineOn_ = engineRunning;

    // Other bodies collide with the helicopter; JSBSim moves it.
    if (RigidBodyPhysicsComponent* rb = GetComponent<RigidBodyPhysicsComponent>()) {
        if (!rb->IsKinematic()) {
            rb->SetKinematic(true);
            rb->physicsobject = {};
        }
    }
    auto findRotor = [&](const std::string& name, XMFLOAT4& rest) -> Entity {
        if (name.empty()) return INVALID_ENTITY;
        const Entity e = scene->Entity_FindByName(name, entity);
        if (const TransformComponent* t = scene->transforms.GetComponent(e)) rest = t->rotation_local;
        return e;
    };
    mainRotor_ = findRotor(mainRotorNode, mainRest_);
    tailRotor_ = findRotor(tailRotorNode, tailRest_);

    if (const TransformComponent* t = GetComponent<TransformComponent>()) {
        placedPos_ = t->GetPosition();
        XMFLOAT3 fwd;
        XMStoreFloat3(&fwd, XMVector3Rotate(XMVectorSet(0, 0, 1, 0), XMLoadFloat4(&t->rotation_local)));
        placedYaw_ = std::atan2(fwd.x, fwd.z);
    }
    ready = Create();
    if (!ready) wi::backlog::post("stFlight: " + status, wi::backlog::LogLevel::Error);
}

bool FlightComponent::Create () {
    if (!st::flight::Preset(preset, spec_)) {
        status = "unknown preset '" + preset + "'";
        return false;
    }
    if (mass > 0) spec_.emptyMass = mass;
    if (maxPowerKW > 0) spec_.engine.maxPowerKW = maxPowerKW;

    std::error_code ec;
    const std::filesystem::path root = std::filesystem::path(st::userdata::BaseDir()) / "flight";
    std::filesystem::create_directories(root, ec);
    const std::string model = "st_" + preset + "_" + std::to_string(entity);
    std::string error;
    if (!st::flight::WriteJSBSimModel(spec_, root.generic_string(), model, &error)) {
        status = error;
        return false;
    }
    char err[512] = {};
    fdm_ = stj_create(root.generic_string().c_str(), model.c_str(), kStep, err, sizeof(err));
    if (fdm_ == nullptr) {
        status = std::string("JSBSim: ") + err;
        return false;
    }
    stj_set_ground(fdm_, Ground, this);
    InitAt(placedPos_, placedYaw_);
    if (!status.empty()) return false;
    wi::backlog::post("stFlight: '" + preset + "' ready (JSBSim " + stj_version() + ")");
    return true;
}

void FlightComponent::InitAt (const XMFLOAT3& origin, float yaw) {
    if (fdm_ == nullptr) return;
    // On whatever is under it - never inside it.
    originY_ = origin.y;
    const double ground = GroundHeight(origin.x, origin.z);
    const double baseY = std::max<double>(origin.y, ground) + 0.05;
    // JSBSim's position is the CG's.
    XMFLOAT3 cg;
    XMStoreFloat3(&cg, XMVector3Rotate(XMVectorSet(float(spec_.cg[0]), float(spec_.cg[1]), float(spec_.cg[2]), 0),
                                       XMQuaternionRotationRollPitchYaw(0, yaw, 0)));
    double lat, lon;
    ToGeodetic(origin.x + cg.x, origin.z + cg.z, lat, lon);
    stj_set(fdm_, "ic/lat-geod-deg", lat);
    stj_set(fdm_, "ic/long-gc-deg", lon);
    stj_set(fdm_, "ic/h-sl-ft", (baseY + cg.y) * kFtPerM);
    stj_set(fdm_, "ic/psi-true-deg", yaw * 180.0 / kPi);
    stj_set(fdm_, "ic/theta-deg", 0.0);
    stj_set(fdm_, "ic/phi-deg", 0.0);
    stj_set(fdm_, "ic/u-fps", 0.0);
    stj_set(fdm_, "ic/v-fps", 0.0);
    stj_set(fdm_, "ic/w-fps", 0.0);
    stj_set(fdm_, "ic/p-rad_sec", 0.0);
    stj_set(fdm_, "ic/q-rad_sec", 0.0);
    stj_set(fdm_, "ic/r-rad_sec", 0.0);
    if (!stj_init(fdm_)) {
        status = std::string("JSBSim initial conditions: ") + stj_last_error(fdm_);
        return;
    }
    controls = Controls();
    accumulator_ = 0.0;
    control_.Reset();
    status.clear();
    WriteTransform();
}

void FlightComponent::Reset () {
    if (const TransformComponent* t = GetComponent<TransformComponent>()) {
        const XMFLOAT3 pos = t->GetPosition();
        InitAt(XMFLOAT3(pos.x, pos.y - 2.0f, pos.z), float(telemetry.heading * kPi / 180.0));
    } else {
        InitAt(placedPos_, placedYaw_);
    }
}

double FlightComponent::GroundHeight (double x, double z) const {
    double best = -1000.0;
    st::terrain::GroundSample sample;
    if (st::terrain::TerrainSystem::Get().Sample(float(x), float(z), sample)) best = sample.height;
    if (scene != nullptr) {
        // From just above the skids: a pad, a roof, a deck - anything with a body, except our own.
        XMFLOAT3 origin(float(x), float(originY_ + 1.0), float(z));
        for (int i = 0; i < 8; ++i) {
            const wi::primitive::Ray ray(origin, XMFLOAT3(0, -1, 0));
            const wi::physics::RayIntersectionResult hit = wi::physics::Intersects(*scene, ray);
            if (!hit.IsValid()) break;
            if (hit.entity != entity) {
                best = std::max(best, double(hit.position.y));
                break;
            }
            origin.y = hit.position.y - 0.05f;
        }
    }
    return best;
}

void FlightComponent::ReadInput (float dt) {
    Controls want;
    if (remoteInput) {
        want = remote;
        controls = want;
        return;
    }
    if (input_ == nullptr) {
        input_ = GetComponent<st::InputComponent>();
        if (input_ != nullptr) {
            idCollective_ = input_->Find("Collective");
            idCyclic_ = input_->Find("Cyclic");
            idPedals_ = input_->Find("Pedals");
            idEngine_ = input_->Find("Engine");
        }
    }
    if (input_ == nullptr) return;
    // The lever stays where it is left; the stick and pedals centre themselves.
    const float lever = std::clamp(input_->Value(idCollective_), -1.0f, 1.0f);
    controls.collective = std::clamp(controls.collective + lever * collectiveRate * dt, 0.0f, 1.0f);
    const XMFLOAT2 stick = input_->Vector2(idCyclic_);
    controls.cyclicX = std::clamp(stick.x, -1.0f, 1.0f);
    controls.cyclicY = std::clamp(stick.y, -1.0f, 1.0f);
    controls.pedals = std::clamp(input_->Value(idPedals_), -1.0f, 1.0f);
    if (input_->Pressed(idEngine_) && !damage_.engineFailed) {
        engineOn_ = !engineOn_;
        wi::backlog::post(std::string("stFlight: engine ") + (engineOn_ ? "start" : "stop"));
    }
}

void FlightComponent::Step () {
    st::flight::Sensors in;
    in.rotorRpm = Get(fdm_, "propulsion/engine[0]/rotor-rpm");
    in.phi = Get(fdm_, "attitude/phi-rad");
    in.theta = Get(fdm_, "attitude/theta-rad");
    in.psi = Get(fdm_, "attitude/psi-rad");
    in.p = Get(fdm_, "velocities/p-rad_sec");
    in.q = Get(fdm_, "velocities/q-rad_sec");
    in.r = Get(fdm_, "velocities/r-rad_sec");
    in.u = Get(fdm_, "velocities/u-fps") / kFtPerM;
    in.v = Get(fdm_, "velocities/v-fps") / kFtPerM;
    in.onGround = telemetry.onGround;
    st::flight::Pilot pilot;
    pilot.collective = controls.collective;
    pilot.cyclicX = controls.cyclicX;
    pilot.cyclicY = controls.cyclicY;
    pilot.pedals = controls.pedals;
    control_.assist = assist;
    control_.engineOn = engineOn_;
    st::flight::Outputs out = control_.Update(spec_, pilot, in, kStep);
    if (damage_.tailRotorLost) {
        // Root pitch that puts the 3/4-radius blade at zero: no thrust (FlightControl's
        //	output is 3/4R pitch minus 0.75 x the tail's twist).
        out.antitorque = -0.75 * spec_.tailRotor.twistDeg * kPi / 180.0;
    }
    ApplyDamage();
    stj_set(fdm_, "fcs/throttle-cmd-norm[0]", out.throttle);
    stj_set(fdm_, "propulsion/engine[0]/collective-ctrl-rad", out.collective);
    stj_set(fdm_, "propulsion/engine[0]/lateral-ctrl-rad", out.lateral);
    stj_set(fdm_, "propulsion/engine[0]/longitudinal-ctrl-rad", out.longitudinal);
    stj_set(fdm_, "propulsion/engine[1]/antitorque-ctrl-rad", out.antitorque);

    if (!stj_run(fdm_)) {
        status = std::string("JSBSim stopped: ") + stj_last_error(fdm_);
        wi::backlog::post("stFlight: " + status, wi::backlog::LogLevel::Error);
        ready = false;
    }
    telemetry.throttle = out.throttle;
}

void FlightComponent::ApplyDamage () {
    // Lift the missing main blades no longer make (body +Z is down), at the hub.
    const double thrust = Get(fdm_, "propulsion/engine[0]/thrust-lbs");
    const int blades = std::max(1, spec_.mainRotor.blades);
    const int lost = std::clamp(damage_.mainBladesLost, 0, blades);
    const double force = std::isfinite(thrust) ? std::max(0.0, thrust) * lost / blades : 0.0;

    double l = 0.0, m = 0.0;
    if (lost > 0 && lost < blades && std::isfinite(thrust)) {
        // Once-per-revolution imbalance: the missing blades' share of the lift at 0.7 R,
        //	turning with the rotor. Scaled down for what the flapping hinges absorb.
        constexpr double kImbalance = 0.25;
        const double omega = Get(fdm_, "propulsion/engine[0]/rotor-rpm") / 60.0 * 2.0 * kPi;
        damageAzimuth_ = std::fmod(damageAzimuth_ + omega * kStep, 2.0 * kPi);
        const double radiusFt = spec_.mainRotor.diameter * 0.5 * kFtPerM;
        const double amp = kImbalance * std::max(0.0, thrust) / blades * 0.7 * radiusFt;
        l = amp * std::cos(damageAzimuth_);
        m = amp * std::sin(damageAzimuth_);
    }
    if (damage_.stabilizerLost) {
        // Take back what the stabiliser contributed (aerodynamics in FlightModel.cpp).
        const double stab = Get(fdm_, "aero/pitch-stabilizer") + Get(fdm_, "aero/pitch-damping");
        if (std::isfinite(stab)) m -= stab;
    }
    stj_set(fdm_, "external_reactions/damage-force/magnitude", force);
    stj_set(fdm_, "external_reactions/damage-moment/magnitude-lbsft", 1.0);
    stj_set(fdm_, "external_reactions/damage-moment/l", l);
    stj_set(fdm_, "external_reactions/damage-moment/m", m);
    stj_set(fdm_, "external_reactions/damage-moment/n", 0.0);
}

void FlightComponent::SetDamage (const Damage& damage) {
    damage_ = damage;
    // A rotor that came off is not ours to spin any more.
    if (damage_.tailRotorLost) tailRotor_ = INVALID_ENTITY;
    if (damage_.engineFailed) engineOn_ = false;
    wi::backlog::post(std::string("stFlight: damage - tail rotor ") + (damage_.tailRotorLost ? "LOST" : "ok") +
                      ", main blades lost " + std::to_string(damage_.mainBladesLost) +
                      ", stabilizer " + (damage_.stabilizerLost ? "LOST" : "ok") +
                      ", engine " + (damage_.engineFailed ? "FAILED" : "ok"));
}

namespace {
// The spin vector of a rotor node: its axis is the one SpinRotors turns it about, which the
//	spin itself leaves where it is in world space.
XMFLOAT3 RotorSpin (Scene* scene, Entity rotor, int axis, double rpm, double sense) {
    const TransformComponent* t = (scene && rotor != INVALID_ENTITY) ? scene->transforms.GetComponent(rotor) : nullptr;
    if (t == nullptr) return XMFLOAT3(0, 0, 0);
    const XMMATRIX w = XMLoadFloat4x4(&t->world);
    const XMVECTOR a = XMVector3Normalize(w.r[axis]);
    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVectorScale(a, float(-sense * rpm / 60.0 * 2.0 * kPi)));
    return out;
}
} // namespace

XMFLOAT3 FlightComponent::MainRotorSpin () const {
    return RotorSpin(scene, mainRotor_, 1, telemetry.rotorRpm, spec_.mainRotor.sense);
}

XMFLOAT3 FlightComponent::TailRotorSpin () const {
    return RotorSpin(scene, tailRotor_, 0, telemetry.tailRpm, spec_.tailRotor.sense);
}

void FlightComponent::ApplyWind () {
    // The scene's weather is the one wind everything sees: grass and trees sway in it, and
    // the rotor flies in it. Its direction vector points where the air GOES (east = +X,
    // north = +Z), which is JSBSim's convention too. JSBSim keeps the hover hold honest:
    // velocities/u,v are over the ground, so a centred stick holds position into the wind.
    double north = 0.0, east = 0.0, speed = 0.0, turbulence = 0.0;
    if (weatherWind) {
        const wi::scene::WeatherComponent& w = scene->weather;
        const double len = std::sqrt(double(w.windDirection.x) * w.windDirection.x +
                                     double(w.windDirection.z) * w.windDirection.z);
        if (len > 1e-6 && w.windSpeed > 0.0f) {
            speed = w.windSpeed;
            north = w.windDirection.z / len * speed;
            east = w.windDirection.x / len * speed;
            turbulence = w.windRandomness;
        }
    }
    stj_set(fdm_, "atmosphere/wind-north-fps", north * kFtPerM);
    stj_set(fdm_, "atmosphere/wind-east-fps", east * kFtPerM);
    // Dryden (MIL-F-8785C) gusts. The weather's turbulence (0..20, what makes grass flutter)
    // becomes the spec's severity index 0..7: 0 none, 1 light, 3 moderate, 5 severe.
    const double severity = std::clamp(std::round(turbulence * 0.5), 0.0, 7.0);
    stj_set(fdm_, "atmosphere/turb-type", severity > 0.0 ? 3.0 : 0.0);
    stj_set(fdm_, "atmosphere/turbulence/milspec/windspeed_at_20ft_AGL-fps", speed * kFtPerM);
    stj_set(fdm_, "atmosphere/turbulence/milspec/severity", severity);
}

void FlightComponent::WriteTransform () {
    TransformComponent* t = GetComponent<TransformComponent>();
    if (t == nullptr || fdm_ == nullptr) return;
    double x, z;
    ToLocal(Get(fdm_, "position/lat-geod-deg"), Get(fdm_, "position/long-gc-deg"), x, z);
    const double y = Get(fdm_, "position/h-sl-ft") / kFtPerM;
    const double psi = Get(fdm_, "attitude/psi-rad");
    const double theta = Get(fdm_, "attitude/theta-rad");
    const double phi = Get(fdm_, "attitude/phi-rad");
    // JSBSim's ZYX Euler angles in the engine's axes: yaw about +Y is psi, pitch about +X
    //	(right) is -theta (nose up is negative there), roll about +Z is -phi.
    const XMVECTOR q = XMQuaternionRotationRollPitchYaw(float(-theta), float(psi), float(-phi));
    // That is the CG; the entity's origin is cg[] behind it in body axes.
    XMFLOAT3 cg;
    XMStoreFloat3(&cg, XMVector3Rotate(XMVectorSet(float(spec_.cg[0]), float(spec_.cg[1]), float(spec_.cg[2]), 0), q));
    t->translation_local = XMFLOAT3(float(x) - cg.x, float(y) - cg.y, float(z) - cg.z);
    XMStoreFloat4(&t->rotation_local, q);
    t->SyncWorldFromLocal(wi::scene::GetRenderOrigin());
    t->SetDirty();
    t->UpdateTransform();
    originY_ = t->translation_local.y;
}

void FlightComponent::SpinRotors (float dt) {
    auto spin = [&](Entity e, const XMFLOAT4& rest, float& angle, double rpm, XMVECTOR axis, double sense) {
        TransformComponent* t = e != INVALID_ENTITY ? scene->transforms.GetComponent(e) : nullptr;
        if (t == nullptr) return;
        angle = float(std::fmod(angle - sense * rpm / 60.0 * 2.0 * kPi * dt, 2.0 * kPi));
        XMStoreFloat4(&t->rotation_local, XMQuaternionMultiply(XMQuaternionRotationAxis(axis, angle), XMLoadFloat4(&rest)));
        t->SetDirty();
    };
    // Counter-clockwise from above is a negative angle about +Y.
    spin(mainRotor_, mainRest_, mainAngle_, telemetry.rotorRpm, XMVectorSet(0, 1, 0, 0), spec_.mainRotor.sense);
    spin(tailRotor_, tailRest_, tailAngle_, telemetry.tailRpm, XMVectorSet(1, 0, 0, 0), spec_.tailRotor.sense);
}

void FlightComponent::Update (float dt) {
    if (!ready || fdm_ == nullptr || dt <= 0.0f) return;
    ReadInput(dt);
    ApplyWind();

    accumulator_ = std::min(accumulator_ + dt, kStep * kMaxStepsPerFrame);
    while (accumulator_ >= kStep && ready) {
        Step();
        accumulator_ -= kStep;
    }
    // A crash hard enough leaves JSBSim's state non-finite, and it never recovers from that.
    if (!std::isfinite(Get(fdm_, "position/h-sl-ft")) || !std::isfinite(Get(fdm_, "attitude/psi-rad"))) {
        // RunIC does not clear the rotor's own state (rpm, inflow, transmission), so a reset
        //	of the same FDM breaks again next frame: build a new one. Give up after a few.
        stj_destroy(fdm_);
        fdm_ = nullptr;
        ready = false;
        if (++rebuilds_ > 3) {
            status = "the simulation keeps breaking - stopped (check the ground under the spawn point)";
            wi::backlog::post("stFlight: " + status, wi::backlog::LogLevel::Error);
            return;
        }
        wi::backlog::post("stFlight: the simulation broke (crash?) - rebuilt it where it was placed",
                          wi::backlog::LogLevel::Warning);
        ready = Create();
        return;
    }
    WriteTransform();

    Telemetry& tm = telemetry;
    tm.rotorRpm = Get(fdm_, "propulsion/engine[0]/rotor-rpm");
    tm.tailRpm = Get(fdm_, "propulsion/engine[1]/rotor-rpm");
    tm.powerKW = Get(fdm_, "propulsion/engine[0]/power-hp") / 1.34102209;
    tm.agl = Get(fdm_, "position/h-agl-ft") / kFtPerM - spec_.cg[1];
    tm.airspeed = Get(fdm_, "velocities/vt-fps") / kFtPerM;
    tm.groundSpeed = Get(fdm_, "velocities/vg-fps") / kFtPerM;
    tm.climb = Get(fdm_, "velocities/h-dot-fps") / kFtPerM;
    tm.heading = Get(fdm_, "attitude/psi-deg");
    tm.pitch = Get(fdm_, "attitude/theta-deg");
    tm.bank = Get(fdm_, "attitude/phi-deg");
    const double omega = spec_.mainRotor.nominalRpm / 60.0 * 2.0 * kPi;
    const double maxTorque = spec_.engine.maxPowerKW * 1.34102209 * 550.0 / omega;   // ft*lbf
    tm.torquePercent = maxTorque > 0 ? 100.0 * Get(fdm_, "propulsion/engine[0]/torque-lbsft") / maxTorque : 0.0;
    tm.onGround = false;
    for (int i = 0; i < 4; ++i) {
        char prop[48];
        std::snprintf(prop, sizeof(prop), "gear/unit[%d]/WOW", i);
        if (Get(fdm_, prop) > 0.5) tm.onGround = true;
    }
    SpinRotors(dt);
}

void FlightComponent::Destroy () {
    stj_destroy(fdm_);
    fdm_ = nullptr;
    ready = false;
}

void FlightComponent::DrawDebug () {
    ImGui::Text("Preset: %s  (%.0f kg, %.0f kW, rotor %.2f m)", preset.c_str(), spec_.emptyMass,
                spec_.engine.maxPowerKW, spec_.mainRotor.diameter);
    if (!ready) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%s", status.empty() ? "not started (Play)" : status.c_str());
        return;
    }
    if (ImGui::Checkbox("Assist (hover + heading hold)", &assist)) SaveBoundParams();
    ImGui::SameLine();
    if (damage_.engineFailed) ImGui::TextDisabled("[engine failed]");
    else if (ImGui::SmallButton(engineOn_ ? "Stop engine" : "Start engine")) engineOn_ = !engineOn_;
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset")) Reset();
    const Telemetry& tm = telemetry;
    ImGui::Text("rotor %.0f rpm  tail %.0f  throttle %.2f  %.0f kW  torque %.0f%%", tm.rotorRpm, tm.tailRpm,
                tm.throttle, tm.powerKW, tm.torquePercent);
    ImGui::Text("AGL %.1f m  climb %+.1f m/s  IAS %.0f km/h  GS %.0f km/h%s", tm.agl, tm.climb, tm.airspeed * 3.6,
                tm.groundSpeed * 3.6, tm.onGround ? "  [ground]" : "");
    ImGui::Text("hdg %.0f  pitch %+.1f  bank %+.1f", tm.heading, tm.pitch, tm.bank);
    ImGui::Text("collective %.2f  cyclic %+.2f %+.2f  pedals %+.2f", controls.collective, controls.cyclicX,
                controls.cyclicY, controls.pedals);
    if (damage_.tailRotorLost || damage_.mainBladesLost || damage_.stabilizerLost || damage_.engineFailed)
        ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "damage: %s%s%s%s", damage_.tailRotorLost ? "tail rotor  " : "",
                           damage_.mainBladesLost ? "main blades  " : "", damage_.stabilizerLost ? "stabilizer  " : "",
                           damage_.engineFailed ? "engine" : "");
}

} // namespace st

using StFlightComponent = st::FlightComponent;
ST_REGISTER_FRAMEWORK_COMPONENT_AS(StFlightComponent, "stFlight", "Vehicle")

#endif // SIMTARY_HAS_JSBSIM
