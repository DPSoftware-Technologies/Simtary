#pragma once
#include "Simtary.h"
#include "io/Nbt.h"
#include "scene/Ray.h" // st::LocalAxes - the one axis table Forward is numbered against
#include "StProjectorInterop.h"

namespace st {

namespace gfx { class Framebuffer; }

// A light projector: a square (or rectangular, elliptical, rounded) cone of light
// that carries an image, with the optics a real projector has.
//
// Why this is not a LightComponent: the engine's spot light is circular by
// construction. light_spot() in Engine/shaders/lightingHF.hlsli discards everything
// outside `dot(L, direction) < cos(outerConeAngle)`, so attaching a mask texture (or
// a video, or a camera render) to a spot light gives you the image cropped into a
// circle - which is exactly what the cinema projector in Milistry was doing. Making
// it square means changing the cone test, and that lives in the engine core repo.
// This system gets the same result without touching Engine/, by projecting in a
// screen-space pass of its own.
//
// The trade for staying out of the core:
//   + arbitrary gate shape, keystone, lens shift, barrel/pincushion, per-projector
//     beam settings - none of which the LightComponent has a place to store
//   + the image is sampled at full resolution rather than through the shadow atlas
//   - no shadow map. `occlusion` is a screen-space approximation and cannot see
//     blockers that are off screen
//   - transparent surfaces are not lit (the pass runs after the transparent pass)
//   - no contribution to reflections, GI or the engine's own volumetric buffer
//
// Use it with the scene's spot light switched off, or turned down to whatever spill
// light the housing should throw.
struct Projector {
	enum class Shape {
		Rect,        // square / rectangle - what a projector gate actually is
		Ellipse,     // circle / ellipse - a spot light, for comparison
		RoundedRect, // rectangle with rounded corners
	};

	// Which local axis the beam travels down. The engine's own conventions differ per
	// component, so this is explicit rather than guessed:
	//
	//   PlusZ   camera-like props (CameraComponent looks down local +Z)
	//   MinusY  what a spot light actually projects along - use this when the
	//           projector is standing in for one. Careful: LightComponent::direction
	//           is the OPPOSITE vector (it points from a lit surface back at the
	//           light); the engine's own shadow/mask camera uses -Y, see SHCAM::init
	//           in wiRenderer.cpp.
	//
	// X was appended rather than inserted where it would read better: scene metadata
	// already stores these as plain integers, so renumbering 0-3 would silently
	// re-aim every projector already placed. st::LocalAxes (Framework/scene/Ray.h) is
	// the one table behind this, shared with the laser, the ray and the optics.
	enum class Forward {
		PlusZ,
		MinusZ,
		MinusY,
		PlusY,
		PlusX,
		MinusX,
	};

	bool enabled = true;

	// ── placement ────────────────────────────────────────────────────────────────
	// With followEntity set, position/rotation are refreshed from that entity's
	// world transform every frame and whatever you wrote into them is overwritten.
	wi::ecs::Entity followEntity = wi::ecs::INVALID_ENTITY;
	XMFLOAT3 position = XMFLOAT3(0, 0, 0);
	XMFLOAT4 rotation = XMFLOAT4(0, 0, 0, 1); // quaternion
	Forward forward = Forward::PlusZ;

	// ── optics ───────────────────────────────────────────────────────────────────
	// Throw ratio is how projectors are actually specified: image width = throw
	// distance / throwRatio. 1.6 is a common lens; 0.5 is short-throw. Set it to 0
	// to drive the gate from `fov` (vertical, radians) instead.
	float throwRatio = 1.6f;
	float fov = 0.5f;
	float aspect = 16.0f / 9.0f; // 1.0 gives the literal square this system is named for
	float roll = 0.0f;           // rotation of the image about the lens axis, radians
	XMFLOAT2 lensShift = XMFLOAT2(0, 0); // in image half-widths: (0, 0.5) raises by half an image
	XMFLOAT2 keystone = XMFLOAT2(0, 0);  // trapezoid correction for an off-axis screen
	float distortion = 0.0f;             // + barrel, - pincushion
	Shape shape = Shape::Rect;
	float cornerRadius = 0.15f; // RoundedRect only, fraction of the half-extent
	float softness = 0.02f;     // feathered edge, in gate units (0 = hard edge)
	float vignette = 0.0f;      // corner darkening
	float nearClip = 0.05f;
	float range = 40.0f; // beyond this the projector contributes nothing

