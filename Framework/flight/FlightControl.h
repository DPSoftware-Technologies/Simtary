#pragma once
// FlightControl - what sits between the pilot and JSBSim's rotor: the engine governor that
// holds rotor rpm, and the stability augmentation. With `assist` the stick commands a bank and
// a pitch angle (attitude command / attitude hold), centred it holds a hover over the ground,
// and centred pedals hold the heading; without it the stick is the swashplate, rate damped.
//
// Pure C++ (no engine, no JSBSim), so flight_test flies exactly the laws the game does.

#include "flight/FlightModel.h"

namespace st::flight {

// The pilot's controls.
struct Pilot {
    double collective = 0.0;    // 0..1, the lever
    double cyclicX = 0.0;       // -1..1, right
    double cyclicY = 0.0;       // -1..1, forward
    double pedals = 0.0;        // -1..1, nose right
};

// What the aircraft reports (JSBSim units converted: radians, rad/s, m/s).
struct Sensors {
    double rotorRpm = 0.0;
    double phi = 0.0, theta = 0.0, psi = 0.0;   // bank right, nose up, heading
    double p = 0.0, q = 0.0, r = 0.0;           // body rates
    double u = 0.0, v = 0.0;                    // body velocity forward, right (m/s)
    bool onGround = true;
};

// JSBSim control inputs.
struct Outputs {
    double throttle = 0.0;      // fcs/throttle-cmd-norm[0]
    double collective = 0.0;    // propulsion/engine[0]/collective-ctrl-rad
    double lateral = 0.0;       // propulsion/engine[0]/lateral-ctrl-rad      (+ rolls right)
    double longitudinal = 0.0;  // propulsion/engine[0]/longitudinal-ctrl-rad (+ nose down)
    double antitorque = 0.0;    // propulsion/engine[1]/antitorque-ctrl-rad   (+ nose left)
};

class Controller {
public:
    bool assist = true;
    bool engineOn = true;

    void Reset ();
    // One step of `dt` seconds.
    Outputs Update (const HelicopterSpec& spec, const Pilot& pilot, const Sensors& s, double dt);

    double LateralTrim () const { return lateralTrim_; }
    double LongitudinalTrim () const { return longitudinalTrim_; }

private:
    double throttleI_ = 0.2;
    double lateralTrim_ = 0.0, longitudinalTrim_ = 0.0;   // cyclic that holds the attitude
    double driftU_ = 0.0, driftV_ = 0.0;                  // attitude that cancels a steady drift
    double headingHold_ = 0.0;
    bool headingHeld_ = false;
};

} // namespace st::flight
