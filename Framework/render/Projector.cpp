#include "render/Projector.h"
#include "render/Framebuffer.h"
#include "render/Optics.h" // mirrors + lenses, for the virtual projectors they produce
#include "imgui.h"

#include <algorithm>
#include <cstring>

namespace st {

using namespace wi::graphics;

namespace {

constexpr const char* PASS_NAME = "StProjector";

// Set by Init(), cleared by the destructor: st::App owns the real system, and this
// is how everything else reaches it. Null until the app has initialised.
ProjectorSystem* g_instance = nullptr;

// Local axes for each Forward setting: {forward, up}. The up vector only has to be
// non-parallel to the forward one - `roll` rotates it about the axis afterwards.
//
// One table, in Framework/scene/Ray.h, shared with the laser, the ray component and
// the optics: Projector::Forward is defined to have the same numbering, and two
// copies of it would eventually disagree about which way -Y points. -Y is the one a
// spot light projects along (the defaults SHCAM::init uses in wiRenderer.cpp), which
// is what drops a projector straight onto an existing spot light entity.
void LocalAxes(Projector::Forward forward, XMVECTOR& localForward, XMVECTOR& localUp) {
	st::LocalAxes((int)forward, localForward, localUp);
}

float Luminance(const XMFLOAT3& c) {
	return c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f;
}

uint32_t ShapeToShader(Projector::Shape shape) {
	switch (shape) {
	case Projector::Shape::Ellipse: return ST_PROJECTOR_SHAPE_ELLIPSE;
	case Projector::Shape::RoundedRect: return ST_PROJECTOR_SHAPE_ROUNDED;
	case Projector::Shape::Rect:
	default: return ST_PROJECTOR_SHAPE_RECT;
	}
}

// Refresh the projected image from `imageEntity`, in the same priority order the
// engine uses when it takes a mask texture off a light (wiScene.cpp): a video frame
// beats a camera render, which beats a plain material texture.
void ResolveImage(const wi::scene::Scene& scene, Projector& projector) {
	if (projector.imageEntity == wi::ecs::INVALID_ENTITY) return;
	if (projector.imageSource == Projector::ImageSource::None) return;

	const bool any = projector.imageSource == Projector::ImageSource::Auto;

	if (any || projector.imageSource == Projector::ImageSource::Video) {
		if (const wi::scene::VideoComponent* video = scene.videos.GetComponent(projector.imageEntity)) {
			projector.texture = video->videoinstance.GetCurrentFrameTexture();
			projector.textureSubresource = video->videoinstance.GetCurrentFrameTextureSRGBSubresource();
			return;
		}
	}

	if (any || projector.imageSource == Projector::ImageSource::Camera) {
		if (const wi::scene::CameraComponent* camera = scene.cameras.GetComponent(projector.imageEntity)) {
			if (camera->render_to_texture.rendertarget_render.IsValid()) {
				projector.texture = camera->render_to_texture.rendertarget_render;
				projector.textureSubresource = -1;
				return;
			}
		}
	}

	if (any || projector.imageSource == Projector::ImageSource::Material) {
		if (const wi::scene::MaterialComponent* material = scene.materials.GetComponent(projector.imageEntity)) {
			const auto& slot = material->textures[wi::scene::MaterialComponent::BASECOLORMAP];
			if (slot.resource.IsValid()) {
				projector.texture = slot.resource.GetTexture();
				projector.textureSubresource = slot.resource.GetTextureSRGBSubresource();
				return;
			}
		}
	}

	// Nothing on that entity (yet - a video's first frame is not decoded until it
	// plays). Leave whatever was there rather than blinking the image off.
}

} // namespace

void Projector::Attach(const gfx::Framebuffer& framebuffer) {
	texture = framebuffer.GetTexture();
	textureSubresource = -1;
	imageEntity = wi::ecs::INVALID_ENTITY;
}

float Projector::ResolveFovY() const {
	float tanHalfY;
	if (throwRatio > 0.0001f) {
		// Throw ratio is defined on the image WIDTH: width = distance / throwRatio,
		// so the half-angle across is atan(0.5 / throwRatio). The gate is square in
		// NDC, so the vertical angle is the horizontal one divided by the aspect.
		const float tanHalfX = 0.5f / throwRatio;
		tanHalfY = tanHalfX / std::max(0.01f, aspect);
	} else {
		tanHalfY = std::tan(std::max(0.01f, fov) * 0.5f);
	}
	return wi::math::Clamp(2.0f * std::atan(tanHalfY), 0.01f, XM_PI * 0.95f);
}

ProjectorSystem& ProjectorSystem::Get() {
	if (g_instance != nullptr) return *g_instance;
	// No app yet. Hand back something inert rather than a null reference - its shader
	// never loads, so it draws nothing and every call on it is a no-op.
	static ProjectorSystem fallback;
	return fallback;
}

ProjectorSystem::~ProjectorSystem() {
	if (g_instance == this) g_instance = nullptr;
}

void ProjectorSystem::Init() {
	g_instance = this;

	if (!wi::renderer::LoadShader(ShaderStage::CS, shader_, "StProjectorCS.cso")) {
		wi::backlog::post(
			"[Projector] failed to load StProjectorCS.cso from " + wi::renderer::GetShaderPath() +
				". The shader source is staged into <exe>/shaders/ by simtary_add_app(); "
				"without it every projector is inert.",
			wi::backlog::LogLevel::Error);
		return;
	}

	GraphicsDevice* device = GetDevice();

	GPUBufferDesc desc;
	desc.size = sizeof(StProjector) * ST_PROJECTOR_MAX;
	desc.bind_flags = BindFlag::SHADER_RESOURCE;
	// Raw, not structured: the shader reads it with ByteAddressBuffer::Load<StProjector>().
	desc.misc_flags = ResourceMiscFlag::BUFFER_RAW;
	// UPLOAD keeps it permanently mapped, so Update() is a memcpy with no copy queue
	// work. One per frame in flight, because the GPU is still reading the previous
	// frame's contents while this frame is being written.
	desc.usage = Usage::UPLOAD;

	for (uint32_t i = 0; i < GraphicsDevice::GetBufferCount(); ++i) {
		if (!device->CreateBuffer(&desc, nullptr, &buffers_[i])) {
			wi::backlog::post("[Projector] projector buffer creation failed", wi::backlog::LogLevel::Error);
			shader_ = {};
			return;
		}
		device->SetName(&buffers_[i], "st::ProjectorSystem::buffer");
	}
}

void ProjectorSystem::Bind(wi::RenderPath3D& path) {
	if (path_ == &path) return;
	Unbind();
	path_ = &path;
}

void ProjectorSystem::Unbind() {
	Unhook();
	path_ = nullptr;
}

wi::RenderPath3D::CustomPostprocess* ProjectorSystem::FindPass() const {
	if (path_ == nullptr) return nullptr;
	for (auto& pass : path_->custom_post_processes) {
		if (pass.name == PASS_NAME) return &pass;
	}
	return nullptr;
}

void ProjectorSystem::Hook() {
	if (path_ == nullptr || !shader_.IsValid()) return;
	if (FindPass() != nullptr) {
		hooked_ = true;
		return;
	}

	wi::RenderPath3D::CustomPostprocess pass;
	pass.name = PASS_NAME;
	pass.computeshader = shader_;
	// Before tonemap: the projector adds HDR light, so it blooms and tone maps with
	// the rest of the frame instead of being pasted on top of the finished image.
	pass.stage = wi::RenderPath3D::CustomPostprocess::Stage::BeforeTonemap;
	path_->custom_post_processes.push_back(pass);
	hooked_ = true;
}

void ProjectorSystem::Unhook() {
	if (!hooked_) return;
	if (wi::RenderPath3D::CustomPostprocess* pass = FindPass()) {
		// begin() + index rather than the raw pointer: wi::vector is std::vector in
		// some configurations, and that one only takes an iterator.
		path_->custom_post_processes.erase(
			path_->custom_post_processes.begin() + (pass - path_->custom_post_processes.data()));
	}
	hooked_ = false;
}

ProjectorSystem::ID ProjectorSystem::Add(const Projector& projector) {
	if (projectors_.size() >= ST_PROJECTOR_MAX) {
		wi::backlog::post(
			"[Projector] ST_PROJECTOR_MAX (" + std::to_string(ST_PROJECTOR_MAX) + ") reached, projector not added",
			wi::backlog::LogLevel::Warning);
		return INVALID;
	}
	Entry& entry = projectors_.emplace_back();
	entry.id = nextID_++;
	entry.projector = projector;
	return entry.id;
}

Projector* ProjectorSystem::Find(ID id) {
	for (auto& entry : projectors_) {
		if (entry.id == id) return &entry.projector;
	}
	return nullptr;
}

const Projector* ProjectorSystem::Find(ID id) const {
	for (const auto& entry : projectors_) {
		if (entry.id == id) return &entry.projector;
	}
	return nullptr;
}

void ProjectorSystem::Remove(ID id) {
	for (size_t i = 0; i < projectors_.size(); ++i) {
		if (projectors_[i].id == id) {
			projectors_.erase(projectors_.begin() + i);
			return;
		}
	}
}

void ProjectorSystem::Clear() {
	projectors_.clear();
}

void ProjectorSystem::Update(const wi::scene::Scene& scene, float /*dt*/) {
	if (path_ == nullptr) return;

	if (!enabled || !shader_.IsValid() || projectors_.empty()) {
		Unhook();
		return;
	}

	GraphicsDevice* device = GetDevice();

	StProjector shaderProjectors[ST_PROJECTOR_MAX] = {};
	uint32_t count = 0;

	// Fill one upload slot, its shadow map and its culling. Everything a projector
	// puts on the GPU goes through here, so a VIRTUAL projector - the image of a real
	// one in a mirror or through a lens - is identical to a real one except for where
	// it stands, what tints it and the aperture it is clipped to. That is the whole
	// point of the planar-reflection trick: the reflected image is just a projector
	// somewhere else, including for the shadow map, which then correctly stops the
	// reflection at whatever blocks it on the far side.
	const auto Emit = [&](const Projector& projector, XMVECTOR position, XMVECTOR forward, XMVECTOR up,
						  const XMFLOAT3& tint, const OpticSurface* clip) -> bool {
		if (count >= ST_PROJECTOR_MAX) return false;

		const uint32_t slot = count;
		ShadowSlot& shadow = shadowSlots_[slot];
		shadow.active = false;

		// The matrix is built through a CameraComponent rather than by hand, so the
		// gate and the shadow map cannot drift apart: the same camera is what
		// DrawScene() renders the depth map with. It also inherits the engine's
		// reverse-Z convention (UpdateCamera passes zFarP as the near argument), which
		// is what the shadow compare in the shader expects.
		wi::scene::CameraComponent& camera = shadow.camera;
		XMStoreFloat3(&camera.Eye, position);
		XMStoreFloat3(&camera.At, forward);
		XMStoreFloat3(&camera.Up, up);
		camera.CreatePerspective(
			std::max(0.01f, projector.aspect),
			1.0f,
			std::max(0.001f, projector.nearClip),
			std::max(projector.nearClip + 0.01f, projector.range),
			projector.ResolveFovY());

		XMFLOAT4X4 viewProjection;
		XMStoreFloat4x4(&viewProjection, camera.GetViewProjection());

		StProjector& out = shaderProjectors[count++];

		// Rows, not columns - the shader multiplies with the position on the left.
		out.vp0 = XMFLOAT4(viewProjection._11, viewProjection._12, viewProjection._13, viewProjection._14);
		out.vp1 = XMFLOAT4(viewProjection._21, viewProjection._22, viewProjection._23, viewProjection._24);
		out.vp2 = XMFLOAT4(viewProjection._31, viewProjection._32, viewProjection._33, viewProjection._34);
		out.vp3 = XMFLOAT4(viewProjection._41, viewProjection._42, viewProjection._43, viewProjection._44);

		XMStoreFloat3(&out.position, position);
		out.range = projector.range;
		XMStoreFloat3(&out.direction, forward);
		out.intensity = projector.intensity;
		out.color = XMFLOAT3(projector.color.x * tint.x, projector.color.y * tint.y, projector.color.z * tint.z);
		out.gamma = std::max(0.01f, projector.gamma);

		out.flags = 0;
		if (projector.lightSurfaces) out.flags |= ST_PROJECTOR_FLAG_LIGHT_SURFACES;
		if (projector.beam) out.flags |= ST_PROJECTOR_FLAG_BEAM;
		if (projector.occlusion) out.flags |= ST_PROJECTOR_FLAG_OCCLUSION;
		if (projector.lambert) out.flags |= ST_PROJECTOR_FLAG_LAMBERT;

		out.shape = ShapeToShader(projector.shape);

		// 0 means "no image, use the flat colour" on the shader side, which is also
		// what GetDescriptorIndex returns -1 for.
		const int descriptor = projector.texture.IsValid()
			? device->GetDescriptorIndex(&projector.texture, SubresourceType::SRV, projector.textureSubresource)
			: -1;
		out.texture_index = descriptor < 0 ? 0u : (uint32_t)descriptor;

		out.softness = std::max(0.0f, projector.softness);
		out.shift = projector.lensShift;
		out.keystone = projector.keystone;
		out.corner_radius = wi::math::Clamp(projector.cornerRadius, 0.0f, 1.0f);
		out.vignette = wi::math::Clamp(projector.vignette, 0.0f, 1.0f);
		out.distortion = projector.distortion;
		out.falloff = wi::math::Clamp(projector.falloff, 0.0f, 1.0f);
		out.focus_distance = std::max(0.01f, projector.focusDistance);

		out.beam_density = std::max(0.0f, projector.beamDensity);
		out.beam_anisotropy = wi::math::Clamp(projector.beamAnisotropy, -0.95f, 0.95f);
		out.beam_samples = (uint32_t)std::clamp(projector.beamSamples, 2, 64);

		out.occlusion_strength = wi::math::Clamp(projector.occlusionStrength, 0.0f, 1.0f);
		out.occlusion_samples = (uint32_t)std::clamp(projector.occlusionSamples, 1, 32);
		out.occlusion_thickness = std::max(0.01f, projector.occlusionThickness);

		// -- aperture clip --------------------------------------------------------
		// A real projector has none and lights its whole cone. A virtual one is a cone
		// from a point behind the glass, and only the part passing through the element
		// is light that actually exists.
		if (clip != nullptr) {
			out.clip_center = clip->position;
			out.clip_normal = clip->normal;
			out.clip_tangent = clip->tangent;
			out.clip_bitangent = clip->bitangent;
			// Negative half-height is the shader's flag for a circular aperture.
			out.clip_radius = std::max(clip->halfExtent.x, 1e-4f);
			out.clip_half_y = clip->circular ? -1.0f : std::max(clip->halfExtent.y, 1e-4f);
		} else {
			out.clip_radius = 0.0f;
			out.clip_half_y = 0.0f;
		}

		// -- shadow map -----------------------------------------------------------
		out.shadow_index = 0;
		if (projector.shadows) {
			const uint32_t resolution = (uint32_t)std::clamp(projector.shadowResolution, 128, 4096);

			if (!shadow.map.IsValid() || shadow.map.desc.width != resolution) {
				TextureDesc desc;
				desc.width = resolution;
				desc.height = resolution;
				desc.format = wi::renderer::format_depthbuffer_shadowmap;
				desc.bind_flags = BindFlag::DEPTH_STENCIL | BindFlag::SHADER_RESOURCE;
				desc.layout = ResourceState::SHADER_RESOURCE;
				desc.clear.depth_stencil.depth = 0; // reverse-Z: far is 0
				if (device->CreateTexture(&desc, nullptr, &shadow.map)) {
					device->SetName(&shadow.map, "st::ProjectorSystem::shadowmap");
				}
			}

			if (shadow.map.IsValid()) {
				const int shadowDescriptor = device->GetDescriptorIndex(&shadow.map, SubresourceType::SRV);
				if (shadowDescriptor >= 0) {
					out.shadow_index = (uint32_t)shadowDescriptor;
					out.shadow_bias = projector.shadowBias;
					out.shadow_texel = 1.0f / (float)resolution;

					// Cull now, on the CPU, while the scene is not being rendered.
					// RenderShadows() then only records draw commands.
					wi::renderer::Visibility& visibility = shadowVisibility_[slot];
					visibility.layerMask = ~0u;
					visibility.scene = &scene;
					visibility.camera = &shadow.camera;
					// Objects only: a depth map wants geometry, not the scene's lights,
					// decals, probes or its shadow atlas packing.
					visibility.flags = wi::renderer::Visibility::ALLOW_OBJECTS;
					wi::renderer::UpdateVisibility(visibility);

					shadow.active = !visibility.visibleObjects.empty();
				}
			}
		}

		return true;
	};

	const OpticsSystem& optics = OpticsSystem::Get();

	for (Entry& entry : projectors_) {
		if (count >= ST_PROJECTOR_MAX) break;

		Projector& projector = entry.projector;
		if (!projector.enabled) continue;
		if (!projector.lightSurfaces && !projector.beam) continue;
		if (projector.intensity <= 0.0f || projector.range <= 0.0f) continue;

		// Orientation comes from the followed entity's world MATRIX rather than from a
		// quaternion: decomposing a mirrored or non-uniformly scaled transform loses
		// the handedness, and the engine's own light direction is taken from the
		// matrix too (wiScene.cpp), so this is the one that agrees with the light the
		// projector is standing in for.
		XMMATRIX orientation = XMMatrixIdentity();
		bool orientationFromMatrix = false;

		if (projector.followEntity != wi::ecs::INVALID_ENTITY) {
			const wi::scene::TransformComponent* transform = scene.transforms.GetComponent(projector.followEntity);
			if (transform == nullptr) continue; // entity gone or not transformable - skip, don't guess
			projector.position = transform->GetPosition();
			projector.rotation = transform->GetRotation();
			orientation = XMLoadFloat4x4(&transform->world);
			orientationFromMatrix = true;
		}

		ResolveImage(scene, projector);

		XMVECTOR localForward, localUp;
		LocalAxes(projector.forward, localForward, localUp);

		if (!orientationFromMatrix) {
			orientation = XMMatrixRotationQuaternion(XMLoadFloat4(&projector.rotation));
		}
		XMVECTOR forward = XMVector3Normalize(XMVector3TransformNormal(localForward, orientation));
		XMVECTOR up = XMVector3Normalize(XMVector3TransformNormal(localUp, orientation));

		if (projector.roll != 0.0f) {
			up = XMVector3Normalize(XMVector3TransformNormal(up, XMMatrixRotationAxis(forward, projector.roll)));
		}

		const XMVECTOR position = XMLoadFloat3(&projector.position);

		Emit(projector, position, forward, up, XMFLOAT3(1, 1, 1), nullptr);

		// -- the image in the optics ----------------------------------------------
		// One level only: a virtual projector does not itself reflect. Two facing
		// mirrors therefore give two images rather than an endless corridor, which is
		// the deliberate trade for not making this recursive - each extra level would
		// square the slot and shadow-map cost, and the second reflection of a
		// projected image is dim enough that almost nobody would notice it missing.
		if (projector.opticBounces <= 0) continue;

		for (size_t i = 0; i < optics.MirrorCount(); ++i) {
			if (count >= ST_PROJECTOR_MAX) break;

			const Mirror* mirror = optics.MirrorAt(i);
			if (mirror == nullptr) continue;
			if (!mirror->surface.enabled || !mirror->surface.resolved) continue;

			const XMVECTOR n = XMLoadFloat3(&mirror->surface.normal);
			const XMVECTOR c = XMLoadFloat3(&mirror->surface.position);

			// Only the silvered face reflects, and only a projector in FRONT of the
			// glass can light it at all.
			const float side = XMVectorGetX(XMVector3Dot(XMVectorSubtract(position, c), n));
			if (!mirror->doubleSided && side <= 0.0f) continue;

			// Reflect the apex through the plane, and the two orientation vectors
			// through its normal. The handedness flips, which is exactly right: a
			// projected image seen in a mirror IS mirror-writing.
			const XMVECTOR mirroredPosition = XMVectorSubtract(position, XMVectorScale(n, 2.0f * side));
			const XMVECTOR mirroredForward = XMVector3Normalize(XMVector3Reflect(forward, n));
			const XMVECTOR mirroredUp = XMVector3Normalize(XMVector3Reflect(up, n));

			XMFLOAT3 tint = mirror->tint;
			const float reflectance = std::max(mirror->reflectance, 0.0f);
			tint.x *= reflectance;
			tint.y *= reflectance;
			tint.z *= reflectance;
			if (Luminance(tint) < std::max(projector.opticMinThroughput, 0.0f)) continue;

			Emit(projector, mirroredPosition, mirroredForward, mirroredUp, tint, &mirror->surface);
		}

		for (size_t i = 0; i < optics.LensCount(); ++i) {
			if (count >= ST_PROJECTOR_MAX) break;

			const Lens* lens = optics.LensAt(i);
			if (lens == nullptr) continue;
			if (!lens->surface.enabled || !lens->surface.resolved) continue;

			const XMVECTOR n = XMLoadFloat3(&lens->surface.normal);
			const XMVECTOR c = XMLoadFloat3(&lens->surface.position);

			// A lens images the apex of the cone, and moving the apex is what zooms and
			// shifts a projected picture - which is what a projector's own zoom lens
			// does. Object distance is measured along the axis, from the lens back to
			// the projector.
			const XMVECTOR offset = XMVectorSubtract(position, c);
			const float axial = XMVectorGetX(XMVector3Dot(offset, n));
			const float object = std::abs(axial);
			if (object < 1e-3f) continue; // sitting in the glass; no meaningful image

			// The propagation axis points AWAY from the projector, so the image is
			// placed on the far side for a real image and the near side for a virtual
			// one, without a separate sign convention to get wrong.
			const XMVECTOR axis = XMVectorScale(n, axial >= 0.0f ? -1.0f : 1.0f);
			const XMVECTOR transverse = XMVectorSubtract(offset, XMVectorScale(n, axial));

			const float f = lens->focalLength;
			float image;      // signed distance from the lens along `axis`
			float magnification;
			if (std::abs(f) < 1e-4f || std::abs(object - f) < 1e-4f) {
				// A window, or the projector sitting exactly at the focal point where
				// the image runs off to infinity. Pass it through unchanged rather than
				// emit a projector at a divide-by-zero.
				image = -object;
				magnification = 1.0f;
			} else {
				image = object * f / (object - f);
				magnification = -image / object;
			}

			const XMVECTOR imagedPosition = XMVectorAdd(c,
				XMVectorAdd(XMVectorScale(axis, image), XMVectorScale(transverse, magnification)));

			// Keep the cone pointing through the lens. A real image (magnification
			// negative) also turns the picture over, which a thin lens does.
			XMVECTOR imagedForward = XMVector3Normalize(XMVectorSubtract(c, imagedPosition));
			XMVECTOR imagedUp = magnification < 0.0f ? XMVectorNegate(up) : up;
			// Re-orthogonalise: up has to stay perpendicular to the new axis or the
			// camera matrix comes out sheared.
			imagedUp = XMVector3Normalize(XMVectorSubtract(imagedUp,
				XMVectorScale(imagedForward, XMVectorGetX(XMVector3Dot(imagedUp, imagedForward)))));
			if (XMVectorGetX(XMVector3LengthSq(imagedUp)) < 1e-6f) continue;

			XMFLOAT3 tint = lens->tint;
			const float transmittance = std::max(lens->transmittance, 0.0f);
			tint.x *= transmittance;
			tint.y *= transmittance;
			tint.z *= transmittance;
			if (Luminance(tint) < std::max(projector.opticMinThroughput, 0.0f)) continue;

			Emit(projector, imagedPosition, imagedForward, imagedUp, tint, &lens->surface);
		}
	}

	// Slots past `count` belong to projectors that dropped out this frame - stop
	// rendering depth maps for them.
	for (uint32_t i = count; i < ST_PROJECTOR_MAX; ++i) {
		shadowSlots_[i].active = false;
	}

	if (count == 0) {
		Unhook();
		return;
	}

	Hook();

	wi::RenderPath3D::CustomPostprocess* pass = FindPass();
	if (pass == nullptr) return;

	GPUBuffer& buffer = buffers_[device->GetBufferIndex()];
	if (buffer.mapped_data == nullptr) return;
	std::memcpy(buffer.mapped_data, shaderProjectors, sizeof(StProjector) * count);

	// Descriptor indices are small integers, so a float carries them exactly - which
	// is all PostProcess::params0 has room for.
	pass->params0 = XMFLOAT4(
		(float)device->GetDescriptorIndex(&buffer, SubresourceType::SRV),
		(float)count,
		0,
		0);
}

void ProjectorSystem::RenderShadows(CommandList cmd) const {
	if (!enabled || !shader_.IsValid()) return;

	GraphicsDevice* device = GetDevice();

	for (uint32_t i = 0; i < ST_PROJECTOR_MAX; ++i) {
		const ShadowSlot& shadow = shadowSlots_[i];
		if (!shadow.active || !shadow.map.IsValid()) continue;

		const wi::renderer::Visibility& visibility = shadowVisibility_[i];
		if (visibility.visibleObjects.empty()) continue;

		// One line, once: a depth map that draws nothing shadows nothing, and that is
		// indistinguishable from "the feature is off" when you are staring at a beam.
		static bool reported = false;
		if (!reported) {
			reported = true;
			wi::backlog::post("[Projector] shadow map " + std::to_string(shadow.map.desc.width) + "px, " +
				std::to_string(visibility.visibleObjects.size()) + " objects");
		}

		device->EventBegin("st::Projector shadow map", cmd);

		wi::renderer::BindCameraCB(shadow.camera, shadow.camera, shadow.camera, cmd);

		const RenderPassImage rp[] = {
			RenderPassImage::DepthStencil(
				&shadow.map,
				RenderPassImage::LoadOp::CLEAR,
				RenderPassImage::StoreOp::STORE,
				ResourceState::SHADER_RESOURCE,
				ResourceState::DEPTHSTENCIL,
				ResourceState::SHADER_RESOURCE),
		};
		device->RenderPassBegin(rp, arraysize(rp), cmd);

		Viewport viewport;
		viewport.width = (float)shadow.map.desc.width;
		viewport.height = (float)shadow.map.desc.height;
		device->BindViewports(1, &viewport, cmd);

		wi::renderer::DrawScene(visibility, wi::enums::RENDERPASS_SHADOW, cmd, wi::renderer::DRAWSCENE_OPAQUE);

		device->RenderPassEnd(cmd);
		device->EventEnd(cmd);
	}
}

void ProjectorSystem::GUI() {
	if (!shader_.IsValid()) {
		ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "StProjectorCS failed to load - see BackLog.");
		return;
	}

