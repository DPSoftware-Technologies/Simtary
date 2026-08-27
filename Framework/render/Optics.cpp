#include "render/Optics.h"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace st {

namespace {

// Set by Init(), cleared by the destructor - st::App owns the real system and this is
// how a scene or a native component reaches it.
OpticsSystem* g_instance = nullptr;

// Nudge used when a leg starts on the surface it just bounced off, so the next cast
// does not immediately find the same aperture again.
constexpr float BOUNCE_EPSILON = 0.001f;

// Hard ceiling on legs in one traced path. maxBounces bounds a single branch, but a
// dichroic splits, and splits compound - two dichroics facing each other would fill
// memory before either budget noticed. This is the backstop that makes that
// impossible rather than merely unlikely.
constexpr size_t MAX_LEGS = 64;

float Luminance(const XMFLOAT3& c) {
	return c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f;
}

// Deterministic pseudo-noise from a world position, quantised to a centimetre. A
// scuffed mirror should look scuffed in a FIXED way: seeding from the frame counter
// instead would make a stationary beam crawl across the wall.
float SurfaceNoise(const XMFLOAT3& p, float salt) {
	const float x = std::floor(p.x * 100.0f);
	const float y = std::floor(p.y * 100.0f);
	const float z = std::floor(p.z * 100.0f);
	const float s = std::sin(x * 12.9898f + y * 78.233f + z * 37.719f + salt) * 43758.5453f;
	return (s - std::floor(s)) * 2.0f - 1.0f; // -1 .. 1
}

XMFLOAT3 Mul(const XMFLOAT3& a, const XMFLOAT3& b) {
	return XMFLOAT3(a.x * b.x, a.y * b.y, a.z * b.z);
}

XMFLOAT3 Along(const XMFLOAT3& origin, const XMFLOAT3& direction, float distance) {
	return XMFLOAT3(
		origin.x + direction.x * distance,
		origin.y + direction.y * distance,
		origin.z + direction.z * distance);
}

// Which element the beam meets first, if any.
struct OpticHit {
	enum class Kind { None, Mirror, Lens } kind = Kind::None;
	size_t index = 0;
	float distance = 0.0f;
	XMFLOAT3 point = XMFLOAT3(0, 0, 0);
};

} // namespace

bool ResolveOpticSurface(const wi::scene::Scene& scene, OpticSurface& surface) {
	XMMATRIX orientation;

	if (surface.followEntity != wi::ecs::INVALID_ENTITY) {
		const wi::scene::TransformComponent* transform = scene.transforms.GetComponent(surface.followEntity);
		if (transform == nullptr) return false;
		surface.position = transform->GetPosition();
		surface.rotation = transform->GetRotation();
		// From the matrix, not the quaternion: decomposing a mirrored transform loses
		// the handedness, and a mirrored mirror facing the wrong way is exactly the
		// bug that is impossible to find by eye.
		orientation = XMLoadFloat4x4(&transform->world);
	} else {
		orientation = XMMatrixRotationQuaternion(XMLoadFloat4(&surface.rotation));
	}

	XMVECTOR localForward, localUp;
	LocalAxes(surface.normalAxis, localForward, localUp);

	const XMVECTOR normal = XMVector3Normalize(XMVector3TransformNormal(localForward, orientation));
	XMVECTOR up = XMVector3Normalize(XMVector3TransformNormal(localUp, orientation));

	XMVECTOR tangent = XMVector3Cross(up, normal);
	if (XMVectorGetX(XMVector3LengthSq(tangent)) < 1e-8f) {
		// up ended up parallel to the normal (a degenerate transform). Any
		// perpendicular will do - the aperture is what it measures, and a square one
		// only cares that the two axes are orthogonal.
		tangent = XMVector3Cross(normal, XMVectorSet(0, 0, 1, 0));
		if (XMVectorGetX(XMVector3LengthSq(tangent)) < 1e-8f) {
			tangent = XMVector3Cross(normal, XMVectorSet(1, 0, 0, 0));
		}
	}
	tangent = XMVector3Normalize(tangent);
	const XMVECTOR bitangent = XMVector3Normalize(XMVector3Cross(normal, tangent));

	XMStoreFloat3(&surface.normal, normal);
	XMStoreFloat3(&surface.tangent, tangent);
	XMStoreFloat3(&surface.bitangent, bitangent);

	// ── fit the plane and the aperture to the mesh ───────────────────────────────
	// After the axes, because the extents are measured ALONG them.
	if (surface.fitToMesh && surface.followEntity != wi::ecs::INVALID_ENTITY) {
		const wi::scene::ObjectComponent* object = scene.objects.GetComponent(surface.followEntity);
		const wi::scene::MeshComponent* mesh = object != nullptr
			? scene.meshes.GetComponent(object->meshID)
			: nullptr;

		if (mesh != nullptr) {
			const XMFLOAT3 localCenter = mesh->aabb.getCenter();
			const XMFLOAT3 localHalf = mesh->aabb.getHalfWidth();

			// Extent of the transformed box along each surface axis. Summing the three
			// projected half-axes is the standard OBB support width: it stays correct
			// for a rotated or non-uniformly scaled mesh, where taking one axis alone
			// would under-report and the world AABB would over-report.
			const XMVECTOR ax = XMVector3TransformNormal(XMVectorSet(localHalf.x, 0, 0, 0), orientation);
			const XMVECTOR ay = XMVector3TransformNormal(XMVectorSet(0, localHalf.y, 0, 0), orientation);
			const XMVECTOR az = XMVector3TransformNormal(XMVectorSet(0, 0, localHalf.z, 0), orientation);

			const auto Extent = [&](XMVECTOR axis) {
				return std::abs(XMVectorGetX(XMVector3Dot(ax, axis))) +
					std::abs(XMVectorGetX(XMVector3Dot(ay, axis))) +
					std::abs(XMVectorGetX(XMVector3Dot(az, axis)));
			};

			const float u = Extent(tangent);
			const float v = Extent(bitangent);

			// The plane's POINT, on the mesh's FRONT FACE - not the entity origin, and
			// not the middle of the mesh either.
			//
			// The origin is wrong whenever the model was authored with an offset pivot.
			// The middle is wrong the moment the mirror is a box rather than a plane:
			// reflecting at the mid-plane starts the outgoing beam INSIDE the solid,
			// where it travels half a thickness and dies on the inside of the mesh's
			// own front face. The bounce happens and nothing is visible, which reads
			// exactly like the mirror not working - and mysteriously comes right if you
			// flatten the mesh to zero thickness.
			//
			// Front face means +normal, so point the normal axis out of the reflective
			// side. A double-sided mirror with real thickness is ambiguous by nature;
			// the leg leaving an element also ignores that element's own mesh, which is
			// what keeps the other side working.
			const XMVECTOR center = XMVector3Transform(XMLoadFloat3(&localCenter), orientation);
			const float halfThickness = Extent(normal);
			XMStoreFloat3(&surface.position, XMVectorAdd(center, XMVectorScale(normal, halfThickness)));

			// A circular aperture is INSCRIBED in the mesh rather than around it, so a
			// round mirror never claims reflective area the glass does not have.
			surface.halfExtent = surface.circular
				? XMFLOAT2(std::min(u, v), std::min(u, v))
				: XMFLOAT2(u, v);
		}
	}

	return true;
}

