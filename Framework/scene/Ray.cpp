#include "scene/Ray.h"

#include <algorithm>
#include <cmath>

namespace st {

namespace {

// How many times the physics backend is allowed to step past an ignored body before
// giving up. Jolt's raycast returns only the closest hit and takes no ignore list, so
// skipping one means casting again from just past it.
constexpr int PHYSICS_IGNORE_RETRIES = 4;

XMFLOAT3 Along(const XMFLOAT3& origin, const XMFLOAT3& direction, float distance) {
	return XMFLOAT3(
		origin.x + direction.x * distance,
		origin.y + direction.y * distance,
		origin.z + direction.z * distance);
}

RayHit CastMesh(const wi::scene::Scene& scene, const RayQuery& query, const XMFLOAT3& direction) {
	RayHit out;

	const wi::primitive::Ray ray(query.origin, direction, query.minDistance, query.maxDistance);

	wi::scene::Scene::RayIntersectionResult result;

	if (query.ignoreEntity == wi::ecs::INVALID_ENTITY) {
		result = scene.Intersects(ray, query.filterMask, query.layerMask, query.lod);
	} else {
		// Intersects() returns only the closest hit and has no ignore list, so the
		// ignored entity would simply stop the ray. IntersectsAll costs more, which
		// is why it is only reached when an ignore is actually asked for.
		wi::vector<wi::scene::Scene::RayIntersectionResult> all;
		scene.IntersectsAll(all, ray, query.filterMask, query.layerMask, query.lod);
		for (const auto& candidate : all) {
			if (candidate.entity == query.ignoreEntity) continue;
			if (candidate.distance < result.distance) result = candidate;
		}
	}

	if (result.entity == wi::ecs::INVALID_ENTITY) return out;

	out.hit = true;
	out.entity = result.entity;
	out.position = result.position;
	out.normal = result.normal;
	out.distance = result.distance;
	out.uv = XMFLOAT2(result.uv.x, result.uv.y);
	out.subsetIndex = result.subsetIndex;
	out.source = RayHit::Source::Mesh;
	return out;
}

RayHit CastPhysics(const wi::scene::Scene& scene, const RayQuery& query, const XMFLOAT3& direction) {
	RayHit out;

	// Distance already walked from query.origin. Starts at minDistance and only ever
	// grows, so the retries below cannot loop on the same body.
	float travelled = query.minDistance;

	for (int attempt = 0; attempt <= PHYSICS_IGNORE_RETRIES; ++attempt) {
		const float remaining = query.maxDistance - travelled;
		if (remaining <= 0.0f) return out;

		const wi::primitive::Ray ray(Along(query.origin, direction, travelled), direction, 0.0f, remaining);

		const wi::physics::RayIntersectionResult result = wi::physics::Intersects(scene, ray);
		if (!result.IsValid()) return out;

		// Jolt reports the point, not the parameter, so the distance is measured back
		// against the ORIGINAL origin - otherwise every retry would restart the count.
		const XMVECTOR fromStart = XMVectorSubtract(XMLoadFloat3(&result.position), XMLoadFloat3(&query.origin));
		const float distance = XMVectorGetX(XMVector3Length(fromStart));

		if (result.entity != query.ignoreEntity) {
			out.hit = true;
			out.entity = result.entity;
			out.position = result.position;
			out.normal = result.normal;
			out.distance = distance;
			out.source = RayHit::Source::Physics;
			return out;
		}

		// Step just past the ignored body and cast again. The nudge has to clear the
		// surface it just landed on or the next cast hits the same face.
		travelled = distance + 0.01f;
	}

	return out;
}

} // namespace

void LocalAxes(int forwardAxis, XMVECTOR& outForward, XMVECTOR& outUp) {
	switch (forwardAxis) {
	case 1: // -Z
		outForward = XMVectorSet(0, 0, -1, 0);
		outUp = XMVectorSet(0, 1, 0, 0);
		break;
	case 2: // -Y, what a spot light projects along (SHCAM::init in wiRenderer.cpp)
		outForward = XMVectorSet(0, -1, 0, 0);
		outUp = XMVectorSet(0, 0, 1, 0);
		break;
	case 3: // +Y
		outForward = XMVectorSet(0, 1, 0, 0);
		outUp = XMVectorSet(0, 0, -1, 0);
		break;
	case 4: // +X
		outForward = XMVectorSet(1, 0, 0, 0);
		outUp = XMVectorSet(0, 1, 0, 0);
		break;
	case 5: // -X
		outForward = XMVectorSet(-1, 0, 0, 0);
		outUp = XMVectorSet(0, 1, 0, 0);
		break;
	case 0: // +Z, camera-like props (CameraComponent looks down local +Z)
	default:
		outForward = XMVectorSet(0, 0, 1, 0);
		outUp = XMVectorSet(0, 1, 0, 0);
		break;
	}
}

bool EntityRay(const wi::scene::Scene& scene, wi::ecs::Entity entity, int forwardAxis,
	XMFLOAT3& outOrigin, XMFLOAT3& outDirection) {
	const wi::scene::TransformComponent* transform = scene.transforms.GetComponent(entity);
	if (transform == nullptr) return false;

	XMVECTOR localForward, localUp;
	LocalAxes(forwardAxis, localForward, localUp);

	const XMMATRIX world = XMLoadFloat4x4(&transform->world);
	const XMVECTOR forward = XMVector3Normalize(XMVector3TransformNormal(localForward, world));

	outOrigin = transform->GetPosition();
	XMStoreFloat3(&outDirection, forward);
	return true;
}

RayHit Raycast(const wi::scene::Scene& scene, const RayQuery& query) {
	RayHit out;

	XMVECTOR direction = XMLoadFloat3(&query.direction);
	if (XMVectorGetX(XMVector3LengthSq(direction)) < 1e-12f) {
		// A zero direction would make every backend answer nonsense; report the miss
		// rather than let it propagate into a beam pointing at the world origin.
		out.position = query.origin;
		return out;
	}
	direction = XMVector3Normalize(direction);

	XMFLOAT3 unit;
	XMStoreFloat3(&unit, direction);

	const float range = std::max(query.maxDistance, query.minDistance);

	switch (query.mode) {
	case RayQuery::Mode::Mesh:
		out = CastMesh(scene, query, unit);
		break;
	case RayQuery::Mode::Physics:
		out = CastPhysics(scene, query, unit);
		break;
	case RayQuery::Mode::Both: {
		const RayHit mesh = CastMesh(scene, query, unit);
		const RayHit physics = CastPhysics(scene, query, unit);
		if (mesh.hit && physics.hit) {
			out = physics.distance < mesh.distance ? physics : mesh;
		} else {
			out = mesh.hit ? mesh : physics;
		}
		break;
	}
	case RayQuery::Mode::None:
	default:
		break;
	}

	if (!out.hit) {
		// A miss still needs somewhere to put the end of the beam, so hand back the
		// far end of the ray rather than an uninitialised position.
		out.position = Along(query.origin, unit, range);
		out.distance = range;
		out.normal = XMFLOAT3(-unit.x, -unit.y, -unit.z);
	}

	return out;
}

} // namespace st