	ImGui::Checkbox("Projectors enabled", &enabled);
	ImGui::Text("%d / %d projectors", (int)projectors_.size(), (int)ST_PROJECTOR_MAX);

	if (projectors_.empty()) {
		ImGui::TextDisabled("Nothing registered. st::App::Projectors().Add(...) from a scene.");
		return;
	}

	selected_ = std::clamp(selected_, 0, (int)projectors_.size() - 1);
	if (projectors_.size() > 1) {
		ImGui::SliderInt("Projector", &selected_, 0, (int)projectors_.size() - 1);
	}

	Projector& projector = projectors_[selected_].projector;

	ImGui::Checkbox("Enabled", &projector.enabled);

	ImGui::SeparatorText("Gate");
	int shape = (int)projector.shape;
	if (ImGui::Combo("Shape", &shape, "Rect\0Ellipse\0Rounded rect\0")) {
		projector.shape = (Projector::Shape)shape;
	}
	if (projector.shape == Projector::Shape::RoundedRect) {
		ImGui::SliderFloat("Corner radius", &projector.cornerRadius, 0.0f, 1.0f);
	}
	ImGui::SliderFloat("Aspect", &projector.aspect, 0.25f, 4.0f);
	ImGui::SliderFloat("Throw ratio", &projector.throwRatio, 0.0f, 4.0f, "%.2f (0 = use FOV)");
	if (projector.throwRatio <= 0.0001f) {
		ImGui::SliderFloat("FOV", &projector.fov, 0.05f, 2.5f);
	}
	ImGui::SliderFloat("Softness", &projector.softness, 0.0f, 0.5f);
	ImGui::SliderFloat("Roll", &projector.roll, -XM_PI, XM_PI);

