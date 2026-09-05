#pragma once
#include "Simtary.h"
#include "io/Nbt.h"
#include "render/Optics.h"
#include "scene/Ray.h"
#include "StLaserInterop.h"

namespace st {

// One laser emitting MANY rays instead of one - a dot projector, a structured-light
// grid, a laser fence, a scanner sweep frozen in place.
//
// Off by default, and deliberately: every ray is a full trace of its own, so a 5x5
// grid does twenty-five scene raycasts per bounce where a plain laser does one. The
// pattern travels through the optics like any other beam, so an array pointed at a
// mirror reflects as an array and an array through a lens focuses to a point.
//
// Two ways to spread the rays, and they are different instruments:
//
//   spreadAngle  every ray leaves the SAME point at a different angle. This is the
//                dot projector / diffraction grating: the pattern grows with
//                distance, which is what depth sensors measure.
//   offset       every ray leaves a DIFFERENT point in the same direction. This is
//                the lenslet array / light curtain: the pattern stays the same size
//                however far it travels.
//
// Both at once is legal and is what a real collimated array does.
struct LaserArray {
	bool enabled = false;

	enum class Shape {
		Grid,   // countX columns by countY rows
		Ring,   // countX rays evenly around a circle
		Fan,    // countX rays along one line
		Cross,  // countX along one axis plus countY along the other
		Spiral, // countX rays on an Archimedean spiral
	};
	Shape shape = Shape::Grid;

	int countX = 5;
	int countY = 5; // Grid rows / Cross second arm; ignored by Ring, Fan and Spiral

	// Half-angle of the outermost ray, radians. The pattern spans twice this.
	XMFLOAT2 spreadAngle = XMFLOAT2(0.12f, 0.12f);

	// Half-width of the outermost ray's starting point, metres.
	XMFLOAT2 offset = XMFLOAT2(0, 0);

	float roll = 0.0f; // rotation of the whole pattern about the beam axis, radians
	float spin = 0.0f; // and how fast it turns, radians per second

	// Spiral only: how many times round it goes on the way out.
	float spiralTurns = 3.0f;

	// Drop the centre ray. A ring has no centre to drop; Grid and Cross do.
	bool hollow = false;

	// Dim the outer rays, 0..1. A real diffracted array loses energy off-axis and a
	// perfectly even grid reads as fake.
	float falloff = 0.0f;

	// How many rays this pattern produces, before `hollow` removes any.
	int Count() const;
};

// Per-laser ray ceiling. The segment budget (ST_LASER_SEGMENT_MAX) bounds what
// actually reaches the GPU; this bounds the CPU tracing, which is the expensive half.
static constexpr int ST_LASER_ARRAY_MAX = 256;

// A laser: a beam that is traced, not just drawn.
//
// Every frame the system walks the beam out of its emitter, through whatever mirrors
// and lenses st::OpticsSystem holds, until it lands on scene geometry or runs out of
// range. What comes back is a list of straight legs and the surface the beam finally
// hit; the legs go to the GPU as StLaserSegment and the impact goes out as
// StLaserDot. Both are integrated analytically per pixel - see StLaserCS.hlsl for why
// the pass has no samples to tune.
//
// Why not a LightComponent or a projector: the beam is millimetres across. A spot
// light cannot make a shaft that thin, and the projector pass ray marches, which
// steps straight over something this small. This one is closed form for exactly that
// reason.
//
// What it does NOT do, being screen space: light the surfaces along the beam (only
// the impact point is drawn on geometry), contribute to reflections or GI, or show
// through transparent surfaces.
struct Laser {
	bool enabled = true;

	// placement
	// With followEntity set, position/direction are refreshed from that entity's world
	// transform every frame and whatever you wrote into them is overwritten.
	wi::ecs::Entity followEntity = wi::ecs::INVALID_ENTITY;
	XMFLOAT3 position = XMFLOAT3(0, 0, 0);
	XMFLOAT3 direction = XMFLOAT3(0, 0, 1);

	// Which local axis of `followEntity` the beam leaves along. Same numbering as
	// st::Projector::Forward: 0 = +Z, 1 = -Z, 2 = -Y (what a spot light points down),
	// 3 = +Y, 4 = +X, 5 = -X.
	int forwardAxis = 0;

	// Aim at another entity instead. Overrides the direction above while it is set,
	// which is how a designator tracks a target without any code in the scene.
	wi::ecs::Entity targetEntity = wi::ecs::INVALID_ENTITY;

	// Offset from the emitter along the beam before the first leg starts, metres.
	// A muzzle-mounted laser wants a little of this so the first leg does not start
	// inside the weapon.
	float startOffset = 0.0f;

	// beam
	XMFLOAT3 color = XMFLOAT3(1.0f, 0.1f, 0.05f);