bool IntersectOpticSurface(const OpticSurface& surface, const XMFLOAT3& origin, const XMFLOAT3& direction,
	float minDistance, float maxDistance, float& distance, XMFLOAT3& point) {
	distance = 0.0f;
	point = origin;

	const XMVECTOR n = XMLoadFloat3(&surface.normal);
	const XMVECTOR d = XMLoadFloat3(&direction);

	const float denominator = XMVectorGetX(XMVector3Dot(d, n));
	if (std::abs(denominator) < 1e-6f) return false; // running along the surface

	const XMVECTOR o = XMLoadFloat3(&origin);
	const XMVECTOR c = XMLoadFloat3(&surface.position);

	const float t = XMVectorGetX(XMVector3Dot(XMVectorSubtract(c, o), n)) / denominator;
	if (t < minDistance || t > maxDistance) return false;

	const XMVECTOR p = XMVectorAdd(o, XMVectorScale(d, t));
	const XMVECTOR local = XMVectorSubtract(p, c);

	const float u = XMVectorGetX(XMVector3Dot(local, XMLoadFloat3(&surface.tangent)));
	const float v = XMVectorGetX(XMVector3Dot(local, XMLoadFloat3(&surface.bitangent)));

	// The beam reached the plane. Whether or not it lands inside the aperture, record
	// by how much - "the beam is crossing your mirror 0.4 m outside the disc" is the
	// answer to almost every "my mirror does nothing" report, and it is invisible
	// from the outside otherwise.
	float margin;
	if (surface.circular) {
		const float radius = std::max(surface.halfExtent.x, 1e-4f);
		margin = std::sqrt(u * u + v * v) - radius;
	} else {
		margin = std::max(
			std::abs(u) - std::max(surface.halfExtent.x, 1e-4f),
			std::abs(v) - std::max(surface.halfExtent.y, 1e-4f));
	}

	if (margin > 0.0f) {
		if (surface.lastMissMargin < 0.0f || margin < surface.lastMissMargin) {
			surface.lastMissMargin = margin;
		}
		return false;
	}

	distance = t;
	XMStoreFloat3(&point, p);

	surface.hitsThisFrame++;
	surface.lastHitDistance = t;
	return true;
}

const char* const LENS_TYPE_ITEMS =
	"Spherical\0Cylindrical\0Toric\0Aspheric\0Axicon\0Prism\0Window\0";