	ImGui::SeparatorText("Optics");
	ImGui::SliderFloat2("Lens shift", &projector.lensShift.x, -1.0f, 1.0f);
	ImGui::SliderFloat2("Keystone", &projector.keystone.x, -0.75f, 0.75f);
	ImGui::SliderFloat("Distortion", &projector.distortion, -0.5f, 0.5f);
	ImGui::SliderFloat("Vignette", &projector.vignette, 0.0f, 1.0f);

	ImGui::SeparatorText("Image");
	ImGui::ColorEdit3("Color", &projector.color.x);
	ImGui::DragFloat("Intensity", &projector.intensity, 0.1f, 0.0f, 200.0f);
	ImGui::SliderFloat("Gamma", &projector.gamma, 0.2f, 3.0f);
	ImGui::SliderFloat("Falloff", &projector.falloff, 0.0f, 1.0f);
	ImGui::DragFloat("Focus distance", &projector.focusDistance, 0.1f, 0.1f, 500.0f);
	ImGui::DragFloat("Range", &projector.range, 0.5f, 1.0f, 500.0f);
	if (projector.imageEntity != wi::ecs::INVALID_ENTITY) {
		ImGui::Text("Image: entity %llu", (unsigned long long)projector.imageEntity);
	} else {
		ImGui::Text("Image: %s", projector.texture.IsValid() ? "attached" : "flat colour");
	}

