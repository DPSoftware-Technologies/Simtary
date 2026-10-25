// flight_test - every helicopter preset, generated into JSBSim XML by Framework/flight and flown
// by JSBSim through the jsbsim_c shim: the model loads, the rotor spins up to its governed rpm on
// the ground, and a simple collective + attitude hold lifts it into a hover and keeps it there.
// Then the UH-60A preset is flown at two conditions NASA measured in flight and compared with
// them (power, attitude). Built like a game (exceptions off); JSBSim's exceptions stay inside
// jsbsim.dll.

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

double g_groundM = 0.0;   // terrain height under every point, metres ASL
double Ground (void*, double, double) { return g_groundM; }

struct Fdm {
    stj_fdm* f;
    double Get (const char* p) { return stj_get(f, p, nullptr); }
    void Set (const char* p, double v) {
        if (!stj_set(f, p, v)) std::printf("  (no property %s)\n", p);
    }
};

double Clamp (double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); }

// windEast: steady wind blowing towards the east, m/s. severity: Dryden turbulence index
//	(0 none, 3 moderate) - what stFlight sets from the scene weather.
void FlyPreset (const std::string& name, const std::string& root, double windEast = 0.0,
                double severity = 0.0) {
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
    fdm.Set("atmosphere/wind-east-fps", windEast * ftPerM);
    if (severity > 0.0) {
        fdm.Set("atmosphere/turb-type", 3.0);
        fdm.Set("atmosphere/turbulence/milspec/windspeed_at_20ft_AGL-fps", windEast * ftPerM);
        fdm.Set("atmosphere/turbulence/milspec/severity", severity);
    }

    const double dt = 1.0 / 120.0;
    const double nominal = spec.mainRotor.nominalRpm;
    st::flight::Controller control;
    double collectiveI = 0.0;
    double maxBank = 0, maxPitch = 0, minAgl = 1e9, maxAgl = -1e9, maxHoverSpeed = 0, peakSpeed = 0;
    double psiStart = 0, maxYaw = 0;
    bool ok = true;
    // In wind the hold also has to find the bank into it, so it gets longer to stop.
    const int steps = int((windEast != 0.0 ? 95.0 : 80.0) / dt);
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
        // Lift-off and hover with the pedals centred: the heading must stay where it was.
        if (t < 20.0) psiStart = in.psi;
        else if (t < 50.0) maxYaw = std::max(maxYaw, std::fabs(std::remainder(in.psi - psiStart, 2.0 * 3.14159265358979)));
        if (i % int((t >= 19.0 && t < 35.0 ? 1.0 : 5.0) / dt) == 0) {
            std::printf("  t=%4.1f rpm=%6.1f thr=%.2f col=%.3f agl=%6.2f vz=%5.2f gs=%5.2f phi=%5.1f theta=%5.1f psi=%6.1f tr=%+.3f trim=%+.3f %+.3f\n",
                        t, in.rotorRpm, out.throttle, out.collective, agl, climb, groundSpeed, in.phi * 57.3,
                        in.theta * 57.3, in.psi * 57.3, out.antitorque, control.LateralTrim(), control.LongitudinalTrim());
        }
    }
    const double finalSpeed = fdm.Get("velocities/vg-fps") / ftPerM;
    const double rpmEnd = fdm.Get("propulsion/engine[0]/rotor-rpm");
    const double tailRpm = fdm.Get("propulsion/engine[1]/rotor-rpm");
    std::printf("  rpm %.1f (nominal %.0f), tail %.0f (nominal %.0f), hover band %.2f..%.2f m, max bank %.1f, max pitch %.1f deg\n"
                "  hover drift <= %.2f m/s, stick forward reached %.1f m/s, %.2f m/s when the run ends\n",
                rpmEnd, nominal, tailRpm, spec.tailRotor.nominalRpm, minAgl, maxAgl, maxBank * 57.3, maxPitch * 57.3,
                maxHoverSpeed, peakSpeed, finalSpeed);
    Check(ok, "JSBSim stopped", name);
    Check(std::fabs(rpmEnd - nominal) < nominal * 0.05, "rotor rpm not governed", name);
    Check(std::fabs(tailRpm - spec.tailRotor.nominalRpm) < spec.tailRotor.nominalRpm * 0.05, "tail rotor rpm not linked", name);
    Check(minAgl > 5.0 && maxAgl < 15.0, "did not hover at 10 m", name);
    Check(maxBank < 0.35 && maxPitch < 0.35, "attitude diverged", name);
    std::printf("  heading moved up to %.1f deg in lift-off + hover (pedals centred)\n", maxYaw * 57.3);
    // A crosswind weathercocks the nose (the fin turns it into the wind) before the hold answers.
    Check(maxYaw < (windEast != 0.0 ? 15.0 : 5.0) / 57.3, "heading wanders on lift-off", name);
    // Gusts push the aircraft around before the hold can answer; steady wind must not.
    Check(maxHoverSpeed < (severity > 0.0 ? 3.0 : 1.0), "hover hold drifts", name);
    Check(peakSpeed > 5.0, "stick forward does not fly forward", name);
    Check(finalSpeed < (severity > 0.0 ? 3.0 : 1.0), "hover hold does not stop the aircraft", name);
    stj_destroy(f);
}