Lens::Type ParseLensType(const std::string& value) {
	if (value == "cylindrical") return Lens::Type::Cylindrical;
	if (value == "toric") return Lens::Type::Toric;
	if (value == "aspheric") return Lens::Type::Aspheric;
	if (value == "axicon") return Lens::Type::Axicon;
	if (value == "prism") return Lens::Type::Prism;
	if (value == "window") return Lens::Type::Window;
	return Lens::Type::Spherical;
}

const char* LensTypeName(Lens::Type type) {
	switch (type) {
	case Lens::Type::Cylindrical: return "cylindrical";
	case Lens::Type::Toric: return "toric";
	case Lens::Type::Aspheric: return "aspheric";
	case Lens::Type::Axicon: return "axicon";
	case Lens::Type::Prism: return "prism";
	case Lens::Type::Window: return "window";
	case Lens::Type::Spherical:
	default: return "spherical";
	}
}

bool LensTypeFields(Lens& lens) {
	bool dirty = false;

	switch (lens.type) {
	case Lens::Type::Window:
		ImGui::TextDisabled("No power. Aperture, tint and spread only.");
		break;

	case Lens::Type::Prism:
		dirty |= ImGui::DragFloat2("Deviation", &lens.prismDeviation.x, 0.002f, -1.0f, 1.0f, "%.4f rad");
		ImGui::TextDisabled("Constant bend wherever the beam lands - never focuses.");
		break;

	case Lens::Type::Axicon:
		dirty |= ImGui::DragFloat("Cone angle", &lens.axiconAngle, 0.002f, -1.0f, 1.0f, "%.4f rad");
		ImGui::TextDisabled("Fixed angle towards the axis at every radius - makes a ring, not a point.");
		break;

	case Lens::Type::Cylindrical:
		dirty |= ImGui::DragFloat("Focal length", &lens.focalLength, 0.01f, -50.0f, 50.0f, "%.3f m");
		ImGui::TextDisabled("Power on the tangent axis only - focuses to a line.");
		break;

	case Lens::Type::Toric:
		dirty |= ImGui::DragFloat("Focal length X", &lens.focalLength, 0.01f, -50.0f, 50.0f, "%.3f m");
		dirty |= ImGui::DragFloat("Focal length Y", &lens.focalLengthY, 0.01f, -50.0f, 50.0f, "%.3f m");
		ImGui::TextDisabled("Independent power per axis - an astigmatic lens.");
		break;

	case Lens::Type::Aspheric:
		dirty |= ImGui::DragFloat("Focal length", &lens.focalLength, 0.01f, -50.0f, 50.0f, "%.3f m");
		dirty |= ImGui::SliderFloat("Asphericity", &lens.asphericity, -1.0f, 1.0f);
		ImGui::TextDisabled("Fractional change in power at the rim. Negative corrects aberration.");
		break;

	case Lens::Type::Spherical:
	default:
		dirty |= ImGui::DragFloat("Focal length", &lens.focalLength, 0.01f, -50.0f, 50.0f, "%.3f m");
		if (lens.focalLength > 0.0f) {
			ImGui::TextDisabled("Converging: a parallel beam crosses the axis %.2f m past it.", lens.focalLength);
		} else if (lens.focalLength < 0.0f) {
			ImGui::TextDisabled("Diverging.");
		} else {
			ImGui::TextDisabled("Zero focal length behaves as a plain window.");
		}
		break;
	}

	return dirty;
}

void OpticDiagnostics(const OpticSurface& surface) {
	ImGui::Separator();

	if (!surface.resolved) {
		ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
			"Unresolved - followEntity %llu has no TransformComponent.",
			(unsigned long long)surface.followEntity);
		return;
	}

	// Which way it faces is the first thing to check and the hardest to see by eye.
	ImGui::TextDisabled("Normal %.2f %.2f %.2f", surface.normal.x, surface.normal.y, surface.normal.z);

	if (!surface.enabled) {
		ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Disabled - beams pass straight through.");
		return;
	}

	if (surface.hitsThisFrame > 0) {
		ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "Beam hit: %d this frame, nearest %.2f m",
			surface.hitsThisFrame, surface.lastHitDistance);

		if (surface.reflectionsThisFrame > 0) {
			ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "%d left again - the element is working.",
				surface.reflectionsThisFrame);
		}
		if (surface.terminationsThisFrame > 0) {
			// The failure that looks exactly like success: a spot on the glass and no
			// beam coming off it.
			ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
				"%d STOPPED here - out of bounces, or too dim to continue.",
				surface.terminationsThisFrame);
			ImGui::TextDisabled("Raise the laser's Max bounces, or its Min throughput is too high.");
		}
		if (surface.reflectionsThisFrame == 0 && surface.terminationsThisFrame == 0) {
			// Hit the aperture but never got as far as being acted on, which only
			// happens when another element in front of it took the beam first.
			ImGui::TextDisabled("Crossed the aperture but something nearer took the beam.");
		}
		return;
	}

	if (surface.lastMissMargin >= 0.0f) {
		// The single most common cause of "my mirror does nothing": the beam reaches
		// the plane and lands off the disc, so the mesh stops it instead.
		ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
			"Beam crossed the plane %.2f m OUTSIDE the aperture.", surface.lastMissMargin);
		ImGui::TextDisabled("Raise the radius / half extent past that, or move the element.");
		return;
	}

	ImGui::TextDisabled("No beam reached this element's plane at all.");
}

