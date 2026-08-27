#include "render/Laser.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace st {

using namespace wi::graphics;

namespace {

constexpr const char* PASS_NAME = "StLaser";

// Set by Init(), cleared by the destructor: st::App owns the real system, and this is
// how everything else reaches it. Null until the app has initialised.
LaserSystem* g_instance = nullptr;

XMFLOAT3 Mul(const XMFLOAT3& a, const XMFLOAT3& b) {
	return XMFLOAT3(a.x * b.x, a.y * b.y, a.z * b.z);
}

XMFLOAT3 Lerp(const XMFLOAT3& a, const XMFLOAT3& b, float t) {
	return XMFLOAT3(
		a.x + (b.x - a.x) * t,
		a.y + (b.y - a.y) * t,
		a.z + (b.z - a.z) * t);
}

float Distance(const XMFLOAT3& a, const XMFLOAT3& b) {
	const float dx = a.x - b.x;
	const float dy = a.y - b.y;
	const float dz = a.z - b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Orthonormal axes across the beam, with the pattern's roll applied. Any pair
// perpendicular to the beam will do: the pattern has no preferred world orientation,
// only a preferred one relative to itself, which is what `roll` sets.
void ArrayBasis(const XMFLOAT3& direction, float roll, XMVECTOR& outTangent, XMVECTOR& outBitangent) {
	const XMVECTOR forward = XMLoadFloat3(&direction);
	// Near-vertical beams need a different reference or the cross product collapses.
	const XMVECTOR up = std::abs(direction.y) < 0.99f ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(0, 0, 1, 0);

	XMVECTOR tangent = XMVector3Normalize(XMVector3Cross(up, forward));
	XMVECTOR bitangent = XMVector3Normalize(XMVector3Cross(forward, tangent));

	if (roll != 0.0f) {
		const XMMATRIX rotation = XMMatrixRotationAxis(forward, roll);
		tangent = XMVector3TransformNormal(tangent, rotation);
		bitangent = XMVector3TransformNormal(bitangent, rotation);
	}

	outTangent = tangent;
	outBitangent = bitangent;
}

// Where ray `index` sits in the pattern, in normalised [-1,1] coordinates across the
// beam. Returns false for a ray the pattern deliberately skips.
bool ArraySample(const LaserArray& array, int index, float& outX, float& outY) {
	const int cx = std::max(array.countX, 1);
	const int cy = std::max(array.countY, 1);

	float nx = 0.0f;
	float ny = 0.0f;

	switch (array.shape) {
	case LaserArray::Shape::Ring: {
		const float angle = XM_2PI * (float)index / (float)cx;
		nx = std::cos(angle);
		ny = std::sin(angle);
		break;
	}
	case LaserArray::Shape::Fan:
		nx = cx > 1 ? (2.0f * index / (cx - 1) - 1.0f) : 0.0f;
		break;
	case LaserArray::Shape::Cross:
		// First arm along the tangent, then the second along the bitangent.
		if (index < cx) {
			nx = cx > 1 ? (2.0f * index / (cx - 1) - 1.0f) : 0.0f;
		} else {
			const int j = index - cx;
			ny = cy > 1 ? (2.0f * j / (cy - 1) - 1.0f) : 0.0f;
		}
		break;
	case LaserArray::Shape::Spiral: {
		const float t = cx > 1 ? (float)index / (float)(cx - 1) : 0.0f;
		const float angle = t * array.spiralTurns * XM_2PI;
		nx = t * std::cos(angle);
		ny = t * std::sin(angle);
		break;
	}
	case LaserArray::Shape::Grid:
	default: {
		const int ix = index % cx;
		const int iy = index / cx;
		nx = cx > 1 ? (2.0f * ix / (cx - 1) - 1.0f) : 0.0f;
		ny = cy > 1 ? (2.0f * iy / (cy - 1) - 1.0f) : 0.0f;
		break;
	}
	}

	if (array.hollow && std::abs(nx) < 1e-4f && std::abs(ny) < 1e-4f) return false;

	outX = nx;
	outY = ny;
	return true;
}

// Engine debug lines for one traced path, coloured by what ended each leg. Drawn from
// the TRACE, not from the uploaded segments, so it tells you what the walk found even
// when the pass draws nothing.
void DrawDebugPath(const BeamPath& path) {
	for (const BeamLeg& leg : path.legs) {
		XMFLOAT4 color;
		switch (leg.termination) {
		case BeamLeg::Termination::Surface: color = XMFLOAT4(0.3f, 1.0f, 0.4f, 1.0f); break;
		case BeamLeg::Termination::Mirror:  color = XMFLOAT4(0.3f, 0.9f, 1.0f, 1.0f); break;
		case BeamLeg::Termination::Lens:    color = XMFLOAT4(1.0f, 0.4f, 1.0f, 1.0f); break;
		case BeamLeg::Termination::Range:
		default:                            color = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f); break;
		}

		wi::renderer::RenderableLine line;
		line.start = leg.start;
		line.end = leg.end;
		line.color_start = color;
		line.color_end = color;
		wi::renderer::DrawLine(line);
	}
}

// Did the beam actually land on something? A leg that simply ran out of range has no
// impact point worth drawing a spot at; one that stopped on geometry - or died on a
// mirror because the bounce budget ran out - does.
bool BeamLanded(const BeamPath& path) {
	return !path.terminals.empty();
}

} // namespace