	// ── image ────────────────────────────────────────────────────────────────────
	// Anything with an SRV: a loaded texture, a st::gfx::Framebuffer, a video frame,
	// a CameraComponent::render_to_texture target. Leave invalid for flat colour.
	wi::graphics::Texture texture;
	// Which view of `texture` to sample. -1 is its default SRV; pass an SRGB
	// subresource (wi::Resource::GetTextureSRGBSubresource(),
	// VideoInstance::GetCurrentFrameTextureSRGBSubresource()) for content authored in
	// sRGB, which is what the engine does for light masks.
	int textureSubresource = -1;

	// Take the image from a scene entity instead, refreshed every frame. Overwrites
	// `texture` and `textureSubresource` while it is set.
	wi::ecs::Entity imageEntity = wi::ecs::INVALID_ENTITY;

	// Which component on `imageEntity` provides the picture.
	//	Auto     video, else camera render target, else material base colour - the same
	//	         order and priority the engine uses for a light's mask texture
	//	Video    VideoComponent only
	//	Camera   CameraComponent::render_to_texture only
	//	Material MaterialComponent base colour only. Pick this when the entity also
	//	         carries a video but you want something else on screen - a
	//	         st::gfx::Framebuffer bound with BindToLightMask, for instance
	//	None     ignore imageEntity; whatever is in `texture` stays
	enum class ImageSource { Auto, Video, Camera, Material, None };
	ImageSource imageSource = ImageSource::Auto;
	XMFLOAT3 color = XMFLOAT3(1, 1, 1);
	float intensity = 6.0f;
	float gamma = 1.0f;
	// 0 keeps the image equally bright at any throw distance, 1 is the physical
	// inverse square normalised so `focusDistance` is where `intensity` holds.
	float falloff = 1.0f;
	float focusDistance = 8.0f;

	// ── shadows ──────────────────────────────────────────────────────────────────
	// A depth map rendered from the lens, once per frame. This is what stops the
	// image from passing through a wall and landing on whatever is behind it. It sees
	// blockers the camera cannot - off screen, or viewed from a completely different
	// angle - which no screen-space trick can. Cost is one extra depth-only pass over
	// the visible geometry per projector.
	bool shadows = true;
	int shadowResolution = 1024;
	float shadowBias = 0.002f; // in projector clip depth; raise if surfaces self-shadow

	// ── reflections and lenses ───────────────────────────────────────────────────
	// Let the image bounce off st::Mirror and bend through st::Lens, the same
	// elements a laser uses (Framework/render/Optics.h).
	//
	// This is NOT a traced beam - a projector is a cone, not a ray. What happens
	// instead is the standard planar-reflection trick: for each element the image can
	// reach, a VIRTUAL projector is uploaded, standing where the mirror image of this
	// one would be, and clipped so it only lights what actually passes through that
	// element's aperture. A mirror reflects position and orientation exactly (the
	// image comes out flipped, as it should); a lens moves the apex to where the thin
	// lens images it, which zooms and shifts the picture the way a projector lens
	// does.
	//
	// Off by default (0), because each element the image reaches costs another
	// ST_PROJECTOR_MAX slot AND another shadow map. 1 is one bounce - a projector
	// pointed at a mirror lighting the wall behind it. Bounces do NOT compound: a
	// virtual projector does not itself reflect, so two facing mirrors give two
	// images, not an infinite corridor of them.
	int opticBounces = 0;

	// Elements dimmer than this are not worth a slot or a shadow map.
	float opticMinThroughput = 0.02f;

	// ── surfaces ─────────────────────────────────────────────────────────────────
	bool lightSurfaces = true;
	bool lambert = true;   // fall off with the angle between surface and lens
	// Screen-space shadowing. Only used when `shadows` is off - it is the weaker of
	// the two: it can only test what the camera itself rendered.
	bool occlusion = true;
	float occlusionStrength = 1.0f;
	int occlusionSamples = 12;
	float occlusionThickness = 1.0f; // metres of assumed blocker depth

	// ── beam ─────────────────────────────────────────────────────────────────────
	bool beam = true;
	float beamDensity = 0.05f;   // scattering per metre of air
	float beamAnisotropy = 0.6f; // 0 isotropic, -> 1 forward scattering (bright head-on)
	int beamSamples = 24;

	// Point the image source at a framebuffer. Equivalent to assigning `texture`.
	void Attach(const gfx::Framebuffer& framebuffer);

