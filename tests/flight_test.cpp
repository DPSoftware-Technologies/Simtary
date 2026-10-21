// flight_test - every helicopter preset, generated into JSBSim XML by Framework/flight and flown
// by JSBSim through the jsbsim_c shim: the model loads, the rotor spins up to its governed rpm on
// the ground, and a simple collective + attitude hold lifts it into a hover and keeps it there.
// Built like a game (exceptions off); JSBSim's exceptions stay inside jsbsim.dll.

#include "flight/FlightControl.h"
#include "flight/FlightModel.h"
#include "jsbsim_c.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {

int failures = 0;

void Check (bool ok, const char* what, const std::string& preset) {
    if (!ok) {
        std::printf("  FAIL %s: %s\n", preset.c_str(), what);
        ++failures;
    }
}

double Ground (void*, double, double) { return 0.0; }

struct Fdm {
    stj_fdm* f;
    double Get (const char* p) { return stj_get(f, p, nullptr); }
    void Set (const char* p, double v) {
        if (!stj_set(f, p, v)) std::printf("  (no property %s)\n", p);
    }
};

double Clamp (double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); }

void FlyPreset (const std::string& name, const std::string& root) {
    st::flight::HelicopterSpec spec;
    if (!st::flight::Preset(name, spec)) {
        Check(false, "preset missing", name);
        return;
    }
    std::string error;
    const std::string model = "test_" + name;
    if (!st::flight::WriteJSBSimModel(spec, root, model, &error)) {
        Check(false, error.c_str(), name);
        return;
    }
    char err[512] = {};
    stj_fdm* f = stj_create(root.c_str(), model.c_str(), 1.0 / 120.0, err, sizeof(err));
    if (f == nullptr) {
        std::printf("  stj_create: %s\n", err);
        Check(false, "model does not load", name);
        return;
    }
    Fdm fdm{ f };
    stj_set_ground(f, Ground, nullptr);
    const double ftPerM = 3.28083990;
    fdm.Set("ic/lat-geod-deg", 13.75);
    fdm.Set("ic/long-gc-deg", 100.5);
    fdm.Set("ic/h-agl-ft", (spec.cg[1] + 0.05) * ftPerM);
    fdm.Set("ic/psi-true-deg", 0.0);
    fdm.Set("ic/u-fps", 0.0);
    fdm.Set("ic/v-fps", 0.0);
    fdm.Set("ic/w-fps", 0.0);
    if (!stj_init(f)) {
        std::printf("  RunIC: %s\n", stj_last_error(f));
        Check(false, "RunIC failed", name);
        stj_destroy(f);
        return;
    }

    const double dt = 1.0 / 120.0;
    const double nominal = spec.mainRotor.nominalRpm;
    st::flight::Controller control;
    double collectiveI = 0.0;
    double maxBank = 0, maxPitch = 0, minAgl = 1e9, maxAgl = -1e9, maxHoverSpeed = 0, peakSpeed = 0;
    bool ok = true;
    const int steps = int(80.0 / dt);
    for (int i = 0; i < steps && ok; ++i) {
        const double t = i * dt;
        st::flight::Sensors in;
        in.rotorRpm = fdm.Get("propulsion/engine[0]/rotor-rpm");
        in.phi = fdm.Get("attitude/phi-rad");
        in.theta = fdm.Get("attitude/theta-rad");
        in.psi = fdm.Get("attitude/psi-rad");
        in.p = fdm.Get("velocities/p-rad_sec");
        in.q = fdm.Get("velocities/q-rad_sec");
        in.r = fdm.Get("velocities/r-rad_sec");
        in.u = fdm.Get("velocities/u-fps") / ftPerM;
        in.v = fdm.Get("velocities/v-fps") / ftPerM;
        in.onGround = false;
        for (int g = 0; g < 4; ++g) {
            char prop[48];
            std::snprintf(prop, sizeof(prop), "gear/unit[%d]/WOW", g);
            if (fdm.Get(prop) > 0.5) in.onGround = true;
        }
        const double agl = fdm.Get("position/h-agl-ft") / ftPerM - spec.cg[1];
        const double climb = fdm.Get("velocities/h-dot-fps") / ftPerM;
        const double groundSpeed = fdm.Get("velocities/vg-fps") / ftPerM;

        // The pilot: 0-20 s spin-up with the lever down, then up to 10 m and hold it there
        //	with the collective alone. The stick stays centred (the hover hold flies it)
        //	except for 50-60 s, half forward.
        st::flight::Pilot pilot;
        if (t >= 20.0) {
            const double targetClimb = Clamp((10.0 - agl) * 0.5, -1.0, 2.0);
            const double climbErr = targetClimb - climb;
            collectiveI = Clamp(collectiveI + climbErr * 0.05 * dt, -0.25, 0.9);
            pilot.collective = Clamp(0.25 + collectiveI + climbErr * 0.075, 0.0, 1.0);
        }
        if (t >= 50.0 && t < 60.0) pilot.cyclicY = 0.5;
        const st::flight::Outputs out = control.Update(spec, pilot, in, dt);
        fdm.Set("fcs/throttle-cmd-norm[0]", out.throttle);
        fdm.Set("propulsion/engine[0]/collective-ctrl-rad", out.collective);
        fdm.Set("propulsion/engine[0]/lateral-ctrl-rad", out.lateral);
        fdm.Set("propulsion/engine[0]/longitudinal-ctrl-rad", out.longitudinal);
        fdm.Set("propulsion/engine[1]/antitorque-ctrl-rad", out.antitorque);

        if (!stj_run(f)) {
            std::printf("  run: %s\n", stj_last_error(f));
            ok = false;
            break;
        }
        if (t > 35.0 && t < 50.0) {
            minAgl = std::min(minAgl, agl);
            maxAgl = std::max(maxAgl, agl);
            maxBank = std::max(maxBank, std::fabs(in.phi));
            maxPitch = std::max(maxPitch, std::fabs(in.theta));
            maxHoverSpeed = std::max(maxHoverSpeed, groundSpeed);
        }
        if (t >= 50.0 && t < 60.0) peakSpeed = std::max(peakSpeed, groundSpeed);
        if (i % int(5.0 / dt) == 0) {
            std::printf("  t=%4.1f rpm=%6.1f thr=%.2f col=%.3f agl=%6.2f vz=%5.2f gs=%5.2f phi=%5.1f theta=%5.1f trim=%+.3f %+.3f\n",
                        t, in.rotorRpm, out.throttle, out.collective, agl, climb, groundSpeed, in.phi * 57.3,
                        in.theta * 57.3, control.LateralTrim(), control.LongitudinalTrim());
        }
    }
    const double finalSpeed = fdm.Get("velocities/vg-fps") / ftPerM;
    const double rpmEnd = fdm.Get("propulsion/engine[0]/rotor-rpm");
    const double tailRpm = fdm.Get("propulsion/engine[1]/rotor-rpm");
    std::printf("  rpm %.1f (nominal %.0f), tail %.0f (nominal %.0f), hover band %.2f..%.2f m, max bank %.1f, max pitch %.1f deg\n"
                "  hover drift <= %.2f m/s, stick forward reached %.1f m/s, %.2f m/s 20 s after release\n",
                rpmEnd, nominal, tailRpm, spec.tailRotor.nominalRpm, minAgl, maxAgl, maxBank * 57.3, maxPitch * 57.3,
                maxHoverSpeed, peakSpeed, finalSpeed);
    Check(ok, "JSBSim stopped", name);
    Check(std::fabs(rpmEnd - nominal) < nominal * 0.05, "rotor rpm not governed", name);
    Check(std::fabs(tailRpm - spec.tailRotor.nominalRpm) < spec.tailRotor.nominalRpm * 0.05, "tail rotor rpm not linked", name);
    Check(minAgl > 5.0 && maxAgl < 15.0, "did not hover at 10 m", name);
    Check(maxBank < 0.35 && maxPitch < 0.35, "attitude diverged", name);
    Check(maxHoverSpeed < 1.0, "hover hold drifts", name);
    Check(peakSpeed > 5.0, "stick forward does not fly forward", name);
    Check(finalSpeed < 1.0, "hover hold does not stop the aircraft", name);
    stj_destroy(f);
}

} // namespace

int main () {
    std::printf("JSBSim %s\n", stj_version());
    std::error_code ec;
    const auto root = std::filesystem::temp_directory_path(ec) / "simtary_flight_test";
    std::filesystem::create_directories(root, ec);
    for (const auto& name : st::flight::PresetNames()) {
        std::printf("%s\n", name.c_str());
        FlyPreset(name, root.generic_string());
    }
    std::printf(failures ? "flight_test: %d failure(s)\n" : "flight_test: ok\n", failures);
    return failures ? 1 : 0;
}
