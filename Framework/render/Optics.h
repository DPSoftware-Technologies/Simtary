#pragma once
#include "Simtary.h"
#include "io/Nbt.h"
#include "scene/Ray.h"

namespace st {

// Mirrors and lenses: the things a laser beam meets on its way to a wall.
//
// Both are flat apertures with a transform, so they share almost everything and
// differ only in what they do to the ray that lands on them:
//
//   Mirror  reflects it            d' = d - 2 (d.n) n
//   Lens    bends it towards (or away from) the optical axis, by the paraxial thin
//           lens relation, so a beam that arrives off-centre leaves aimed at the
//           focal point rather than parallel to where it came from
//
// They live on the CPU rather than in the shader because the beam has to be TRACED,
// not just drawn: where the second leg goes depends on where the first one landed,
// and that is a sequential walk, not something a per-pixel pass can discover. What
// the shader gets is the finished list of straight legs (see StLaserInterop.h).
//
// Nothing here renders. A mirror is invisible unless the scene also puts a mesh
// there - which is the right split: the reflective surface you SEE is a material
// question, and this is only where the beam goes.

// An optical element's placement. Shared by both kinds so a component can bind one
// set of fields and a trace can treat them uniformly.
struct OpticSurface {
	bool enabled = true;

	// With followEntity set, position/rotation are refreshed from that entity's world
	// transform every frame and whatever you wrote into them is overwritten.
	wi::ecs::Entity followEntity = wi::ecs::INVALID_ENTITY;
	XMFLOAT3 position = XMFLOAT3(0, 0, 0);
	XMFLOAT4 rotation = XMFLOAT4(0, 0, 0, 1);

	// Which local axis is the surface normal / the optical axis. Same numbering as
	// st::Projector::Forward and st::EntityRay: 0 = +Z, 1 = -Z, 2 = -Y, 3 = +Y,
	// 4 = +X, 5 = -X.
	int normalAxis = 0;

	// Aperture. Circular uses `halfExtent.x` as the radius; rectangular uses both as
	// half-width and half-height measured in the surface's local axes.
	bool circular = true;
	XMFLOAT2 halfExtent = XMFLOAT2(0.25f, 0.25f);

	// Take the plane's POINT and its aperture from the entity's own mesh instead of
	// from `position` and `halfExtent`.
	//
	// This is on by default in the components, and it exists because both of the ways
	// a hand-set aperture goes wrong are invisible from the scene:
	//
	//   too small  the beam reaches the plane, lands off the disc, and stops on the
	//              mesh instead - the mirror looks broken
	//   too big    the beam bounces off the plane EXTENDED past the glass, so the
	//              reflection happens in mid-air where there is nothing to reflect it
	//
	// and because the plane is pinned to the entity's ORIGIN, which is not where the
	// mesh is whenever the model was authored with an offset pivot. Fitting takes the
	// mesh's own bounds through its world matrix, so the plane sits on the glass and
	// stops at its edges.
	//
	// Falls back to the manual values when the entity has no mesh, so a mirror created
	// from code without one still works.
	bool fitToMesh = false;

	// Cached each frame by OpticsSystem::Update. Public because Trace reads them and
	// because seeing the resolved axes in the inspector is how you find a mirror that
	// is facing the wrong way.
	XMFLOAT3 normal = XMFLOAT3(0, 0, 1);
	XMFLOAT3 tangent = XMFLOAT3(1, 0, 0); // local +X of the aperture, for the rect test
	XMFLOAT3 bitangent = XMFLOAT3(0, 1, 0);

	// False when followEntity has no transform this frame. Kept separate from
	// `enabled` so a frame where the entity was missing does not latch the element
	// off forever - which is indistinguishable, from the inspector, from a mirror
	// that simply refuses to work.
	bool resolved = false;

	// diagnostics
	// Written by Trace, reset by Update, read by the inspector. Mutable because Trace
	// is const and this is not state - it is the answer to "is a beam actually
	// reaching this thing?", which is otherwise pure guesswork.
	mutable int hitsThisFrame = 0;
	mutable float lastHitDistance = 0.0f;
	// What the element DID with those hits. "Hit" alone cannot tell a working mirror
	// from one the beam dies on, and those two look identical in the scene: both put a
	// spot on the glass, and only one of them also puts a beam somewhere else.
	mutable int reflectionsThisFrame = 0;  // beam left again - the element worked
	mutable int terminationsThisFrame = 0; // beam stopped here - bounce budget, or too dim
	// How far OUTSIDE the aperture the nearest beam crossed this element's plane,
	// metres. -1 when no beam crossed it at all. A positive number is the single most
	// useful thing to know when a mirror does nothing: the beam is reaching the plane
	// and missing the disc, so the aperture is too small.
	mutable float lastMissMargin = -1.0f;
};

struct Mirror {
	OpticSurface surface;

