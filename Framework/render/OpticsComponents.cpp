#include "render/Optics.h"
#include "stNativeComponent.h"
#include "wiScene.h"
#include "wiBacklog.h"
#include "imgui.h"

#include <string>

// The "Mirror" and "Lens" native components: st::Mirror / st::Lens attached to an
// entity from the editor instead of from code.
//
//     NCI_0             = "sticMirror"
//     NCA_0_normalAxis  = 0      (0 +Z, 1 -Z, 2 -Y, 3 +Y, 4 +X, 5 -X)
//     NCA_0_fitToMesh   = true     (plane and aperture taken from the entity's mesh)
//     NCA_0_radius      = 0.3      (only used when fitToMesh is false)
//     NCA_0_reflectance = 0.95
//     NCA_0_dichroic    = false    (true splits the beam: reflect tint, transmit the rest)
//
//     NCI_0             = "sticLens"
//     NCA_0_type        = "spherical"   (spherical | cylindrical | toric | aspheric |
//                                        axicon | prism | window)
//     NCA_0_focalLength = 1.5
//     NCA_0_beamScale   = 0.4
//
// Both follow their OWN entity, so putting one on a mesh is all it takes: move the
// mesh and the optical element moves with it. Neither draws anything - what you SEE
// where a mirror is, is whatever material the scene put there. This is only where
// the beam goes, and the beam is st::LaserSystem's (Framework/render/Laser.h).
//
// The aperture is the element's optical extent, not the mesh's: a beam that lands
// outside it passes straight by. Match it to the visible glass, or the mirror will
// reflect light that visibly missed it.

namespace {

// Both components bind the same placement fields, so the parsing lives once.
struct SurfaceParams {
	int normalAxis = 0;
	bool circular = true;
	// Default ON: a component always has an entity, that entity normally carries the
	// mesh the element is standing in for, and a hand-set aperture is wrong in both
	// directions - too small and the beam stops on the glass, too big and it bounces
	// off the plane EXTENDED out past the glass, in mid-air where there is nothing.
	// See OpticSurface::fitToMesh.
	bool fitToMesh = true;
	float radius = 0.25f;      // circular, when not fitting
	float halfWidth = 0.25f;   // rectangular, when not fitting
	float halfHeight = 0.25f;

	void Bind(wi::scene::NativeComponent& component) {
		component.Bind(normalAxis, "normalAxis");
		component.Bind(circular, "circular");
		component.Bind(fitToMesh, "fitToMesh");
		component.Bind(radius, "radius");
		component.Bind(halfWidth, "halfWidth");
		component.Bind(halfHeight, "halfHeight");
	}

	void Apply(st::OpticSurface& surface, wi::ecs::Entity entity) const {
		surface.followEntity = entity;
		surface.normalAxis = (int)wi::math::Clamp((float)normalAxis, 0.0f, 5.0f);
		surface.circular = circular;
		surface.fitToMesh = fitToMesh;
		// Written even while fitting: these are the fallback for an entity with no
		// mesh, and what the panel goes back to when fitting is switched off.
		surface.halfExtent = circular ? XMFLOAT2(radius, radius) : XMFLOAT2(halfWidth, halfHeight);
	}

	// `live` is the resolved surface, so the fitted numbers can be shown read-only
	// instead of the panel claiming a radius that is not the one in use.
	bool GUI(const st::OpticSurface& live) {
		bool dirty = false;
		dirty |= ImGui::Combo("Normal axis", &normalAxis, "+Z\0-Z\0-Y\0+Y\0+X\0-X\0");
		dirty |= ImGui::Checkbox("Circular", &circular);
		dirty |= ImGui::Checkbox("Fit to mesh", &fitToMesh);

		if (fitToMesh) {
			if (circular) {
				ImGui::TextDisabled("Radius %.3f m (fitted, inscribed in the mesh)", live.halfExtent.x);
			} else {
				ImGui::TextDisabled("Half extent %.3f x %.3f m (fitted)", live.halfExtent.x, live.halfExtent.y);
			}
			ImGui::TextDisabled("Plane sits on the mesh centre, not the entity origin.");
		} else if (circular) {
			dirty |= ImGui::DragFloat("Radius", &radius, 0.01f, 0.001f, 20.0f, "%.3f m");
		} else {
			dirty |= ImGui::DragFloat("Half width", &halfWidth, 0.01f, 0.001f, 20.0f, "%.3f m");
			dirty |= ImGui::DragFloat("Half height", &halfHeight, 0.01f, 0.001f, 20.0f, "%.3f m");
		}
		return dirty;
	}
};

void ReportAttachment(const wi::scene::Scene* scene, wi::ecs::Entity entity, const char* kind) {
	std::string owner = "entity " + std::to_string(entity);
	if (scene != nullptr) {
		if (const wi::scene::NameComponent* name = scene->names.GetComponent(entity)) {
			owner = "'" + name->name + "' (entity " + std::to_string(entity) + ")";
		}
	}
	wi::backlog::post(std::string("[Optics] ") + kind + " attached to " + owner + " from scene metadata");
}

} // namespace