OpticsSystem& OpticsSystem::Get() {
	if (g_instance != nullptr) return *g_instance;
	// No app yet. Hand back something inert rather than a null reference - it holds
	// no elements, so every trace through it is a straight line.
	static OpticsSystem fallback;
	return fallback;
}

OpticsSystem::~OpticsSystem() {
	if (g_instance == this) g_instance = nullptr;
}

void OpticsSystem::Init() {
	g_instance = this;
}

OpticsSystem::ID OpticsSystem::AddMirror(const Mirror& mirror) {
	MirrorEntry& entry = mirrors_.emplace_back();
	entry.id = nextID_++;
	entry.mirror = mirror;
	return entry.id;
}

Mirror* OpticsSystem::FindMirror(ID id) {
	for (auto& entry : mirrors_) {
		if (entry.id == id) return &entry.mirror;
	}
	return nullptr;
}

const Mirror* OpticsSystem::FindMirror(ID id) const {
	for (const auto& entry : mirrors_) {
		if (entry.id == id) return &entry.mirror;
	}
	return nullptr;
}

void OpticsSystem::RemoveMirror(ID id) {
	for (size_t i = 0; i < mirrors_.size(); ++i) {
		if (mirrors_[i].id == id) {
			mirrors_.erase(mirrors_.begin() + i);
			return;
		}
	}
}

OpticsSystem::ID OpticsSystem::AddLens(const Lens& lens) {
	LensEntry& entry = lenses_.emplace_back();
	entry.id = nextID_++;
	entry.lens = lens;
	return entry.id;
}

Lens* OpticsSystem::FindLens(ID id) {
	for (auto& entry : lenses_) {
		if (entry.id == id) return &entry.lens;
	}
	return nullptr;
}

const Lens* OpticsSystem::FindLens(ID id) const {
	for (const auto& entry : lenses_) {
		if (entry.id == id) return &entry.lens;
	}
	return nullptr;
}

void OpticsSystem::RemoveLens(ID id) {
	for (size_t i = 0; i < lenses_.size(); ++i) {
		if (lenses_[i].id == id) {
			lenses_.erase(lenses_.begin() + i);
			return;
		}
	}
}

const Mirror* OpticsSystem::MirrorAt(size_t index) const {
	if (index >= mirrors_.size()) return nullptr;
	return &mirrors_[index].mirror;
}

const Lens* OpticsSystem::LensAt(size_t index) const {
	if (index >= lenses_.size()) return nullptr;
	return &lenses_[index].lens;
}

void OpticsSystem::Clear() {
	mirrors_.clear();
	lenses_.clear();
}

void OpticsSystem::Update(const wi::scene::Scene& scene) {
	// `resolved`, not `enabled`: a frame in which the followed entity is missing must
	// not latch the element off permanently. Trace skips an unresolved element for
	// that frame and picks it up again the moment the transform is back - and the
	// inspector can tell "switched off" apart from "cannot find its entity".
	auto refresh = [&](OpticSurface& surface) {
		surface.hitsThisFrame = 0;
		surface.reflectionsThisFrame = 0;
		surface.terminationsThisFrame = 0;
		surface.lastMissMargin = -1.0f;
		surface.resolved = ResolveOpticSurface(scene, surface);
	};

	for (auto& entry : mirrors_) refresh(entry.mirror.surface);
	for (auto& entry : lenses_) refresh(entry.lens.surface);
}

