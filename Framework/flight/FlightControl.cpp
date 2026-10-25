#include "flight/FlightControl.h"

#include <algorithm>
#include <cmath>

namespace st::flight {

namespace {

constexpr double kPi = 3.14159265358979;
// Lever and stick travel in rotor angles, radians.
constexpr double kCollectiveMin = -0.05;  // at 3/4 radius
constexpr double kCollectiveMax = 0.30;
constexpr double kCyclicMax = 0.12;     // direct mode: full stick
constexpr double kCyclicLimit = 0.25;   // swashplate travel (UH-60: about +-14 deg) - what the SAS may use
constexpr double kPedalMax = 0.30;
constexpr double kSpoolRate = 0.25;      // throttle per second, rising (turbine spool-up)
constexpr double kYawGain = 0.5;         // tail pitch rad per rad of heading error
constexpr double kYawIntegral = 0.4;     // per second: learns the pedal this aircraft needs
constexpr double kYawTrimMax = 0.30;
// Stability augmentation (assist).
constexpr double kMaxBank = 0.50;        // rad at full stick
constexpr double kMaxPitch = 0.35;
constexpr double kHoldAngle = 0.20;      // most the hover hold asks for
constexpr double kVelocityGain = 0.03;   // rad of attitude per m/s of drift
constexpr double kVelocityIntegral = 0.01;
constexpr double kIntegrateBelow = 2.0;   // m/s
constexpr double kAttitudeGain = 0.50;   // cyclic rad per rad of attitude error
constexpr double kRateGain = 0.15;       // cyclic rad per rad/s of body rate
constexpr double kTrimRate = 0.30;
constexpr double kTrimMax = kCyclicLimit;   // the trim may need all of the swashplate (fast flight)

double Clamp (double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); }

double WrapPi (double a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

} // namespace

void Controller::Reset () {
    throttleI_ = 0.2;
    lateralTrim_ = longitudinalTrim_ = 0.0;
    driftU_ = driftV_ = 0.0;
    headingHeld_ = false;
    yawTrim_ = 0.0;
    throttle_ = 0.0;
}

Outputs Controller::Update (const HelicopterSpec& spec, const Pilot& pilot, const Sensors& s, double dt) {
    Outputs out;

    // Governor: what a turboshaft's fuel control does - hold rotor rpm whatever the load.
    if (engineOn) {
        const double nominal = spec.mainRotor.nominalRpm;
        const double err = (nominal - s.rotorRpm) / nominal;
        const double want = Clamp(throttleI_ + err * 4.0, 0.0, 1.0);
        // A turbine spools up over seconds; the stand-in engine would hand the rotor full
        //	power at once, and on the ground that torque turned the whole aircraft on its
        //	skids during the start. Power may rise only so fast; it may always fall.
        out.throttle = std::min(want, throttle_ + kSpoolRate * dt);
        // no integrating while held back by the spool, or it winds up and overspeeds the rotor
        if (out.throttle >= want) throttleI_ = Clamp(throttleI_ + err * 2.0 * dt, 0.0, 1.0);
        throttle_ = out.throttle;
    } else {
        throttleI_ = 0.0;
        throttle_ = 0.0;
    }

    // The lever sets the blade pitch at 3/4 radius; JSBSim takes it at the root, which a
    //	twisted blade (twist is negative, tip down) needs more of for the same lift.
    const double twistRad = spec.mainRotor.twistDeg * kPi / 180.0;
    out.collective = kCollectiveMin + Clamp(pilot.collective, 0.0, 1.0) * (kCollectiveMax - kCollectiveMin) - 0.75 * twistRad;
    const bool stickCentred = std::fabs(pilot.cyclicX) < 0.05 && std::fabs(pilot.cyclicY) < 0.05;
    const bool pedalsCentred = std::fabs(pilot.pedals) < 0.05;
    const bool airborne = !s.onGround;

    double lateral, longitudinal;
    if (assist && airborne) {
        // Attitude command: the stick asks for a bank and a pitch angle, and lets go of
        //	them when released. Centred, it asks for the attitude that brings the ground
        //	velocity to zero instead (nose up to stop going forward, bank against a sideways
        //	drift). The integrated trim is the cyclic that holds an attitude - a real hover
        //	is not wings-level: the tail rotor pushes sideways and the hub moment wants the
        //	nose down.
        if (!stickCentred || !hoverHold) driftU_ = driftV_ = 0.0;
        double phiWant = pilot.cyclicX * kMaxBank;
        double thetaWant = -pilot.cyclicY * kMaxPitch;
        if (stickCentred && hoverHold) {
            // The integral is what removes the last metre per second: the tail rotor's
            //	side force alone holds a P-only loop at a steady drift.
            //	Only near the hover: integrating while slowing down from cruise winds it up
            //	and the aircraft overshoots into a backwards drift.
            if (std::fabs(s.u) < kIntegrateBelow && std::fabs(s.v) < kIntegrateBelow) {
                driftV_ = Clamp(driftV_ + s.v * kVelocityIntegral * dt, -kHoldAngle, kHoldAngle);
                driftU_ = Clamp(driftU_ + s.u * kVelocityIntegral * dt, -kHoldAngle, kHoldAngle);
            }
            phiWant = Clamp(-s.v * kVelocityGain - driftV_, -kHoldAngle, kHoldAngle);
            thetaWant = Clamp(s.u * kVelocityGain + driftU_, -kHoldAngle, kHoldAngle);
        }
        const double ePhi = s.phi - phiWant, eTheta = s.theta - thetaWant;
        lateralTrim_ = Clamp(lateralTrim_ - ePhi * kTrimRate * dt, -kTrimMax, kTrimMax);
        longitudinalTrim_ = Clamp(longitudinalTrim_ + eTheta * kTrimRate * dt, -kTrimMax, kTrimMax);
        lateral = lateralTrim_ - ePhi * kAttitudeGain - s.p * kRateGain;
        longitudinal = longitudinalTrim_ + eTheta * kAttitudeGain + s.q * kRateGain;
    } else {
        // Direct: the stick is the swashplate, with rate damping only.
        lateral = pilot.cyclicX * kCyclicMax - s.p * kRateGain;
        longitudinal = pilot.cyclicY * kCyclicMax + s.q * kRateGain;
        if (!airborne) lateralTrim_ = longitudinalTrim_ = driftU_ = driftV_ = 0.0;
    }
    out.lateral = Clamp(lateral, -kCyclicLimit, kCyclicLimit);
    out.longitudinal = Clamp(longitudinal, -kCyclicLimit, kCyclicLimit);

    // Tail rotor: torque compensation follows the collective, yaw rate damped. That feed-
    //	forward is a guess that fits the small presets; the UH-60 and AH-64 need a different
    //	amount, so the heading hold integrates the rest (yawTrim_ - what a pilot's feet learn).
    //	The hold starts on the ground: a lift-off is when the torque changes most, and
    //	nothing held the heading while the skids were still light. It only LEARNS in the air,
    //	where a heading error means pedal, not skid friction.
    double antitorque = out.collective * 0.5 - pilot.pedals * kPedalMax + s.r * 0.4;
    if (assist && pedalsCentred) {
        if (!headingHeld_) {
            headingHold_ = s.psi;
            headingHeld_ = true;
        }
        const double err = WrapPi(s.psi - headingHold_);
        if (airborne) yawTrim_ = Clamp(yawTrim_ + err * kYawIntegral * dt, -kYawTrimMax, kYawTrimMax);
        antitorque += err * kYawGain;
    } else {
        headingHeld_ = false;
    }
    if (assist) antitorque += yawTrim_;
    else yawTrim_ = 0.0;
    // Everything above is tail blade pitch at 3/4 radius, like the lever; JSBSim takes it at
    //	the root. Without this a twisted tail (UH-60 -18 deg, AH-64 -9 deg) lost a quarter
    //	radian of its travel and hovered almost against the pedal stop.
    const double tailTwistRad = spec.tailRotor.twistDeg * kPi / 180.0;
    out.antitorque = Clamp(antitorque, -0.45, 0.45) - 0.75 * tailTwistRad;
    return out;
}

} // namespace st::flight
