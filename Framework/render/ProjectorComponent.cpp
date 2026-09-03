#include "render/Projector.h"
#include "stNativeComponent.h"
#include "wiScene.h"
#include "wiBacklog.h"
#include "imgui.h"

#include <string>

// The "Projector" native component: st::Projector attached to an entity from the
// editor instead of from code.
//
// Drop it on the spot light that is meant to throw a picture and the light stops
// being a circle:
//
//     NCI_0            = "Projector"
//     NCA_0_shape      = 0        (0 rect, 1 ellipse, 2 rounded rect)
//     NCA_0_aspect     = 1.7778
//     NCA_0_intensity  = 3.0
//
// The component follows its OWN entity, so nothing else has to be wired up. It also
// zeroes that entity's LightComponent by default (`dimLight`), because the engine
// spot light IS the circle you are trying to get rid of - set NCA_0_dimLight = false
// to keep it as spill from the housing.
//
// The image comes from the same entity, in the order the engine uses for a light
// mask: VideoComponent, then CameraComponent::render_to_texture, then the base colour
// of a MaterialComponent. NCA_0_imageSource pins it to one of those - use "material"
// when the entity carries a video but you want a st::gfx::Framebuffer on screen
// instead (bind the framebuffer with Framebuffer::BindToLightMask, which creates the
// MaterialComponent for you).
struct StProjectorComponent : wi::scene::NativeComponent {
private:
	st::ProjectorSystem::ID id = st::ProjectorSystem::INVALID;

	// Bound from NCA_<localID>_<name>; the defaults mirror st::Projector's own.
	int shape = 0;             // 0 rect, 1 ellipse, 2 rounded rect
	int forwardAxis = 2;       // 0 +Z, 1 -Z, 2 -Y (spot light), 3 +Y
	float aspect = 16.0f / 9.0f;
	float throwRatio = 1.6f;
	float fov = 0.5f;
	float intensity = 3.0f;
	float range = 0.0f;        // 0 = take the light's own range
	float focusDistance = 30.0f;
	float falloff = 1.0f;
	float softness = 0.02f;
	float cornerRadius = 0.15f;
	float vignette = 0.0f;
	float gamma = 1.0f;
	float roll = 0.0f;
	float shiftX = 0.0f, shiftY = 0.0f;
	float keystoneX = 0.0f, keystoneY = 0.0f;
	float distortion = 0.0f;
	bool lightSurfaces = true;
	bool lambert = true;
	bool shadows = true;
	int shadowResolution = 1024;
	float shadowBias = 0.002f;
	bool occlusion = true;
	int occlusionSamples = 12;
	float occlusionThickness = 1.0f;
	bool beam = true;
	float beamDensity = 0.02f;
	float beamAnisotropy = 0.6f;
	int beamSamples = 24;
	int opticBounces = 0;
	float opticMinThroughput = 0.02f;
	bool dimLight = true;
	std::string imageSource = "auto"; // auto | video | camera | material | none

	static st::Projector::ImageSource ParseImageSource(const std::string& value) {
		if (value == "video") return st::Projector::ImageSource::Video;
		if (value == "camera") return st::Projector::ImageSource::Camera;
		if (value == "material") return st::Projector::ImageSource::Material;
		if (value == "none") return st::Projector::ImageSource::None;
		return st::Projector::ImageSource::Auto;
	}

