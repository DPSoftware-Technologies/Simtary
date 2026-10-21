#pragma once
// FlightModel - a helicopter described in plain SI terms, turned into a JSBSim model.
//
// Nobody edits JSBSim XML: a HelicopterSpec is what the inspector, the presets and the
// `.stfd` descriptor hold, and WriteJSBSimModel() generates the aircraft, engine and rotor
// files JSBSim reads (into a cache folder, see FlightComponent). The rotor itself is JSBSim's
// FGRotor (blade element with flapping, inflow lag, ground effect); this file only fills in its
// numbers and the things around it - mass and inertia, skids, fuselage drag and stabilisers.
//
// Frames. The spec uses the engine's body frame in metres: +X right, +Y up, +Z forward, origin
// on the ground under the main rotor mast. JSBSim's structural frame (+X aft, +Y right, +Z up,
// inches) is derived here and nowhere else.
//
// Pure C++ (standard library only), so tests can run it against JSBSim with no engine.

#include <string>
#include <vector>

namespace st::flight {

struct RotorSpec {
    double diameter = 10.0;          // m
    int    blades = 2;
    double chord = 0.3;              // m
    double nominalRpm = 400.0;
    double twistDeg = -8.0;          // root to tip
    double hingeOffset = 0.05;       // fraction of the radius (flapping hinge)
    double bladeMass = 40.0;         // kg, one blade
    double liftCurveSlope = 5.7;     // per rad
    double inflowLag = 0.1;          // s
    double tipLoss = 0.97;
    double groundEffectExp = 0.1;    // 0 = no ground effect
    int    sense = 1;                // 1 = counter-clockwise seen from above, -1 = clockwise
    double hub[3] = { 0, 2.6, 0 };   // m, body frame (main rotor); tail rotor: its hub
};

struct EngineSpec {
    double maxPowerKW = 450.0;
    double engineRpm = 6000.0;       // output shaft at the governed rotor rpm
};

struct HelicopterSpec {
    std::string name = "helicopter";
    // body
    double emptyMass = 1500.0;       // kg (with crew and fuel: the flying mass)
    double cg[3] = { 0, 1.4, 0.1 };  // m, body frame
    double length = 10.0, width = 2.0, height = 3.0;   // m, fuselage box, for inertia and skids
    double inertia[3] = { 0, 0, 0 }; // kg*m^2 about roll (Z), pitch (X), yaw (Y); 0 = estimate from the box
    // aerodynamics
    double dragAreaFront = 1.2;      // m^2 flat-plate area seen from ahead
    double dragAreaSide = 6.0;       // m^2 seen from the side
    double dragAreaTop = 8.0;        // m^2 seen from above (vertical flow, rotor downwash on the body ignored)
    double finArea = 0.8;            // m^2 vertical fin
    double stabilizerArea = 0.6;     // m^2 horizontal stabiliser
    double tailArm = 6.0;            // m, cg to fin / stabiliser
    // rotors and engine
    RotorSpec mainRotor;
    RotorSpec tailRotor;             // hub[] = its position; diameter etc. its own
    EngineSpec engine;
    // skids: two rails under the body
    double skidHalfWidth = 1.1;      // m
    double skidFront = 1.6;          // m ahead of the mast
    double skidBack = -1.4;          // m (negative = behind)
};

// Built-in helicopters (SI, from published figures where they exist, estimates marked in the
// source): "light" (a Bell 206-class utility helicopter), "utility" (UH-1-class),
// "attack" (AH-1S-class, close to JSBSim's own ah1s numbers).
std::vector<std::string> PresetNames ();
bool Preset (const std::string& name, HelicopterSpec& out);

// The inertia the spec flies with (its own, or estimated from the fuselage box and rotor).
void Inertia (const HelicopterSpec& spec, double out[3]);

// Writes <root>/aircraft/<model>/<model>.xml and <root>/engine/<model>_*.xml. False with
// `error` set when a folder or file cannot be written.
bool WriteJSBSimModel (const HelicopterSpec& spec, const std::string& root, const std::string& model,
                       std::string* error = nullptr);

} // namespace st::flight