int LaserArray::Count() const {
	const int cx = std::max(countX, 1);
	const int cy = std::max(countY, 1);
	switch (shape) {
	case Shape::Grid: return cx * cy;
	case Shape::Cross: return cx + cy;
	case Shape::Ring:
	case Shape::Fan:
	case Shape::Spiral:
	default: return cx;
	}
}

LaserSystem& LaserSystem::Get() {
	if (g_instance != nullptr) return *g_instance;
	// No app yet. Hand back something inert rather than a null reference - its shader
	// never loads, so it draws nothing and every call on it is a no-op.
	static LaserSystem fallback;
	return fallback;
}

LaserSystem::~LaserSystem() {
	if (g_instance == this) g_instance = nullptr;
}

void LaserSystem::Init() {
	g_instance = this;

	if (!wi::renderer::LoadShader(ShaderStage::CS, shader_, "StLaserCS.cso")) {
		wi::backlog::post(
			"[Laser] failed to load StLaserCS.cso from " + wi::renderer::GetShaderPath() +
				". The shader is compiled into <exe>/shaders/ by simtary_add_app(); "
				"without it every laser is inert.",
			wi::backlog::LogLevel::Error);
		return;
	}

	GraphicsDevice* device = GetDevice();

	GPUBufferDesc desc;
	// Raw, not structured: the shader reads both with ByteAddressBuffer::Load<T>().
	desc.bind_flags = BindFlag::SHADER_RESOURCE;
	desc.misc_flags = ResourceMiscFlag::BUFFER_RAW;
	// UPLOAD keeps them permanently mapped, so Update() is a memcpy with no copy
	// queue work. One per frame in flight, because the GPU is still reading the
	// previous frame's contents while this frame is being written.
	desc.usage = Usage::UPLOAD;

	for (uint32_t i = 0; i < GraphicsDevice::GetBufferCount(); ++i) {
		desc.size = sizeof(StLaserSegment) * ST_LASER_SEGMENT_MAX;
		if (!device->CreateBuffer(&desc, nullptr, &segmentBuffers_[i])) {
			wi::backlog::post("[Laser] segment buffer creation failed", wi::backlog::LogLevel::Error);
			shader_ = {};
			return;
		}
		device->SetName(&segmentBuffers_[i], "st::LaserSystem::segments");

		desc.size = sizeof(StLaserDot) * ST_LASER_DOT_MAX;
		if (!device->CreateBuffer(&desc, nullptr, &dotBuffers_[i])) {
			wi::backlog::post("[Laser] dot buffer creation failed", wi::backlog::LogLevel::Error);
			shader_ = {};
			return;
		}
		device->SetName(&dotBuffers_[i], "st::LaserSystem::dots");
	}
}

void LaserSystem::Bind(wi::RenderPath3D& path) {
	if (path_ == &path) return;
	Unbind();
	path_ = &path;
}

void LaserSystem::Unbind() {
	Unhook();
	path_ = nullptr;
}

wi::RenderPath3D::CustomPostprocess* LaserSystem::FindPass() const {
	if (path_ == nullptr) return nullptr;
	for (auto& pass : path_->custom_post_processes) {
		if (pass.name == PASS_NAME) return &pass;
	}
	return nullptr;
}

void LaserSystem::Hook() {
	if (path_ == nullptr || !shader_.IsValid()) return;
	if (FindPass() != nullptr) {
		hooked_ = true;
		return;
	}

	wi::RenderPath3D::CustomPostprocess pass;
	pass.name = PASS_NAME;
	pass.computeshader = shader_;
	// Before tonemap: a laser is HDR light, so it blooms and tone maps with the rest
	// of the frame instead of being pasted on top of the finished image. The bloom is
	// most of what sells a beam this thin.
	pass.stage = wi::RenderPath3D::CustomPostprocess::Stage::BeforeTonemap;
	path_->custom_post_processes.push_back(pass);
	hooked_ = true;
}

void LaserSystem::Unhook() {
	if (!hooked_) return;
	if (wi::RenderPath3D::CustomPostprocess* pass = FindPass()) {
		// begin() + index rather than the raw pointer: wi::vector is std::vector in
		// some configurations, and that one only takes an iterator.
		path_->custom_post_processes.erase(
			path_->custom_post_processes.begin() + (pass - path_->custom_post_processes.data()));
	}
	hooked_ = false;
}

