#include "render/Laser.h"
#include "scene/RayComponent.h"
#include "stNativeComponent.h"
#include "wiScene.h"
#include "wiBacklog.h"
#include "imgui.h"

#include <string>

// The "Laser" native component: st::Laser attached to an entity from the editor
// instead of from code.
//
//     NCI_0             = "sticLaser"
//     NCA_0_forwardAxis = 0        (0 +Z, 1 -Z, 2 -Y, 3 +Y, 4 +X, 5 -X)
//     NCA_0_colorR      = 1.0
//     NCA_0_colorG      = 0.08
//     NCA_0_colorB      = 0.05
//     NCA_0_intensity   = 40.0
//     NCA_0_maxBounces  = 4
//
// The laser leaves the entity's own transform, so nothing else has to be wired up.
// It bounces off every "sticMirror" and bends through every "sticLens" in the scene
// (see Framework/render/OpticsComponents.cpp) until it lands on geometry - and the
// spot it lands on leaves a fading trail behind it, so a beam swept across a wall
// draws a line rather than blinking a single dot along.
//
// A "sticRay" on the same entity is picked up automatically: the laser takes that
// component's mode and range rather than casting a second time with settings of its
// own. Set NCA_0_useSiblingRay = false to keep them independent.
struct StLaserComponent : wi::scene::NativeComponent {
private:
	st::LaserSystem::ID id = st::LaserSystem::INVALID;

	// Bound from NCA_<localID>_<name>; the defaults mirror st::Laser's own.
	int forwardAxis = 0;
	float colorR = 1.0f, colorG = 0.08f, colorB = 0.05f;
	float coreRadius = 0.004f;
	float intensity = 40.0f;
	float glowRadius = 0.08f;
	float glowIntensity = 1.5f;
	float attenuation = 0.25f;
	float range = 200.0f;
	float startOffset = 0.0f;
	bool occluded = true;
	float flicker = 0.0f;
	float flickerRate = 24.0f;
	int maxBounces = 4;
	float opticBias = 0.02f;
	// Array projection - off unless the scene asks for it.
	bool arrayEnabled = false;
	std::string arrayShape = "grid"; // grid | ring | fan | cross | spiral
	int arrayCountX = 5;
	int arrayCountY = 5;
	float arraySpreadX = 0.12f, arraySpreadY = 0.12f;
	float arrayOffsetX = 0.0f, arrayOffsetY = 0.0f;
	float arrayRoll = 0.0f;
	float arraySpin = 0.0f;
	float arraySpiralTurns = 3.0f;
	bool arrayHollow = false;
	float arrayFalloff = 0.0f;
	bool dot = true;
	float dotRadius = 0.03f;
	float dotIntensity = 12.0f;
	float surfaceRadius = 0.035f;
	float surfaceIntensity = 8.0f;
	bool trail = true;
	float trailLife = 0.35f;
	float trailSpacing = 0.015f;
	int trailMax = 48;
	float trailFalloff = 1.6f;
	float trailShrink = 0.5f;
	bool ignoreSelf = true;
	bool useSiblingRay = true;
	bool debugDraw = false;
	std::string mode = "mesh"; // mesh | physics | both | none

	static st::RayQuery::Mode ParseMode(const std::string& value) {
		if (value == "physics") return st::RayQuery::Mode::Physics;
		if (value == "both") return st::RayQuery::Mode::Both;
		if (value == "none") return st::RayQuery::Mode::None;
		return st::RayQuery::Mode::Mesh;
	}

	static st::LaserArray::Shape ParseArrayShape(const std::string& value) {
		if (value == "ring") return st::LaserArray::Shape::Ring;
		if (value == "fan") return st::LaserArray::Shape::Fan;
		if (value == "cross") return st::LaserArray::Shape::Cross;
		if (value == "spiral") return st::LaserArray::Shape::Spiral;
		return st::LaserArray::Shape::Grid;
	}

	static const char* ArrayShapeName(st::LaserArray::Shape shape) {
		switch (shape) {
		case st::LaserArray::Shape::Ring: return "ring";
		case st::LaserArray::Shape::Fan: return "fan";
		case st::LaserArray::Shape::Cross: return "cross";
		case st::LaserArray::Shape::Spiral: return "spiral";
		case st::LaserArray::Shape::Grid:
		default: return "grid";
		}
	}