struct StMirrorComponent : wi::scene::NativeComponent {
private:
	st::OpticsSystem::ID id = st::OpticsSystem::INVALID;

	SurfaceParams surface;
	float reflectance = 0.95f;
	float tintR = 1.0f, tintG = 1.0f, tintB = 1.0f;
	bool doubleSided = true;
	float scatter = 0.0f;
	// Dichroic: reflect `tint`, pass `transmitTint` on as a second beam.
	bool dichroic = false;
	float transmitR = 1.0f, transmitG = 1.0f, transmitB = 1.0f;
	float transmittance = 0.9f;

	void Apply() {
		st::Mirror* mirror = st::OpticsSystem::Get().FindMirror(id);
		if (mirror == nullptr) return;

		surface.Apply(mirror->surface, entity);
		mirror->reflectance = reflectance;
		mirror->tint = XMFLOAT3(tintR, tintG, tintB);
		mirror->doubleSided = doubleSided;
		mirror->scatter = scatter;
		mirror->dichroic = dichroic;
		mirror->transmitTint = XMFLOAT3(transmitR, transmitG, transmitB);
		mirror->transmittance = transmittance;
	}

public:
	void Start() override {
		surface.Bind(*this);
		Bind(reflectance, "reflectance");
		Bind(tintR, "tintR");
		Bind(tintG, "tintG");
		Bind(tintB, "tintB");
		Bind(doubleSided, "doubleSided");
		Bind(scatter, "scatter");
		Bind(dichroic, "dichroic");
		Bind(transmitR, "transmitR");
		Bind(transmitG, "transmitG");
		Bind(transmitB, "transmitB");
		Bind(transmittance, "transmittance");

		id = st::OpticsSystem::Get().AddMirror();
		Apply();
		ReportAttachment(scene, entity, "mirror");
	}

	void Destroy() override {
		st::OpticsSystem::Get().RemoveMirror(id);
		id = st::OpticsSystem::INVALID;
	}

	void OnDisable() override {
		if (st::Mirror* mirror = st::OpticsSystem::Get().FindMirror(id)) {
			mirror->surface.enabled = false;
		}
	}

	void OnEnable() override {
		if (st::Mirror* mirror = st::OpticsSystem::Get().FindMirror(id)) {
			mirror->surface.enabled = true;
		}
	}

	void DrawDebug() override {
		st::Mirror* mirror = st::OpticsSystem::Get().FindMirror(id);
		if (mirror == nullptr) {
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Not registered - see BackLog.");
			return;
		}

		bool dirty = surface.GUI(mirror->surface);
		dirty |= ImGui::SliderFloat("Reflectance", &reflectance, 0.0f, 1.0f);

		float tint[3] = { tintR, tintG, tintB };
		if (ImGui::ColorEdit3("Tint", tint, ImGuiColorEditFlags_Float)) {
			tintR = tint[0];
			tintG = tint[1];
			tintB = tint[2];
			dirty = true;
		}

		dirty |= ImGui::Checkbox("Double sided", &doubleSided);
		dirty |= ImGui::SliderFloat("Scatter", &scatter, 0.0f, 0.2f, "%.4f rad");

		dirty |= ImGui::Checkbox("Dichroic", &dichroic);
		if (dichroic) {
			// The two tints ARE the pass bands: what Tint keeps is reflected, what
			// Transmit tint keeps carries on through as a second beam.
			float transmit[3] = { transmitR, transmitG, transmitB };
			if (ImGui::ColorEdit3("Transmit tint", transmit, ImGuiColorEditFlags_Float)) {
				transmitR = transmit[0];
				transmitG = transmit[1];
				transmitB = transmit[2];
				dirty = true;
			}
			dirty |= ImGui::SliderFloat("Transmittance", &transmittance, 0.0f, 1.0f);
			ImGui::TextDisabled("Reflects Tint, passes Transmit tint on as a second beam.");
		}

		// Facing the wrong way and having too small an aperture are the two ways a
		// mirror silently does nothing, and neither is visible from the scene.
		st::OpticDiagnostics(mirror->surface);

		// Apply() pushes the change into the live system; SaveBoundParams() writes the same
		// values into the NCA_ metadata Bind() read them from, which is what makes an edit
		// here survive a save and reload instead of living until the next scene load.
		if (dirty) { Apply(); SaveBoundParams(); }
	}
};