LaserSystem::ID LaserSystem::Add(const Laser& laser) {
	Entry& entry = lasers_.emplace_back();
	entry.id = nextID_++;
	entry.laser = laser;
	return entry.id;
}

Laser* LaserSystem::Find(ID id) {
	for (auto& entry : lasers_) {
		if (entry.id == id) return &entry.laser;
	}
	return nullptr;
}

const Laser* LaserSystem::Find(ID id) const {
	for (const auto& entry : lasers_) {
		if (entry.id == id) return &entry.laser;
	}
	return nullptr;
}

const BeamPath* LaserSystem::Path(ID id) const {
	return Path(id, 0);
}

size_t LaserSystem::BeamCount(ID id) const {
	for (const auto& entry : lasers_) {
		if (entry.id == id) return entry.beams.size();
	}
	return 0;
}

const BeamPath* LaserSystem::Path(ID id, size_t ray) const {
	for (const auto& entry : lasers_) {
		if (entry.id != id) continue;
		if (ray >= entry.beams.size()) return nullptr;
		return &entry.beams[ray].path;
	}
	return nullptr;
}

void LaserSystem::Remove(ID id) {
	for (size_t i = 0; i < lasers_.size(); ++i) {
		if (lasers_[i].id == id) {
			lasers_.erase(lasers_.begin() + i);
			return;
		}
	}
}

void LaserSystem::Clear() {
	lasers_.clear();
}

bool LaserSystem::ResolveEmitter(const wi::scene::Scene& scene, Laser& laser,
	XMFLOAT3& outOrigin, XMFLOAT3& outDirection) const {
	XMFLOAT3 origin = laser.position;
	XMFLOAT3 direction = laser.direction;

	if (laser.followEntity != wi::ecs::INVALID_ENTITY) {
		// Entity gone or not transformable - skip, rather than fire the beam from
		// wherever the emitter happened to be standing last frame.
		if (!EntityRay(scene, laser.followEntity, laser.forwardAxis, origin, direction)) return false;
	}

	if (laser.targetEntity != wi::ecs::INVALID_ENTITY) {
		if (const wi::scene::TransformComponent* target = scene.transforms.GetComponent(laser.targetEntity)) {
			const XMFLOAT3 targetPosition = target->GetPosition();
			const XMVECTOR toTarget = XMVectorSubtract(XMLoadFloat3(&targetPosition), XMLoadFloat3(&origin));
			if (XMVectorGetX(XMVector3LengthSq(toTarget)) > 1e-8f) {
				XMStoreFloat3(&direction, XMVector3Normalize(toTarget));
			}
		}
	}

	if (laser.startOffset != 0.0f) {
		origin.x += direction.x * laser.startOffset;
		origin.y += direction.y * laser.startOffset;
		origin.z += direction.z * laser.startOffset;
	}

	laser.resolvedOrigin = origin;
	laser.resolvedDirection = direction;

	outOrigin = origin;
	outDirection = direction;
	return true;
}

