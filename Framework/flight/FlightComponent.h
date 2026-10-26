#pragma once
// The "stFlight" native component - a helicopter flown by JSBSim.
//
// Attach it to the helicopter's root entity (origin on the ground under the main rotor mast,
// +Z forward, +Y up), next to an "stInput" pointed at a map with these actions:
//
//     Collective  Axis -1..1   rate: holding up raises the lever, it stays where it is left
//     Cyclic      Vector2      x = roll right, y = pitch forward (nose down)
//     Pedals      Axis -1..1   positive = nose right
//     Engine      Button       start / stop the engine
//
//     NCI_0 = "stInput"   NCA_0_actionMap = "Helicopter"
//     NCI_1 = "stFlight"  NCA_1_preset    = "utility"
//
// Start() turns the preset (flight/FlightModel.h - nobody edits JSBSim XML) into a JSBSim
// model in a cache folder and loads it through jsbsim.dll (libs/JSBSimC). From then on JSBSim
// is the authority: every frame it is stepped at a fixed 120 Hz and the entity's transform is
// written from its position and attitude. The rotor (blade element, flapping, inflow, ground
// effect), the skids and the fuselage aerodynamics are JSBSim's; what a real cockpit has
// around them - the rpm governor and the stability augmentation - is flight/FlightControl.
//
// Ground: JSBSim asks for the height under each contact point; the answer is the higher of
// the authored terrain and a physics ray (pads, roofs, any Jolt body except this one).
// A rigid body on the entity is made kinematic and follows the transform, so other bodies
// still collide with the helicopter.
//
// World <-> JSBSim: a flat local frame around a fixed geodetic origin (north = +Z, east = +X,
// up = +Y, metres), exact to well under a centimetre across a game map.

#include "stNativeComponent.h"
#include "flight/FlightControl.h"

#include <string>

struct stj_fdm;
namespace st { struct InputComponent; }

namespace st {

struct FlightComponent : wi::scene::NativeComponent {
    // JSBSim runs on the main thread, and its ground callback casts physics rays.
    ST_NATIVE_COMPONENT_MAIN_THREAD()

    // bound from NCA_<localID>_<name>
    std::string preset = "utility";
    float mass = 0.0f;              // kg, 0 = the preset's
    float maxPowerKW = 0.0f;        // 0 = the preset's
    bool  engineRunning = true;     // at start
    bool  assist = true;            // hover hold while the stick is centred, heading hold while the pedals are
    float collectiveRate = 0.5f;    // lever travel per second at full input
    std::string mainRotorNode = "MainRotor";   // descendants spun at rotor rpm (optional)
    std::string tailRotorNode = "TailRotor";
    bool  weatherWind = true;       // fly in the scene weather's wind and turbulence

    // the pilot's controls this frame (after rate limits), and what JSBSim reports back
    struct Controls {
        float collective = 0.0f;    // 0..1
        float cyclicX = 0.0f;       // -1..1, right
        float cyclicY = 0.0f;       // -1..1, forward
        float pedals = 0.0f;        // -1..1, nose right
    };
    Controls controls;
    bool remoteInput = false;       // true: `remote` flies it, the stInput is ignored
    Controls remote;

    struct Telemetry {
        double rotorRpm = 0, tailRpm = 0, throttle = 0, powerKW = 0;
        double agl = 0;             // m, skids above ground
        double airspeed = 0;        // m/s true
        double groundSpeed = 0;     // m/s
        double climb = 0;           // m/s
        double heading = 0, pitch = 0, bank = 0;   // degrees
        double torquePercent = 0;   // main rotor torque / torque at max power and nominal rpm
        bool onGround = true;
    };
    Telemetry telemetry;
    bool ready = false;             // JSBSim model loaded and initialised
    std::string status;             // why not, when not

    // Put the helicopter back on the ground where it is (or where it was placed), at rest,
    // with the rotor still turning if the engine runs.
    void Reset ();

    // What is broken, as the flight model sees it (stDamage sets it when a part comes off).
    // Applied every step through the model's damage force / moment - see FlightModel.cpp.
    struct Damage {
        bool tailRotorLost = false;  // no anti-torque: the main rotor's torque turns the airframe
        int  mainBladesLost = 0;     // lift lost per blade, plus a once-per-rev imbalance
        bool stabilizerLost = false; // its pitch stability and damping are gone
        bool engineFailed = false;   // no power, and it cannot be restarted: autorotate
    };
    void SetDamage (const Damage& damage);
    const Damage& GetDamage () const { return damage_; }
    // World-space angular velocity of the main / tail rotor right now (rad/s) and its hub,
    // for debris that leaves a spinning rotor.
    XMFLOAT3 MainRotorSpin () const;
    XMFLOAT3 TailRotorSpin () const;

    void DescribeRequired (wi::vector<wi::scene::RequiredComponent>& out) override;
    void Start () override;
    void Update (float dt) override;
    void Destroy () override;
    void DrawDebug () override;

    // used by the ground callback
    double GroundHeight (double x, double z) const;

private:
    bool Create ();
    void InitAt (const XMFLOAT3& origin, float yaw);
    void ReadInput (float dt);
    void Step ();
    void ApplyWind ();
    void WriteTransform ();
    void SpinRotors (float dt);
    void ApplyDamage ();

    st::flight::HelicopterSpec spec_;
    stj_fdm* fdm_ = nullptr;
    st::InputComponent* input_ = nullptr;
    int idCollective_ = -1, idCyclic_ = -1, idPedals_ = -1, idEngine_ = -1;
    double accumulator_ = 0.0;
    st::flight::Controller control_;
    double originY_ = 0.0;          // current entity height, for the ground ray
    XMFLOAT3 placedPos_ = XMFLOAT3(0, 0, 0);
    float placedYaw_ = 0.0f;
    float mainAngle_ = 0.0f, tailAngle_ = 0.0f;
    wi::ecs::Entity mainRotor_ = wi::ecs::INVALID_ENTITY, tailRotor_ = wi::ecs::INVALID_ENTITY;
    XMFLOAT4 mainRest_ = XMFLOAT4(0, 0, 0, 1), tailRest_ = XMFLOAT4(0, 0, 0, 1);
    bool engineOn_ = true;
    int rebuilds_ = 0;              // simulations rebuilt after breaking; stops after a few
    Damage damage_;
    double damageAzimuth_ = 0.0;    // rad, where the missing blade's imbalance points
};

} // namespace st