	// Energy kept per bounce. A front-surface mirror is ~0.95; a window pane used as
	// a beam splitter is more like 0.08.
	float reflectance = 0.95f;
	XMFLOAT3 tint = XMFLOAT3(1, 1, 1);

	// A real mirror is silvered on one face. Off, a beam arriving from behind passes
	// straight through instead of bouncing off the back of the glass.
	bool doubleSided = true;

	// dichroic / beam splitter
	// A dichroic mirror reflects one band and TRANSMITS the rest, so one beam arrives
	// and two leave. Turn this on and the trace branches: the reflected beam carries
	// `tint * reflectance`, and a second beam continues straight on carrying
	// `transmitTint * transmittance`.
	//
	// That is the whole difference between a dichroic and a plain mirror here - the
	// per-channel tints ARE the pass bands. A red-reflecting dichroic is
	// tint = (1, 0, 0) with transmitTint = (0, 1, 1); a 50/50 beam splitter is
	// tint = transmitTint = (1, 1, 1) with both coefficients at 0.5.
	//
	// Splitting costs: each branch is another walk and another set of legs against the
	// same segment budget, which is why BeamTraceDesc::maxSplits bounds it.
	bool dichroic = false;
	XMFLOAT3 transmitTint = XMFLOAT3(1, 1, 1);
	float transmittance = 0.9f;

	// Roughens the reflection: the outgoing direction is tilted by this many radians
	// about the ideal one. 0 is a laboratory mirror. Deterministic per bounce (there
	// is no per-frame noise), so a beam pointed at a scuffed mirror stays put rather
	// than jittering.
	float scatter = 0.0f;
};

struct Lens {
	OpticSurface surface;

	// Every element here is a RAY TRANSFER: it takes where on the aperture the beam
	// landed and changes the beam's angle by some function of that. Every lens whose
	// behaviour can be written that way is expressible, and the list below is that
	// set - spherical, cylindrical, toric, aspheric, axicon, prism, plain window. A
	// Fresnel lens is optically a spherical one (Spherical with the right focal
	// length is the correct model; only its appearance differs), and a plano-convex
	// or biconvex or meniscus lens differs only in which focal length you give it.
	//
	// What a paraxial ray model CANNOT do, so that it is stated rather than
	// discovered: chromatic dispersion (a leg carries one colour, not a spectrum, so
	// nothing splits into a rainbow), thick-lens and total internal reflection
	// (elements are infinitely thin), and any element that turns ONE beam into MANY -
	// a lenslet array, a diffraction grating. No LENS branches the trace; a dichroic
	// Mirror is the one element that does.
	enum class Type {
		Spherical,   // equal power in both axes - the ordinary lens
		Cylindrical, // power on the tangent axis only; the other axis passes straight
		Toric,       // independent power per axis (an astigmatic lens)
		Aspheric,    // spherical plus a conic term, so power varies with radius
		Axicon,      // conical: a fixed angle towards the axis wherever it lands,
		             // which turns a beam into a ring rather than a point
		Prism,       // constant deviation, the same wherever the beam lands
		Window,      // no power at all - tint, aperture and spread only
	};
	Type type = Type::Spherical;

	// Paraxial focal length in metres, on the tangent axis. Positive converges,
	// negative diverges. 0 behaves as Window whatever `type` says.
	float focalLength = 1.0f;
	// Toric only: the bitangent axis. Ignored by every other type.
	float focalLengthY = 1.0f;
	// Aspheric only: how much the power grows (+) or falls off (-) towards the rim.
	// Deviation is scaled by (1 + asphericity * r^2 / aperture^2), so 0 is spherical
	// and a small negative value is the usual spherical-aberration correction.
	float asphericity = 0.0f;
	// Axicon only: the half-angle it bends by, radians. Positive turns the beam
	// towards the optical axis.
	float axiconAngle = 0.05f;
	// Prism only: constant angular deviation per axis, radians.
	XMFLOAT2 prismDeviation = XMFLOAT2(0.0f, 0.0f);

	float transmittance = 0.92f;
	XMFLOAT3 tint = XMFLOAT3(1, 1, 1);

	// What the element does to the beam's own thickness on the way out. A collimator
	// narrows it (< 1), a diffuser widens it (> 1).
	float beamScale = 1.0f;