	// The filament. Real laser beams are 1-3 mm; anything above a centimetre reads as
	// a sci-fi bolt rather than a laser.
	float coreRadius = 0.004f;
	float intensity = 40.0f;

	// The halo around it - what actually makes the beam visible in air, and what
	// carries into the bloom. Typically 10-40x the core radius and far dimmer.
	float glowRadius = 0.08f;
	float glowIntensity = 1.5f;

	// 0 keeps the leg even end to end, 1 fades it out towards its far end (dust and
	// scatter thinning the shaft with distance).
	float attenuation = 0.25f;

	// Total path length across every leg. A beam that bounces twice covers this once,
	// not three times.
	float range = 200.0f;

	// Hidden where scene geometry is nearer than the beam. Turn it off for a beam
	// that should read through walls (a debug trace, a targeting overlay).
	bool occluded = true;

	// Sinusoidal brightness wobble - the instability of a cheap diode. 0 is off.
	float flicker = 0.0f;
	float flickerRate = 24.0f; // Hz

	// optics
	// How many mirrors/lenses the beam may pass through. 0 traces a straight line and
	// ignores every element.
	int maxBounces = 4;
	float minThroughput = 0.01f;
	// How far past a geometry hit a mirror or lens still counts as being in front of
	// it. A mirror is coplanar with the mesh that shows it, so without this the beam
	// stops on the glass instead of bouncing. See BeamTraceDesc::opticBias.
	float opticBias = 0.02f;

	// array projection
	// Emit a pattern of rays instead of one. Off by default; see LaserArray above for
	// what it costs and what the two spread modes mean.
	LaserArray array;

	// where the beam stops
	// Mesh hits anything drawn, Physics hits Jolt bodies, Both keeps the nearer, None
	// lets the beam run its full range through the world. See scene/Ray.h.
	RayQuery::Mode rayMode = RayQuery::Mode::Mesh;
	uint32_t filterMask = wi::enums::FILTER_OPAQUE;
	uint32_t layerMask = ~0u;
	// Never stop on this entity. Set it to the weapon the laser is bolted to.
	wi::ecs::Entity ignoreEntity = wi::ecs::INVALID_ENTITY;

	// the projected spot
	// The dot the beam paints where it lands: a small ball of glow in the air plus a
	// mark spread across the surface itself.
	bool dot = true;
	float dotRadius = 0.03f;      // air glow, metres
	float dotIntensity = 12.0f;
	float surfaceRadius = 0.035f; // how far the mark spreads on the surface, metres
	float surfaceIntensity = 8.0f;

	// persistence trail
	// Impact points are kept for `trailLife` seconds and fade out over that time, so
	// a beam swept across a wall DRAWS on it instead of showing a single dot that
	// vanishes the instant it moves. This is the same effect that makes a laser show
	// legible: the picture only exists because the last few hundred milliseconds of
	// the spot are still glowing.
	bool trail = true;
	float trailLife = 0.35f;    // seconds until a point is gone
	float trailSpacing = 0.015f; // minimum gap between recorded points, metres
	int trailMax = 48;          // per laser
	// Shape of the fade. 1 is linear; above 1 the tail dims quickly and leaves a short
	// bright streak behind the head, which is what reads as motion.
	float trailFalloff = 1.6f;
	// Trail points shrink as they age as well as dimming. 0 keeps them full size.
	float trailShrink = 0.5f;

	// debug
	// Draw the traced path as engine debug lines, one per leg, coloured by what ended
	// it: white = ran out of range, green = landed on geometry, cyan = reflected off a
	// mirror, magenta = refracted through a lens.
	//
	// This exists to separate the two halves of the system. The lines come straight
	// from the trace, so if a bounce is in the lines but not on screen the renderer is
	// at fault, and if it is in neither the trace never found the element - which is
	// otherwise a guess.
	bool debugDraw = false;

	// Resolved direction actually used this frame, after followEntity/targetEntity.
	// Read-only for callers; Update overwrites it.
	XMFLOAT3 resolvedDirection = XMFLOAT3(0, 0, 1);
	XMFLOAT3 resolvedOrigin = XMFLOAT3(0, 0, 0);
};

// Owns the laser list, the trace, the persistence trail, the GPU upload and the
// render path hook.
//
//   st::LaserSystem::Get()                      the instance the framework runs
//   ID id = ...Get().Add(laser)                 add one
//   ...Get().Find(id)->intensity = 80.0f        edit it live
//   ...Get().Path(id)->hit                      what the beam is pointing at
//   ...Get().Remove(id)                         drop it
//
// st::App calls Init/Bind/Update for you, and owns the instance - Get() only hands
// out a reference, so a scene or a native component can reach it without being given
// a pointer to the app.
class LaserSystem {
public:
	using ID = uint32_t;
	static constexpr ID INVALID = ~0u;