	// Push the component's fields onto the live projector. Called from Start() and
	// again after every DrawDebug() edit, so the inspector sliders are live.
	void Apply() {
		st::Projector* projector = st::ProjectorSystem::Get().Find(id);
		if (projector == nullptr) return;

		projector->shape = (st::Projector::Shape)wi::math::Clamp((float)shape, 0.0f, 2.0f);
		projector->forward = (st::Projector::Forward)wi::math::Clamp((float)forwardAxis, 0.0f, 5.0f);
		projector->aspect = aspect;
		projector->throwRatio = throwRatio;
		projector->fov = fov;
		projector->intensity = intensity;
		projector->focusDistance = focusDistance;
		projector->falloff = falloff;
		projector->softness = softness;
		projector->cornerRadius = cornerRadius;
		projector->vignette = vignette;
		projector->gamma = gamma;
		projector->roll = roll;
		projector->lensShift = XMFLOAT2(shiftX, shiftY);
		projector->keystone = XMFLOAT2(keystoneX, keystoneY);
		projector->distortion = distortion;
		projector->lightSurfaces = lightSurfaces;
		projector->lambert = lambert;
		projector->shadows = shadows;
		projector->shadowResolution = shadowResolution;
		projector->shadowBias = shadowBias;
		projector->occlusion = occlusion;
		projector->occlusionSamples = occlusionSamples;
		projector->occlusionThickness = occlusionThickness;
		projector->beam = beam;
		projector->beamDensity = beamDensity;
		projector->beamAnisotropy = beamAnisotropy;
		projector->beamSamples = beamSamples;
		projector->opticBounces = opticBounces;
		projector->opticMinThroughput = opticMinThroughput;
		projector->imageSource = ParseImageSource(imageSource);

		if (range > 0.0f) {
			projector->range = range;
		} else if (const wi::scene::LightComponent* light = GetComponent<wi::scene::LightComponent>()) {
			projector->range = light->GetRange();
		}
	}

public:
	void Start() override {
		Bind(shape, "shape");
		Bind(forwardAxis, "forwardAxis");
		Bind(aspect, "aspect");
		Bind(throwRatio, "throwRatio");
		Bind(fov, "fov");
		Bind(intensity, "intensity");
		Bind(range, "range");
		Bind(focusDistance, "focusDistance");
		Bind(falloff, "falloff");
		Bind(softness, "softness");
		Bind(cornerRadius, "cornerRadius");
		Bind(vignette, "vignette");
		Bind(gamma, "gamma");
		Bind(roll, "roll");
		Bind(shiftX, "shiftX");
		Bind(shiftY, "shiftY");
		Bind(keystoneX, "keystoneX");
		Bind(keystoneY, "keystoneY");
		Bind(distortion, "distortion");
		Bind(lightSurfaces, "lightSurfaces");
		Bind(lambert, "lambert");
		Bind(shadows, "shadows");
		Bind(shadowResolution, "shadowResolution");
		Bind(shadowBias, "shadowBias");
		Bind(occlusion, "occlusion");
		Bind(occlusionSamples, "occlusionSamples");
		Bind(occlusionThickness, "occlusionThickness");
		Bind(beam, "beam");
		Bind(beamDensity, "beamDensity");
		Bind(beamAnisotropy, "beamAnisotropy");
		Bind(beamSamples, "beamSamples");
		Bind(opticBounces, "opticBounces");
		Bind(opticMinThroughput, "opticMinThroughput");
		Bind(dimLight, "dimLight");
		Bind(imageSource, "imageSource");

		st::Projector projector;
		projector.followEntity = entity;
		projector.imageEntity = entity;

		id = st::ProjectorSystem::Get().Add(projector);
		if (id == st::ProjectorSystem::INVALID) {
			wi::backlog::post("Projector component: ProjectorSystem is full, nothing added",
				wi::backlog::LogLevel::Warning);
			return;
		}

		Apply();

		// Say so in the backlog. A projector attached through scene metadata is
		// otherwise invisible in code review - the beam appears with nothing in the
		// game's source to explain it.
		std::string owner = "entity " + std::to_string(entity);
		if (scene != nullptr) {
			if (const wi::scene::NameComponent* name = scene->names.GetComponent(entity)) {
				owner = "'" + name->name + "' (entity " + std::to_string(entity) + ")";
			}
		}
		// Which image source the entity actually offers. A projector with none of them
		// throws flat white, which reads as "broken" rather than "no picture assigned".
		std::string sources;
		if (scene != nullptr) {
			if (scene->videos.GetComponent(entity) != nullptr) sources += " video";
			if (scene->cameras.GetComponent(entity) != nullptr) sources += " camera";
			if (scene->materials.GetComponent(entity) != nullptr) sources += " material";
		}
		if (sources.empty()) sources = " none (flat colour)";

		wi::backlog::post("[Projector] component attached to " + owner +
			" from scene metadata, imageSource=" + imageSource + ", available:" + sources);

		// The engine spot light is the circle this component exists to replace.
		if (dimLight) {
			if (wi::scene::LightComponent* light = GetComponent<wi::scene::LightComponent>()) {
				light->intensity = 0.0f;
			}
		}
	}

