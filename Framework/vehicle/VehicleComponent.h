#pragma once
// The "stVehicle" native component - a drivable wheeled vehicle.
//
// Attach it to the entity that IS the chassis (its origin at the centre of the chassis
// collision box), next to an "stInput" pointed at a map with these actions:
//
//     Throttle   Axis 0..1        Brake      Axis 0..1
//     Steer      Axis -1..1       Handbrake  Button
//     ResetVehicle Button
//
//     NCI_0 = "stInput"     NCA_0_actionMap = "Vehicle"
//     NCI_1 = "stVehicle"   NCA_1_preset    = "HMMWV M1097A2"
//
// Start() turns the entity's rigid body into the preset (vehicle/VehiclePreset.h) and binds
// the visual wheels by NAME among the entity's descendants. Every frame it turns the input
// into driver commands - rate-limited steering, brake-to-reverse at a standstill, as a real
// automatic is driven - and reads the simulation back as telemetry.
//
// The vehicle physics itself is the engine's (Jolt's wheeled vehicle). This component is
// the seam the tyre, drivetrain and soil models plug into as they arrive.

#include "stNativeComponent.h"
#include "wiPhysics.h"

#include <string>

namespace st { struct InputComponent; }

namespace st {

struct VehicleComponent : wi::scene::NativeComponent {
    // DriveVehicle and the telemetry read touch the physics system, which is not re-entrant
    // from job threads.
    ST_NATIVE_COMPONENT_MAIN_THREAD()

    // bound from NCA_<localID>_<name>
    std::string preset  = "HMMWV M1097A2";
    std::string wheelFL = "Wheel Front Left";
    std::string wheelFR = "Wheel Front Right";
    std::string wheelRL = "Wheel Rear Left";
    std::string wheelRR = "Wheel Rear Right";
    float steerSpeed  = 1.6f;   // fraction of full lock per second towards the input
    float steerReturn = 2.5f;   // ... and back to centre when the input is released
    bool  brakeToReverse = true;
    // Re-apply the preset on every start. Turn off once the vehicle has been hand-tuned in
    // the inspector, or the tuning is overwritten on the next load.
    bool  applyPreset = true;

    // what was sent to the simulation this frame
    float throttle  = 0.0f;   // -1..1, negative = reverse
    float brake     = 0.0f;
    float steer     = 0.0f;   // -1..1, positive = right
    float handbrake = 0.0f;

    // The driver's wishes before steering rate limits and brake-to-reverse: what a network
    // client sends, and what a networked copy is driven by (Framework/net/VehicleSync).
    struct DriverInput {
        float throttle = 0.0f;   // 0..1
        float brake = 0.0f;      // 0..1
        float steer = 0.0f;      // -1..1
        bool handbrake = false;
    };
    DriverInput wanted;          // this frame's, from whichever source drove it
    bool remoteInput = false;    // true: `remote` drives it, the stInput is ignored
    DriverInput remote;
    uint8_t resetCount = 0;      // +1 on every ResetUpright; the network replays a change

    // read back after the last physics step
    wi::physics::VehicleTelemetry telemetry;
    bool hasTelemetry = false;

    // Put the vehicle back on its wheels where it stands, 1.5 m up, at rest.
    void ResetUpright ();

    // Fit a visual model to the simulated vehicle and parent it under this entity: scaled
    // to the preset's wheelbase, turned so its front is +Z, and placed so its wheel centres
    // sit on the simulated ones. Measured off the model's wheel nodes (the wheelFL.. names),
    // so it needs those. An authoring step - the fit is saved with the map, so a loaded
    // vehicle never refits. Returns a one-line report; empty `model` = the first child
    // that has the wheel nodes.
    std::string FitVisual (wi::ecs::Entity model = wi::ecs::INVALID_ENTITY);
    // Wheel vertical load in newtons, from the last step's suspension impulse.
    float WheelLoad (size_t wheel) const;

    void DescribeRequired (wi::vector<wi::scene::RequiredComponent>& out) override;
    void Start () override;
    void Update (float dt) override;
    void DrawDebug () override;

private:
    void ResolveInput ();
    // On load the chassis body exists frames before the terrain's heightfield under it does,
    // and an unheld vehicle falls through the world. Keeps it where it was placed until a
    // physics ray finds ground beneath it; true once it may drive.
    bool HoldUntilGround (wi::scene::RigidBodyPhysicsComponent& rb, float dt);
    // Safety net: well below an authored terrain's ground = fell through; put it back on top.
    void CatchFallThrough (wi::scene::RigidBodyPhysicsComponent& rb);
    void BindWheels (wi::scene::RigidBodyPhysicsComponent& rb);
    wi::ecs::Entity FindDescendant (const std::string& name) const;

    st::InputComponent* input_ = nullptr;
    int idThrottle_ = -1, idBrake_ = -1, idSteer_ = -1, idHandbrake_ = -1, idReset_ = -1;
    bool reversing_ = false;
    bool grounded_ = false;
    bool holdInit_ = false;
    float holdTime_ = 0.0f;
    XMFLOAT3 holdPos_ = XMFLOAT3(0, 0, 0);
    XMFLOAT4 holdRot_ = XMFLOAT4(0, 0, 0, 1);
    std::string fitReport_;
};

} // namespace st
