#pragma once
// Breakable parts - the "stDamage" native component (DamageComponent.cpp), and the calls the game
// and tools reach it through.
//
//     NCI_3 = "stDamage"   NCA_3_parts = "TailRotor,Blade_1,Blade_2"   NCA_3_partHealth = 100
//
// Every listed part is a named node under the entity - a model's object, with whatever hangs
// under it. Damage lowers a part's health; at zero the part BREAKS:
//
//   - it leaves the hierarchy for a new root "Debris_<part>", which gets a dynamic rigid body
//     whose convex hull is built from the part's own vertices (a hidden proxy mesh under it)
//   - the body starts with the velocity the part had - the vehicle's, plus the spin of a rotor
//     it was on - and a small kick away from the vehicle
//   - a sibling stFlight is told what the aircraft lost (flight/FlightComponent.h, Damage),
//     going by the part's name: the tail rotor node, a main rotor blade, "Stabilizer", "Engine"
//   - the game hears about it through SetPartBrokenHandler (Milistry sets off a small explosion)
//
// Crashes (with a sibling stFlight, `crashDamage` on): at touchdown the sink rate just before
// decides - up to `hardLanding` m/s nothing, up to `crashSpeed` the gear and tail take damage in
// proportion, beyond it (or touching down faster than `crashSpeed` x 2.5 along the ground, or
// on its back) the vehicle is destroyed: every part breaks and the handler gets a `crash` event.
// Every frame each main blade's tip and the tail rotor are checked against the ground under
// them (stFlight's own ground: terrain and physics bodies): a blade that touches it shatters the
// rotor, the tail rotor touching it breaks off.
//
// A sibling stInput with a "BreakPart" action breaks the next intact part in the list - a
// debug key. Editor Stop puts everything back (debris is made during Play).
// Not yet: repair, saving the damage with the vehicle, networking it, hitting objects in the
// air (JSBSim only knows the ground).

#include "wiECS.h"
#include "wiMath.h"

#include <functional>
#include <string>

namespace wi::scene { struct Scene; }

namespace st::damage {

struct PartBrokenEvent {
    wi::scene::Scene* scene = nullptr;
    wi::ecs::Entity owner = wi::ecs::INVALID_ENTITY;   // the entity carrying the stDamage
    wi::ecs::Entity debris = wi::ecs::INVALID_ENTITY;  // the new root the part now hangs under
    std::string part;
    XMFLOAT3 position = XMFLOAT3(0, 0, 0);             // the part's centre, scene space
    bool engine = false;                               // the part was the engine: a bigger bang
    bool crash = false;                                // the whole vehicle was destroyed (part = "Crash")
};
using PartBrokenHandler = std::function<void (const PartBrokenEvent&)>;
void SetPartBrokenHandler (PartBrokenHandler handler);

// Damage every breakable part within reach of a blast of `kg` TNT at `at` (scene space).
void ApplyExplosion (wi::scene::Scene& scene, const XMFLOAT3& at, float kg);

// The stDamage on `owner`. false (with `error`) when there is none or no such intact part.
bool BreakPart (wi::scene::Scene& scene, wi::ecs::Entity owner, const std::string& part, std::string* error = nullptr);
bool DamagePart (wi::scene::Scene& scene, wi::ecs::Entity owner, const std::string& part, float amount,
                 std::string* error = nullptr);
// One line per part: "name  health  [broken]".
std::string Status (wi::scene::Scene& scene, wi::ecs::Entity owner);

} // namespace st::damage