// One steady flight condition, flown by a test pilot through the game's own control laws:
//	spin up on ground placed `aglM` below the target altitude, climb to it, accelerate to
//	`speedMps` true airspeed, settle, then average what the aircraft needs.
struct Condition {
    double massKg = 0;
    double altitudeFt = 0;       // pressure altitude flown
    double oatC = 15;            // outside air temperature there
    double aglM = 150;           // above the ground: out of ground effect
    double speedMps = 0;
    double seconds = 150;        // whole flight
    double average = 20;         // last seconds averaged
};

struct Measured {
    double mainHp = 0, tailHp = 0, pitchDeg = 0, rollDeg = 0, speedKt = 0, densityAltFt = 0, oatC = 0, collectiveDeg = 0;
    bool ok = false;
};

Measured FlyCondition (st::flight::HelicopterSpec spec, const Condition& c, const std::string& root, const std::string& model) {
    Measured m;
    const double ftPerM = 3.28083990;
    const double pi = 3.14159265358979;
    spec.emptyMass = c.massKg;
    std::string error;
    if (!st::flight::WriteJSBSimModel(spec, root, model, &error)) {
        std::printf("  %s\n", error.c_str());
        return m;
    }
    char err[512] = {};
    stj_fdm* f = stj_create(root.c_str(), model.c_str(), 1.0 / 120.0, err, sizeof(err));
    if (f == nullptr) {
        std::printf("  stj_create: %s\n", err);
        return m;
    }
    Fdm fdm{ f };
    g_groundM = c.altitudeFt / ftPerM - c.aglM;
    stj_set_ground(f, Ground, nullptr);
    // ISA at the target altitude plus a uniform offset that gives the measured temperature.
    const double isaC = 15.0 - 1.98 * c.altitudeFt / 1000.0;
    fdm.Set("atmosphere/delta-T", (c.oatC - isaC) * 1.8);
    fdm.Set("ic/lat-geod-deg", 13.75);
    fdm.Set("ic/long-gc-deg", 100.5);
    fdm.Set("ic/h-agl-ft", (spec.cg[1] + 0.05) * ftPerM);
    fdm.Set("ic/psi-true-deg", 0.0);
    if (!stj_init(f)) {
        std::printf("  RunIC: %s\n", stj_last_error(f));
        stj_destroy(f);
        return m;
    }
    const double dt = 1.0 / 120.0;
    st::flight::Controller control;
    double collectiveI = 0.0, speedI = 0.0;
    int samples = 0;
    const int steps = int(c.seconds / dt);
    for (int i = 0; i < steps; ++i) {
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
        const double tas = fdm.Get("velocities/vtrue-kts") / 1.94384449;

        st::flight::Pilot pilot;
        if (t >= 20.0) {
            const double targetClimb = Clamp((c.aglM - agl) * 0.3, -3.0, 4.0);
            const double climbErr = targetClimb - climb;
            collectiveI = Clamp(collectiveI + climbErr * 0.05 * dt, -0.25, 0.9);
            pilot.collective = Clamp(0.4 + collectiveI + climbErr * 0.075, 0.0, 1.0);
        }
        // Cruise: the stick asks for the pitch that holds the airspeed (a gentle ramp, so the
        //	altitude hold keeps up); the hover hold is off so a small stick is not "centred".
        if (c.speedMps > 0 && agl > c.aglM - 20.0) {
            control.hoverHold = false;
            const double target = std::min(c.speedMps, std::max(0.0, t - 60.0) * 1.5);
            const double e = target - tas;
            speedI = Clamp(speedI + e * 0.004 * dt, -0.8, 0.8);
            pilot.cyclicY = Clamp(e * 0.03 + speedI, -0.8, 0.8);
        }
        const st::flight::Outputs out = control.Update(spec, pilot, in, dt);
        fdm.Set("fcs/throttle-cmd-norm[0]", out.throttle);
        fdm.Set("propulsion/engine[0]/collective-ctrl-rad", out.collective);
        fdm.Set("propulsion/engine[0]/lateral-ctrl-rad", out.lateral);
        fdm.Set("propulsion/engine[0]/longitudinal-ctrl-rad", out.longitudinal);
        fdm.Set("propulsion/engine[1]/antitorque-ctrl-rad", out.antitorque);
        if (!stj_run(f)) {
            std::printf("  run: %s\n", stj_last_error(f));
            stj_destroy(f);
            return m;
        }
        if (i % int(15.0 / dt) == 0)
            std::printf("    t=%5.1f agl=%6.1f tas=%5.1f kt pitch=%5.1f col=%.3f thr=%.2f lon=%+.3f stick=%+.2f stab=%.0f a1=%.3f alpha=%.1f\n",
                        t, agl, tas * 1.94384449, in.theta * 57.2958, out.collective, out.throttle, out.longitudinal,
                        pilot.cyclicY, fdm.Get("aero/pitch-stabilizer"), fdm.Get("propulsion/engine[0]/a1-rad"),
                        fdm.Get("aero/alpha-deg"));
        if (t >= c.seconds - c.average) {
            const double mainOmega = fdm.Get("propulsion/engine[0]/rotor-rpm") * 2.0 * pi / 60.0;
            const double tailOmega = fdm.Get("propulsion/engine[1]/rotor-rpm") * 2.0 * pi / 60.0;
            m.mainHp += std::fabs(fdm.Get("propulsion/engine[0]/torque-lbsft")) * mainOmega / 550.0;
            m.tailHp += std::fabs(fdm.Get("propulsion/engine[1]/torque-lbsft")) * tailOmega / 550.0;
            m.pitchDeg += fdm.Get("attitude/theta-deg");
            m.rollDeg += fdm.Get("attitude/phi-deg");
            m.speedKt += fdm.Get("velocities/vtrue-kts");
            m.densityAltFt += fdm.Get("atmosphere/density-altitude");
            m.oatC += (fdm.Get("atmosphere/T-R") - 491.67) / 1.8;
            m.collectiveDeg += (out.collective + 0.75 * spec.mainRotor.twistDeg * pi / 180.0) * 57.2958;
            ++samples;
        }
    }
    stj_destroy(f);
    if (samples > 0) {
        const double n = samples;
        m.mainHp /= n; m.tailHp /= n; m.pitchDeg /= n; m.rollDeg /= n; m.speedKt /= n;
        m.densityAltFt /= n; m.oatC /= n; m.collectiveDeg /= n;
        m.ok = true;
    }
    return m;
}