	// Extra divergence added to the outgoing leg, radians. This is what turns a laser
	// into a cone - a diffusing element rather than a focusing one. Applied as a
	// widening of the beam radius over the leg, not as a fan of extra rays: one
	// visible shaft is what the pass draws.
	float spread = 0.0f;
};

// One straight leg of a traced beam.
struct BeamLeg {
	XMFLOAT3 start = XMFLOAT3(0, 0, 0);
	XMFLOAT3 end = XMFLOAT3(0, 0, 0);
	XMFLOAT3 direction = XMFLOAT3(0, 0, 1);
	float length = 0.0f;

	// Colour multiplier accumulated through every element the beam has already
	// passed, including the one this leg starts at. Starts at (1,1,1).
	XMFLOAT3 throughput = XMFLOAT3(1, 1, 1);

	// Beam thickness multiplier for this leg, likewise accumulated. A lens with
	// beamScale 0.5 halves it for everything downstream.
	float radiusScale = 1.0f;

	// Radians of divergence picked up from lenses upstream. The renderer widens the
	// leg along its length by this much.
	float divergence = 0.0f;

	// What stopped this leg. Named `termination` rather than `end` because `end` is
	// already the leg's far endpoint above.
	enum class Termination {
		Range,   // ran out of maxDistance without meeting anything
		Surface, // landed on scene geometry - this is the last leg
		Mirror,  // reflected onwards
		Lens,    // refracted onwards
	};
	Termination termination = Termination::Range;
};

// Where one branch of a beam finished, and how bright it still was when it got
// there. A plain beam has one of these; a beam through a dichroic has one per branch.
struct BeamTerminal {
	RayHit hit;
	// Colour multiplier the beam still carried at this endpoint. The two halves of a
	// dichroic land in different colours, so this cannot be read off the laser.
	XMFLOAT3 throughput = XMFLOAT3(1, 1, 1);
};

// The result of walking a ray through the optics and the scene.
struct BeamPath {
	wi::vector<BeamLeg> legs;

	// Every endpoint worth putting a spot on: a branch that landed on geometry, or one
	// that died on an element because the bounce budget ran out. A branch that simply
	// ran out of range is not in here - there is nothing there to light.
	wi::vector<BeamTerminal> terminals;

	// The FIRST terminal, as a convenience for the common single-beam case.
	// hit=false when nothing landed - `hit.position` is still the end of the last leg
	// either way, so a caller that just needs somewhere to point can use it.
	RayHit hit;

	// True when the walk stopped because it hit the bounce ceiling rather than
	// because it ran out of beam. Worth surfacing: a beam trapped between two facing
	// mirrors looks identical to a broken one otherwise.
	bool exhaustedBounces = false;
};

struct BeamTraceDesc {
	XMFLOAT3 origin = XMFLOAT3(0, 0, 0);
	XMFLOAT3 direction = XMFLOAT3(0, 0, 1);

	// Total path length across every leg, not per leg.
	float maxDistance = 200.0f;

	// How many times the beam may reflect or refract. 0 is a straight line that
	// ignores the optics entirely.
	int maxBounces = 4;

	// Below this the beam is not worth another leg. Stops a chain of dim mirrors from
	// eating the segment budget with legs nobody can see.
	float minThroughput = 0.01f;

	// How many times a dichroic may split the beam in two, across the whole walk. Each
	// split doubles the branches that follow it, so this is a hard cap on an
	// exponential: two dichroics facing each other would otherwise fill the segment
	// budget on their own. 0 makes every dichroic behave as a plain mirror.
	int maxSplits = 3;

	// How far PAST the first geometry hit an element still counts as being in front
	// of it, metres.
	//
	// This is not a fudge, it is the whole reason a mirror works at all. A mirror is
	// a plane with no geometry of its own, and the thing that makes it VISIBLE is a
	// mesh sitting in exactly the same place. Ray-vs-triangle and ray-vs-plane then
	// answer the same question in two different ways and disagree in the last few
	// bits - so without a bias the beam stops on the glass roughly half the time and
	// reflects the other half, which reads as "mirrors are broken".
	//
	// Raise it for an element embedded in a thick prop (a mirror inside a 5 cm frame
	// wants ~0.05). Lower it if a beam is bouncing off an element that is genuinely
	// behind a wall.
	float opticBias = 0.02f;