	void Destroy() override {
		st::ProjectorSystem::Get().Remove(id);
		id = st::ProjectorSystem::INVALID;
	}

	void OnDisable() override {
		if (st::Projector* projector = st::ProjectorSystem::Get().Find(id)) {
			projector->enabled = false;
		}
	}

	void OnEnable() override {
		if (st::Projector* projector = st::ProjectorSystem::Get().Find(id)) {
			projector->enabled = true;
		}
	}

	void DrawDebug() override {
		if (st::ProjectorSystem::Get().Find(id) == nullptr) {
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Not registered - see BackLog.");
			return;
		}

		bool dirty = false;
		dirty |= ImGui::Combo("Shape", &shape, "Rect\0Ellipse\0Rounded rect\0");
		dirty |= ImGui::Combo("Forward axis", &forwardAxis, "+Z\0-Z\0-Y (spot light)\0+Y\0+X\0-X\0");
		dirty |= ImGui::SliderFloat("Aspect", &aspect, 0.25f, 4.0f);
		dirty |= ImGui::SliderFloat("Throw ratio", &throwRatio, 0.0f, 4.0f, "%.2f (0 = use FOV)");
		if (throwRatio <= 0.0001f) dirty |= ImGui::SliderFloat("FOV", &fov, 0.05f, 2.5f);
		dirty |= ImGui::DragFloat("Intensity", &intensity, 0.1f, 0.0f, 200.0f);
		dirty |= ImGui::DragFloat("Focus distance", &focusDistance, 0.1f, 0.1f, 500.0f);
		dirty |= ImGui::SliderFloat("Falloff", &falloff, 0.0f, 1.0f);
		dirty |= ImGui::SliderFloat("Softness", &softness, 0.0f, 0.5f);
		dirty |= ImGui::SliderFloat("Vignette", &vignette, 0.0f, 1.0f);
		dirty |= ImGui::SliderFloat("Roll", &roll, -XM_PI, XM_PI);
		dirty |= ImGui::SliderFloat("Lens shift X", &shiftX, -1.0f, 1.0f);
		dirty |= ImGui::SliderFloat("Lens shift Y", &shiftY, -1.0f, 1.0f);
		dirty |= ImGui::SliderFloat("Keystone X", &keystoneX, -0.75f, 0.75f);
		dirty |= ImGui::SliderFloat("Keystone Y", &keystoneY, -0.75f, 0.75f);
		dirty |= ImGui::SliderFloat("Distortion", &distortion, -0.5f, 0.5f);
		dirty |= ImGui::Checkbox("Beam", &beam);
		if (beam) {
			dirty |= ImGui::SliderFloat("Beam density", &beamDensity, 0.0f, 0.5f);
			dirty |= ImGui::SliderInt("Beam samples", &beamSamples, 2, 64);
		}
		dirty |= ImGui::Checkbox("Shadow map", &shadows);
		if (shadows) {
			dirty |= ImGui::SliderInt("Shadow resolution", &shadowResolution, 128, 4096);
			dirty |= ImGui::SliderFloat("Shadow bias", &shadowBias, 0.0f, 0.02f, "%.4f");
		}
		dirty |= ImGui::SliderInt("Optic bounces", &opticBounces, 0, 1);
		if (opticBounces > 0) {
			ImGui::TextDisabled("Reflects off every sticMirror, images through every sticLens.");
		}
		dirty |= ImGui::Checkbox("Screen-space occlusion (fallback)", &occlusion);
		if (!shadows && occlusion) {
			dirty |= ImGui::SliderInt("Occlusion samples", &occlusionSamples, 2, 32);
			dirty |= ImGui::SliderFloat("Occlusion thickness", &occlusionThickness, 0.05f, 10.0f);
		}

		ImGui::TextDisabled("Image source: %s", imageSource.c_str());

		// Apply() pushes the change into the live system; SaveBoundParams() writes the same
		// values into the NCA_ metadata Bind() read them from, which is what makes an edit
		// here survive a save and reload instead of living until the next scene load.
		if (dirty) { Apply(); SaveBoundParams(); }
	}
};

ST_REGISTER_NATIVE_COMPONENT_AS(StProjectorComponent, "sticProjector")