	ImGui::SeparatorText("Mirrors and lenses");
	ImGui::SliderInt("Optic bounces", &projector.opticBounces, 0, 1);
	if (projector.opticBounces > 0) {
		ImGui::SliderFloat("Optic min throughput", &projector.opticMinThroughput, 0.0f, 0.5f);
		ImGui::TextDisabled("One virtual projector per element, each with its own shadow map.");
	} else {
		ImGui::TextDisabled("0 = the image ignores every mirror and lens.");
	}

	ImGui::SeparatorText("Surfaces");
	ImGui::Checkbox("Light surfaces", &projector.lightSurfaces);
	ImGui::Checkbox("Lambert", &projector.lambert);
	ImGui::Checkbox("Screen-space occlusion", &projector.occlusion);
	if (projector.occlusion) {
		ImGui::SliderFloat("Occlusion strength", &projector.occlusionStrength, 0.0f, 1.0f);
		ImGui::SliderInt("Occlusion samples", &projector.occlusionSamples, 1, 32);
		ImGui::SliderFloat("Occlusion thickness", &projector.occlusionThickness, 0.05f, 5.0f);
	}

	ImGui::SeparatorText("Beam");
	ImGui::Checkbox("Volumetric beam", &projector.beam);
	if (projector.beam) {
		ImGui::SliderFloat("Density", &projector.beamDensity, 0.0f, 0.5f);
		ImGui::SliderFloat("Anisotropy", &projector.beamAnisotropy, -0.95f, 0.95f);
		ImGui::SliderInt("Beam samples", &projector.beamSamples, 2, 64);
	}

	ImGui::SeparatorText("Placement");
	int forward = (int)projector.forward;
	if (ImGui::Combo("Forward axis", &forward, "+Z\0-Z\0-Y (spot light)\0+Y\0+X\0-X\0")) {
		projector.forward = (Projector::Forward)forward;
	}
	if (projector.followEntity != wi::ecs::INVALID_ENTITY) {
		ImGui::Text("Following entity %llu", (unsigned long long)projector.followEntity);
		ImGui::BeginDisabled();
	}
	ImGui::DragFloat3("Position", &projector.position.x, 0.1f);
	if (projector.followEntity != wi::ecs::INVALID_ENTITY) {
		ImGui::EndDisabled();
	}
}

void ProjectorSystem::SaveTo(nbt::Tag& out) const {
	// Only the master switch belongs in options.stad. The projectors themselves are
	// scene content - whoever created them owns their lifetime.
	out.putBool("enabled", enabled);
}

void ProjectorSystem::LoadFrom(const nbt::Tag& in) {
	enabled = in.getBool("enabled", enabled);
}

} // namespace st
