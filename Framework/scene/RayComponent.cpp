#include "scene/RayComponent.h"

#include "wiScene.h"
#include "wiRenderer.h"
#include "imgui.h"

namespace st {

RayQuery::Mode RayComponent::ResolvedMode() const {
	if (mode == "physics") return RayQuery::Mode::Physics;
	if (mode == "both") return RayQuery::Mode::Both;
	if (mode == "none") return RayQuery::Mode::None;
	return RayQuery::Mode::Mesh;
}

void RayComponent::Start() {
	Bind(forwardAxis, "forwardAxis");
	Bind(mode, "mode");
	Bind(maxDistance, "maxDistance");
	Bind(startOffset, "startOffset");
	Bind(ignoreSelf, "ignoreSelf");
	Bind(everyFrame, "everyFrame");
	Bind(debugDraw, "debugDraw");

	Cast();
}

const RayHit& RayComponent::Cast() {
	if (scene == nullptr) return hit_;

	if (!EntityRay(*scene, entity, forwardAxis, origin_, direction_)) {
		// No transform on the owning entity: there is no ray to cast, and reporting
		// the previous frame's hit would be worse than reporting none.
		hit_ = RayHit();
		return hit_;
	}

	if (startOffset != 0.0f) {
		origin_.x += direction_.x * startOffset;
		origin_.y += direction_.y * startOffset;
		origin_.z += direction_.z * startOffset;
	}

	RayQuery query;
	query.origin = origin_;
	query.direction = direction_;
	query.maxDistance = maxDistance;
	query.mode = ResolvedMode();
	query.ignoreEntity = ignoreSelf ? entity : wi::ecs::INVALID_ENTITY;

	hit_ = Raycast(*scene, query);
	return hit_;
}

void RayComponent::Update(float /*dt*/) {
	// Runs on a job thread (the default): the cast itself only reads the scene, which is
	// exactly the work worth spreading over cores - a scene full of rays now costs one
	// core's worth of casting divided by however many the machine has.
	if (everyFrame) Cast();

	if (debugDraw) {
		wi::renderer::RenderableLine line;
		line.start = origin_;
		line.end = hit_.position;
		// Green when it found something, red when it ran to full range - the two
		// states a ray you are placing needs to tell apart at a glance.
		const XMFLOAT4 color = hit_.hit ? XMFLOAT4(0.2f, 1.0f, 0.3f, 1.0f) : XMFLOAT4(1.0f, 0.3f, 0.2f, 1.0f);
		line.color_start = color;
		line.color_end = color;
		// wi::renderer keeps debug lines in a plain global vector with no lock, so the one
		// unsafe line here is handed to the main thread rather than dropping the whole
		// component out of the parallel pass. It still lands this frame.
		RunOnMainThread([line] { wi::renderer::DrawLine(line); });
	}
}

void RayComponent::DrawDebug() {
	// Every widget feeds one flag, so an edit can be written back to the NCA_ metadata
	// Bind() read it from -- without that, tuning a ray in the inspector is lost on reload.
	bool dirty = false;
	dirty |= ImGui::Combo("Forward axis", &forwardAxis, "+Z\0-Z\0-Y (spot light)\0+Y\0+X\0-X\0");
	dirty |= ImGui::DragFloat("Max distance", &maxDistance, 1.0f, 0.1f, 10000.0f, "%.1f m");
	dirty |= ImGui::DragFloat("Start offset", &startOffset, 0.01f, 0.0f, 50.0f, "%.3f m");
	dirty |= ImGui::Checkbox("Ignore self", &ignoreSelf);
	dirty |= ImGui::Checkbox("Cast every frame", &everyFrame);
	ImGui::SameLine();
	if (ImGui::Button("Cast now")) Cast();
	dirty |= ImGui::Checkbox("Debug line", &debugDraw);
	if (dirty) SaveBoundParams();

	ImGui::TextDisabled("Mode: %s", mode.c_str());

	ImGui::Separator();
	if (hit_.hit) {
		ImGui::Text("Hit entity %llu", (unsigned long long)hit_.entity);
		ImGui::Text("Distance   %.3f m", hit_.distance);
		ImGui::Text("Position   %.2f %.2f %.2f", hit_.position.x, hit_.position.y, hit_.position.z);
		ImGui::Text("Normal     %.2f %.2f %.2f", hit_.normal.x, hit_.normal.y, hit_.normal.z);
		ImGui::Text("Source     %s", hit_.source == RayHit::Source::Physics ? "physics" : "mesh");
		if (hit_.source == RayHit::Source::Mesh) {
			ImGui::Text("UV         %.3f %.3f (subset %d)", hit_.uv.x, hit_.uv.y, hit_.subsetIndex);
		}
	} else {
		ImGui::TextDisabled("No hit - ray ran the full %.1f m.", maxDistance);
	}
}

} // namespace st

// The registration macro pastes the type name into an identifier, so it cannot take a
// qualified one. The alias is the type, so GetNativeTypeID<> still resolves to the
// same identity a GetComponent<st::RayComponent>() lookup asks for.
using StRayComponent = st::RayComponent;
ST_REGISTER_NATIVE_COMPONENT_AS(StRayComponent, "sticRay")