bool LaserSystem::TraceOne(const wi::scene::Scene& scene, Entry& entry) {
	Laser& laser = entry.laser;

	XMFLOAT3 origin, direction;
	if (!ResolveEmitter(scene, laser, origin, direction)) return false;

	const bool arrayed = laser.array.enabled;
	const int rayCount = arrayed
		? std::clamp(laser.array.Count(), 1, ST_LASER_ARRAY_MAX)
		: 1;

	// Resize in place rather than rebuild. The persistence trails are the only state
	// this system carries between frames, and clearing the vector would wipe every
	// streak the moment anything touched the pattern size.
	while ((int)entry.beams.size() > rayCount) entry.beams.pop_back();
	while ((int)entry.beams.size() < rayCount) entry.beams.emplace_back();

	BeamTraceDesc desc;
	desc.maxDistance = std::max(laser.range, 0.0f);
	desc.maxBounces = std::max(laser.maxBounces, 0);
	desc.minThroughput = laser.minThroughput;
	desc.opticBias = laser.opticBias;
	desc.mode = laser.rayMode;
	desc.filterMask = laser.filterMask;
	desc.layerMask = laser.layerMask;
	desc.ignoreEntity = laser.ignoreEntity;

	// The optics are a separate system on purpose: a mirror is scene content that
	// several lasers may share, and the beam is the only thing that needs to know
	// about both. Every ray of an array goes through the same walk, so a pattern
	// pointed at a mirror reflects as a pattern.
	OpticsSystem& optics = OpticsSystem::Get();

	if (!arrayed) {
		Beam& beam = entry.beams[0];
		beam.origin = origin;
		beam.direction = direction;
		beam.weight = 1.0f;
		desc.origin = origin;
		desc.direction = direction;
		optics.Trace(scene, desc, beam.path);
		return !beam.path.legs.empty();
	}

	const LaserArray& array = laser.array;

	XMVECTOR tangent, bitangent;
	ArrayBasis(direction, array.roll + array.spin * time_, tangent, bitangent);

	const XMVECTOR axis = XMLoadFloat3(&direction);
	const XMVECTOR base = XMLoadFloat3(&origin);
	const float falloff = wi::math::Clamp(array.falloff, 0.0f, 1.0f);

	bool any = false;

	for (int i = 0; i < rayCount; ++i) {
		Beam& beam = entry.beams[i];

		float nx, ny;
		if (!ArraySample(array, i, nx, ny)) {
			// A skipped ray keeps its slot rather than being removed, so toggling
			// `hollow` does not shift every later ray onto a different trail.
			beam.path.legs.clear();
			beam.path.hit = RayHit();
			beam.trail.clear();
			continue;
		}

		// Offset moves where the ray STARTS, angle moves where it points. Both at
		// once is what a real collimated array does; see LaserArray.
		const XMVECTOR rayOrigin = XMVectorAdd(base,
			XMVectorAdd(XMVectorScale(tangent, array.offset.x * nx),
				XMVectorScale(bitangent, array.offset.y * ny)));

		const XMVECTOR rayDirection = XMVector3Normalize(XMVectorAdd(axis,
			XMVectorAdd(XMVectorScale(tangent, std::tan(array.spreadAngle.x * nx)),
				XMVectorScale(bitangent, std::tan(array.spreadAngle.y * ny)))));

		XMStoreFloat3(&beam.origin, rayOrigin);
		XMStoreFloat3(&beam.direction, rayDirection);

		// Radial, not per-axis: a grid dimmed by |nx| alone would darken in stripes.
		const float radial = std::min(std::sqrt(nx * nx + ny * ny), 1.0f);
		beam.weight = 1.0f - falloff * radial;

		desc.origin = beam.origin;
		desc.direction = beam.direction;
		optics.Trace(scene, desc, beam.path);

		any = any || !beam.path.legs.empty();
	}

	return any;
}

