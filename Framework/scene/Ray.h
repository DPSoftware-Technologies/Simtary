#pragma once
#include "Simtary.h"

namespace st {

// One raycast, one answer, whichever backend you want it from.
//
// The engine has two raycasts and they do NOT see the same world:
//
//   wi::scene::Scene::Intersects   walks mesh triangles and colliders. Hits anything
//                                  that is drawn, with a UV and a subset index, and
//                                  needs no physics body. Costs more on dense scenes.
//   wi::physics::Intersects        casts against Jolt bodies. Fast, and the only one
//                                  that agrees with what the simulation thinks is
//                                  solid - but it is blind to anything without a
//                                  rigid body or collider on it.
//
// A laser designator wants the first (it should stop on the visible surface, body or
// no body); a ballistics trace wants the second (it should agree with the physics).
// Mode::Both runs the two and keeps whichever is nearer, which is the honest answer
// when you do not know what the scene author attached.
struct RayHit {
	bool hit = false;
	wi::ecs::Entity entity = wi::ecs::INVALID_ENTITY;
	XMFLOAT3 position = XMFLOAT3(0, 0, 0);
	XMFLOAT3 normal = XMFLOAT3(0, 0, 1);
	float distance = 0.0f;

	// Mesh backend only; left at their defaults for a physics hit.
	XMFLOAT2 uv = XMFLOAT2(0, 0);
	int subsetIndex = -1;

	// Which backend produced this. Useful when Mode::Both is in play and the caller
	// cares (a physics hit has no UV to sample a decal from, for instance).
	enum class Source { None, Mesh, Physics };
	Source source = Source::None;

	bool IsValid() const { return hit; }
};

struct RayQuery {
	XMFLOAT3 origin = XMFLOAT3(0, 0, 0);
	XMFLOAT3 direction = XMFLOAT3(0, 0, 1); // normalised by Raycast if it is not already

	float minDistance = 0.0f;
	float maxDistance = 1000.0f;

	enum class Mode {
		Mesh,     // wi::scene::Scene::Intersects - visible geometry, no body required
		Physics,  // wi::physics::Intersects - Jolt bodies only
		Both,     // run both, keep the nearer hit
		None,     // no cast at all; the ray always runs its full length
	};
	Mode mode = Mode::Mesh;

	// Mesh backend only. FILTER_OPAQUE is the engine's own default for picking;
	// FILTER_ALL also catches transparents, water and colliders.
	uint32_t filterMask = wi::enums::FILTER_OPAQUE;
	uint32_t layerMask = ~0u;
	uint32_t lod = 0;

	// Never report a hit on this entity. A laser mounted on a rifle would otherwise
	// stop dead on the rifle's own barrel mesh; `minDistance` handles the simple case
	// but not a weapon that extends past the muzzle.
	wi::ecs::Entity ignoreEntity = wi::ecs::INVALID_ENTITY;
};

// Cast `query` against `scene`. A miss returns hit=false with `position` set to the
// end of the ray, so callers that just want somewhere to put a beam can use it
// without branching.
RayHit Raycast(const wi::scene::Scene& scene, const RayQuery& query);

// The world-space ray an entity points along. `forwardAxis` picks which local axis
// the ray travels down, using the same numbering as st::Projector::Forward
// (0 = +Z, 1 = -Z, 2 = -Y, 3 = +Y, 4 = +X, 5 = -X) so a component can share one
// metadata field between a projector, a laser, a mirror and a ray.
//
// The order is not tidy on purpose: 0-3 came first and scene metadata already stores
// those numbers, so X was appended rather than inserted where it would read better.
//
// Direction comes from the entity's world MATRIX, not from its quaternion:
// decomposing a mirrored or non-uniformly scaled transform loses the handedness, and
// the engine takes a light's direction from the matrix for the same reason.
// Returns false when the entity has no TransformComponent.
bool EntityRay(const wi::scene::Scene& scene, wi::ecs::Entity entity, int forwardAxis,
	XMFLOAT3& outOrigin, XMFLOAT3& outDirection);

// The local {forward, up} pair for a forwardAxis value. Shared by the laser, the ray
// component and the optics so they cannot disagree about which way "forward" is.
void LocalAxes(int forwardAxis, XMVECTOR& outForward, XMVECTOR& outUp);

} // namespace st
