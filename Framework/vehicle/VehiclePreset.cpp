#include "vehicle/VehiclePreset.h"

#include "wiMath.h"

#include <cstring>

namespace st::vehicle {

namespace {

using Rb = wi::scene::RigidBodyPhysicsComponent;

const char* kPresetNames[] = { "HMMWV M1097A2" };

// HMMWV M1097A2 (heavy variant, 6.5 L naturally aspirated diesel, 4-speed automatic).
//
// Published (TM 9-2320-280-10 class figures and common spec sheets):
//   wheelbase 3.30 m (130 in), track 1.82 m (71.6 in), width 2.16 m, length 4.57 m,
//   ground clearance 0.41 m (16 in), tyres 37x12.50R16.5 (0.94 m diameter, 0.32 m wide),
//   engine 6.5 L V8 diesel 160 hp @ 3400 rpm, 393 Nm (290 lb ft) @ 1700 rpm,
//   transmission 4L80-E: 2.48 / 1.48 / 1.00 / 0.75, reverse 2.08,
//   differentials 2.56:1 with 1.92:1 geared hubs (4.92:1 overall), Torsen front and rear,
//   top speed ~113 km/h (70 mph), turning circle ~16 m.
// Estimated (not published, reasoned from the above):
//   test mass 2700 kg (curb plus crew and fuel), CG 0.90 m above ground, suspension
//   natural frequency ~1.4 Hz with ~0.28 m of travel, brake torques sized for ~0.8 g,
//   wheel+tyre rotational inertia 8 kg m^2, anti-roll bar stiffness.
void Hmmwv (Rb& rb, PresetReference* ref) {
    constexpr float mass         = 2700.0f;
    constexpr float wheelbase    = 3.30f;
    constexpr float track        = 1.82f;
    constexpr float tyreRadius   = 0.47f;
    constexpr float tyreWidth    = 0.32f;
    constexpr float cgHeight     = 0.90f;   // above flat ground
    constexpr float boxBottom    = 0.45f;   // body underside above ground (clearance + margin)
    constexpr float boxHalfY     = 0.50f;
    constexpr float boxCentre    = boxBottom + boxHalfY;

    if (ref != nullptr) {
        ref->topSpeedKmh      = 113.0f;
        ref->turningCircleM   = 16.0f;
        ref->groundClearanceM = 0.41f;
        ref->massKg           = mass;
        ref->wheelbaseM       = wheelbase;
        ref->trackM           = track;
        ref->cgHeightM        = cgHeight;
        ref->boxCentreHeightM = boxCentre;
        ref->notes = "M1097A2: 6.5 L NA diesel 160 hp / 393 Nm, 4L80-E, 4.92:1 overall, Torsen. "
                     "Mass, CG, suspension rates and brake torques are estimates.";
    }
    rb.shape = Rb::CollisionShape::BOX;
    rb.box.halfextents = XMFLOAT3(1.05f, boxHalfY, 2.25f);
    rb.mass = mass;
    rb.friction = 0.5f;         // body against the world (a rollover, a wall), not the tyres
    rb.restitution = 0.05f;
    rb.damping_linear = 0.0f;   // aerodynamic drag belongs in the vehicle model, not here
    rb.damping_angular = 0.05f;
    rb.SetDisableDeactivation(true);

    Rb::Vehicle& v = rb.vehicle;
    v.type = Rb::Vehicle::Type::Car;
    v.collision_mode = Rb::Vehicle::CollisionMode::Cylinder; // rough ground: a ray misses ledges
    // The engine drops the centre of mass to this height above the box bottom.
    v.chassis_half_height = cgHeight - boxBottom;
    v.chassis_half_width  = track * 0.5f;
    v.chassis_half_length = wheelbase * 0.5f;
    v.wheel_radius = tyreRadius;
    v.wheel_width  = tyreWidth;
    // No artificial anti-tip limit: a real HMMWV can roll over, and a test vehicle that
    // cannot is not telling the truth about its roll threshold.
    v.max_roll_angle = wi::math::DegreesToRadians(180.0f);
    v.max_steering_angle = wi::math::DegreesToRadians(27.0f); // ~16 m turning circle

    // Suspension: natural length = max_length, static sag g / (2 pi f)^2 = 0.127 m at
    // 1.4 Hz, so at rest the wheel centre sits 0.233 m below the attachment point. Put the
    // attachment where that lands the tyre on the ground: wheel centre at tyreRadius.
    constexpr float frequency = 1.4f;
    constexpr float maxLength = 0.36f;
    constexpr float minLength = 0.08f;
    const float sag = 9.81f / ((2.0f * 3.14159265f * frequency) * (2.0f * 3.14159265f * frequency));
    const float attachY = (tyreRadius - boxCentre) + (maxLength - sag);

    v.wheels.clear();
    for (int i = 0; i < 4; ++i) {
        const bool front = i < 2;
        const bool right = (i % 2) == 1;
        Rb::Vehicle::Wheel w;
        w.position = XMFLOAT3(right ? track * 0.5f : -track * 0.5f, attachY,
                              front ? wheelbase * 0.5f : -wheelbase * 0.5f);
        w.radius = tyreRadius;
        w.width = tyreWidth;
        w.suspension_min_length = minLength;
        w.suspension_max_length = maxLength;
        w.suspension_frequency = front ? frequency : frequency + 0.1f; // flat-ride: rear slightly stiffer
        w.suspension_damping = 0.35f;
        w.max_steer_angle = front ? v.max_steering_angle : 0.0f;
        w.max_brake_torque = front ? 3500.0f : 2500.0f;
        w.max_hand_brake_torque = front ? 0.0f : 2500.0f;
        w.inertia = 8.0f;
        w.angular_damping = 0.2f;
        w.mirror_x = right;
        v.wheels.push_back(w);
    }

    // Full-time four wheel drive: one Torsen per axle, half the torque each, and the same
    // bias ratio between the axles.
    v.differentials.clear();
    for (int axle = 0; axle < 2; ++axle) {
        Rb::Vehicle::Differential d;
        d.left_wheel = axle * 2;
        d.right_wheel = axle * 2 + 1;
        d.differential_ratio = 2.56f * 1.92f;
        d.left_right_split = 0.5f;
        d.limited_slip_ratio = 2.5f;
        d.engine_torque_ratio = 0.5f;
        v.differentials.push_back(d);
    }

    v.anti_roll_bars.clear();
    {
        Rb::Vehicle::AntiRollBar front;
        front.left_wheel = 0;
        front.right_wheel = 1;
        front.stiffness = 3000.0f;
        v.anti_roll_bars.push_back(front);
        Rb::Vehicle::AntiRollBar rear;
        rear.left_wheel = 2;
        rear.right_wheel = 3;
        rear.stiffness = 1500.0f;
        v.anti_roll_bars.push_back(rear);
    }

    v.max_engine_torque = 393.0f;
    v.engine_min_rpm = 700.0f;
    v.engine_max_rpm = 3600.0f;
    v.engine_inertia = 0.6f;
    v.engine_angular_damping = 0.2f;
    v.clutch_strength = 10.0f;

    v.auto_transmission = true;
    v.gear_ratios = { 2.48f, 1.48f, 1.00f, 0.75f };
    v.reverse_gear_ratios = { -2.08f };
    v.transmission_shift_up_rpm = 3200.0f;
    v.transmission_shift_down_rpm = 1500.0f;
    v.transmission_switch_time = 0.4f;
    v.transmission_clutch_release_time = 0.3f;
    v.transmission_switch_latency = 0.5f;

    // Physically correct tyre impulses. Jolt's stock 10x only exists to keep its own
    // presets feeling the way they were tuned against an old solver bug.
    v.longitudinal_impulse_scale = 1.0f;

    rb.physicsobject = {}; // rebuild the body with all of this
}

} // namespace

const char* const* PresetNames (size_t& count) {
    count = sizeof(kPresetNames) / sizeof(kPresetNames[0]);
    return kPresetNames;
}

bool ApplyPreset (const std::string& name, Rb& rb) {
    if (name == kPresetNames[0] || name == "HMMWV") {
        Hmmwv(rb, nullptr);
        return true;
    }
    return false;
}

PresetReference Reference (const std::string& name) {
    PresetReference ref;
    if (name == kPresetNames[0] || name == "HMMWV") {
        Rb scratch;
        Hmmwv(scratch, &ref);
    }
    return ref;
}

} // namespace st::vehicle