void OpticsSystem::Trace(const wi::scene::Scene& scene, const BeamTraceDesc& desc, BeamPath& out) const {
	out.legs.clear();
	out.terminals.clear();
	out.hit = RayHit();
	out.exhaustedBounces = false;

	XMVECTOR startDirection = XMLoadFloat3(&desc.direction);
	if (XMVectorGetX(XMVector3LengthSq(startDirection)) < 1e-12f) return;
	startDirection = XMVector3Normalize(startDirection);

	// maxBounces == 0 is documented as "ignore the optics entirely", which is also
	// what a caller gets when the master switch is off.
	const bool useOptics = enabled && desc.maxBounces > 0;
	const float minThroughput = std::max(desc.minThroughput, 0.0f);

	// One branch of the walk. A plain beam has exactly one of these; a dichroic pushes
	// a second and the loop picks it up once the first has finished.
	struct Walk {
		XMFLOAT3 origin = XMFLOAT3(0, 0, 0);
		XMFLOAT3 direction = XMFLOAT3(0, 0, 1);
		float remaining = 0.0f;
		XMFLOAT3 throughput = XMFLOAT3(1, 1, 1);
		float radiusScale = 1.0f;
		float divergence = 0.0f;
		int bounce = 0;
		// The element this branch has just left, if any. Its mesh must not stop the
		// very leg that is leaving it - see the query below.
		wi::ecs::Entity leaving = wi::ecs::INVALID_ENTITY;
	};

	wi::vector<Walk> pending;
	{
		Walk& first = pending.emplace_back();
		first.origin = desc.origin;
		XMStoreFloat3(&first.direction, startDirection);
		first.remaining = std::max(desc.maxDistance, 0.0f);
	}

	int splits = 0;

	while (!pending.empty()) {
		// Depth first, and that ordering is load-bearing: the reflected chain finishes
		// before the transmitted half starts, so terminals[0] is the primary beam's
		// endpoint and a designator reading Path(id)->hit still gets the beam the
		// player is aiming, not whichever half a dichroic happened to pass.
		const Walk walk = pending.back();
		pending.pop_back();

		XMFLOAT3 origin = walk.origin;
		XMFLOAT3 dir = walk.direction;
		float remaining = walk.remaining;
		XMFLOAT3 throughput = walk.throughput;
		float radiusScale = walk.radiusScale;
		float divergence = walk.divergence;
		wi::ecs::Entity leaving = walk.leaving;

		for (int bounce = walk.bounce;; ++bounce) {
			if (remaining <= 1e-4f) break;
			if (out.legs.size() >= MAX_LEGS) break; // splitting cannot run away

			// -- where the scene stops the beam --------------------------------
			RayQuery query;
			query.origin = origin;
			query.direction = dir;
			// Only the legs AFTER the first start on a surface; the first one starts
			// at the emitter, where a nudge would just shorten the beam.
			query.minDistance = bounce == 0 ? 0.0f : BOUNCE_EPSILON;
			query.maxDistance = remaining;
			query.mode = desc.mode;
			query.filterMask = desc.filterMask;
			query.layerMask = desc.layerMask;
			// A beam leaving a mirror is leaving the MESH that makes that mirror
			// visible, and for anything thicker than a plane it is starting inside it.
			// Without this exemption the outgoing leg travels half a thickness and dies
			// on the inside of the glass it just bounced off: the reflection is real,
			// the beam is real, and none of it is visible. Only the leg immediately
			// after the bounce is exempt, so the beam can still stop on that mesh later.
			query.ignoreEntity = leaving != wi::ecs::INVALID_ENTITY ? leaving : desc.ignoreEntity;

			const RayHit geometry = Raycast(scene, query);
			leaving = wi::ecs::INVALID_ENTITY;
			const float geometryDistance = geometry.hit ? geometry.distance : remaining;

			// -- the nearest optical element in front of it ---------------------
			//
			// The hard part is not finding the element, it is deciding whether the
			// element or the geometry comes first - because for a mirror they are THE
			// SAME OBJECT. A mirror is a bare plane; what makes it visible is a mesh
			// in the same place, on the same entity. Comparing their two distances
			// compares ray-vs-triangle against ray-vs-plane, which disagree by however
			// far the mesh surface sits from its own origin: a hair for a flat plane,
			// centimetres for a mirror inside a frame, and the sign of the difference
			// decides whether the beam bounces or stops dead.
			//
			// So the ownership test comes first and is exact: if the geometry hit is
			// on the element's OWN entity and the ray also crosses that element's
			// aperture, the element wins outright, at any distance. `opticBias` is
			// only the fallback for glass that belongs to a different entity than the
			// component.
			OpticHit optic;
			if (useOptics) {
				const float minDistance = bounce == 0 ? 0.0f : BOUNCE_EPSILON;
				const float biasedLimit = geometryDistance + std::max(desc.opticBias, 0.0f);

				const auto LimitFor = [&](const OpticSurface& surface) {
					const bool ownsTheGeometry = geometry.hit &&
						surface.followEntity != wi::ecs::INVALID_ENTITY &&
						geometry.entity == surface.followEntity;
					return ownsTheGeometry ? remaining : biasedLimit;
				};

				float best = FLT_MAX;

				for (size_t i = 0; i < mirrors_.size(); ++i) {
					const Mirror& mirror = mirrors_[i].mirror;
					if (!mirror.surface.enabled || !mirror.surface.resolved) continue;
					// A mirror is silvered on one face: the beam has to arrive against
					// the normal for the front face to be the one it meets.
					if (!mirror.doubleSided) {
						const float facing = dir.x * mirror.surface.normal.x +
							dir.y * mirror.surface.normal.y + dir.z * mirror.surface.normal.z;
						if (facing >= 0.0f) continue;
					}
					float distance;
					XMFLOAT3 point;
					if (!IntersectOpticSurface(mirror.surface, origin, dir, minDistance,
							LimitFor(mirror.surface), distance, point)) {
						continue;
					}
					if (distance >= best) continue;
					optic.kind = OpticHit::Kind::Mirror;
					optic.index = i;
					optic.distance = distance;
					optic.point = point;
					best = distance;
				}

				for (size_t i = 0; i < lenses_.size(); ++i) {
					const Lens& lens = lenses_[i].lens;
					if (!lens.surface.enabled || !lens.surface.resolved) continue;
					float distance;
					XMFLOAT3 point;
					if (!IntersectOpticSurface(lens.surface, origin, dir, minDistance,
							LimitFor(lens.surface), distance, point)) {
						continue;
					}
					if (distance >= best) continue;
					optic.kind = OpticHit::Kind::Lens;
					optic.index = i;
					optic.distance = distance;
					optic.point = point;
					best = distance;
				}
			}

			// -- nothing optical in the way: this branch ends here ---------------
			if (optic.kind == OpticHit::Kind::None) {
				BeamLeg& leg = out.legs.emplace_back();
				leg.start = origin;
				leg.end = geometry.hit ? geometry.position : Along(origin, dir, remaining);
				leg.direction = dir;
				leg.length = geometryDistance;
				leg.throughput = throughput;
				leg.radiusScale = radiusScale;
				leg.divergence = divergence;
				leg.termination = geometry.hit ? BeamLeg::Termination::Surface
											   : BeamLeg::Termination::Range;

				if (geometry.hit) {
					BeamTerminal& terminal = out.terminals.emplace_back();
					terminal.hit = geometry;
					terminal.throughput = throughput;
				}
				break;
			}

			// -- the beam meets an element --------------------------------------
			const Mirror* mirror = optic.kind == OpticHit::Kind::Mirror
				? &mirrors_[optic.index].mirror : nullptr;
			const Lens* lens = optic.kind == OpticHit::Kind::Lens
				? &lenses_[optic.index].lens : nullptr;

			{
				BeamLeg& leg = out.legs.emplace_back();
				leg.start = origin;
				leg.end = optic.point;
				leg.direction = dir;
				leg.length = optic.distance;
				leg.throughput = throughput;
				leg.radiusScale = radiusScale;
				leg.divergence = divergence;
				leg.termination = mirror != nullptr ? BeamLeg::Termination::Mirror
												   : BeamLeg::Termination::Lens;
			}

			remaining -= optic.distance;

			if (bounce >= desc.maxBounces) {
				// Out of budget. The beam stops ON the element, which is the honest
				// picture - a mirror that swallows the beam reads as "too many
				// bounces", where letting it pass through would read as "the mirror is
				// broken".
				out.exhaustedBounces = true;
				if (mirror != nullptr) mirror->surface.terminationsThisFrame++;
				else if (lens != nullptr) lens->surface.terminationsThisFrame++;

				BeamTerminal& terminal = out.terminals.emplace_back();
				terminal.hit.position = optic.point;
				terminal.hit.distance = optic.distance;
				terminal.hit.normal = XMFLOAT3(-dir.x, -dir.y, -dir.z);
				terminal.throughput = throughput;
				break;
			}

			// -- dichroic: one beam in, two out ---------------------------------
			// Pushed BEFORE the reflection touches `throughput` and `dir`, because the
			// transmitted half carries what ARRIVED, not what bounced.
			if (mirror != nullptr && mirror->dichroic && splits < desc.maxSplits) {
				XMFLOAT3 transmitted = Mul(throughput, mirror->transmitTint);
				const float transmittance = std::max(mirror->transmittance, 0.0f);
				transmitted.x *= transmittance;
				transmitted.y *= transmittance;
				transmitted.z *= transmittance;

				if (Luminance(transmitted) >= minThroughput && remaining > 1e-4f) {
					Walk& branch = pending.emplace_back();
					branch.origin = optic.point;
					branch.direction = dir; // straight on, undeviated
					branch.remaining = remaining;
					branch.throughput = transmitted;
					branch.radiusScale = radiusScale;
					branch.divergence = divergence;
					branch.bounce = bounce + 1;
					branch.leaving = mirror->surface.followEntity;
					++splits;
				}
			}

			const XMVECTOR d = XMLoadFloat3(&dir);
			XMVECTOR next;

			if (mirror != nullptr) {
				const XMVECTOR n = XMLoadFloat3(&mirror->surface.normal);
				next = XMVector3Reflect(d, n);

				if (mirror->scatter > 0.0f) {
					// Tilt about the two in-plane axes. Deterministic in the impact
					// point, so a beam held still on a scuffed mirror stays still.
					const XMVECTOR t = XMLoadFloat3(&mirror->surface.tangent);
					const XMVECTOR b = XMLoadFloat3(&mirror->surface.bitangent);
					const float a0 = mirror->scatter * SurfaceNoise(optic.point, 0.0f);
					const float a1 = mirror->scatter * SurfaceNoise(optic.point, 17.0f);
					next = XMVector3TransformNormal(next, XMMatrixRotationAxis(t, a0));
					next = XMVector3TransformNormal(next, XMMatrixRotationAxis(b, a1));
				}

				throughput = Mul(throughput, mirror->tint);
				const float reflectance = std::max(mirror->reflectance, 0.0f);
				throughput.x *= reflectance;
				throughput.y *= reflectance;
				throughput.z *= reflectance;
			} else {
				// Paraxial thin lens. Slopes are measured against the optical axis
				// taken in the direction the beam is travelling, so a lens works the
				// same from either side - which is what a thin lens actually does.
				const XMVECTOR n = XMLoadFloat3(&lens->surface.normal);
				const XMVECTOR t = XMLoadFloat3(&lens->surface.tangent);
				const XMVECTOR b = XMLoadFloat3(&lens->surface.bitangent);

				const float axial = XMVectorGetX(XMVector3Dot(d, n));
				const float side = axial >= 0.0f ? 1.0f : -1.0f;
				const XMVECTOR axis = XMVectorScale(n, side);
				const float alongAxis = std::abs(axial);

				if (alongAxis < 1e-4f) {
					// Grazing the element: there is no meaningful paraxial answer, so
					// let it pass rather than fold the beam back on itself.
					next = d;
				} else {
					const XMVECTOR offset = XMVectorSubtract(
						XMLoadFloat3(&optic.point), XMLoadFloat3(&lens->surface.position));
					const float u = XMVectorGetX(XMVector3Dot(offset, t));
					const float v = XMVectorGetX(XMVector3Dot(offset, b));

					float slopeU = XMVectorGetX(XMVector3Dot(d, t)) / alongAxis;
					float slopeV = XMVectorGetX(XMVector3Dot(d, b)) / alongAxis;

					// Every type is the same ray transfer with a different deviation
					// term: the outgoing slope is the incoming slope minus some
					// function of where on the aperture the beam landed. Slope, not
					// angle, so composition stays linear and a stack of elements
					// behaves like the product of their matrices - which is what makes
					// a telescope out of two lenses.
					const float f = lens->focalLength;
					const float power = std::abs(f) > 1e-4f ? 1.0f / f : 0.0f;

					switch (lens->type) {
					case Lens::Type::Window:
						break;

					case Lens::Type::Cylindrical:
						// Power on the tangent axis only. The bitangent axis passes
						// through untouched, which is what makes a line, not a dot.
						slopeU -= u * power;
						break;

					case Lens::Type::Toric: {
						const float fy = lens->focalLengthY;
						slopeU -= u * power;
						slopeV -= v * (std::abs(fy) > 1e-4f ? 1.0f / fy : 0.0f);
						break;
					}

					case Lens::Type::Aspheric: {
						// Power varies with radius. Normalised against the aperture so
						// the coefficient means the same thing whatever size the
						// element is: asphericity is the FRACTIONAL change at the rim.
						const float aperture = std::max(
							lens->surface.circular
								? lens->surface.halfExtent.x
								: std::max(lens->surface.halfExtent.x, lens->surface.halfExtent.y),
							1e-4f);
						const float r2 = (u * u + v * v) / (aperture * aperture);
						const float shaped = power * (1.0f + lens->asphericity * r2);
						slopeU -= u * shaped;
						slopeV -= v * shaped;
						break;
					}

					case Lens::Type::Axicon: {
						// Conical: a FIXED angle towards the axis regardless of how far
						// out the beam landed. That is what makes a ring instead of a
						// focus - every radius converges at a different distance.
						const float r = std::sqrt(u * u + v * v);
						if (r > 1e-5f) {
							const float deviation = std::tan(lens->axiconAngle);
							slopeU -= (u / r) * deviation;
							slopeV -= (v / r) * deviation;
						}
						break;
					}

					case Lens::Type::Prism:
						// Constant deviation, independent of where it lands - so a
						// prism moves the beam without ever focusing it.
						slopeU -= std::tan(lens->prismDeviation.x);
						slopeV -= std::tan(lens->prismDeviation.y);
						break;

					case Lens::Type::Spherical:
					default:
						slopeU -= u * power;
						slopeV -= v * power;
						break;
					}

					next = XMVectorAdd(axis,
						XMVectorAdd(XMVectorScale(t, slopeU), XMVectorScale(b, slopeV)));
				}

				throughput = Mul(throughput, lens->tint);
				const float transmittance = std::max(lens->transmittance, 0.0f);
				throughput.x *= transmittance;
				throughput.y *= transmittance;
				throughput.z *= transmittance;

				radiusScale *= std::max(lens->beamScale, 0.01f);
				divergence += std::max(lens->spread, 0.0f);
			}

			next = XMVector3Normalize(next);
			XMStoreFloat3(&dir, next);
			origin = optic.point;

			const OpticSurface& usedSurface = mirror != nullptr ? mirror->surface : lens->surface;
			leaving = usedSurface.followEntity;
			if (Luminance(throughput) >= minThroughput) {
				usedSurface.reflectionsThisFrame++;
			} else {
				usedSurface.terminationsThisFrame++;
			}

			if (Luminance(throughput) < minThroughput) {
				// Too dim to be worth another leg, and another leg is what would eat
				// the segment budget a brighter beam still needs. No terminal: there
				// is nothing bright enough there to put a spot on.
				break;
			}
		}
	}

	// A caller that only wants somewhere to put the impact still needs a position,
	// whether the beam landed on a wall, ran out of range or died on a mirror.
	if (!out.terminals.empty()) {
		out.hit = out.terminals[0].hit;
	} else if (!out.legs.empty()) {
		out.hit.position = out.legs.back().end;
		out.hit.distance = out.legs.back().length;
		const XMFLOAT3& d = out.legs.back().direction;
		out.hit.normal = XMFLOAT3(-d.x, -d.y, -d.z);
	}
}