	// Vertical field of view actually used, after throwRatio/fov/aspect are resolved.
	float ResolveFovY() const;
};

// Owns the projector list, the GPU upload and the render path hook.
//
//   st::ProjectorSystem::Get()                  the instance the framework runs
//   ID id = ...Get().Add(projector)             add one
//   ...Get().Find(id)->intensity = 12.0f        edit it live
//   ...Get().Remove(id)                         drop it
//
// Nothing else is needed: st::App calls Init/Bind/Update for you. st::App owns the
// instance - Get() only hands out a reference to it, so a scene or a native
// component can reach it without being given a pointer to the app.
class ProjectorSystem {
public:
	using ID = uint32_t;
	static constexpr ID INVALID = ~0u;

	// The system st::App drives. Before the app has initialised (or in a test with no
	// app at all) this is an inert instance: adding to it is harmless and draws
	// nothing.
	static ProjectorSystem& Get();

	ProjectorSystem() = default;
	~ProjectorSystem();
	ProjectorSystem(const ProjectorSystem&) = delete;
	ProjectorSystem& operator=(const ProjectorSystem&) = delete;

	// Loads StProjectorCS and creates the upload buffers. Call once, after the
	// graphics device exists.
	void Init();

	// Attaches the pass to a render path. Safe to call again with another path; the
	// previous one is unhooked first.
	void Bind(wi::RenderPath3D& path);
	void Unbind();

	ID Add(const Projector& projector = Projector());
	Projector* Find(ID id);
	const Projector* Find(ID id) const;
	void Remove(ID id);
	void Clear();
	size_t Count() const { return projectors_.size(); }

	// Refreshes followed transforms, rebuilds the matrices and uploads. Call once per
	// frame, after the scene update so followed entities carry this frame's transform,
	// and after st::OpticsSystem::Update when any projector has opticBounces set.
	void Update(const wi::scene::Scene& scene, float dt);

	// Records each shadow-casting projector's depth map into `cmd`. Must run after the
	// engine has uploaded this frame's render data and before the post process chain
	// that samples the maps - st::ProjectorRenderPath places it for you.
	void RenderShadows(wi::graphics::CommandList cmd) const;

	void GUI();
	void SaveTo(nbt::Tag& out) const;
	void LoadFrom(const nbt::Tag& in);

	bool IsValid() const { return shader_.IsValid(); }

	// Master switch. Off costs nothing: the pass is removed from the render path.
	bool enabled = true;

private:
	struct Entry {
		ID id = INVALID;
		Projector projector;
	};

	void Hook();
	void Unhook();
	wi::RenderPath3D::CustomPostprocess* FindPass() const;

	wi::vector<Entry> projectors_;
	ID nextID_ = 0;

	wi::graphics::Shader shader_;
	// One buffer per frame in flight: the CPU writes next frame's projectors while
	// the GPU is still reading last frame's.
	wi::graphics::GPUBuffer buffers_[wi::graphics::GraphicsDevice::GetBufferCount()];

	// Shadow slots run parallel to the uploaded projector array, not to projectors_:
	// slot i belongs to whatever projector landed in shaderProjectors[i] this frame.
	// wi::renderer::Visibility holds atomics and a spinlock, so it cannot live inside
	// a wi::vector element - a fixed array is the simple way to keep it put.
	struct ShadowSlot {
		wi::graphics::Texture map;
		wi::scene::CameraComponent camera;
		bool active = false;
	};
	ShadowSlot shadowSlots_[ST_PROJECTOR_MAX];
	wi::renderer::Visibility shadowVisibility_[ST_PROJECTOR_MAX];

	wi::RenderPath3D* path_ = nullptr;
	bool hooked_ = false;

	int selected_ = 0; // GUI only
};

// The render path st::App runs. RenderPath3D with one addition: the projector depth
// maps are recorded at the top of the post process chain.
//
// Why here and not from App::Render(): command lists submit in the order they are
// begun, so a list opened before the path runs BEFORE the engine has uploaded this
// frame's instance data - the depth map then draws last frame's (or a half-written)
// transform set and the shadows flicker. Inside the chain the upload is long done,
// and the maps are still recorded ahead of the custom post process that samples them.
class ProjectorRenderPath : public wi::RenderPath3D {
public:
	ProjectorSystem* projectors = nullptr;

	void RenderPostprocessChain(wi::graphics::CommandList cmd) const override {
		if (projectors != nullptr) {
			projectors->RenderShadows(cmd);
		}
		wi::RenderPath3D::RenderPostprocessChain(cmd);
	}
};

} // namespace st
