#pragma once
#include "scene/Ray.h"
#include "stNativeComponent.h"

namespace st {

// The "Ray" native component: a raycast bolted to an entity from the editor.
//
//     NCI_0             = "sticRay"
//     NCA_0_forwardAxis = 0        (0 +Z, 1 -Z, 2 -Y, 3 +Y, 4 +X, 5 -X)
//     NCA_0_mode        = "mesh"   (mesh | physics | both | none)
//     NCA_0_maxDistance = 250.0
//
// The ray leaves the entity's own transform along `forwardAxis` and is re-cast every
// frame. Read what it found from anywhere on that entity:
//
//     if (const st::RayComponent* ray = GetComponent<st::RayComponent>()) {
//         if (ray->Hit().hit) { ... ray->Hit().entity ... }
//     }
//
// or from outside it, through the scene's native component manager:
//
//     auto* ray = static_cast<st::RayComponent*>(scene.nativeComponents.Get(
//         entity, wi::scene::GetNativeTypeID<st::RayComponent>()));
//
// This is the seam a laser sight, a rangefinder, an interaction prompt and a "what
// am I aiming at" HUD all share, so it is one component rather than four.
//
// It does not draw anything. Turn on `debugDraw` to get an engine debug line while
// you are placing it; for a beam you can actually see, attach "sticLaser"
// (Framework/render/LaserComponent.cpp) instead - the laser will pick up this
// component's result rather than casting a second time.
struct RayComponent : wi::scene::NativeComponent {
	// ── bound from NCA_<localID>_<name> ──────────────────────────────────────────
	int forwardAxis = 0;         // 0 +Z, 1 -Z, 2 -Y (spot light), 3 +Y, 4 +X, 5 -X
	std::string mode = "mesh";   // mesh | physics | both | none
	float maxDistance = 250.0f;
	float startOffset = 0.0f;    // metres along the ray before the cast begins
	bool ignoreSelf = true;      // never report a hit on the entity the ray leaves
	bool everyFrame = true;      // false casts only when Cast() is called by hand
	bool debugDraw = false;      // engine debug line, green on a hit, red on a miss

	// ── results ──────────────────────────────────────────────────────────────────
	const RayHit& Hit() const { return hit_; }
	const XMFLOAT3& Origin() const { return origin_; }
	const XMFLOAT3& Direction() const { return direction_; }

	// Distance to the hit, or maxDistance when there is none. What a rangefinder
	// reads out.
	float Distance() const { return hit_.distance; }

	// Cast now, outside the frame's own update. Returns the fresh result. Use it when
	// the ray has to be sampled at a specific moment (the frame a trigger is pulled)
	// rather than wherever Update happens to fall.
	const RayHit& Cast();

	// The mode string resolved to the enum. Public so a laser on the same entity can
	// inherit it.
	RayQuery::Mode ResolvedMode() const;

	void Start() override;
	void Update(float dt) override;
	void DrawDebug() override;

private:
	RayHit hit_;
	XMFLOAT3 origin_ = XMFLOAT3(0, 0, 0);
	XMFLOAT3 direction_ = XMFLOAT3(0, 0, 1);
};

} // namespace st