void LaserSystem::UpdateTrail(const Laser& laser, Beam& beam, float dt) {
	if (!laser.trail || laser.trailLife <= 0.0f || laser.trailMax <= 0) {
		beam.trail.clear();
		return;
	}

	// Compact in place rather than erase a range: wi::vector is std::vector in some
	// configurations and a hand-rolled one in others, and only the former has a
	// two-iterator erase.
	size_t write = 0;
	for (size_t read = 0; read < beam.trail.size(); ++read) {
		TrailPoint point = beam.trail[read];
		point.age += dt;
		if (point.age >= laser.trailLife) continue;
		beam.trail[write++] = point;
	}
	while (beam.trail.size() > write) {
		beam.trail.pop_back();
	}

	if (!BeamLanded(beam.path)) return;

	// The PRIMARY terminal only. A dichroic ends the beam in two places, and one ring
	// of trail points cannot follow both without zig-zagging between them - so the
	// split half gets its live spot but no streak, which is the honest trade.
	const BeamTerminal& primary = beam.path.terminals[0];
	const XMFLOAT3 to = primary.hit.position;
	const XMFLOAT3 normal = primary.hit.normal;
	const XMFLOAT3 color = Mul(laser.color, primary.throughput);
	const float spacing = std::max(laser.trailSpacing, 0.001f);
	const int capacity = std::max(laser.trailMax, 1);

	if (beam.trail.empty()) {
		TrailPoint& point = beam.trail.emplace_back();
		point.position = to;
		point.normal = normal;
		point.color = color;
		point.age = 0.0f;
	} else {
		const XMFLOAT3 from = beam.trail.back().position;
		const float travelled = Distance(to, from);

		if (travelled >= spacing) {
			// Fill the gap rather than record one point per frame. A spot moving 4 m
			// in a frame would otherwise leave four dots twenty centimetres apart -
			// which is the dotted line, not the drawn line, and the whole reason the
			// trail exists is to make a moving beam read as a continuous mark.
			const int steps = std::min((int)(travelled / spacing), capacity);
			for (int i = 1; i <= steps; ++i) {
				const float t = (float)i / (float)steps;
				TrailPoint& point = beam.trail.emplace_back();
				point.position = Lerp(from, to, t);
				point.normal = normal;
				point.color = color;
				// Points nearer the head happened later in the frame, so they are
				// younger. Without this the whole gap fades as one block and the
				// streak flickers instead of trailing.
				point.age = dt * (1.0f - t);
			}
		}
	}

	// Over budget: drop the oldest, which are at the front. Shift-then-shrink for the
	// same portability reason as the compaction above.
	if ((int)beam.trail.size() > capacity) {
		const size_t excess = beam.trail.size() - (size_t)capacity;
		for (size_t i = 0; i < (size_t)capacity; ++i) {
			beam.trail[i] = beam.trail[i + excess];
		}
		while ((int)beam.trail.size() > capacity) {
			beam.trail.pop_back();
		}
	}
}
void LaserSystem::Update(const wi::scene::Scene& scene, float dt) {
	if (path_ == nullptr) return;

	if (!enabled || !shader_.IsValid() || lasers_.empty()) {
		Unhook();
		return;
	}

	time_ += dt;

	GraphicsDevice* device = GetDevice();

	StLaserSegment segments[ST_LASER_SEGMENT_MAX] = {};
	StLaserDot dots[ST_LASER_DOT_MAX] = {};
	uint32_t segmentCount = 0;
	uint32_t dotCount = 0;

	// Per-laser brightness wobble. [1 - flicker, 1], cosine rather than sine so a
	// laser with flicker set starts at full brightness instead of mid-dip.
	auto Flicker = [&](const Laser& laser) {
		if (laser.flicker <= 0.0f) return 1.0f;
		const float depth = wi::math::Clamp(laser.flicker, 0.0f, 1.0f);
		return 1.0f - depth * 0.5f * (1.0f - std::cos(time_ * laser.flickerRate * XM_2PI));
	};

	// ── trace + segments ─────────────────────────────────────────────────────────
	for (Entry& entry : lasers_) {
		Laser& laser = entry.laser;

		if (!laser.enabled || laser.range <= 0.0f) {
			// Clear the beams too, not just the legs: Path(id) is what gameplay reads,
			// and a disabled designator reporting last frame's target is worse than
			// one reporting none.
			entry.beams.clear();
			continue;
		}

		if (!TraceOne(scene, entry)) {
			entry.beams.clear();
			continue;
		}

		const float modulation = Flicker(laser);

		for (Beam& beam : entry.beams) {
			UpdateTrail(laser, beam, dt);

			if (laser.debugDraw) DrawDebugPath(beam.path);

			for (const BeamLeg& leg : beam.path.legs) {
				if (segmentCount >= ST_LASER_SEGMENT_MAX) {
					if (!warnedSegments_) {
						warnedSegments_ = true;
						wi::backlog::post(
							"[Laser] ST_LASER_SEGMENT_MAX (" + std::to_string(ST_LASER_SEGMENT_MAX) +
								") reached - some beam legs are not drawn. Shrink the array, lower "
								"maxBounces, or raise the ceiling in assets/shaders/StLaserInterop.h.",
							wi::backlog::LogLevel::Warning);
					}
					break;
				}

				StLaserSegment& out = segments[segmentCount++];

				out.start = leg.start;
				out.end = leg.end;

				// A lens with `spread` widens the beam along the leg. The shader
				// carries one radius per segment, so this is the radius at the leg's
				// midpoint - the error at either end is smaller than the glow it
				// lives inside.
				const float grow = leg.divergence * leg.length * 0.5f;
				out.core_radius = std::max(laser.coreRadius * leg.radiusScale + grow, 1e-4f);
				out.glow_radius = std::max(laser.glowRadius * leg.radiusScale + grow * 4.0f, 1e-4f);

				const float gain = modulation * beam.weight;
				out.color = Mul(laser.color, leg.throughput);
				out.intensity = std::max(laser.intensity, 0.0f) * gain;
				out.glow_intensity = std::max(laser.glowIntensity, 0.0f) * gain;
				out.attenuation = wi::math::Clamp(laser.attenuation, 0.0f, 1.0f);
				out.pad0 = 0.0f;

				out.flags = ST_LASER_FLAG_CORE;
				if (laser.glowIntensity > 0.0f) out.flags |= ST_LASER_FLAG_GLOW;
				if (laser.occluded) out.flags |= ST_LASER_FLAG_OCCLUDED;
			}
		}
	}

	// ── live impact spots ────────────────────────────────────────────────────────
	// Emitted before the trails so that under budget pressure it is the tail that is
	// dropped, never the head. A trail with no head reads as a bug; a head with no
	// trail just reads as a shorter trail. With an array that matters more, not less:
	// sixty-four heads and no tails is still a legible pattern.
	for (Entry& entry : lasers_) {
		const Laser& laser = entry.laser;
		if (!laser.enabled || !laser.dot) continue;

		const float modulation = Flicker(laser);

		for (const Beam& beam : entry.beams) {
			// One spot per TERMINAL, not per beam: a dichroic ends one traced beam in
			// two places, and the transmitted half landing nowhere would read as the
			// splitter being broken. Each terminal carries the throughput it arrived
			// with, so the two halves land in their own colours.
			for (const BeamTerminal& terminal : beam.path.terminals) {
				if (dotCount >= ST_LASER_DOT_MAX) break;

				const float gain = modulation * beam.weight;

				StLaserDot& out = dots[dotCount++];
				out.position = terminal.hit.position;
				out.radius = std::max(laser.dotRadius, 1e-4f);
				out.color = Mul(laser.color, terminal.throughput);
				out.intensity = std::max(laser.dotIntensity, 0.0f) * gain;
				out.normal = terminal.hit.normal;
				out.surface_radius = std::max(laser.surfaceRadius, 1e-4f);
				out.surface_intensity = std::max(laser.surfaceIntensity, 0.0f) * gain;
			}
			if (dotCount >= ST_LASER_DOT_MAX) break;
		}
	}

	// ── persistence trails ───────────────────────────────────────────────────────
	for (Entry& entry : lasers_) {
		const Laser& laser = entry.laser;
		if (!laser.enabled || !laser.trail) continue;

		const float life = std::max(laser.trailLife, 1e-3f);
		const float falloff = std::max(laser.trailFalloff, 0.01f);
		const float shrink = wi::math::Clamp(laser.trailShrink, 0.0f, 1.0f);

		for (const Beam& beam : entry.beams) {
			// Newest first: the youngest points are the brightest, and they are the
			// ones worth keeping when the budget runs out.
			for (size_t i = beam.trail.size(); i-- > 0;) {
				if (dotCount >= ST_LASER_DOT_MAX) {
					if (!warnedDots_) {
						warnedDots_ = true;
						wi::backlog::post(
							"[Laser] ST_LASER_DOT_MAX (" + std::to_string(ST_LASER_DOT_MAX) +
								") reached - trails are being truncated. With array projection on, "
								"trailMax is PER RAY: a 5x5 grid at trailMax 48 asks for 1200 dots.",
							wi::backlog::LogLevel::Warning);
					}
					break;
				}

				const TrailPoint& point = beam.trail[i];
				const float remaining = wi::math::Clamp(1.0f - point.age / life, 0.0f, 1.0f);
				const float fade = std::pow(remaining, falloff);
				if (fade <= 0.002f) continue;

				// Old points shrink as well as dim, which is what turns a uniform
				// smear into a streak with a definite head.
				const float scale = 1.0f - shrink * (1.0f - remaining);
				const float gain = fade * beam.weight;

				StLaserDot& out = dots[dotCount++];
				out.position = point.position;
				out.radius = std::max(laser.dotRadius * scale, 1e-4f);
				out.color = point.color;
				out.intensity = std::max(laser.dotIntensity, 0.0f) * gain;
				out.normal = point.normal;
				out.surface_radius = std::max(laser.surfaceRadius * scale, 1e-4f);
				out.surface_intensity = std::max(laser.surfaceIntensity, 0.0f) * gain;
			}
		}
	}

	if (segmentCount == 0 && dotCount == 0) {
		Unhook();
		return;
	}

	Hook();

	wi::RenderPath3D::CustomPostprocess* pass = FindPass();
	if (pass == nullptr) return;

	const uint32_t frame = device->GetBufferIndex();
	GPUBuffer& segmentBuffer = segmentBuffers_[frame];
	GPUBuffer& dotBuffer = dotBuffers_[frame];
	if (segmentBuffer.mapped_data == nullptr || dotBuffer.mapped_data == nullptr) return;

	if (segmentCount > 0) {
		std::memcpy(segmentBuffer.mapped_data, segments, sizeof(StLaserSegment) * segmentCount);
	}
	if (dotCount > 0) {
		std::memcpy(dotBuffer.mapped_data, dots, sizeof(StLaserDot) * dotCount);
	}

	// Descriptor indices are small integers, so a float carries them exactly - which
	// is all PostProcess::params0 has room for.
	pass->params0 = XMFLOAT4(
		(float)device->GetDescriptorIndex(&segmentBuffer, SubresourceType::SRV),
		(float)segmentCount,
		(float)device->GetDescriptorIndex(&dotBuffer, SubresourceType::SRV),
		(float)dotCount);
}