	// Push the component's fields onto the live laser. Called from Start() and again
	// after every DrawDebug() edit, so the inspector sliders are live.
	void Apply() {
		st::Laser* laser = st::LaserSystem::Get().Find(id);
		if (laser == nullptr) return;

		laser->forwardAxis = (int)wi::math::Clamp((float)forwardAxis, 0.0f, 5.0f);
		laser->color = XMFLOAT3(colorR, colorG, colorB);
		laser->coreRadius = coreRadius;
		laser->intensity = intensity;
		laser->glowRadius = glowRadius;
		laser->glowIntensity = glowIntensity;
		laser->attenuation = attenuation;
		laser->range = range;
		laser->startOffset = startOffset;
		laser->occluded = occluded;
		laser->flicker = flicker;
		laser->flickerRate = flickerRate;
		laser->maxBounces = maxBounces;
		laser->opticBias = opticBias;

		laser->array.enabled = arrayEnabled;
		laser->array.shape = ParseArrayShape(arrayShape);
		laser->array.countX = arrayCountX;
		laser->array.countY = arrayCountY;
		laser->array.spreadAngle = XMFLOAT2(arraySpreadX, arraySpreadY);
		laser->array.offset = XMFLOAT2(arrayOffsetX, arrayOffsetY);
		laser->array.roll = arrayRoll;
		laser->array.spin = arraySpin;
		laser->array.spiralTurns = arraySpiralTurns;
		laser->array.hollow = arrayHollow;
		laser->array.falloff = arrayFalloff;
		laser->dot = dot;
		laser->dotRadius = dotRadius;
		laser->dotIntensity = dotIntensity;
		laser->surfaceRadius = surfaceRadius;
		laser->surfaceIntensity = surfaceIntensity;
		laser->trail = trail;
		laser->trailLife = trailLife;
		laser->trailSpacing = trailSpacing;
		laser->trailMax = trailMax;
		laser->trailFalloff = trailFalloff;
		laser->trailShrink = trailShrink;
		laser->debugDraw = debugDraw;
		laser->ignoreEntity = ignoreSelf ? entity : wi::ecs::INVALID_ENTITY;
		laser->rayMode = ParseMode(mode);

		// A ray component on the same entity is the authority on how far the beam
		// reaches and what it is allowed to stop on. Deferring to it keeps a
		// designator's readout and its visible beam pointing at the same surface,
		// which two independent casts do not guarantee.
		if (useSiblingRay) {
			if (const st::RayComponent* ray = GetComponent<st::RayComponent>()) {
				laser->rayMode = ray->ResolvedMode();
				laser->range = ray->maxDistance;
				laser->forwardAxis = ray->forwardAxis;
			}
		}
	}

public:
	void Start() override {
		Bind(forwardAxis, "forwardAxis");
		Bind(colorR, "colorR");
		Bind(colorG, "colorG");
		Bind(colorB, "colorB");
		Bind(coreRadius, "coreRadius");
		Bind(intensity, "intensity");
		Bind(glowRadius, "glowRadius");
		Bind(glowIntensity, "glowIntensity");
		Bind(attenuation, "attenuation");
		Bind(range, "range");
		Bind(startOffset, "startOffset");
		Bind(occluded, "occluded");
		Bind(flicker, "flicker");
		Bind(flickerRate, "flickerRate");
		Bind(maxBounces, "maxBounces");
		Bind(opticBias, "opticBias");
		Bind(arrayEnabled, "arrayEnabled");
		Bind(arrayShape, "arrayShape");
		Bind(arrayCountX, "arrayCountX");
		Bind(arrayCountY, "arrayCountY");
		Bind(arraySpreadX, "arraySpreadX");
		Bind(arraySpreadY, "arraySpreadY");
		Bind(arrayOffsetX, "arrayOffsetX");
		Bind(arrayOffsetY, "arrayOffsetY");
		Bind(arrayRoll, "arrayRoll");
		Bind(arraySpin, "arraySpin");
		Bind(arraySpiralTurns, "arraySpiralTurns");
		Bind(arrayHollow, "arrayHollow");
		Bind(arrayFalloff, "arrayFalloff");
		Bind(dot, "dot");
		Bind(dotRadius, "dotRadius");
		Bind(dotIntensity, "dotIntensity");
		Bind(surfaceRadius, "surfaceRadius");
		Bind(surfaceIntensity, "surfaceIntensity");
		Bind(trail, "trail");
		Bind(trailLife, "trailLife");
		Bind(trailSpacing, "trailSpacing");
		Bind(trailMax, "trailMax");
		Bind(trailFalloff, "trailFalloff");
		Bind(trailShrink, "trailShrink");
		Bind(ignoreSelf, "ignoreSelf");
		Bind(useSiblingRay, "useSiblingRay");
		Bind(debugDraw, "debugDraw");
		Bind(mode, "mode");

		st::Laser laser;
		laser.followEntity = entity;

		id = st::LaserSystem::Get().Add(laser);
		Apply();

		// Say so in the backlog. A laser attached through scene metadata is otherwise
		// invisible in code review - the beam appears with nothing in the game's
		// source to explain it.
		std::string owner = "entity " + std::to_string(entity);
		if (scene != nullptr) {
			if (const wi::scene::NameComponent* name = scene->names.GetComponent(entity)) {
				owner = "'" + name->name + "' (entity " + std::to_string(entity) + ")";
			}
		}
		wi::backlog::post("[Laser] component attached to " + owner + " from scene metadata, mode=" + mode +
			", bounces=" + std::to_string(maxBounces));
	}