struct StLensComponent : wi::scene::NativeComponent {
private:
	st::OpticsSystem::ID id = st::OpticsSystem::INVALID;

	SurfaceParams surface;
	// spherical | cylindrical | toric | aspheric | axicon | prism | window
	std::string type = "spherical";
	float focalLength = 1.0f;
	float focalLengthY = 1.0f;
	float asphericity = 0.0f;
	float axiconAngle = 0.05f;
	float prismX = 0.0f, prismY = 0.0f;
	float transmittance = 0.92f;
	float tintR = 1.0f, tintG = 1.0f, tintB = 1.0f;
	float beamScale = 1.0f;
	float spread = 0.0f;

	void Apply() {
		st::Lens* lens = st::OpticsSystem::Get().FindLens(id);
		if (lens == nullptr) return;

		surface.Apply(lens->surface, entity);
		lens->type = st::ParseLensType(type);
		lens->focalLength = focalLength;
		lens->focalLengthY = focalLengthY;
		lens->asphericity = asphericity;
		lens->axiconAngle = axiconAngle;
		lens->prismDeviation = XMFLOAT2(prismX, prismY);
		lens->transmittance = transmittance;
		lens->tint = XMFLOAT3(tintR, tintG, tintB);
		lens->beamScale = beamScale;
		lens->spread = spread;
	}

public:
	void Start() override {
		surface.Bind(*this);
		Bind(type, "type");
		Bind(focalLength, "focalLength");
		Bind(focalLengthY, "focalLengthY");
		Bind(asphericity, "asphericity");
		Bind(axiconAngle, "axiconAngle");
		Bind(prismX, "prismX");
		Bind(prismY, "prismY");
		Bind(transmittance, "transmittance");
		Bind(tintR, "tintR");
		Bind(tintG, "tintG");
		Bind(tintB, "tintB");
		Bind(beamScale, "beamScale");
		Bind(spread, "spread");

		id = st::OpticsSystem::Get().AddLens();
		Apply();
		ReportAttachment(scene, entity, "lens");
	}

	void Destroy() override {
		st::OpticsSystem::Get().RemoveLens(id);
		id = st::OpticsSystem::INVALID;
	}

	void OnDisable() override {
		if (st::Lens* lens = st::OpticsSystem::Get().FindLens(id)) {
			lens->surface.enabled = false;
		}
	}

	void OnEnable() override {
		if (st::Lens* lens = st::OpticsSystem::Get().FindLens(id)) {
			lens->surface.enabled = true;
		}
	}

	void DrawDebug() override {
		st::Lens* lens = st::OpticsSystem::Get().FindLens(id);
		if (lens == nullptr) {
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Not registered - see BackLog.");
			return;
		}

		bool dirty = surface.GUI(lens->surface);

		// Edit the live lens, then read the changed fields back onto the component's
		// own copy: those are what Apply() pushes and what the metadata names, so
		// letting the shared widget write straight through would lose them the next
		// time anything else calls Apply().
		int typeIndex = (int)lens->type;
		if (ImGui::Combo("Type", &typeIndex, st::LENS_TYPE_ITEMS)) {
			lens->type = (st::Lens::Type)typeIndex;
			type = st::LensTypeName(lens->type);
			dirty = true;
		}
		if (st::LensTypeFields(*lens)) {
			focalLength = lens->focalLength;
			focalLengthY = lens->focalLengthY;
			asphericity = lens->asphericity;
			axiconAngle = lens->axiconAngle;
			prismX = lens->prismDeviation.x;
			prismY = lens->prismDeviation.y;
			dirty = true;
		}

		dirty |= ImGui::SliderFloat("Transmittance", &transmittance, 0.0f, 1.0f);

		float tint[3] = { tintR, tintG, tintB };
		if (ImGui::ColorEdit3("Tint", tint, ImGuiColorEditFlags_Float)) {
			tintR = tint[0];
			tintG = tint[1];
			tintB = tint[2];
			dirty = true;
		}

		dirty |= ImGui::SliderFloat("Beam scale", &beamScale, 0.05f, 8.0f);
		dirty |= ImGui::SliderFloat("Spread", &spread, 0.0f, 0.5f, "%.4f rad");

		st::OpticDiagnostics(lens->surface);

		// Apply() pushes the change into the live system; SaveBoundParams() writes the same
		// values into the NCA_ metadata Bind() read them from, which is what makes an edit
		// here survive a save and reload instead of living until the next scene load.
		if (dirty) { Apply(); SaveBoundParams(); }
	}
};

ST_REGISTER_NATIVE_COMPONENT_AS(StMirrorComponent, "sticMirror")
ST_REGISTER_NATIVE_COMPONENT_AS(StLensComponent, "sticLens")