// UH-60A against NASA flight test.
//	Hover OGE design point: Bousman, "Power Measurement Errors on a Utility Aircraft", AHS 2002,
//	  Table 5 - 16,200 lb, 4,000 ft pressure altitude, 35 C, engine power 2,067 HP.
//	Level flight at maximum continuous power: Kufeld, NASA/TM-2019-220311 (Airloads counter
//	  8534), Table 3 - 16,114.5 lb, density altitude 4,469 ft, 22.1 C, 157.93 kt, engines
//	  1387.22 + 1380.20 HP, pitch -4.63 deg.
//	Engine power also feeds the accessories (66 HP, Bousman) and the transmission (~3 % loss,
//	assumed), which the model does not have; they are added to its rotor powers below.
void ValidateUH60 (const std::string& root) {
    st::flight::HelicopterSpec spec;
    st::flight::Preset("uh60", spec);
    auto report = [&](const char* what, const Measured& m, double refHp, double refPitch, bool hasPitch) {
        if (!m.ok) {
            Check(false, "condition not flown", what);
            return;
        }
        const double hp = (m.mainHp + m.tailHp) / 0.97 + 66.0;
        const double err = 100.0 * (hp - refHp) / refHp;
        std::printf("  %s: %.0f kt, density alt %.0f ft, %.1f C\n", what, m.speedKt, m.densityAltFt, m.oatC);
        std::printf("    power: main rotor %.0f + tail rotor %.0f HP -> engine-equivalent %.0f HP, flight test %.0f HP (%+.1f %%)\n",
                    m.mainHp, m.tailHp, hp, refHp, err);
        std::printf("    pitch %+.2f deg (flight test %s), roll %+.2f deg, collective %.1f deg at 3/4 R\n", m.pitchDeg,
                    hasPitch ? std::to_string(refPitch).substr(0, 5).c_str() : "-", m.rollDeg, m.collectiveDeg);
        Check(std::fabs(err) < 15.0, "power differs from flight test by more than 15 %", what);
        if (hasPitch) Check(std::fabs(m.pitchDeg - refPitch) < 3.0, "pitch attitude differs from flight test by more than 3 deg", what);
    };

    std::printf("uh60 vs NASA flight test\n");
    Condition hover;
    hover.massKg = 16200 * 0.45359237;
    hover.altitudeFt = 4000;
    hover.oatC = 35;
    hover.seconds = 150;
    report("hover OGE", FlyCondition(spec, hover, root, "test_uh60_hover"), 2067.0, 0, false);

    Condition cruise;
    cruise.massKg = 16114.5 * 0.45359237;
    // Density altitude 4,469 ft at 22.1 C is about 2,936 ft pressure altitude.
    cruise.altitudeFt = 2936;
    cruise.oatC = 22.1;
    cruise.speedMps = 157.93 / 1.94384449;
    cruise.seconds = 260;
    cruise.average = 30;
    report("level 158 kt", FlyCondition(spec, cruise, root, "test_uh60_cruise"), 1387.22 + 1380.20, -4.63, true);
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
    for (const char* name : { "utility", "uh60" }) {
        std::printf("%s, 10 m/s crosswind\n", name);
        FlyPreset(name, root.generic_string(), 10.0);
        std::printf("%s, 10 m/s wind + moderate turbulence\n", name);
        FlyPreset(name, root.generic_string(), 10.0, 3.0);
    }
    ValidateUH60(root.generic_string());
    std::printf(failures ? "flight_test: %d failure(s)\n" : "flight_test: ok\n", failures);
    return failures ? 1 : 0;
}
