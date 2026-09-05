#pragma once
#include "Simtary.h"
#include "io/Nbt.h"

namespace st {

// Procedural screen-space lens flare, drawn as one additive fullscreen triangle
// over the composed frame (see assets/shaders/StLensFlarePS.hlsl for the maths).
//
// This is deliberately separate from the engine's own lens flare
// (Simtary/shaders/lensFlare*.hlsl), which is a texture-billboard system driven by
// LightComponent::lensFlareNames. This one samples nothing and needs no authored
// textures - it only needs the sun's screen position.
//
// Usage from Milistry:
//   Init()               once, after the graphics device exists
//   Update(dt)           each frame, before Compose: finds the sun, projects it
//   Draw(canvas, cmd)    inside Compose, after the render path, before ImGui
class LensFlare {
public:
	struct Settings {
		bool  enabled = false;

		// Track the scene's brightest directional light. Off = drive sunDirection
		// manually (there is no directional light, or you want the flare elsewhere).
		bool  followSun = true;
		// Tint the flare with the sun's own colour, so it warms up at sunset along
		// with the light. Off = use `tint` verbatim.
		bool  tintFromSun = true;

		float    intensity = 1.0f;
		XMFLOAT3 tint = XMFLOAT3(1.0f, 0.85f, 0.65f);

		float glowIntensity = 1.0f;
		float streakIntensity = 0.5f;
		float starburstIntensity = 0.0f;

		int   ghostCount = 6;
		float ghostSpacing = 0.3f;

		float haloWidth = 0.45f;
		float chromaOffset = 0.01f;

		// Fade the flare out as the sun drops to the horizon. Below `horizonFadeLow`
		// (sun direction .y) it is gone; above `horizonFadeHigh` it is at full
		// strength. Keeps the flare from burning through the ground at night.
		float horizonFadeLow = -0.05f;
		float horizonFadeHigh = 0.10f;

		// How far past the edge of the frame (in UV units) the sun may drift before
		// the flare is fully gone. Real ghosts persist a little past the edge.
		float offscreenFade = 0.35f;
	};
	Settings settings;

	// World-space direction *towards* the light, matching LightComponent::direction.
	// Overwritten every Update when settings.followSun is on.
	XMFLOAT3 sunDirection = XMFLOAT3(0.0f, 1.0f, 0.0f);

	void Init();
	void Update(const wi::scene::Scene& scene, const wi::scene::CameraComponent& camera, float dt);
	void Draw(const wi::Canvas& canvas, wi::graphics::CommandList cmd);
	void GUI();

	// Persistence: (de)serialize `settings` (+ the manual sunDirection) into an NBT
	// compound. Stored under the "lensflare" child of options.stad by SettingsManager.
	void SaveTo(st::nbt::Tag& out) const;
	void LoadFrom(const st::nbt::Tag& in);

	bool IsValid() const { return pso_.IsValid(); }

private:
	// Mirrors LensFlareCB in assets/shaders/StLensFlare.hlsli. Four 16-byte rows;
	// keep the two in sync, field for field.
	struct Constants {
		XMFLOAT2 sunUV = XMFLOAT2(0.5f, 0.5f);
		float    aspect = 1.0f;
		float    intensity = 1.0f;

		XMFLOAT3 tint = XMFLOAT3(1.0f, 1.0f, 1.0f);
		float    ghostSpacing = 0.3f;

		float    ghostCount = 6.0f;
		float    haloWidth = 0.45f;
		float    streakIntensity = 0.5f;
		float    glowIntensity = 1.0f;

		float    chromaOffset = 0.01f;
		float    starburstIntensity = 0.35f;
		float    time = 0.0f;
		float    occlusion = 0.0f;
	};
	static_assert(sizeof(Constants) == 64, "Constants must match LensFlareCB's four 16-byte rows");

	Constants constants_;
	float     time_ = 0.0f;

	wi::graphics::Shader        vs_, ps_;
	wi::graphics::PipelineState pso_;
};

} // namespace st