	// How the geometry end of the beam is found. Mode::None makes the beam pass
	// through the world and only interact with the optics.
	RayQuery::Mode mode = RayQuery::Mode::Mesh;
	uint32_t filterMask = wi::enums::FILTER_OPAQUE;
	uint32_t layerMask = ~0u;
	wi::ecs::Entity ignoreEntity = wi::ecs::INVALID_ENTITY;
};

// Owns the mirror and lens lists. One instance, driven by st::App, reachable from
// anywhere the way ProjectorSystem is:
//
//   st::OpticsSystem::Get().AddMirror(mirror)
//   st::OpticsSystem::Get().FindMirror(id)->reflectance = 0.5f
class OpticsSystem {
public:
	using ID = uint32_t;
	static constexpr ID INVALID = ~0u;

	// The system st::App drives. Before the app has initialised (or in a test with no
	// app at all) this is an inert instance holding no elements.
	static OpticsSystem& Get();

	OpticsSystem() = default;
	~OpticsSystem();
	OpticsSystem(const OpticsSystem&) = delete;
	OpticsSystem& operator=(const OpticsSystem&) = delete;

	// Claims Get(). There is no GPU state here, so unlike ProjectorSystem::Init this
	// does not need the graphics device and can be called at any time.
	void Init();

	ID AddMirror(const Mirror& mirror = Mirror());
	Mirror* FindMirror(ID id);
	const Mirror* FindMirror(ID id) const;
	void RemoveMirror(ID id);

	ID AddLens(const Lens& lens = Lens());
	Lens* FindLens(ID id);
	const Lens* FindLens(ID id) const;
	void RemoveLens(ID id);

	void Clear();
	size_t MirrorCount() const { return mirrors_.size(); }
	size_t LensCount() const { return lenses_.size(); }

	// By index rather than by id, for a caller that wants to walk every element -
	// st::ProjectorSystem does, to build a virtual projector per mirror. Null when the
	// index is out of range; the pointer is valid until the list is next edited.
	const Mirror* MirrorAt(size_t index) const;
	const Lens* LensAt(size_t index) const;

	// Refreshes followed transforms and recomputes every element's cached axes. Call
	// once per frame, after the scene update and BEFORE anything traces a beam.
	void Update(const wi::scene::Scene& scene);

	// Walks `desc` through the elements and the scene. Safe to call as often as you
	// like; it allocates only into `out`.
	void Trace(const wi::scene::Scene& scene, const BeamTraceDesc& desc, BeamPath& out) const;

	void GUI();
	void SaveTo(nbt::Tag& out) const;
	void LoadFrom(const nbt::Tag& in);

	// Master switch. Off, beams travel in straight lines and ignore every element.
	bool enabled = true;

private:
	struct MirrorEntry {
		ID id = INVALID;
		Mirror mirror;
	};
	struct LensEntry {
		ID id = INVALID;
		Lens lens;
	};

	wi::vector<MirrorEntry> mirrors_;
	wi::vector<LensEntry> lenses_;
	ID nextID_ = 0;

	int selectedMirror_ = 0; // GUI only
	int selectedLens_ = 0;
};

// Refresh one surface's cached position/rotation/axes from the scene. Exposed
// because a component that owns a surface outside the system still wants the same
// resolution rules. Returns false when followEntity is set but has no transform.
bool ResolveOpticSurface(const wi::scene::Scene& scene, OpticSurface& surface);

// Ray/aperture intersection in world space. `distance` is along `direction` from
// `origin`, `point` is where it landed. Returns false for a miss, for a ray running
// parallel to the surface, and for a hit closer than `minDistance`.
//
// Also records the surface's diagnostics on the way through, which is why it takes a
// const reference to something it writes to - see OpticSurface's mutable block.
bool IntersectOpticSurface(const OpticSurface& surface, const XMFLOAT3& origin, const XMFLOAT3& direction,
	float minDistance, float maxDistance, float& distance, XMFLOAT3& point);

// shared inspector pieces
// The system panel and the native components both draw these, so they live here
// rather than being written twice and drifting apart.

// ImGui combo item list for Lens::Type, in enum order.
extern const char* const LENS_TYPE_ITEMS;

// Lens::Type by name, for the "type" metadata argument.
Lens::Type ParseLensType(const std::string& value);
const char* LensTypeName(Lens::Type type);

// Draws only the fields the given type actually uses - a prism has no focal length
// and showing it one is how a knob gets turned for ten minutes with no effect.
// Returns true when something was edited. No Begin/End.
bool LensTypeFields(Lens& lens);

// "Is a beam actually reaching this thing?" - resolved state, hit count, and how far
// outside the aperture the nearest beam crossed. No Begin/End.
void OpticDiagnostics(const OpticSurface& surface);

} // namespace st
