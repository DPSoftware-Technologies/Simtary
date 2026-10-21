#include "flight/FlightControl.h"

#include <algorithm>
#include <cmath>

namespace st::flight {

namespace {

constexpr double kPi = 3.14159265358979;
// Lever and stick travel in rotor angles, radians.
constexpr double kCollectiveMax = 0.40;
constexpr double kCyclicMax = 0.12;
constexpr double kPedalMax = 0.30;
// Stability augmentation (assist).
constexpr double kMaxBank = 0.50;        // rad at full stick
constexpr double kMaxPitch = 0.35;
constexpr double kHoldAngle = 0.20;      // most the hover hold asks for
constexpr double kVelocityGain = 0.03;   // rad of attitude per m/s of drift
constexpr double kVelocityIntegral = 0.01;
constexpr double kAttitudeGain = 0.50;   // cyclic rad per rad of attitude error
constexpr double kRateGain = 0.15;       // cyclic rad per rad/s of body rate
constexpr double kTrimRate = 0.30;
constexpr double kTrimMax = 0.08;

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
}

Outputs Controller::Update (const HelicopterSpec& spec, const Pilot& pilot, const Sensors& s, double dt) {
    Outputs out;

    // Governor: what a turboshaft's fuel control does - hold rotor rpm whatever the load.
    if (engineOn) {
        const double nominal = spec.mainRotor.nominalRpm;
        const double err = (nominal - s.rotorRpm) / nominal;
        throttleI_ = Clamp(throttleI_ + err * 2.0 * dt, 0.0, 1.0);
        out.throttle = Clamp(throttleI_ + err * 4.0, 0.0, 1.0);
    } else {
        throttleI_ = 0.0;
    }

    out.collective = Clamp(pilot.collective, 0.0, 1.0) * kCollectiveMax;
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
        double phiWant = pilot.cyclicX * kMaxBank;
        double thetaWant = -pilot.cyclicY * kMaxPitch;
        if (stickCentred) {
            // The integral is what removes the last metre per second: the tail rotor's
            //	side force alone holds a P-only loop at a steady drift.
            driftV_ = Clamp(driftV_ + s.v * kVelocityIntegral * dt, -kHoldAngle, kHoldAngle);
            driftU_ = Clamp(driftU_ + s.u * kVelocityIntegral * dt, -kHoldAngle, kHoldAngle);
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
    out.lateral = Clamp(lateral, -kCyclicMax * 1.5, kCyclicMax * 1.5);
    out.longitudinal = Clamp(longitudinal, -kCyclicMax * 1.5, kCyclicMax * 1.5);

    // Tail rotor: torque compensation follows the collective, yaw rate damped.
    double antitorque = out.collective * 0.5 - pilot.pedals * kPedalMax + s.r * 0.4;
    if (assist && pedalsCentred && airborne) {
        if (!headingHeld_) {
            headingHold_ = s.psi;
            headingHeld_ = true;
        }
        antitorque += WrapPi(s.psi - headingHold_) * 0.5;
    } else {
        headingHeld_ = false;
    }
    out.antitorque = Clamp(antitorque, -0.45, 0.45);
    return out;
}

} // namespace st::flight