	// The system st::App drives. Before the app has initialised (or in a test with no
	// app at all) this is an inert instance: adding to it is harmless and draws
	// nothing.
	static LaserSystem& Get();

	LaserSystem() = default;
	~LaserSystem();
	LaserSystem(const LaserSystem&) = delete;
	LaserSystem& operator=(const LaserSystem&) = delete;

	// Loads StLaserCS and creates the upload buffers. Call once, after the graphics
	// device exists.
	void Init();

	// Attaches the pass to a render path. Safe to call again with another path; the
	// previous one is unhooked first.
	void Bind(wi::RenderPath3D& path);
	void Unbind();

	ID Add(const Laser& laser = Laser());
	Laser* Find(ID id);
	const Laser* Find(ID id) const;
	void Remove(ID id);
	void Clear();
	size_t Count() const { return lasers_.size(); }

	// The legs and the impact from the last Update, for the laser's FIRST ray. Null
	// for an unknown id, and empty for a laser that was disabled this frame. This is
	// how gameplay reads a designator: Path(id)->hit.entity is what the beam is on.
	//
	// With array projection off there is only one ray and this is it. With it on, the
	// first ray is the pattern's own index 0 - the centre of a grid or a fan, and the
	// first point on a ring - so a designator with a decorative array around it still
	// reads the same way.
	const BeamPath* Path(ID id) const;

	// Every ray of an array. `BeamCount` is 1 for a plain laser.
	size_t BeamCount(ID id) const;
	const BeamPath* Path(ID id, size_t ray) const;

	// Traces every laser, ages the trails, and uploads. Call once per frame, after
	// the scene update (so followed entities carry this frame's transform) and after
	// st::OpticsSystem::Update (so mirrors and lenses are where they will be drawn).
	void Update(const wi::scene::Scene& scene, float dt);

	void GUI();
	void SaveTo(nbt::Tag& out) const;
	void LoadFrom(const nbt::Tag& in);

	bool IsValid() const { return shader_.IsValid(); }

	// Master switch. Off costs nothing: the pass is removed from the render path.
	bool enabled = true;

private:
	// One recorded impact. The live spot is not one of these - it is emitted at full
	// brightness every frame regardless of whether it was worth recording.
	struct TrailPoint {
		XMFLOAT3 position = XMFLOAT3(0, 0, 0);
		XMFLOAT3 normal = XMFLOAT3(0, 0, 1);
		XMFLOAT3 color = XMFLOAT3(1, 1, 1);
		float age = 0.0f;
	};

	// One traced ray. A plain laser has exactly one of these; an array has as many as
	// its pattern makes. The trail is per RAY, not per laser - each ray draws its own
	// streak, which is the whole point of sweeping a grid across a wall.
	struct Beam {
		BeamPath path;
		// Oldest first, so the head of the streak is always trail.back().
		wi::vector<TrailPoint> trail;
		XMFLOAT3 origin = XMFLOAT3(0, 0, 0);
		XMFLOAT3 direction = XMFLOAT3(0, 0, 1);
		// Per-ray brightness from LaserArray::falloff. 1 for a plain laser.
		float weight = 1.0f;
	};

	struct Entry {
		ID id = INVALID;
		Laser laser;
		wi::vector<Beam> beams;
	};

	void Hook();
	void Unhook();
	wi::RenderPath3D::CustomPostprocess* FindPass() const;

	// Resolves the emitter's world origin and direction. False when the followed
	// entity is gone or not transformable.
	bool ResolveEmitter(const wi::scene::Scene& scene, Laser& laser,
		XMFLOAT3& outOrigin, XMFLOAT3& outDirection) const;

	// Fills entry.beams with the rays this laser emits this frame - one for a plain
	// laser, the whole pattern for an array - and traces every one of them. Returns
	// false when it produced nothing.
	bool TraceOne(const wi::scene::Scene& scene, Entry& entry);
	void UpdateTrail(const Laser& laser, Beam& beam, float dt);

	wi::vector<Entry> lasers_;
	ID nextID_ = 0;

	wi::graphics::Shader shader_;
	// One buffer per frame in flight: the CPU writes next frame's beams while the GPU
	// is still reading last frame's.
	wi::graphics::GPUBuffer segmentBuffers_[wi::graphics::GraphicsDevice::GetBufferCount()];
	wi::graphics::GPUBuffer dotBuffers_[wi::graphics::GraphicsDevice::GetBufferCount()];

	wi::RenderPath3D* path_ = nullptr;
	bool hooked_ = false;

	float time_ = 0.0f; // drives flicker

	// Reported once each, so a beam that quietly lost its far end is visible in the
	// backlog instead of just looking wrong.
	bool warnedSegments_ = false;
	bool warnedDots_ = false;

	int selected_ = 0; // GUI only
};

} // namespace st
