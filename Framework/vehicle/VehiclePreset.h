#pragma once
// Vehicle presets - real vehicles as data for the engine's Jolt vehicle
// (RigidBodyPhysicsComponent::Vehicle).
//
// A preset fills in everything the simulation reads: the chassis box and mass, the centre
// of gravity, every wheel (position, tyre size, suspension, steering, brakes), the engine,
// the gearbox and the differentials. The numbers are a real vehicle's published specs
// where those exist, and reasoned estimates where they do not; each preset says which.
//
// Coordinates are chassis-local, the engine's: +Z forward, +X right, +Y up, origin at the
// centre of the chassis collision box. Wheels are ordered FL, FR, RL, RR.

#include "wiScene_Components.h"

#include <string>

namespace st::vehicle {

// Reference figures for a preset, so the test scene can show "simulated vs real" side
// by side. Zero means unknown.
struct PresetReference {
    float topSpeedKmh       = 0.0f;
    float turningCircleM    = 0.0f;  // kerb to kerb, diameter
    float groundClearanceM  = 0.0f;
    float massKg            = 0.0f;
    float wheelbaseM        = 0.0f;
    float trackM            = 0.0f;
    float cgHeightM         = 0.0f;
    float boxCentreHeightM  = 0.0f;  // chassis box centre above flat ground, at rest
    const char* notes       = "";
};

// Names of the presets this build knows, for an inspector combo.
const char* const* PresetNames (size_t& count);

// Configure `rb` as the named vehicle. False (and `rb` untouched) for an unknown name.
// Wheel visual entities are NOT set here; the caller binds those.
bool ApplyPreset (const std::string& name, wi::scene::RigidBodyPhysicsComponent& rb);
PresetReference Reference (const std::string& name);

} // namespace st::vehicle