	void Update(float /*dt*/) override {
		// Re-read the sibling ray every frame rather than once in Start(). Instances
		// on one entity are created together but started in metadata order, so at
		// Start() time the ray component may not have bound its own parameters yet -
		// and the values this reads would then be its defaults, not the scene's.
		if (!useSiblingRay) return;
		st::Laser* laser = st::LaserSystem::Get().Find(id);
		if (laser == nullptr) return;
		if (const st::RayComponent* ray = GetComponent<st::RayComponent>()) {
			laser->rayMode = ray->ResolvedMode();
			laser->range = ray->maxDistance;
			laser->forwardAxis = ray->forwardAxis;
		}
	}

	void Destroy() override {
		st::LaserSystem::Get().Remove(id);
		id = st::LaserSystem::INVALID;
	}

	void OnDisable() override {
		if (st::Laser* laser = st::LaserSystem::Get().Find(id)) {
			laser->enabled = false;
		}
	}

	void OnEnable() override {
		if (st::Laser* laser = st::LaserSystem::Get().Find(id)) {
			laser->enabled = true;
		}
	}

	void DrawDebug() override {
		if (st::LaserSystem::Get().Find(id) == nullptr) {
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Not registered - see BackLog.");
			return;
		}

		bool dirty = false;

		float color[3] = { colorR, colorG, colorB };
		if (ImGui::ColorEdit3("Color", color, ImGuiColorEditFlags_Float)) {
			colorR = color[0];
			colorG = color[1];
			colorB = color[2];
			dirty = true;
		}

		dirty |= ImGui::Combo("Forward axis", &forwardAxis, "+Z\0-Z\0-Y (spot light)\0+Y\0+X\0-X\0");
		dirty |= ImGui::DragFloat("Core radius", &coreRadius, 0.0005f, 0.0005f, 0.5f, "%.4f m");
		dirty |= ImGui::DragFloat("Intensity", &intensity, 0.5f, 0.0f, 500.0f);
		dirty |= ImGui::DragFloat("Glow radius", &glowRadius, 0.005f, 0.001f, 2.0f, "%.3f m");
		dirty |= ImGui::DragFloat("Glow intensity", &glowIntensity, 0.05f, 0.0f, 50.0f);
		dirty |= ImGui::SliderFloat("Attenuation", &attenuation, 0.0f, 1.0f);
		dirty |= ImGui::DragFloat("Range", &range, 1.0f, 0.1f, 5000.0f, "%.1f m");
		dirty |= ImGui::DragFloat("Start offset", &startOffset, 0.01f, 0.0f, 50.0f, "%.3f m");
		dirty |= ImGui::Checkbox("Occluded by geometry", &occluded);
		dirty |= ImGui::SliderFloat("Flicker", &flicker, 0.0f, 1.0f);
		if (flicker > 0.0f) {
			dirty |= ImGui::DragFloat("Flicker rate", &flickerRate, 0.5f, 0.1f, 200.0f, "%.1f Hz");
		}
		dirty |= ImGui::SliderInt("Max bounces", &maxBounces, 0, 12);
		dirty |= ImGui::DragFloat("Optic bias", &opticBias, 0.001f, 0.0f, 0.5f, "%.3f m");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"How far past a surface a mirror/lens still counts as in front of it.\n"
				"Raise it when the beam stops on the mesh instead of bouncing off the mirror in it.");
		}

		dirty |= ImGui::Checkbox("Array projection", &arrayEnabled);
		if (arrayEnabled) {
			int shape = (int)ParseArrayShape(arrayShape);
			if (ImGui::Combo("Pattern", &shape, "Grid\0Ring\0Fan\0Cross\0Spiral\0")) {
				arrayShape = ArrayShapeName((st::LaserArray::Shape)shape);
				dirty = true;
			}
			const st::LaserArray::Shape resolved = ParseArrayShape(arrayShape);

			dirty |= ImGui::SliderInt("Count X", &arrayCountX, 1, 32);
			if (resolved == st::LaserArray::Shape::Grid || resolved == st::LaserArray::Shape::Cross) {
				dirty |= ImGui::SliderInt("Count Y", &arrayCountY, 1, 32);
			}
			if (resolved == st::LaserArray::Shape::Spiral) {
				dirty |= ImGui::DragFloat("Turns", &arraySpiralTurns, 0.1f, 0.1f, 20.0f);
			}

			dirty |= ImGui::DragFloat("Spread X", &arraySpreadX, 0.002f, 0.0f, 1.4f, "%.4f rad");
			dirty |= ImGui::DragFloat("Spread Y", &arraySpreadY, 0.002f, 0.0f, 1.4f, "%.4f rad");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("All rays from one point, fanning out. The pattern grows with distance.");
			}
			dirty |= ImGui::DragFloat("Offset X", &arrayOffsetX, 0.005f, 0.0f, 10.0f, "%.3f m");
			dirty |= ImGui::DragFloat("Offset Y", &arrayOffsetY, 0.005f, 0.0f, 10.0f, "%.3f m");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Rays from different points, parallel. The pattern keeps its size.");
			}

			dirty |= ImGui::SliderFloat("Pattern roll", &arrayRoll, -XM_PI, XM_PI);
			dirty |= ImGui::DragFloat("Pattern spin", &arraySpin, 0.05f, -20.0f, 20.0f, "%.2f rad/s");
			dirty |= ImGui::Checkbox("Hollow", &arrayHollow);
			dirty |= ImGui::SliderFloat("Edge falloff", &arrayFalloff, 0.0f, 1.0f);

			// Every ray is a full trace, and the trace is the expensive half.
			const int rays = (int)st::LaserSystem::Get().BeamCount(id);
			ImGui::TextDisabled("%d rays = %d scene raycasts per bounce, every frame.", rays, rays);
		}

		dirty |= ImGui::Checkbox("Spot", &dot);
		if (dot) {
			dirty |= ImGui::DragFloat("Air glow radius", &dotRadius, 0.002f, 0.001f, 1.0f, "%.3f m");
			dirty |= ImGui::DragFloat("Air glow intensity", &dotIntensity, 0.2f, 0.0f, 200.0f);
			dirty |= ImGui::DragFloat("Surface radius", &surfaceRadius, 0.002f, 0.001f, 2.0f, "%.3f m");
			dirty |= ImGui::DragFloat("Surface intensity", &surfaceIntensity, 0.2f, 0.0f, 200.0f);
		}

		dirty |= ImGui::Checkbox("Persistence trail", &trail);
		if (trail) {
			dirty |= ImGui::DragFloat("Trail life", &trailLife, 0.01f, 0.01f, 5.0f, "%.2f s");
			dirty |= ImGui::DragFloat("Trail spacing", &trailSpacing, 0.001f, 0.001f, 1.0f, "%.3f m");
			dirty |= ImGui::SliderInt("Trail points", &trailMax, 1, 128);
			dirty |= ImGui::SliderFloat("Trail falloff", &trailFalloff, 0.2f, 6.0f);
			dirty |= ImGui::SliderFloat("Trail shrink", &trailShrink, 0.0f, 1.0f);
		}

		dirty |= ImGui::Checkbox("Use sibling sticRay", &useSiblingRay);
		dirty |= ImGui::Checkbox("Debug lines", &debugDraw);
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Draws the traced path: white ran out of range, green landed on geometry,\n"
				"cyan reflected off a mirror, magenta refracted through a lens.\n"
				"A bounce missing from the LINES means the trace never found the mirror.");
		}
		ImGui::TextDisabled("Stop on: %s", mode.c_str());

		if (const st::BeamPath* path = st::LaserSystem::Get().Path(id)) {
			ImGui::Separator();
			ImGui::Text("%d legs", (int)path->legs.size());
			for (size_t i = 0; i < path->legs.size(); ++i) {
				const st::BeamLeg& leg = path->legs[i];
				const char* ended = "range";
				switch (leg.termination) {
				case st::BeamLeg::Termination::Surface: ended = "surface"; break;
				case st::BeamLeg::Termination::Mirror: ended = "mirror"; break;
				case st::BeamLeg::Termination::Lens: ended = "lens"; break;
				default: break;
				}
				ImGui::BulletText("leg %d: %.2f m, ended on %s", (int)i, leg.length, ended);
			}
			// One leg ending on "range" or "surface" when you expected a bounce means
			// the trace never found the element - check the mirror's own inspector,
			// which says whether a beam reached its plane and by how much it missed.
			if (path->legs.size() == 1 && maxBounces > 0) {
				ImGui::TextDisabled("Single leg: no mirror or lens was met.");
			}
			if (path->exhaustedBounces) {
				ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Bounce budget exhausted.");
			}
			if (path->hit.hit) {
				ImGui::Text("Hit entity %llu at %.2f m", (unsigned long long)path->hit.entity, path->hit.distance);
			} else {
				ImGui::TextDisabled("No surface hit.");
			}
		}

		// Apply() pushes the change into the live system; SaveBoundParams() writes the same
		// values into the NCA_ metadata Bind() read them from, which is what makes an edit
		// here survive a save and reload instead of living until the next scene load.
		if (dirty) { Apply(); SaveBoundParams(); }
	}
};

ST_REGISTER_NATIVE_COMPONENT_AS(StLaserComponent, "sticLaser")