void LaserSystem::GUI() {
	if (!shader_.IsValid()) {
		ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "StLaserCS failed to load - see BackLog.");
		return;
	}

	ImGui::Checkbox("Lasers enabled", &enabled);
	ImGui::Text("%d lasers", (int)lasers_.size());

	if (lasers_.empty()) {
		ImGui::TextDisabled("Nothing registered. Attach a \"sticLaser\" component,");
		ImGui::TextDisabled("or call st::LaserSystem::Get().Add(...) from a scene.");
		return;
	}

	selected_ = std::clamp(selected_, 0, (int)lasers_.size() - 1);
	if (lasers_.size() > 1) {
		ImGui::SliderInt("Laser", &selected_, 0, (int)lasers_.size() - 1);
	}

	Entry& entry = lasers_[selected_];
	Laser& laser = entry.laser;

	ImGui::Checkbox("Enabled", &laser.enabled);

	ImGui::SeparatorText("Beam");
	ImGui::ColorEdit3("Color", &laser.color.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
	ImGui::DragFloat("Core radius", &laser.coreRadius, 0.0005f, 0.0005f, 0.5f, "%.4f m");
	ImGui::DragFloat("Intensity", &laser.intensity, 0.5f, 0.0f, 500.0f);
	ImGui::DragFloat("Glow radius", &laser.glowRadius, 0.005f, 0.001f, 2.0f, "%.3f m");
	ImGui::DragFloat("Glow intensity", &laser.glowIntensity, 0.05f, 0.0f, 50.0f);
	ImGui::SliderFloat("Attenuation", &laser.attenuation, 0.0f, 1.0f);
	ImGui::DragFloat("Range", &laser.range, 1.0f, 0.1f, 5000.0f, "%.1f m");
	ImGui::Checkbox("Occluded by geometry", &laser.occluded);
	ImGui::SliderFloat("Flicker", &laser.flicker, 0.0f, 1.0f);
	if (laser.flicker > 0.0f) {
		ImGui::DragFloat("Flicker rate", &laser.flickerRate, 0.5f, 0.1f, 200.0f, "%.1f Hz");
	}

	ImGui::SeparatorText("Optics");
	ImGui::SliderInt("Max bounces", &laser.maxBounces, 0, 12);
	ImGui::SliderFloat("Min throughput", &laser.minThroughput, 0.0f, 0.5f);
	ImGui::DragFloat("Optic bias", &laser.opticBias, 0.001f, 0.0f, 0.5f, "%.3f m");
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"How far past a surface a mirror/lens still counts as in front of it.\n"
			"Raise it when a beam stops on the mesh instead of bouncing off the element in it.");
	}
	int mode = (int)laser.rayMode;
	if (ImGui::Combo("Stop on", &mode, "Mesh\0Physics\0Both\0Nothing\0")) {
		laser.rayMode = (RayQuery::Mode)mode;
	}

	ImGui::SeparatorText("Array projection");
	ImGui::Checkbox("Array", &laser.array.enabled);
	if (laser.array.enabled) {
		LaserArray& array = laser.array;

		int shape = (int)array.shape;
		if (ImGui::Combo("Pattern", &shape, "Grid\0Ring\0Fan\0Cross\0Spiral\0")) {
			array.shape = (LaserArray::Shape)shape;
		}

		ImGui::SliderInt("Count X", &array.countX, 1, 32);
		if (array.shape == LaserArray::Shape::Grid || array.shape == LaserArray::Shape::Cross) {
			ImGui::SliderInt("Count Y", &array.countY, 1, 32);
		}
		if (array.shape == LaserArray::Shape::Spiral) {
			ImGui::DragFloat("Turns", &array.spiralTurns, 0.1f, 0.1f, 20.0f);
		}

		// The two spreads are different instruments, so they are labelled by what they
		// make rather than by their units.
		ImGui::DragFloat2("Spread angle", &array.spreadAngle.x, 0.002f, 0.0f, 1.4f, "%.4f rad");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("All rays from one point, fanning out. The pattern grows with distance.");
		}
		ImGui::DragFloat2("Offset", &array.offset.x, 0.005f, 0.0f, 10.0f, "%.3f m");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Rays from different points, parallel. The pattern keeps its size.");
		}

		ImGui::SliderFloat("Roll", &array.roll, -XM_PI, XM_PI);
		ImGui::DragFloat("Spin", &array.spin, 0.05f, -20.0f, 20.0f, "%.2f rad/s");
		ImGui::Checkbox("Hollow", &array.hollow);
		ImGui::SliderFloat("Edge falloff", &array.falloff, 0.0f, 1.0f);

		const int rays = std::clamp(array.Count(), 1, ST_LASER_ARRAY_MAX);
		ImGui::Text("%d rays", rays);
		if (array.Count() > ST_LASER_ARRAY_MAX) {
			ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Clamped to %d.", ST_LASER_ARRAY_MAX);
		}
		// Each ray is a full trace, and the trace is the expensive half of the system.
		ImGui::TextDisabled("%d scene raycasts per bounce, every frame.", rays);
	}

	ImGui::SeparatorText("Spot");
	ImGui::Checkbox("Draw spot", &laser.dot);
	if (laser.dot) {
		ImGui::DragFloat("Air glow radius", &laser.dotRadius, 0.002f, 0.001f, 1.0f, "%.3f m");
		ImGui::DragFloat("Air glow intensity", &laser.dotIntensity, 0.2f, 0.0f, 200.0f);
		ImGui::DragFloat("Surface radius", &laser.surfaceRadius, 0.002f, 0.001f, 2.0f, "%.3f m");
		ImGui::DragFloat("Surface intensity", &laser.surfaceIntensity, 0.2f, 0.0f, 200.0f);
	}

	ImGui::SeparatorText("Persistence trail");
	ImGui::Checkbox("Trail", &laser.trail);
	if (laser.trail) {
		ImGui::TextDisabled("Keeps the last impacts glowing so a moving beam draws.");
		ImGui::DragFloat("Life", &laser.trailLife, 0.01f, 0.01f, 5.0f, "%.2f s");
		ImGui::DragFloat("Spacing", &laser.trailSpacing, 0.001f, 0.001f, 1.0f, "%.3f m");
		ImGui::SliderInt("Max points", &laser.trailMax, 1, (int)ST_LASER_DOT_MAX);
		ImGui::SliderFloat("Falloff", &laser.trailFalloff, 0.2f, 6.0f);
		ImGui::SliderFloat("Shrink", &laser.trailShrink, 0.0f, 1.0f);

		// Per RAY, not per laser: an array multiplies this by its ray count, and the
		// total is what runs into ST_LASER_DOT_MAX.
		size_t live = 0;
		for (const Beam& beam : entry.beams) live += beam.trail.size();
		ImGui::Text("%d points live across %d rays", (int)live, (int)entry.beams.size());
		if (laser.array.enabled) {
			const int asked = laser.trailMax * (int)entry.beams.size();
			if (asked > (int)ST_LASER_DOT_MAX) {
				ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
					"Max points is per ray: %d rays x %d asks for %d, budget is %d.",
					(int)entry.beams.size(), laser.trailMax, asked, (int)ST_LASER_DOT_MAX);
			}
		}
	}

	ImGui::SeparatorText("This frame");
	ImGui::Checkbox("Debug lines", &laser.debugDraw);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip(
			"Draws the traced path: white ran out of range, green landed on geometry,\n"
			"cyan reflected off a mirror, magenta refracted through a lens.\n"
			"A bounce missing from the LINES means the trace never found the element.");
	}
	if (entry.beams.empty()) {
		ImGui::TextDisabled("Not traced this frame.");
	} else {
		// Ray 0 in full - it is the pattern's centre and the one Path(id) hands to
		// gameplay - then a count for the rest, because sixty-four leg lists is not a
		// readout, it is a wall.
		const Beam& primary = entry.beams[0];
		ImGui::Text("%d rays, ray 0 has %d legs", (int)entry.beams.size(), (int)primary.path.legs.size());

		for (size_t i = 0; i < primary.path.legs.size(); ++i) {
			const BeamLeg& leg = primary.path.legs[i];
			const char* ended = "range";
			switch (leg.termination) {
			case BeamLeg::Termination::Surface: ended = "surface"; break;
			case BeamLeg::Termination::Mirror: ended = "mirror"; break;
			case BeamLeg::Termination::Lens: ended = "lens"; break;
			default: break;
			}
			ImGui::BulletText("leg %d: %.2f m, ended on %s", (int)i, leg.length, ended);
		}

		if (primary.path.exhaustedBounces) {
			ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Bounce budget exhausted - beam ends on an element.");
		}
		if (primary.path.hit.hit) {
			ImGui::Text("Hit entity %llu at %.2f m",
				(unsigned long long)primary.path.hit.entity, primary.path.hit.distance);
		} else {
			ImGui::TextDisabled("No surface hit.");
		}

		if (entry.beams.size() > 1) {
			size_t landed = 0;
			size_t segments = 0;
			for (const Beam& beam : entry.beams) {
				segments += beam.path.legs.size();
				if (!beam.path.legs.empty() &&
					beam.path.legs.back().termination != BeamLeg::Termination::Range) {
					++landed;
				}
			}
			ImGui::Text("Array: %d/%d rays landed, %d segments of %d",
				(int)landed, (int)entry.beams.size(), (int)segments, (int)ST_LASER_SEGMENT_MAX);
			if (segments > ST_LASER_SEGMENT_MAX) {
				ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Over budget - later rays are not drawn.");
			}
		}
	}
	if (laser.followEntity != wi::ecs::INVALID_ENTITY) {
		ImGui::Text("Following entity %llu", (unsigned long long)laser.followEntity);
	}
}

void LaserSystem::SaveTo(nbt::Tag& out) const {
	// Only the master switch belongs in options.stad. The lasers themselves are scene
	// content - whoever created them owns their lifetime.
	out.putBool("enabled", enabled);
}

void LaserSystem::LoadFrom(const nbt::Tag& in) {
	enabled = in.getBool("enabled", enabled);
}

} // namespace st