void OpticsSystem::GUI() {
	ImGui::Checkbox("Optics enabled", &enabled);
	ImGui::Text("%d mirrors, %d lenses", (int)mirrors_.size(), (int)lenses_.size());

	if (mirrors_.empty() && lenses_.empty()) {
		ImGui::TextDisabled("Nothing registered. Attach a \"sticMirror\" / \"sticLens\" component,");
		ImGui::TextDisabled("or call st::OpticsSystem::Get().AddMirror(...) from a scene.");
		return;
	}

	if (!mirrors_.empty()) {
		ImGui::SeparatorText("Mirrors");
		selectedMirror_ = std::clamp(selectedMirror_, 0, (int)mirrors_.size() - 1);
		if (mirrors_.size() > 1) {
			ImGui::SliderInt("Mirror", &selectedMirror_, 0, (int)mirrors_.size() - 1);
		}
		Mirror& mirror = mirrors_[selectedMirror_].mirror;
		ImGui::Checkbox("Enabled##mirror", &mirror.surface.enabled);
		ImGui::SliderFloat("Reflectance", &mirror.reflectance, 0.0f, 1.0f);
		ImGui::ColorEdit3("Tint##mirror", &mirror.tint.x);
		ImGui::Checkbox("Double sided", &mirror.doubleSided);
		ImGui::SliderFloat("Scatter", &mirror.scatter, 0.0f, 0.2f, "%.4f rad");
		ImGui::Checkbox("Dichroic", &mirror.dichroic);
		if (mirror.dichroic) {
			// The two tints ARE the pass bands, so they are edited side by side.
			ImGui::ColorEdit3("Transmit tint", &mirror.transmitTint.x);
			ImGui::SliderFloat("Transmittance", &mirror.transmittance, 0.0f, 1.0f);
			ImGui::TextDisabled("Reflects `Tint`, passes `Transmit tint` on as a second beam.");
		}
		ImGui::Checkbox("Circular##mirror", &mirror.surface.circular);
		ImGui::Checkbox("Fit to mesh##mirror", &mirror.surface.fitToMesh);
		ImGui::BeginDisabled(mirror.surface.fitToMesh);
		ImGui::DragFloat2("Half extent##mirror", &mirror.surface.halfExtent.x, 0.01f, 0.001f, 20.0f);
		ImGui::EndDisabled();
		OpticDiagnostics(mirror.surface);
	}

	if (!lenses_.empty()) {
		ImGui::SeparatorText("Lenses");
		selectedLens_ = std::clamp(selectedLens_, 0, (int)lenses_.size() - 1);
		if (lenses_.size() > 1) {
			ImGui::SliderInt("Lens", &selectedLens_, 0, (int)lenses_.size() - 1);
		}
		Lens& lens = lenses_[selectedLens_].lens;
		ImGui::Checkbox("Enabled##lens", &lens.surface.enabled);
		int type = (int)lens.type;
		if (ImGui::Combo("Type", &type, LENS_TYPE_ITEMS)) {
			lens.type = (Lens::Type)type;
		}
		LensTypeFields(lens);
		ImGui::SliderFloat("Transmittance", &lens.transmittance, 0.0f, 1.0f);
		ImGui::ColorEdit3("Tint##lens", &lens.tint.x);
		ImGui::SliderFloat("Beam scale", &lens.beamScale, 0.05f, 8.0f);
		ImGui::SliderFloat("Spread", &lens.spread, 0.0f, 0.5f, "%.4f rad");
		ImGui::Checkbox("Circular##lens", &lens.surface.circular);
		ImGui::Checkbox("Fit to mesh##lens", &lens.surface.fitToMesh);
		ImGui::BeginDisabled(lens.surface.fitToMesh);
		ImGui::DragFloat2("Half extent##lens", &lens.surface.halfExtent.x, 0.01f, 0.001f, 20.0f);
		ImGui::EndDisabled();
		OpticDiagnostics(lens.surface);
	}
}

void OpticsSystem::SaveTo(nbt::Tag& out) const {
	// Only the master switch belongs in options.stad. The elements themselves are
	// scene content - whoever created them owns their lifetime.
	out.putBool("enabled", enabled);
}

void OpticsSystem::LoadFrom(const nbt::Tag& in) {
	enabled = in.getBool("enabled", enabled);
}

} // namespace st
